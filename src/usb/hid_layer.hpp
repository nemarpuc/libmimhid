// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#pragma once

/*
 * What only HID needs when the interfaces of the profile Nodes become interfaces for the composer
 * (descriptor_composer.hpp): Nodes sharing an interface join their report descriptors, and so every
 * Report ID must be unique on it; the interface is described from its first Node, so the others
 * must agree; the HID descriptor (HID 1.11 6.2.1) is the interface's class descriptor and the report
 * descriptor goes along on the side. Pure: no I/O.
 */

#include "usb/descriptor_composer.hpp"

#include <bitset>

namespace mimhid::usb {

struct HidNodeInput {
    mimhid_hid_node_config config{}; // config.name is not read; name is
    std::u16string name;
    const std::vector<std::uint8_t>* report_descriptor{};
    bool has_report_ids{};
    std::bitset<256> report_ids;
    std::uint16_t node_id{}; // the public id, for the details of an error
};

// One group (one interface, no association) for each interface number the Nodes use, in the order
// of the first Node on it. MIMHID_OK, or the first rule that is broken, in `error`.
mimhid_result compose_hid_groups(const std::vector<HidNodeInput>& nodes,
                                 std::vector<ComposerGroup>* out, ComposerError* error);

} // namespace mimhid::usb
