// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
#pragma once

/*
 * What only HID needs when the interfaces of the HID Nodes become interfaces for the composer
 * (descriptor_composer.hpp): Nodes sharing an interface join their report descriptors, and so every
 * Report ID must be unique on it; the interface is described from its first Node, so the others
 * must agree; the HID descriptor (HID 1.11 6.2.1) is the interface's class descriptor and the
 * report descriptor goes along on the side. Pure: no I/O.
 */

#include "classes/hid/hid_node_config.hpp"
#include "usb/board_rule.hpp"
#include "usb/descriptor_composer.hpp"

#include <bitset>

namespace mimusb::usb {

// What the HID class calls the rules it writes (BoardRule::tag).
enum class HidTag : std::uint8_t {
    other = 1,  // a descriptor or an Input report the board answers by itself
    feature,    // GET_REPORT of a Feature report: the answer a Node set
    set_report, // SET_REPORT, SET_IDLE, SET_PROTOCOL: taken, and reported to the program
    set_idle,
    set_protocol,
    get_idle, // live answers: the idle rate and the protocol the target set
    get_protocol,
};

constexpr std::uint8_t tag(const HidTag value) noexcept { return static_cast<std::uint8_t>(value); }

struct HidNodeInput {
    mimusb::usb::HidNodeConfig config{}; // config.name is not read; name is
    std::u16string name;
    const std::vector<std::uint8_t>* report_descriptor{};
    bool has_report_ids{};
    std::bitset<256> report_ids;
    std::uint16_t node_id{}; // the public id, for the details of an error
    // What the target reads from this Node with a Feature report (GET_REPORT of type Feature): the
    // Report ID and the bytes after it.
    struct Feature {
        std::uint8_t report_id{};
        std::vector<std::uint8_t> data;
    };
    std::vector<Feature> features;
};

// One group (one interface, no association) for each interface number the Nodes use, in the order
// of the first Node on it. MIMUSB_OK, or the first rule that is broken, in `error`.
mimusb_result compose_hid_groups(const std::vector<HidNodeInput>& nodes,
                                 std::vector<ComposerGroup>* out, ComposerError* error);

// The control rules that make the interfaces of the profile Nodes behave as HID 1.11 asks, for each
// interface in the order compose_hid_groups makes them: the report and HID descriptors (7.1.1),
// GET_REPORT of an Input report from the last report sent (7.2.1) and of a Feature report from the
// answers the Nodes set, SET_REPORT (7.2.2), GET/SET_IDLE (7.2.3, 7.2.4) and GET/SET_PROTOCOL
// (7.2.5, 7.2.6). The GET_IDLE and GET_PROTOCOL answers are live rules: the idle rate starts at 0
// and the protocol at 1, the report protocol (7.2.4, 7.2.6). The rules' owner is left null; the
// HID class sets it.
std::vector<BoardRule> compose_hid_rules(const std::vector<HidNodeInput>& nodes);

} // namespace mimusb::usb
