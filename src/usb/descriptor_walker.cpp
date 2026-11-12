// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/*
 * Descriptor layouts: USB 2.0 9.6.1 (Device), 9.6.3 (Configuration), 9.6.5 (Interface), 9.6.6
 * (Endpoint); the Interface Association Descriptor ECN; USB 3.2 9.6.2 (Binary Object Store).
 */

#include "usb/descriptor_walker.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace mimhid::usb {

namespace {

constexpr std::uint8_t kTypeDevice = 1;
constexpr std::uint8_t kTypeConfiguration = 2;
constexpr std::uint8_t kTypeInterface = 4;
constexpr std::uint8_t kTypeEndpoint = 5;
constexpr std::uint8_t kTypeAssociation = 0x0B;
constexpr std::uint8_t kTypeBos = 0x0F;
constexpr std::uint8_t kTypeDeviceCapability = 0x10;
// The BOS and the LPM ECN: a host asks for a BOS descriptor only from bcdUSB 0x0201 on.
constexpr std::uint16_t kBcdUsbWithBos = 0x0201;

mimhid_result refuse(ComposerError* error, mimhid_result result, const char* field,
                     const char* reason, std::uint32_t offset, std::uint32_t length) {
    if (error != nullptr)
        *error = ComposerError{result, field, reason, offset, length, 0};
    return result;
}

std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8U));
}

} // namespace

mimhid_result check_bos(const std::uint8_t* bos, const std::size_t length,
                        const ComposerLimits& lim, const char* field, ComposerError* error) {
    const auto size = static_cast<std::uint32_t>(length);
    if (bos == nullptr || length < 5U)
        return refuse(error, MIMHID_ERR_PARAM, field,
                      "A BOS descriptor has a 5-byte header (USB 3.2 9.6.2).", 0, size);
    if (length > lim.max_bos_bytes)
        return refuse(error, MIMHID_ERR_OVERFLOW, field,
                      "The BOS descriptor is longer than the board holds "
                      "(mimhid_board_limits.max_bos_bytes).",
                      static_cast<std::uint32_t>(lim.max_bos_bytes), size);
    if (bos[0] != 5U)
        return refuse(error, MIMHID_ERR_PARAM, field,
                      "bLength of a BOS descriptor is 5 (USB 3.2 9.6.2).", 0, bos[0]);
    if (bos[1] != kTypeBos)
        return refuse(error, MIMHID_ERR_PARAM, field,
                      "bDescriptorType of a BOS descriptor is 0x0F (USB 3.2 9.6.2).", 1, bos[1]);
    if (le16(&bos[2]) != length)
        return refuse(error, MIMHID_ERR_PARAM, field,
                      "wTotalLength of a BOS descriptor is the length of all of it.", 2,
                      le16(&bos[2]));
    std::size_t at = 5U;
    std::uint32_t found = 0U;
    while (at < length) {
        const std::uint8_t l = bos[at];
        if (l < 3U || at + l > length)
            return refuse(error, MIMHID_ERR_PARAM, field,
                          "A device capability descriptor is at least 3 bytes (bLength, "
                          "bDescriptorType, bDevCapabilityType) and stays inside the BOS "
                          "descriptor (USB 3.2 9.6.2).",
                          static_cast<std::uint32_t>(at), l);
        if (bos[at + 1] != kTypeDeviceCapability)
            return refuse(error, MIMHID_ERR_PARAM, field,
                          "bDescriptorType of a device capability descriptor is 0x10 (USB 3.2 "
                          "9.6.2).",
                          static_cast<std::uint32_t>(at + 1U), bos[at + 1]);
        at += l;
        ++found;
    }
    if (found != bos[4])
        return refuse(error, MIMHID_ERR_PARAM, field,
                      "bNumDeviceCaps is not the number of capability descriptors the BOS "
                      "descriptor holds; the number found is in length.",
                      4, found);
    return MIMHID_OK;
}

mimhid_result walk_descriptors(const std::uint8_t* device, const std::size_t device_length,
                               const std::uint8_t* configuration,
                               const std::size_t configuration_length, const std::uint8_t* bos,
                               const std::size_t bos_length, const ComposerLimits& lim,
                               WalkedDescriptors* out, ComposerError* error) {
    *out = WalkedDescriptors{};
    constexpr const char* kDevice = "descriptors.device";
    constexpr const char* kConfig = "descriptors.configuration";

    // The device descriptor: 18 bytes, its own length and type, one configuration, a bcdUSB, a
    // control endpoint size the controller works with (USB 2.0 9.6.1).
    if (device == nullptr || device_length != 18U)
        return refuse(error, MIMHID_ERR_PARAM, kDevice,
                      "The device descriptor is 18 bytes (USB 2.0 9.6.1).", 0,
                      static_cast<std::uint32_t>(device_length));
    if (device[0] != 18U)
        return refuse(error, MIMHID_ERR_PARAM, kDevice, "bLength of the device descriptor is 18.",
                      0, device[0]);
    if (device[1] != kTypeDevice)
        return refuse(error, MIMHID_ERR_PARAM, kDevice,
                      "bDescriptorType of the device descriptor is 1.", 1, device[1]);
    out->bcd_usb = le16(&device[2]);
    if (out->bcd_usb == 0U)
        return refuse(error, MIMHID_ERR_UNSET_FIELD, kDevice,
                      "bcdUSB is required: nothing is filled in.", 2, 0);
    const std::uint8_t ep0 = device[7];
    if (lim.high_speed ? ep0 != 64U : (ep0 != 8U && ep0 != 16U && ep0 != 32U && ep0 != 64U))
        return refuse(error, MIMHID_ERR_PARAM, kDevice,
                      lim.high_speed
                          ? "bMaxPacketSize0 is 64 on a high-speed board (USB 2.0 9.6.1)."
                          : "bMaxPacketSize0 is 8, 16, 32 or 64 (USB 2.0 9.6.1).",
                      7, ep0);
    if (device[17] != 1U)
        return refuse(error, MIMHID_ERR_PARAM, kDevice,
                      "bNumConfigurations is 1: the board has one configuration.", 17, device[17]);

    // The configuration descriptor: its own header (USB 2.0 9.6.3), the value the stack selects,
    // the attribute bits (D7 set, D4..0 reserved and zero).
    if (configuration == nullptr || configuration_length < 9U)
        return refuse(error, MIMHID_ERR_PARAM, kConfig,
                      "The configuration descriptor has a 9-byte header (USB 2.0 9.6.3).", 0,
                      static_cast<std::uint32_t>(configuration_length));
    if (configuration_length > lim.max_config_bytes)
        return refuse(error, MIMHID_ERR_OVERFLOW, kConfig,
                      "The configuration descriptor is longer than the board holds "
                      "(mimhid_board_limits.max_config_descriptor_bytes).",
                      static_cast<std::uint32_t>(lim.max_config_bytes),
                      static_cast<std::uint32_t>(configuration_length));
    const std::uint8_t* c = configuration;
    const auto total = static_cast<std::uint32_t>(configuration_length);
    if (c[0] != 9U || c[1] != kTypeConfiguration)
        return refuse(error, MIMHID_ERR_PARAM, kConfig,
                      "The configuration descriptor starts with bLength 9 and type 2.",
                      c[0] != 9U ? 0U : 1U, c[0] != 9U ? c[0] : c[1]);
    if (le16(&c[2]) != configuration_length)
        return refuse(error, MIMHID_ERR_PARAM, kConfig,
                      "wTotalLength is the length of all of the configuration descriptor.", 2,
                      le16(&c[2]));
    if (c[5] != 1U)
        return refuse(error, MIMHID_ERR_PARAM, kConfig,
                      "bConfigurationValue is 1: the board selects its one configuration with it.",
                      5, c[5]);
    if ((c[7] & 0x80U) == 0U || (c[7] & 0x1FU) != 0U)
        return refuse(error, MIMHID_ERR_PARAM, kConfig,
                      "bmAttributes has bit 7 set and bits 4..0 zero (USB 2.0 9.6.3).", 7, c[7]);

    // The interfaces, their settings, their endpoints and the descriptors between them.
    struct Owner {
        std::uint8_t interface_number;
        std::uint8_t attributes;
        std::uint16_t max_packet_size;
        std::uint8_t interval;
    };
    std::map<std::uint8_t, Owner> owners; // address -> the interface that has it, in any setting
    std::set<std::uint8_t> setting_addresses;         // the addresses of the setting being walked
    std::vector<std::uint8_t> settings;               // settings of each interface so far
    std::uint8_t interface_number = 0, alternate = 0; // of the interface descriptor walked
    std::uint32_t pos = 9U;
    bool inside = false;       // after an interface descriptor
    std::uint8_t declared = 0; // bNumEndpoints of the interface
    std::uint8_t seen = 0;
    std::uint32_t itf_at = 0;
    std::uint8_t group_first = 0, group_left = 0; // an association and the interfaces it names
    bool group_pending = false;
    const auto finish_interface = [&]() -> mimhid_result {
        if (seen != declared)
            return refuse(error, MIMHID_ERR_PARAM, kConfig,
                          "bNumEndpoints of an interface is not the number of endpoint "
                          "descriptors that follow it; the number found is in length.",
                          itf_at + 4U, seen);
        return MIMHID_OK;
    };
    while (pos < total) {
        const std::uint8_t l = c[pos];
        if (l < 2U || pos + l > total)
            return refuse(error, MIMHID_ERR_PARAM, kConfig,
                          "Every descriptor's bLength is at least 2 and stays inside the "
                          "configuration descriptor.",
                          pos, l);
        const std::uint8_t type = c[pos + 1U];
        if ((type == kTypeInterface || type == kTypeAssociation) && inside) {
            if (const mimhid_result r = finish_interface(); r != MIMHID_OK)
                return r;
            inside = false;
        }
        switch (type) {
        case kTypeAssociation:
            // 8 bytes; names the interfaces that follow it (Interface Association Descriptor ECN).
            if (l != 8U || group_pending || group_left != 0U || c[pos + 3U] == 0U)
                return refuse(error, MIMHID_ERR_PARAM, kConfig,
                              "An interface association descriptor is 8 bytes, names at least "
                              "one interface, and does not come inside another.",
                              pos, l);
            group_first = c[pos + 2U];
            group_left = c[pos + 3U];
            group_pending = true;
            if (group_first != out->interface_count)
                return refuse(error, MIMHID_ERR_PARAM, kConfig,
                              "bFirstInterface of an association is the number of the interface "
                              "that follows it; that number is in length.",
                              pos + 2U, out->interface_count);
            break;
        case kTypeInterface: {
            // 9.6.5: 9 bytes, the number is the index of the interface (0, 1, 2, ...), and its
            // alternate settings follow its setting 0 as 1, 2, ... in order. Nothing comes between
            // the settings of an interface (an association, in particular, does not).
            if (l != 9U)
                return refuse(error, MIMHID_ERR_PARAM, kConfig,
                              "An interface descriptor is 9 bytes (USB 2.0 9.6.5).", pos, l);
            const std::uint8_t number = c[pos + 2U];
            const std::uint8_t alt = c[pos + 3U];
            if (alt == 0U) {
                if (number != out->interface_count)
                    return refuse(error, MIMHID_ERR_PARAM, kConfig,
                                  "Interface numbers are 0, 1, 2, ... in order; the number "
                                  "expected is in length.",
                                  pos + 2U, out->interface_count);
                if (out->interface_count >= lim.max_interfaces)
                    return refuse(error, MIMHID_ERR_OVERFLOW, kConfig,
                                  "The interfaces exceed the board's maximum interfaces "
                                  "(mimhid_board_limits.max_interfaces_any).",
                                  static_cast<std::uint32_t>(lim.max_interfaces),
                                  out->interface_count + 1U);
                if (group_left != 0U)
                    --group_left;
                group_pending = false;
                ++out->interface_count;
                settings.push_back(1U);
            } else {
                if (out->interface_count == 0U || number != out->interface_count - 1U ||
                    group_pending)
                    return refuse(error, MIMHID_ERR_PARAM, kConfig,
                                  "An alternate setting follows the setting before it of the "
                                  "same interface, with nothing between them; the number of the "
                                  "interface expected is in length.",
                                  pos + 2U,
                                  out->interface_count == 0U ? 0U : out->interface_count - 1U);
                if (alt != settings[number] || settings[number] == 255U)
                    return refuse(error, MIMHID_ERR_PARAM, kConfig,
                                  "The alternate settings of an interface are 0, 1, 2, ... in "
                                  "order; the setting expected is in length.",
                                  pos + 3U, settings[number]);
                ++settings[number];
            }
            inside = true;
            declared = c[pos + 4U];
            seen = 0;
            itf_at = pos;
            interface_number = number;
            alternate = alt;
            setting_addresses.clear();
            break;
        }
        case kTypeEndpoint: {
            // 7 bytes, or 9 with the bRefresh and bSynchAddress of an audio-class endpoint; the
            // board reads no class, and the two bytes are served as given.
            if (!inside || (l != 7U && l != 9U) || seen >= declared)
                return refuse(error, MIMHID_ERR_PARAM, kConfig,
                              "An endpoint descriptor is 7 bytes (9 for the audio-class form) "
                              "and follows the interface that announced it (bNumEndpoints).",
                              pos, l);
            ++seen;
            const std::uint8_t attributes = c[pos + 3U];
            const std::uint8_t xfer = attributes & 0x03U;
            mimhid_usb_endpoint ep{};
            ep.address = c[pos + 2U];
            ep.interval = c[pos + 6U];
            ep.max_packet_size = le16(&c[pos + 4U]);
            if (xfer == MIMHID_TRANSFER_ISOCHRONOUS) {
                // 5.6.3, 9.6.6: bits 5..2 of bmAttributes are the synchronization and usage type
                // and bits 7..6 are reserved; a packet of 1 to the largest the board takes (one
                // transaction per frame: the bits that name more are above that); bInterval 1 at
                // full speed, an exponent 1 to 16 at high speed.
                if ((lim.transfer_types & (1U << MIMHID_TRANSFER_ISOCHRONOUS)) == 0U)
                    return refuse(error, MIMHID_ERR_UNSUPPORTED, kConfig,
                                  "The board has no isochronous endpoints "
                                  "(mimhid_board_limits.transfer_types).",
                                  pos + 3U, attributes);
                if ((attributes & 0xC0U) != 0U)
                    return refuse(error, MIMHID_ERR_PARAM, kConfig,
                                  "Bits 7..6 of bmAttributes are reserved and zero (USB 2.0 "
                                  "9.6.6).",
                                  pos + 3U, attributes);
                const std::size_t largest =
                    std::min<std::size_t>(lim.max_transfer_bytes, lim.high_speed ? 1024U : 1023U);
                if ((ep.address & 0x70U) != 0U || (ep.address & 0x0FU) == 0U ||
                    (ep.address & 0x0FU) > lim.max_endpoint_number)
                    return refuse(error, MIMHID_ERR_PARAM, kConfig,
                                  "An endpoint address has bits 6..4 zero and a number from 1 to "
                                  "the highest the board's controller has.",
                                  pos + 2U, ep.address);
                if (ep.max_packet_size == 0U || ep.max_packet_size > largest)
                    return refuse(error, MIMHID_ERR_OVERFLOW, kConfig,
                                  "An isochronous packet is 1 to the board's largest (1023 "
                                  "bytes at full speed, 1024 at high speed, and no more than "
                                  "mimhid_board_limits.max_transfer_bytes).",
                                  pos + 4U, ep.max_packet_size);
                if (lim.high_speed ? (ep.interval == 0U || ep.interval > 16U) : ep.interval != 1U)
                    return refuse(error, MIMHID_ERR_PARAM, kConfig,
                                  lim.high_speed
                                      ? "bInterval of an isochronous endpoint is 1 to 16 at "
                                        "high speed (USB 2.0 9.6.6)."
                                      : "bInterval of an isochronous endpoint is 1 at full "
                                        "speed (USB 2.0 9.6.6).",
                                  pos + 6U, ep.interval);
            } else {
                if (attributes != MIMHID_TRANSFER_BULK && attributes != MIMHID_TRANSFER_INTERRUPT)
                    return refuse(error, MIMHID_ERR_UNSUPPORTED, kConfig,
                                  "bmAttributes of an endpoint is 1 (isochronous, with the "
                                  "synchronization bits), 2 (bulk) or 3 (interrupt).",
                                  pos + 3U, attributes);
                ep.transfer_type = attributes;
                ComposerError shape;
                if (check_endpoint_shape(ep, lim, 0, &shape) != MIMHID_OK) {
                    // The rule that was broken, at the place in the caller's bytes.
                    return refuse(error, shape.result, kConfig, shape.reason, pos + 2U,
                                  shape.offset);
                }
            }
            if (!setting_addresses.insert(ep.address).second)
                return refuse(error, MIMHID_ERR_PARAM, kConfig,
                              "Two endpoints of one setting use the same address; the address "
                              "is in length.",
                              pos + 2U, ep.address);
            const auto owner = owners.find(ep.address);
            if (owner == owners.end()) {
                owners.emplace(ep.address, Owner{interface_number, attributes, ep.max_packet_size,
                                                 ep.interval});
            } else {
                // Seen: in another setting of the same interface, which is how the settings share
                // an endpoint, or in another interface, which is not allowed. The board opens a
                // bulk or interrupt endpoint once for the whole configuration, so it is the same
                // in every setting; an isochronous endpoint is started with the descriptor of the
                // setting selected, so its packet size may differ.
                const bool iso = xfer == MIMHID_TRANSFER_ISOCHRONOUS;
                const bool same_type = (owner->second.attributes & 0x03U) == xfer;
                if (owner->second.interface_number != interface_number || !same_type ||
                    (!iso && (owner->second.attributes != attributes ||
                              owner->second.max_packet_size != ep.max_packet_size ||
                              owner->second.interval != ep.interval)))
                    return refuse(error, MIMHID_ERR_PARAM, kConfig,
                                  "An address is used again only by the same interface in another "
                                  "setting, with the same type, and, for a bulk or interrupt "
                                  "endpoint, the same descriptor; the address is in length.",
                                  pos + 2U, ep.address);
            }
            out->endpoints.push_back(WalkedEndpoint{ep.address, xfer, ep.interval,
                                                    ep.max_packet_size, interface_number, alternate,
                                                    pos});
            break;
        }
        default:
            // A class descriptor of the interface it follows, as given.
            if (!inside)
                return refuse(error, MIMHID_ERR_PARAM, kConfig,
                              "A descriptor that is not an interface, endpoint or association "
                              "descriptor belongs to an interface and follows its descriptor.",
                              pos, type);
            break;
        }
        pos += l;
    }
    if (inside) {
        if (const mimhid_result r = finish_interface(); r != MIMHID_OK)
            return r;
    }
    if (group_pending || group_left != 0U)
        return refuse(error, MIMHID_ERR_PARAM, kConfig,
                      "An interface association names interfaces that are not all there.", total,
                      group_left);
    if (out->interface_count == 0U)
        return refuse(error, MIMHID_ERR_PARAM, kConfig, "The configuration has no interface.", 9,
                      0);
    if (c[4] != out->interface_count)
        return refuse(error, MIMHID_ERR_PARAM, kConfig,
                      "bNumInterfaces is not the number of interfaces the configuration holds; "
                      "the number found is in length.",
                      4, out->interface_count);

    if (bos != nullptr) {
        if (const mimhid_result r = check_bos(bos, bos_length, lim, "descriptors.bos", error);
            r != MIMHID_OK)
            return r;
        if (out->bcd_usb < kBcdUsbWithBos)
            return refuse(error, MIMHID_ERR_PARAM, kDevice,
                          "A BOS descriptor needs bcdUSB 0x0201 or more: a host asks for one only "
                          "then (USB 2.0 Link Power Management ECN, USB 3.2 9.6.2).",
                          2, out->bcd_usb);
    }
    return MIMHID_OK;
}

} // namespace mimhid::usb
