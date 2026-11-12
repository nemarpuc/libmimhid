// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* HID 1.11 6.2.1: the HID descriptor; 7.1: it follows the Interface descriptor. */

#include "usb/hid_layer.hpp"

#include <map>

namespace mimusb::usb {

namespace {

constexpr std::uint8_t kClassHid = 0x03;
constexpr std::uint8_t kTypeHid = 0x21;
constexpr std::uint8_t kTypeHidReport = 0x22;

// The depth of the policy is not compared: Nodes on one interface share its queue, and the board
// gets the least depth any of them set (mimusb_node_endpoint_set_policy).
bool same_endpoint(const mimusb_usb_endpoint& a, const mimusb_usb_endpoint& b) {
    return a.address == b.address && a.transfer_type == b.transfer_type &&
           a.interval == b.interval && a.max_packet_size == b.max_packet_size &&
           a.policy.on_full == b.policy.on_full;
}

bool same_settings(const HidNodeInput& a, const HidNodeInput& b) {
    const mimusb::usb::HidNodeConfig& x = a.config;
    const mimusb::usb::HidNodeConfig& y = b.config;
    return x.subclass == y.subclass && x.protocol == y.protocol && x.bcd_hid == y.bcd_hid &&
           x.country_code == y.country_code && a.name == b.name &&
           same_endpoint(x.ep_in, y.ep_in) && same_endpoint(x.ep_out, y.ep_out);
}

// bmRequestType (USB 2.0 9.3.1): direction, type and recipient, all three compared.
constexpr std::uint8_t kStandardInterfaceIn = 0x81;
constexpr std::uint8_t kClassInterfaceIn = 0xA1;
constexpr std::uint8_t kClassInterfaceOut = 0x21;
// bRequest: GET_DESCRIPTOR (USB 2.0 9.4.3); the HID class requests (HID 1.11 7.2).
constexpr std::uint8_t kGetDescriptor = 0x06;
constexpr std::uint8_t kGetReport = 0x01;
constexpr std::uint8_t kGetIdle = 0x02;
constexpr std::uint8_t kGetProtocol = 0x03;
constexpr std::uint8_t kSetReport = 0x09;
constexpr std::uint8_t kSetIdle = 0x0A;
constexpr std::uint8_t kSetProtocol = 0x0B;
// Report types in the high byte of wValue of GET_REPORT (HID 1.11 7.2.1).
constexpr std::uint16_t kInputReport = 0x0100;
constexpr std::uint16_t kFeatureReport = 0x0300;

// The nodes grouped by interface number, in the order of the first Node on each.
struct Grouped {
    std::map<std::uint32_t, std::vector<const HidNodeInput*>> by_number;
    std::vector<std::uint32_t> order;
};

Grouped group_by_interface(const std::vector<HidNodeInput>& nodes) {
    Grouped grouped;
    for (const HidNodeInput& node : nodes) {
        auto& group = grouped.by_number[node.config.interface_number];
        if (group.empty())
            grouped.order.push_back(node.config.interface_number);
        group.push_back(&node);
    }
    return grouped;
}

// Nodes sharing an interface join their report descriptors, in the order of the Nodes.
std::vector<std::uint8_t> joined_descriptor(const std::vector<const HidNodeInput*>& group) {
    std::vector<std::uint8_t> joined;
    for (const HidNodeInput* node : group)
        joined.insert(joined.end(), node->report_descriptor->begin(),
                      node->report_descriptor->end());
    return joined;
}

// The HID descriptor (HID 1.11 6.2.1): bLength 9, type, bcdHID, bCountryCode, one descriptor (the
// report descriptor) and its length.
std::vector<std::uint8_t> hid_descriptor(const mimusb::usb::HidNodeConfig& config,
                                         const std::size_t report_length) {
    return {9,
            kTypeHid,
            static_cast<std::uint8_t>(config.bcd_hid & 0xFFU),
            static_cast<std::uint8_t>(config.bcd_hid >> 8U),
            config.country_code,
            1,
            kTypeHidReport,
            static_cast<std::uint8_t>(report_length & 0xFFU),
            static_cast<std::uint8_t>((report_length >> 8U) & 0xFFU)};
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
BoardRule make_rule(const BoardRule::Role role, const std::uint8_t iface,
                    const std::uint8_t request_type, const std::uint8_t request,
                    const std::uint16_t value_mask, const std::uint16_t value,
                    const mimusb_control_rule_action action) {
    BoardRule r;
    r.role = role;
    r.iface = iface;
    r.rule.request_type_mask = 0xFFU;
    r.rule.request_type_value = request_type;
    r.rule.request_mask = 0xFFU;
    r.rule.request_value = request;
    r.rule.value_mask = value_mask;
    r.rule.value_value = value;
    r.rule.index_mask = 0xFFFFU;
    r.rule.index_value = iface;
    r.rule.action = action;
    return r;
}

} // namespace

mimusb_result compose_hid_groups(const std::vector<HidNodeInput>& nodes,
                                 std::vector<ComposerGroup>* out, ComposerError* error) {
    const auto refuse = [&](const char* field, const char* reason, std::uint32_t offset,
                            std::uint16_t node_id) {
        if (error != nullptr)
            *error = ComposerError{MIMUSB_ERR_PARAM, field, reason, offset, 0, node_id};
        return MIMUSB_ERR_PARAM;
    };

    const Grouped grouped = group_by_interface(nodes);
    const auto& by_number = grouped.by_number;
    const auto& order = grouped.order;

    out->clear();
    for (const std::uint32_t number : order) {
        const auto& group = by_number.at(number);
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
        // One setting: the HID descriptor, then the endpoints.
        ComposerSetting setting;
        ComposerItem hid;
        hid.descriptor = hid_descriptor(first.config, joined_descriptor(group).size());
        setting.items.push_back(std::move(hid));
        ComposerItem in;
        in.is_endpoint = true;
        in.endpoint = first.config.ep_in;
        setting.items.push_back(std::move(in));
        if (first.config.ep_out.address != 0U || first.config.ep_out.max_packet_size != 0U) {
            ComposerItem out_item;
            out_item.is_endpoint = true;
            out_item.endpoint = first.config.ep_out;
            setting.items.push_back(std::move(out_item));
        }
        itf.settings.push_back(std::move(setting));
        made.interfaces.push_back(std::move(itf));
        out->push_back(std::move(made));
    }
    return MIMUSB_OK;
}

std::vector<BoardRule> compose_hid_rules(const std::vector<HidNodeInput>& nodes) {
    const Grouped grouped = group_by_interface(nodes);
    std::vector<BoardRule> out;
    for (const std::uint32_t number : grouped.order) {
        const auto& group = grouped.by_number.at(number);
        const HidNodeInput& first = *group.front();
        const std::uint8_t iface = static_cast<std::uint8_t>(number);
        const std::vector<std::uint8_t> report_descriptor = joined_descriptor(group);
        using Role = BoardRule::Role;

        // GET_DESCRIPTOR of the report descriptor and of the HID descriptor (HID 1.11 7.1.1): the
        // type in the high byte of wValue, any index.
        BoardRule r =
            make_rule(Role::hid_other, iface, kStandardInterfaceIn, kGetDescriptor, 0xFF00U,
                      static_cast<std::uint16_t>(kTypeHidReport << 8U), MIMUSB_CONTROL_ANSWER);
        r.data = report_descriptor;
        out.push_back(r);
        r = make_rule(Role::hid_other, iface, kStandardInterfaceIn, kGetDescriptor, 0xFF00U,
                      static_cast<std::uint16_t>(kTypeHid << 8U), MIMUSB_CONTROL_ANSWER);
        r.data = hid_descriptor(first.config, report_descriptor.size());
        out.push_back(r);

        // GET_REPORT of an Input report (7.2.1): the last report the board sent on the IN
        // endpoint, whose first byte is the Report ID when the interface has them.
        std::bitset<256> ids;
        bool has_ids = false;
        for (const HidNodeInput* node : group) {
            if (node->has_report_ids) {
                has_ids = true;
                ids |= node->report_ids;
            }
        }
        const auto last_sent = [&](const std::uint16_t value_mask, const std::uint16_t value,
                                   const bool filter, const std::uint8_t id) {
            BoardRule rule = make_rule(Role::hid_other, iface, kClassInterfaceIn, kGetReport,
                                       value_mask, value, MIMUSB_CONTROL_ANSWER_LAST_SENT);
            rule.rule.endpoint_address = first.config.ep_in.address;
            rule.rule.first_byte_filter = filter ? 1U : 0U;
            rule.rule.first_byte = id;
            out.push_back(std::move(rule));
        };
        if (has_ids) {
            for (std::size_t id = 0U; id < ids.size(); ++id) {
                if (ids.test(id))
                    last_sent(0xFFFFU, static_cast<std::uint16_t>(kInputReport | id), true,
                              static_cast<std::uint8_t>(id));
            }
        } else {
            last_sent(0xFF00U, kInputReport, false, 0U);
        }

        // GET_REPORT of a Feature report: what the Node set, with the Report ID in front when
        // there is one (it is the first byte of the data stage, as of an Input report).
        for (const HidNodeInput* node : group) {
            for (const HidNodeInput::Feature& feature : node->features) {
                BoardRule rule =
                    make_rule(Role::hid_other, iface, kClassInterfaceIn, kGetReport, 0xFFFFU,
                              static_cast<std::uint16_t>(kFeatureReport | feature.report_id),
                              MIMUSB_CONTROL_ANSWER);
                if (feature.report_id != 0U)
                    rule.data.push_back(feature.report_id);
                rule.data.insert(rule.data.end(), feature.data.begin(), feature.data.end());
                out.push_back(std::move(rule));
            }
        }

        // SET_REPORT, SET_IDLE and SET_PROTOCOL (7.2.2, 7.2.4, 7.2.6): taken, and reported to the
        // program by the library. Any value: the report type and ID, the idle rate and the
        // protocol are in wValue, and are read from the setup packet that is reported.
        out.push_back(make_rule(Role::hid_set_report, iface, kClassInterfaceOut, kSetReport, 0U, 0U,
                                MIMUSB_CONTROL_ACCEPT));
        out.push_back(make_rule(Role::hid_set_idle, iface, kClassInterfaceOut, kSetIdle, 0U, 0U,
                                MIMUSB_CONTROL_ACCEPT));
        out.push_back(make_rule(Role::hid_set_protocol, iface, kClassInterfaceOut, kSetProtocol, 0U,
                                0U, MIMUSB_CONTROL_ACCEPT));

        // GET_IDLE and GET_PROTOCOL (7.2.3, 7.2.5): one byte each, kept by the library as the
        // target sets them. HID 1.11 7.2.4: the idle rate is 0 until the host sets it; 7.2.6: the
        // protocol is the report protocol, 1, at the start.
        r = make_rule(Role::hid_get_idle, iface, kClassInterfaceIn, kGetIdle, 0U, 0U,
                      MIMUSB_CONTROL_ANSWER);
        r.data = {0U};
        out.push_back(r);
        r = make_rule(Role::hid_get_protocol, iface, kClassInterfaceIn, kGetProtocol, 0U, 0U,
                      MIMUSB_CONTROL_ANSWER);
        r.data = {1U};
        out.push_back(r);
    }
    for (BoardRule& rule : out)
        rule.capacity = static_cast<std::uint16_t>(rule.data.size());
    return out;
}

} // namespace mimusb::usb
