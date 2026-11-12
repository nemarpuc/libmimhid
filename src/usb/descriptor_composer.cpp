// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/*
 * Descriptor layouts: USB 2.0 9.6.1 (Device), 9.6.3 (Configuration), 9.6.5 (Interface), 9.6.6
 * (Endpoint), 9.6.7 (String); the Interface Association Descriptor of the USB-IF's Interface
 * Association Descriptor ECN.
 */

#include "usb/descriptor_composer.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace mimusb::usb {

namespace {

constexpr std::uint8_t kTypeDevice = 1;
constexpr std::uint8_t kTypeConfiguration = 2;
constexpr std::uint8_t kTypeString = 3;
constexpr std::uint8_t kTypeInterface = 4;
constexpr std::uint8_t kTypeEndpoint = 5;
constexpr std::uint8_t kTypeAssociation = 0x0B;

void put_u16(std::vector<std::uint8_t>* out, std::uint32_t value) {
    out->push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out->push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

std::vector<std::uint8_t> string_descriptor(const std::u16string& units) {
    std::vector<std::uint8_t> d;
    d.push_back(static_cast<std::uint8_t>(2U + 2U * units.size()));
    d.push_back(kTypeString);
    for (const char16_t unit : units)
        put_u16(&d, unit);
    return d;
}

void endpoint_descriptor(std::vector<std::uint8_t>* out, const mimusb_usb_endpoint& ep) {
    out->push_back(ep.audio_form != 0U ? 9U : 7U);
    out->push_back(kTypeEndpoint);
    out->push_back(ep.address);
    // USB 2.0 Table 9-13: bits 1..0 transfer type, 3..2 synchronization type, 5..4 usage type.
    out->push_back(
        static_cast<std::uint8_t>(ep.transfer_type | (ep.sync_type << 2U) | (ep.usage_type << 4U)));
    put_u16(out, ep.max_packet_size);
    out->push_back(ep.interval);
    if (ep.audio_form != 0U) {
        out->push_back(ep.refresh);
        out->push_back(ep.synch_address);
    }
}

} // namespace

std::size_t queue_slots(const std::uint8_t depth, const std::uint8_t on_full) noexcept {
    return on_full == MIMUSB_FULL_DROP_OLDEST ? 2U * static_cast<std::size_t>(depth) + 1U
                                              : static_cast<std::size_t>(depth) + 1U;
}

mimusb_result check_endpoint_shape(const mimusb_usb_endpoint& ep, const ComposerLimits& lim,
                                   const std::uint16_t node_id, ComposerError* error) {
    const auto refuse = [&](mimusb_result result, const char* field, const char* reason,
                            std::uint32_t offset = 0, std::uint32_t length = 0,
                            std::uint16_t id = 0) {
        if (error != nullptr)
            *error = ComposerError{result, field, reason, offset, length, id};
        return result;
    };
    const std::uint8_t number = ep.address & 0x0FU;
    // USB 2.0 9.6.6: bits 6..4 are reserved and zero; number 0 is the control endpoint,
    // which has no endpoint descriptor.
    if ((ep.address & 0x70U) != 0U || number == 0U)
        return refuse(MIMUSB_ERR_PARAM, "node_config.interfaces.settings.endpoints.address",
                      "An endpoint address is 0x81 to 0x8F (IN) or 0x01 to 0x0F (OUT): "
                      "bits 6..4 zero, endpoint number 1 to 15.",
                      ep.address, 0, node_id);
    if (number > lim.max_endpoint_number)
        return refuse(MIMUSB_ERR_OVERFLOW, "node_config.interfaces.settings.endpoints.address",
                      "The endpoint number is above the highest the board's USB controller "
                      "has (mimusb_board_limits.max_endpoint_number).",
                      static_cast<std::uint32_t>(lim.max_endpoint_number), number, node_id);
    if (ep.audio_form > 1U)
        return refuse(MIMUSB_ERR_PARAM, "node_config.interfaces.settings.endpoints.audio_form",
                      "audio_form is 0 (a 7-byte descriptor) or 1 (9 bytes, with refresh and "
                      "synch_address).",
                      ep.address, ep.audio_form, node_id);
    if (ep.audio_form == 0U && (ep.refresh != 0U || ep.synch_address != 0U))
        return refuse(MIMUSB_ERR_PARAM, "node_config.interfaces.settings.endpoints.refresh",
                      "refresh and synch_address are for audio_form 1 and are zero otherwise.",
                      ep.address, 0, node_id);
    if (ep.transfer_type == 0U)
        return refuse(MIMUSB_ERR_UNSET_FIELD,
                      "node_config.interfaces.settings.endpoints.transfer_type",
                      "transfer_type is required: MIMUSB_TRANSFER_ISOCHRONOUS, _BULK or "
                      "_INTERRUPT.",
                      ep.address, 0, node_id);
    if (ep.transfer_type > 3U)
        return refuse(MIMUSB_ERR_PARAM, "node_config.interfaces.settings.endpoints.transfer_type",
                      "transfer_type is bmAttributes bits 1..0: 1 isochronous, 2 bulk or 3 "
                      "interrupt.",
                      ep.address, ep.transfer_type, node_id);
    if ((lim.transfer_types & (1U << ep.transfer_type)) == 0U)
        return refuse(MIMUSB_ERR_UNSUPPORTED,
                      "node_config.interfaces.settings.endpoints.transfer_type",
                      "The board has no endpoints of this transfer type "
                      "(mimusb_board_limits.transfer_types).",
                      ep.address, ep.transfer_type, node_id);
    const bool iso = ep.transfer_type == MIMUSB_TRANSFER_ISOCHRONOUS;
    if (!iso && (ep.sync_type != 0U || ep.usage_type != 0U))
        return refuse(MIMUSB_ERR_PARAM, "node_config.interfaces.settings.endpoints.sync_type",
                      "sync_type and usage_type are for isochronous endpoints and are zero "
                      "otherwise.",
                      ep.address, 0, node_id);
    if (iso && (ep.sync_type > MIMUSB_ISO_SYNC_SYNCHRONOUS ||
                ep.usage_type > MIMUSB_ISO_USAGE_IMPLICIT_FEEDBACK))
        return refuse(MIMUSB_ERR_PARAM, "node_config.interfaces.settings.endpoints.sync_type",
                      "sync_type is 0 to 3 and usage_type 0 to 2 (USB 2.0 Table 9-13).", ep.address,
                      ep.sync_type, node_id);
    if (ep.max_packet_size == 0U && !iso)
        return refuse(MIMUSB_ERR_UNSET_FIELD,
                      "node_config.interfaces.settings.endpoints.max_packet_size",
                      "max_packet_size must be non-zero (1 to the board's largest packet).",
                      ep.address, 0, node_id);
    if (iso) {
        // USB 2.0 5.6.3, 9.6.6: one transaction per frame, so at most 1023 bytes at full speed
        // and 1024 at high speed, and no more than one frame to the board holds; bInterval is 1
        // at full speed and an exponent 1 to 16 at high speed.
        const std::size_t largest =
            std::min<std::size_t>(lim.max_transfer_bytes, lim.high_speed ? 1024U : 1023U);
        if (ep.max_packet_size == 0U || ep.max_packet_size > largest)
            return refuse(MIMUSB_ERR_OVERFLOW,
                          "node_config.interfaces.settings.endpoints.max_packet_size",
                          "An isochronous packet is 1 to the board's largest (1023 bytes at full "
                          "speed, 1024 at high speed, and no more than "
                          "mimusb_board_limits.max_transfer_bytes).",
                          static_cast<std::uint32_t>(largest), ep.max_packet_size, node_id);
        if (lim.high_speed ? (ep.interval == 0U || ep.interval > 16U) : ep.interval != 1U)
            return refuse(MIMUSB_ERR_PARAM, "node_config.interfaces.settings.endpoints.interval",
                          lim.high_speed ? "bInterval of an isochronous endpoint is 1 to 16 at "
                                           "high speed (USB 2.0 9.6.6)."
                                         : "bInterval of an isochronous endpoint is 1 at full "
                                           "speed (USB 2.0 9.6.6).",
                          ep.address, ep.interval, node_id);
        return MIMUSB_OK;
    }
    if (ep.max_packet_size > lim.max_endpoint_bytes)
        return refuse(
            MIMUSB_ERR_OVERFLOW, "node_config.interfaces.settings.endpoints.max_packet_size",
            "An endpoint packet is larger than the board's endpoints hold.",
            static_cast<std::uint32_t>(lim.max_endpoint_bytes), ep.max_packet_size, node_id);
    if (ep.transfer_type == MIMUSB_TRANSFER_BULK) {
        // USB 2.0 5.8.3: bulk packets are 8, 16, 32 or 64 bytes at full speed and 512 at
        // high speed.
        const bool allowed = lim.high_speed
                                 ? ep.max_packet_size == 512U
                                 : (ep.max_packet_size == 8U || ep.max_packet_size == 16U ||
                                    ep.max_packet_size == 32U || ep.max_packet_size == 64U);
        if (!allowed)
            return refuse(
                MIMUSB_ERR_PARAM, "node_config.interfaces.settings.endpoints.max_packet_size",
                lim.high_speed ? "A bulk endpoint of a high-speed device has 512-byte "
                                 "packets (USB 2.0 5.8.3)."
                               : "A bulk endpoint of a full-speed device has 8, 16, 32 or "
                                 "64-byte packets (USB 2.0 5.8.3).",
                ep.address, ep.max_packet_size, node_id);
    } else {
        if (ep.interval == 0U)
            return refuse(MIMUSB_ERR_UNSET_FIELD,
                          "node_config.interfaces.settings.endpoints.interval",
                          "An interrupt endpoint needs a bInterval (1 to 255; 1 to 16 at "
                          "high speed; USB 2.0 9.6.6).",
                          ep.address, 0, node_id);
        if (lim.high_speed && ep.interval > 16U)
            return refuse(MIMUSB_ERR_PARAM, "node_config.interfaces.settings.endpoints.interval",
                          "A high-speed board takes a bInterval of 1 to 16 (USB 2.0 "
                          "9.6.6).",
                          ep.address, ep.interval, node_id);
    }
    return MIMUSB_OK;
}

mimusb_result compose_descriptors(const ComposerInput& input, Composed* out, ComposerError* error) {
    const ComposerLimits& lim = input.limits;
    const auto refuse = [&](mimusb_result result, const char* field, const char* reason,
                            std::uint32_t offset = 0, std::uint32_t length = 0,
                            std::uint16_t node_id = 0) {
        if (error != nullptr)
            *error = ComposerError{result, field, reason, offset, length, node_id};
        return result;
    };

    // Every interface in number order, with the group it came in.
    struct Entry {
        const ComposerInterface* itf;
        const ComposerGroup* group;
    };
    std::map<std::uint32_t, Entry> by_number;
    for (const ComposerGroup& group : input.groups) {
        for (const ComposerInterface& itf : group.interfaces) {
            if (!by_number.emplace(itf.number, Entry{&itf, &group}).second)
                return refuse(MIMUSB_ERR_PARAM, "node_config.interfaces.number",
                              "Two interfaces have the same number; the number is in offset.",
                              itf.number, 0, group.node_id);
        }
    }
    if (by_number.size() > lim.max_interfaces)
        return refuse(MIMUSB_ERR_OVERFLOW, "node_config.interfaces",
                      "The interfaces of all Nodes exceed the board's maximum interfaces.",
                      static_cast<std::uint32_t>(lim.max_interfaces),
                      static_cast<std::uint32_t>(by_number.size()));

    // USB 2.0 9.6.5: bInterfaceNumber is the index of the interface in the configuration, so the
    // numbers are 0, 1, 2, ... in order. A gap is the caller's to close; nothing is renumbered.
    std::uint32_t expected = 0;
    for (const auto& entry : by_number) {
        if (input.partial)
            break;
        if (entry.first != expected)
            return refuse(MIMUSB_ERR_PARAM, "node_config.interfaces.number",
                          "Interface numbers must be 0, 1, 2, ... without a gap; the missing "
                          "number is in offset.",
                          expected, static_cast<std::uint32_t>(by_number.size()),
                          entry.second.group->node_id);
        ++expected;
    }

    // Associations: 8 bytes, type 0x0B, naming this Node's interfaces, which are consecutive.
    for (const ComposerGroup& group : input.groups) {
        if (group.association.empty())
            continue;
        const std::vector<std::uint8_t>& a = group.association;
        if (a.size() != 8U || a[0] != 8U || a[1] != kTypeAssociation)
            return refuse(MIMUSB_ERR_PARAM, "node_config.association",
                          "An interface association descriptor is 8 bytes of type 0x0B.", 0,
                          static_cast<std::uint32_t>(a.size()), group.node_id);
        if (group.interfaces.empty() || a[3] != group.interfaces.size())
            return refuse(MIMUSB_ERR_PARAM, "node_config.association",
                          "bInterfaceCount must be the number of this Node's interfaces.", a[3],
                          static_cast<std::uint32_t>(group.interfaces.size()), group.node_id);
        std::uint32_t lowest = 256U;
        for (const ComposerInterface& itf : group.interfaces)
            lowest = std::min<std::uint32_t>(lowest, itf.number);
        if (a[2] != lowest)
            return refuse(MIMUSB_ERR_PARAM, "node_config.association",
                          "bFirstInterface must be the lowest interface number of this Node.", a[2],
                          lowest, group.node_id);
        for (std::size_t i = 0; i < group.interfaces.size(); ++i) {
            const auto wanted = by_number.find(lowest + static_cast<std::uint32_t>(i));
            if (wanted == by_number.end() || wanted->second.group != &group)
                return refuse(MIMUSB_ERR_PARAM, "node_config.association",
                              "The interfaces of a Node with an association must have "
                              "consecutive numbers.",
                              static_cast<std::uint32_t>(lowest + i), 0, group.node_id);
        }
    }

    // Settings and their items. An interface has one or more settings (bAlternateSetting 0, 1, 2,
    // ... in order, so none can be missing). An endpoint address is unique inside a setting; it may
    // come again in another setting of the same interface (the same type, and for bulk and
    // interrupt the same descriptor and policy: the board opens those once for the configuration;
    // an isochronous endpoint is started with the descriptor of the setting selected, so its
    // packet size and format may differ) and never in another interface. An isochronous endpoint
    // is in a setting other than 0 (USB 2.0 5.6.3: the default setting carries no isochronous
    // bandwidth). Every item that is a descriptor is one whole descriptor.
    struct Owner {
        std::uint32_t interface_number;
        mimusb_usb_endpoint first;
    };
    std::map<std::uint8_t, Owner> owners;
    std::size_t pool_slots = 0;
    for (const auto& entry : by_number) {
        const ComposerInterface& itf = *entry.second.itf;
        const std::uint16_t node_id = entry.second.group->node_id;
        if (itf.settings.empty())
            return refuse(MIMUSB_ERR_PARAM, "node_config.interfaces.settings",
                          "An interface has at least one setting (setting 0); the interface "
                          "number is in offset.",
                          itf.number, 0, node_id);
        if (itf.settings.size() > 255U)
            return refuse(MIMUSB_ERR_OVERFLOW, "node_config.interfaces.settings",
                          "An interface has at most 255 alternate settings (bAlternateSetting is "
                          "one byte); the interface number is in offset.",
                          itf.number, static_cast<std::uint32_t>(itf.settings.size()), node_id);
        for (std::size_t setting_index = 0; setting_index < itf.settings.size(); ++setting_index) {
            std::set<std::uint8_t> in_setting;
            for (const ComposerItem& item : itf.settings[setting_index].items) {
                if (!item.is_endpoint) {
                    const std::vector<std::uint8_t>& d = item.descriptor;
                    // USB 2.0 9.5: bLength, then bDescriptorType; the whole descriptor is the item.
                    if (d.size() < 2U || d[0] != d.size())
                        return refuse(MIMUSB_ERR_PARAM,
                                      "node_config.interfaces.settings.items.descriptor",
                                      "A descriptor item is one whole descriptor: its bLength is "
                                      "at least 2 and equals descriptor_length; the interface "
                                      "number is in offset.",
                                      itf.number, static_cast<std::uint32_t>(d.size()), node_id);
                    if (d[1] == kTypeInterface || d[1] == kTypeEndpoint)
                        return refuse(MIMUSB_ERR_PARAM,
                                      "node_config.interfaces.settings.items.descriptor",
                                      "A descriptor item is not an Interface (4) or Endpoint (5) "
                                      "descriptor: the library writes those from the settings "
                                      "and endpoints.",
                                      itf.number, d[1], node_id);
                    continue;
                }
                const mimusb_usb_endpoint& ep = item.endpoint;
                const bool in = (ep.address & 0x80U) != 0U;
                if (const mimusb_result shape = check_endpoint_shape(ep, lim, node_id, error);
                    shape != MIMUSB_OK)
                    return shape;
                if (ep.transfer_type == MIMUSB_TRANSFER_ISOCHRONOUS && setting_index == 0U)
                    return refuse(MIMUSB_ERR_PARAM,
                                  "node_config.interfaces.settings.endpoints.transfer_type",
                                  "An isochronous endpoint belongs to a setting other than 0: the "
                                  "default setting carries no isochronous bandwidth (USB 2.0 "
                                  "5.6.3); the address is in offset.",
                                  ep.address, itf.number, node_id);
                if (!in_setting.insert(ep.address).second)
                    return refuse(MIMUSB_ERR_PARAM,
                                  "node_config.interfaces.settings.endpoints.address",
                                  "Two endpoints of one setting use the same address; it is in "
                                  "offset.",
                                  ep.address, entry.first, node_id);
                if (in) {
                    if (ep.policy.on_full == 0U || ep.policy.depth == 0U)
                        return refuse(MIMUSB_ERR_UNSET_FIELD,
                                      "node_config.interfaces.settings.endpoints.policy",
                                      "An IN endpoint needs a policy: on_full and depth are "
                                      "required.",
                                      ep.address, 0, node_id);
                    if (ep.policy.on_full > MIMUSB_FULL_WAIT)
                        return refuse(MIMUSB_ERR_PARAM,
                                      "node_config.interfaces.settings.endpoints.policy",
                                      "on_full is MIMUSB_FULL_DROP_NEWEST, _DROP_OLDEST or _WAIT.",
                                      ep.address, ep.policy.on_full, node_id);
                } else if (ep.policy.on_full != 0U || ep.policy.depth != 0U) {
                    return refuse(
                        MIMUSB_ERR_PARAM, "node_config.interfaces.settings.endpoints.policy",
                        "The policy of an OUT endpoint must be all zero.", ep.address, 0, node_id);
                }
                const auto owner = owners.find(ep.address);
                if (owner == owners.end()) {
                    owners.emplace(ep.address, Owner{entry.first, ep});
                    if (in)
                        pool_slots += queue_slots(ep.policy.depth, ep.policy.on_full);
                    continue;
                }
                const mimusb_usb_endpoint& first = owner->second.first;
                const bool iso = ep.transfer_type == MIMUSB_TRANSFER_ISOCHRONOUS;
                if (owner->second.interface_number != entry.first ||
                    first.transfer_type != ep.transfer_type ||
                    first.policy.on_full != ep.policy.on_full ||
                    first.policy.depth != ep.policy.depth ||
                    (!iso && (first.interval != ep.interval ||
                              first.max_packet_size != ep.max_packet_size ||
                              first.audio_form != ep.audio_form || first.refresh != ep.refresh ||
                              first.synch_address != ep.synch_address)))
                    return refuse(MIMUSB_ERR_PARAM,
                                  "node_config.interfaces.settings.endpoints.address",
                                  "An address is used again only by the same interface in "
                                  "another setting, with the same type and policy and, for a "
                                  "bulk or interrupt endpoint, the same descriptor; it is in "
                                  "offset.",
                                  ep.address, entry.first, node_id);
            }
        }
    }
    if (pool_slots > lim.endpoint_queue_slots)
        return refuse(MIMUSB_ERR_OVERFLOW, "node_config.interfaces.settings.endpoints.policy",
                      "The queues of the IN endpoints need more slots than the board's pool "
                      "(mimusb_board_limits.endpoint_queue_slots); the slots needed are in "
                      "length.",
                      static_cast<std::uint32_t>(lim.endpoint_queue_slots),
                      static_cast<std::uint32_t>(pool_slots));

    Composed composed;

    // Strings: 1..4, the caller's from 5 up, and one per named interface from the lowest index
    // nothing else has; the language of all of them is the Device's (USB 2.0 9.6.7). The serial
    // number is the Device's own text, else the board's when asked.
    const mimusb_usb_device& dev = input.device;
    const mimusb_usb_config& cfg = input.config;
    const auto add_string = [&](std::uint8_t index, const std::u16string& text) {
        composed.strings.push_back(ComposedString{
            index, static_cast<std::uint16_t>(dev.language_id), false, string_descriptor(text)});
    };
    if (!input.manufacturer.empty())
        add_string(kStringManufacturer, input.manufacturer);
    if (!input.product.empty())
        add_string(kStringProduct, input.product);
    std::uint8_t serial_index = 0;
    if (!input.serial.empty()) {
        add_string(kStringSerial, input.serial);
        serial_index = kStringSerial;
    } else if (dev.serial_from_chip_id == 1U && lim.has_unique_id) {
        composed.strings.push_back(
            ComposedString{kStringSerial, static_cast<std::uint16_t>(dev.language_id), true, {}});
        serial_index = kStringSerial;
    }
    if (!input.configuration.empty())
        add_string(kStringConfiguration, input.configuration);
    std::set<std::uint8_t> used_indices;
    for (const auto& entry : input.user_strings) {
        add_string(entry.first, entry.second);
        used_indices.insert(entry.first);
    }
    std::map<std::uint32_t, std::uint8_t> name_index;
    std::uint32_t next_free = kStringFirstFree;
    for (const auto& entry : by_number) {
        if (entry.second.itf->name.empty())
            continue;
        while (next_free <= 255U && used_indices.count(static_cast<std::uint8_t>(next_free)) != 0U)
            ++next_free;
        if (next_free > 255U)
            return refuse(MIMUSB_ERR_OVERFLOW, "node_config.interfaces.name",
                          "There is no free string index left for an interface name.", 255U, 0,
                          entry.second.group->node_id);
        name_index[entry.first] = static_cast<std::uint8_t>(next_free);
        add_string(static_cast<std::uint8_t>(next_free), entry.second.itf->name);
        used_indices.insert(static_cast<std::uint8_t>(next_free));
    }
    if (composed.strings.size() > lim.max_strings)
        return refuse(MIMUSB_ERR_OVERFLOW, "node_config.interfaces.name",
                      "The strings exceed the board's max_strings.",
                      static_cast<std::uint32_t>(lim.max_strings),
                      static_cast<std::uint32_t>(composed.strings.size()));

    // The Configuration descriptor with, per interface in number order, an association before
    // its first interface, then each setting: the Interface descriptor and the items in order.
    std::vector<std::uint8_t>& c = composed.configuration;
    c.assign(9, 0);
    for (const auto& entry : by_number) {
        const ComposerInterface& itf = *entry.second.itf;
        const ComposerGroup& group = *entry.second.group;
        if (!group.association.empty() && group.association[2] == itf.number)
            c.insert(c.end(), group.association.begin(), group.association.end());
        const auto name = name_index.find(entry.first);
        for (std::size_t setting_index = 0; setting_index < itf.settings.size(); ++setting_index) {
            const ComposerSetting& setting = itf.settings[setting_index];
            std::size_t endpoints = 0;
            for (const ComposerItem& item : setting.items)
                endpoints += item.is_endpoint ? 1U : 0U;
            c.push_back(9);
            c.push_back(kTypeInterface);
            c.push_back(itf.number);
            c.push_back(static_cast<std::uint8_t>(setting_index)); // bAlternateSetting
            c.push_back(static_cast<std::uint8_t>(endpoints));
            c.push_back(itf.class_code);
            c.push_back(itf.subclass);
            c.push_back(itf.protocol);
            c.push_back(name == name_index.end() ? 0U : name->second);
            for (const ComposerItem& item : setting.items) {
                if (item.is_endpoint)
                    endpoint_descriptor(&c, item.endpoint);
                else
                    c.insert(c.end(), item.descriptor.begin(), item.descriptor.end());
            }
        }
        ++composed.interface_count;
    }
    const std::size_t total = c.size();
    if (total > lim.max_config_bytes)
        return refuse(MIMUSB_ERR_OVERFLOW, "node_config.interfaces",
                      "The Configuration descriptor exceeds the board's "
                      "max_config_descriptor_bytes.",
                      static_cast<std::uint32_t>(lim.max_config_bytes),
                      static_cast<std::uint32_t>(total));
    c[0] = 9;
    c[1] = kTypeConfiguration;
    c[2] = static_cast<std::uint8_t>(total & 0xFFU);
    c[3] = static_cast<std::uint8_t>((total >> 8U) & 0xFFU);
    c[4] = static_cast<std::uint8_t>(composed.interface_count);
    c[5] = cfg.configuration_value;
    c[6] = input.configuration.empty() ? 0 : kStringConfiguration;
    c[7] = cfg.bm_attributes;
    c[8] = cfg.bmax_power;

    // The Device descriptor.
    std::vector<std::uint8_t>& d = composed.device;
    d.push_back(18);
    d.push_back(kTypeDevice);
    put_u16(&d, dev.bcd_usb);
    d.push_back(dev.device_class);
    d.push_back(dev.device_subclass);
    d.push_back(dev.device_protocol);
    d.push_back(dev.ep0_max_packet_size);
    put_u16(&d, dev.vendor_id);
    put_u16(&d, dev.product_id);
    put_u16(&d, dev.bcd_device);
    d.push_back(input.manufacturer.empty() ? 0 : kStringManufacturer);
    d.push_back(input.product.empty() ? 0 : kStringProduct);
    d.push_back(serial_index);
    d.push_back(1); // bNumConfigurations

    std::sort(composed.strings.begin(), composed.strings.end(),
              [](const ComposedString& a, const ComposedString& b) { return a.index < b.index; });
    *out = std::move(composed);
    return MIMUSB_OK;
}

} // namespace mimusb::usb
