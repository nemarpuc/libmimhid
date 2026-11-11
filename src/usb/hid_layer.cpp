// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* HID 1.11 6.2.1: the HID descriptor; 7.1: it follows the Interface descriptor. */

#include "usb/hid_layer.hpp"

#include <map>

namespace mimhid::usb {

namespace {

constexpr std::uint8_t kClassHid = 0x03;
constexpr std::uint8_t kTypeHid = 0x21;
constexpr std::uint8_t kTypeHidReport = 0x22;

// The depth of the policy is not compared: Nodes on one interface share its queue, and the board
// gets the least depth any of them set (mimhid_node_endpoint_set_policy).
bool same_endpoint(const mimhid_usb_endpoint& a, const mimhid_usb_endpoint& b) {
    return a.address == b.address && a.transfer_type == b.transfer_type &&
           a.interval == b.interval && a.max_packet_size == b.max_packet_size &&
           a.policy.on_full == b.policy.on_full;
}

bool same_settings(const HidNodeInput& a, const HidNodeInput& b) {
    const mimhid_hid_node_config& x = a.config;
    const mimhid_hid_node_config& y = b.config;
    return x.subclass == y.subclass && x.protocol == y.protocol && x.bcd_hid == y.bcd_hid &&
           x.country_code == y.country_code && a.name == b.name &&
           same_endpoint(x.ep_in, y.ep_in) && same_endpoint(x.ep_out, y.ep_out);
}

} // namespace

mimhid_result compose_hid_groups(const std::vector<HidNodeInput>& nodes,
                                 std::vector<ComposerGroup>* out, ComposerError* error) {
    const auto refuse = [&](const char* field, const char* reason, std::uint32_t offset,
                            std::uint16_t node_id) {
        if (error != nullptr)
            *error = ComposerError{MIMHID_ERR_PARAM, field, reason, offset, 0, node_id};
        return MIMHID_ERR_PARAM;
    };

    std::map<std::uint32_t, std::vector<const HidNodeInput*>> by_number;
    std::vector<std::uint32_t> order;
    for (const HidNodeInput& node : nodes) {
        auto& group = by_number[node.config.interface_number];
        if (group.empty())
            order.push_back(node.config.interface_number);
        group.push_back(&node);
    }

    out->clear();
    for (const std::uint32_t number : order) {
        const auto& group = by_number[number];
        // HID 1.11 6.2.1 / 5.6: Nodes sharing an interface join their report descriptors, so every
        // Report ID must be unique on it and every Node must have some.
        if (group.size() >= 2U) {
            std::bitset<256> combined;
            for (const HidNodeInput* node : group) {
                if (!node->has_report_ids || node->report_ids.none())
                    return refuse("node.report_id",
                                  "Multiple nodes sharing an interface must all declare Report "
                                  "IDs.",
                                  number, node->node_id);
                if ((combined & node->report_ids).any())
                    return refuse("node.report_id",
                                  "Duplicate Report ID assigned on the same interface.", number,
                                  node->node_id);
                combined |= node->report_ids;
            }
            // The interface is described from its first Node, so the others must agree with it.
            for (const HidNodeInput* node : group) {
                if (!same_settings(*group.front(), *node))
                    return refuse("node_config.iface",
                                  "Nodes sharing an interface must have the same interface and "
                                  "endpoint settings.",
                                  number, node->node_id);
            }
        }
        const HidNodeInput& first = *group.front();
        ComposerGroup made;
        made.node_id = first.node_id;
        ComposerInterface itf;
        itf.number = static_cast<std::uint8_t>(number);
        itf.class_code = kClassHid;
        itf.subclass = first.config.subclass;
        itf.protocol = first.config.protocol;
        itf.name = first.name;
        itf.hid = true;
        for (const HidNodeInput* node : group)
            itf.report_descriptor.insert(itf.report_descriptor.end(),
                                         node->report_descriptor->begin(),
                                         node->report_descriptor->end());
        const std::size_t report_length = itf.report_descriptor.size();
        // The HID descriptor: bLength 9, type, bcdHID, bCountryCode, one descriptor (the report
        // descriptor) and its length.
        itf.class_descriptors = {9,
                                 kTypeHid,
                                 static_cast<std::uint8_t>(first.config.bcd_hid & 0xFFU),
                                 static_cast<std::uint8_t>(first.config.bcd_hid >> 8U),
                                 first.config.country_code,
                                 1,
                                 kTypeHidReport,
                                 static_cast<std::uint8_t>(report_length & 0xFFU),
                                 static_cast<std::uint8_t>((report_length >> 8U) & 0xFFU)};
        itf.endpoints.push_back(first.config.ep_in);
        if (first.config.ep_out.address != 0U || first.config.ep_out.max_packet_size != 0U)
            itf.endpoints.push_back(first.config.ep_out);
        made.interfaces.push_back(std::move(itf));
        out->push_back(std::move(made));
    }
    return MIMHID_OK;
}

} // namespace mimhid::usb
