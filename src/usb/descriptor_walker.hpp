// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#pragma once

/*
 * Checks a USB device given as bytes (mimhid_device_set_descriptors) and a BOS descriptor, and
 * finds the endpoints in it. Pure: no I/O, no board. Nothing is changed or filled in, and no class
 * is read: an interface is what its descriptor says, its class descriptors are bytes between
 * its descriptors. The board checks the same things again when it commits (docs/PROTOCOL.md), so
 * this is what lets a refusal name the offset in the caller's bytes.
 */

#include "usb/descriptor_composer.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mimhid::usb {

// One endpoint descriptor found in a configuration.
struct WalkedEndpoint {
    std::uint8_t address{};
    std::uint8_t transfer_type{}; // bmAttributes bits 1..0: 1 isochronous, 2 bulk, 3 interrupt
    std::uint8_t interval{};
    std::uint16_t max_packet_size{};
    std::uint8_t interface_number{};
    std::uint8_t alternate_setting{}; // the setting of the interface whose descriptor it follows
    std::uint32_t offset{};           // of the endpoint descriptor in the configuration
};

struct WalkedDescriptors {
    std::uint16_t bcd_usb{};
    std::vector<WalkedEndpoint> endpoints; // in the order of the configuration, every setting's
    std::uint8_t interface_count{};        // interfaces, not interface descriptors
};

// The header of a BOS descriptor and the length of each capability (USB 3.2 9.6.2): a 5-byte
// header of type 0x0F whose wTotalLength is `length` and whose bNumDeviceCaps is the number of
// capability descriptors that follow, each at least 3 bytes long, of type 0x10, and inside the
// bytes; at most lim.max_bos_bytes. No capability is read further. The error's `field` is `field`,
// and its `offset` the offset of what is wrong.
mimhid_result check_bos(const std::uint8_t* bos, std::size_t length, const ComposerLimits& lim,
                        const char* field, ComposerError* error);

// The whole device: the device descriptor (USB 2.0 9.6.1), the configuration (9.6.3) with its
// interfaces (9.6.5), endpoints (9.6.6) and association descriptors, and the BOS (bos null: none).
// MIMHID_OK and `out` filled, or the first thing that is wrong, with the byte offset in `error`.
// The fields of the error are "descriptors.device", "descriptors.configuration" and
// "descriptors.bos".
mimhid_result walk_descriptors(const std::uint8_t* device, std::size_t device_length,
                               const std::uint8_t* configuration, std::size_t configuration_length,
                               const std::uint8_t* bos, std::size_t bos_length,
                               const ComposerLimits& lim, WalkedDescriptors* out,
                               ComposerError* error);

} // namespace mimhid::usb
