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
    out->push_back(7);
    out->push_back(kTypeEndpoint);
    out->push_back(ep.address);
    out->push_back(ep.transfer_type);
    put_u16(out, ep.max_packet_size);
    out->push_back(ep.interval);
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
        return refuse(MIMUSB_ERR_PARAM, "node_config.interfaces.endpoints.address",
                      "An endpoint address is 0x81 to 0x8F (IN) or 0x01 to 0x0F (OUT): "
                      "bits 6..4 zero, endpoint number 1 to 15.",
                      ep.address, 0, node_id);
    if (number > lim.max_endpoint_number)
        return refuse(MIMUSB_ERR_OVERFLOW, "node_config.interfaces.endpoints.address",
                      "The endpoint number is above the highest the board's USB controller "
                      "has (mimusb_board_limits.max_endpoint_number).",
                      static_cast<std::uint32_t>(lim.max_endpoint_number), number, node_id);
    if (ep.reserved8 != 0U || ep.reserved16 != 0U)
        return refuse(MIMUSB_ERR_PARAM, "node_config.interfaces.endpoints.reserved",
                      "The reserved fields of an endpoint must be zero.", ep.address, 0, node_id);
    if (ep.transfer_type == 0U)
        return refuse(MIMUSB_ERR_UNSET_FIELD, "node_config.interfaces.endpoints.transfer_type",
                      "transfer_type is required: MIMUSB_TRANSFER_BULK or "
                      "MIMUSB_TRANSFER_INTERRUPT.",
                      ep.address, 0, node_id);
    if (ep.transfer_type == MIMUSB_TRANSFER_ISOCHRONOUS)
        return refuse(MIMUSB_ERR_UNSUPPORTED, "node_config.interfaces.endpoints.transfer_type",
                      "A Node has no isochronous endpoints: they come with alternate settings, "
                      "which only a Device given as bytes (mimusb_device_set_descriptors) has.",
                      ep.address, ep.transfer_type, node_id);
    if (ep.transfer_type > 3U)
        return refuse(MIMUSB_ERR_PARAM, "node_config.interfaces.endpoints.transfer_type",
                      "transfer_type is bmAttributes bits 1..0: 2 bulk or 3 interrupt.", ep.address,
                      ep.transfer_type, node_id);
    if ((lim.transfer_types & (1U << ep.transfer_type)) == 0U)
        return refuse(MIMUSB_ERR_UNSUPPORTED, "node_config.interfaces.endpoints.transfer_type",
                      "The board has no endpoints of this transfer type "
                      "(mimusb_board_limits.transfer_types).",
                      ep.address, ep.transfer_type, node_id);
    if (ep.max_packet_size == 0U)
        return refuse(MIMUSB_ERR_UNSET_FIELD, "node_config.interfaces.endpoints.max_packet_size",
                      "max_packet_size must be non-zero (1 to board max_endpoint_bytes).",
                      ep.address, 0, node_id);
    if (ep.max_packet_size > lim.max_endpoint_bytes)
        return refuse(MIMUSB_ERR_OVERFLOW, "node_config.interfaces.endpoints.max_packet_size",
                      "An endpoint packet is larger than the board's endpoints hold.",
                      static_cast<std::uint32_t>(lim.max_endpoint_bytes), ep.max_packet_size,
                      node_id);
    if (ep.transfer_type == MIMUSB_TRANSFER_BULK) {
        // USB 2.0 5.8.3: bulk packets are 8, 16, 32 or 64 bytes at full speed and 512 at
        // high speed.
        const bool allowed = lim.high_speed
                                 ? ep.max_packet_size == 512U
                                 : (ep.max_packet_size == 8U || ep.max_packet_size == 16U ||
                                    ep.max_packet_size == 32U || ep.max_packet_size == 64U);
        if (!allowed)
            return refuse(MIMUSB_ERR_PARAM, "node_config.interfaces.endpoints.max_packet_size",
                          lim.high_speed
                              ? "A bulk endpoint of a high-speed device has 512-byte "
                                "packets (USB 2.0 5.8.3)."
                              : "A bulk endpoint of a full-speed device has 8, 16, 32 or "
                                "64-byte packets (USB 2.0 5.8.3).",
                          ep.address, ep.max_packet_size, node_id);
    } else {
        if (ep.interval == 0U)
            return refuse(MIMUSB_ERR_UNSET_FIELD, "node_config.interfaces.endpoints.interval",
                          "An interrupt endpoint needs a bInterval (1 to 255; 1 to 16 at "
                          "high speed; USB 2.0 9.6.6).",
                          ep.address, 0, node_id);
        if (lim.high_speed && ep.interval > 16U)
            return refuse(MIMUSB_ERR_PARAM, "node_config.interfaces.endpoints.interval",
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

    // Endpoints: addresses valid and unique over the device, numbers the controller has, types the
    // board has, packet sizes USB allows, intervals, policies; class descriptors well formed.
    std::set<std::uint8_t> addresses;
    std::size_t pool_slots = 0;
    for (const auto& entry : by_number) {
        const ComposerInterface& itf = *entry.second.itf;
        const std::uint16_t node_id = entry.second.group->node_id;
        const std::vector<std::uint8_t>& cd = itf.class_descriptors;
        for (std::size_t at = 0; at < cd.size();) {
            // bLength at least 2 and inside the bytes: every descriptor has bLength and type.
            if (cd[at] < 2U || at + cd[at] > cd.size())
                return refuse(MIMUSB_ERR_PARAM, "node_config.interfaces.class_descriptors",
                              "A class descriptor's bLength must be at least 2 and stay inside "
                              "the bytes; the offset of the descriptor is in offset.",
                              static_cast<std::uint32_t>(at), static_cast<std::uint32_t>(cd.size()),
                              node_id);
            at += cd[at];
        }
        for (const mimusb_usb_endpoint& ep : itf.endpoints) {
            const bool in = (ep.address & 0x80U) != 0U;
            if (const mimusb_result shape = check_endpoint_shape(ep, lim, node_id, error);
                shape != MIMUSB_OK)
                return shape;
            if (!addresses.insert(ep.address).second)
                return refuse(MIMUSB_ERR_PARAM, "node_config.interfaces.endpoints.address",
                              "Two endpoints use the same address; it is in offset.", ep.address,
                              entry.first, node_id);
            if (in) {
                if (ep.policy.on_full == 0U || ep.policy.depth == 0U)
                    return refuse(MIMUSB_ERR_UNSET_FIELD, "node_config.interfaces.endpoints.policy",
                                  "An IN endpoint needs a policy: on_full and depth are required.",
                                  ep.address, 0, node_id);
                if (ep.policy.on_full > MIMUSB_FULL_WAIT)
                    return refuse(MIMUSB_ERR_PARAM, "node_config.interfaces.endpoints.policy",
                                  "on_full is MIMUSB_FULL_DROP_NEWEST, _DROP_OLDEST or _WAIT.",
                                  ep.address, ep.policy.on_full, node_id);
                pool_slots += queue_slots(ep.policy.depth, ep.policy.on_full);
            } else if (ep.policy.on_full != 0U || ep.policy.depth != 0U) {
                return refuse(MIMUSB_ERR_PARAM, "node_config.interfaces.endpoints.policy",
                              "The policy of an OUT endpoint must be all zero.", ep.address, 0,
                              node_id);
            }
        }
    }
    if (pool_slots > lim.endpoint_queue_slots)
        return refuse(MIMUSB_ERR_OVERFLOW, "node_config.interfaces.endpoints.policy",
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
    // its first interface, then the Interface descriptor, the class descriptors and the Endpoint
    // descriptors.
    std::vector<std::uint8_t>& c = composed.configuration;
    c.assign(9, 0);
    for (const auto& entry : by_number) {
        const ComposerInterface& itf = *entry.second.itf;
        const ComposerGroup& group = *entry.second.group;
        if (!group.association.empty() && group.association[2] == itf.number)
            c.insert(c.end(), group.association.begin(), group.association.end());
        const auto name = name_index.find(entry.first);
        c.push_back(9);
        c.push_back(kTypeInterface);
        c.push_back(itf.number);
        c.push_back(0); // bAlternateSetting
        c.push_back(static_cast<std::uint8_t>(itf.endpoints.size()));
        c.push_back(itf.class_code);
        c.push_back(itf.subclass);
        c.push_back(itf.protocol);
        c.push_back(name == name_index.end() ? 0U : name->second);
        c.insert(c.end(), itf.class_descriptors.begin(), itf.class_descriptors.end());
        for (const mimusb_usb_endpoint& ep : itf.endpoints)
            endpoint_descriptor(&c, ep);
        ComposedInterface made;
        made.number = itf.number;
        made.endpoints = itf.endpoints;
        composed.interfaces.push_back(std::move(made));
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
    c[4] = static_cast<std::uint8_t>(composed.interfaces.size());
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
