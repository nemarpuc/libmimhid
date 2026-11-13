// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/*
 * The HID class (HID 1.11) as a FunctionClass. A HID Node is one interface of class 3 with the HID
 * descriptor and an interrupt IN endpoint (and an OUT one if the placement gives it); Nodes placed
 * on the same interface number share it, their report descriptors joined (compose). The class
 * writes the control rules that answer the HID requests (hid_layer.cpp), turns the requests its
 * ACCEPT rules take into the HID events (SET_REPORT, SET_IDLE, SET_PROTOCOL) and keeps the idle
 * rate and protocol it answers with as live rules. A Node's report state is its HidNode.
 */

#include "classes/hid/hid_class.hpp"
#include "api/support.hpp"
#include "classes/hid/hid_layer.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <utility>

using mimusb::detail::FeatureEntry;
using mimusb::detail::HidNode;
using mimusb::detail::HidSpec;
using mimusb::detail::public_node_id;
using mimusb::detail::require_timeout;
using mimusb::detail::saturate_u32;
using mimusb::detail::set_error;
using mimusb::detail::valid_usb_string;
using mimusb::usb::HidTag;
using mimusb::usb::tag;

namespace {

const std::vector<std::uint8_t> kNoDescriptor;

mimusb_result initialize_node_state(HidNode* node) {
    switch (node->spec->kind) {
    case MIMUSB_PROFILE_KEYBOARD: {
        const auto* spec = std::get_if<mimusb::detail::KeyboardConfig>(&node->spec->config);
        mimusb::detail::KeyboardState value{};
        const std::size_t key_count =
            static_cast<std::size_t>(spec->options.usage_maximum - spec->options.usage_minimum) +
            1U;
        value.key_transitions.resize(key_count);
        value.pressed_bitmap.resize(key_count);
        node->state = std::move(value);
        break;
    }
    case MIMUSB_PROFILE_MOUSE: {
        const auto* spec = std::get_if<mimusb::detail::MouseConfig>(&node->spec->config);
        mimusb::detail::MouseState value{};
        value.buttons.resize(spec->options.button_count);
        value.button_transitions.resize(spec->options.button_count);
        node->state = std::move(value);
        break;
    }
    case MIMUSB_PROFILE_TOGGLE: {
        const auto* spec = std::get_if<mimusb::detail::ConsumerConfig>(&node->spec->config);
        mimusb::detail::ConsumerState value{};
        value.pressed.resize(spec->allowed_usages.size());
        value.transitions.resize(spec->allowed_usages.size());
        node->state = std::move(value);
        break;
    }
    case MIMUSB_PROFILE_GAMEPAD: {
        const auto* spec = std::get_if<mimusb::detail::GamepadConfig>(&node->spec->config);
        mimusb::detail::GamepadState value{};
        value.axes.reserve(spec->axes.size());
        for (const auto& axis : spec->axes)
            value.axes.push_back(axis.axis.neutral_value);
        value.buttons.resize(spec->options.button_count);
        value.button_transitions.resize(spec->options.button_count);
        node->state = std::move(value);
        break;
    }
    case MIMUSB_PROFILE_TOUCHSCREEN:
    case MIMUSB_PROFILE_TOUCHPAD: {
        const auto* spec = std::get_if<mimusb::detail::TouchConfig>(&node->spec->config);
        mimusb::detail::TouchState value{};
        value.buttons.resize(spec->fields.button_count);
        value.button_transitions.resize(spec->fields.button_count);
        node->state = std::move(value);
        break;
    }
    case MIMUSB_PROFILE_PEN:
    case MIMUSB_PROFILE_DIGITIZER:
        node->state = mimusb::detail::PenState{};
        break;
    case MIMUSB_PROFILE_HID:
        node->state = mimusb::detail::RawState{};
        node->dirty = false;
        break;
    default:
        set_error(MIMUSB_ERR_INTERNAL, "spec.kind",
                  "A validated ProfileSpec contained an unknown profile kind.");
        return MIMUSB_ERR_INTERNAL;
    }
    return MIMUSB_OK;
}

// The Feature report a touch Spec answers by itself: Contact Count Maximum, when its options
// declare it (HID Usage Tables: Contact Count Maximum).
} // namespace

namespace mimusb::detail {

std::optional<FeatureEntry> declared_feature(const HidSpec& spec) {
    const auto* touch = std::get_if<mimusb::detail::TouchConfig>(&spec.config);
    if (touch == nullptr || touch->fields.enable_contact_count_maximum_feature_declaration != 1U)
        return std::nullopt;
    FeatureEntry entry;
    entry.report_id = touch->fields.report_id.enabled == 1U ? touch->fields.report_id.value : 0U;
    const std::size_t bytes = (touch->fields.contact_count.bit_width + 7U) / 8U;
    for (std::size_t i = 0U; i < bytes; ++i)
        entry.data.push_back(
            static_cast<std::uint8_t>((touch->fields.maximum_contacts >> (8U * i)) & 0xFFU));
    return entry;
}

} // namespace mimusb::detail

namespace {

// What only a HID Node has to satisfy; the endpoints themselves (addresses, packet sizes, ...) are
// checked by the composer, which sees every endpoint of the device.
mimusb_result validate_hid_node_config(const mimusb_device* device,
                                       const mimusb::usb::HidNodeConfig& effective) noexcept {
    if (effective.interface_number >= device->board.max_interfaces_any) {
        set_error(MIMUSB_ERR_OVERFLOW, "placement.first_interface",
                  "The board has fewer interfaces.", 0, 0, 0, 0,
                  static_cast<std::uint32_t>(device->board.max_interfaces_any) - 1U,
                  effective.interface_number);
        return MIMUSB_ERR_OVERFLOW;
    }
    if (!valid_usb_string(effective.name)) {
        set_error(MIMUSB_ERR_PARAM, "placement.name",
                  "A string is not UTF-8 text of at most 126 UTF-16 code units without control "
                  "characters.");
        return MIMUSB_ERR_PARAM;
    }
    // HID 1.11 4.4: a HID interface's endpoints are interrupt endpoints. The IN endpoint is one of
    // the generic driver's, with any of the three policies.
    const bool has_out = effective.ep_out.address != 0U || effective.ep_out.max_packet_size != 0U ||
                         effective.ep_out.transfer_type != 0U || effective.ep_out.interval != 0U;
    for (const mimusb_usb_endpoint* ep :
         {&effective.ep_in, has_out ? &effective.ep_out : nullptr}) {
        if (ep == nullptr)
            continue;
        const char* field = ep == &effective.ep_in ? "placement.ep_in.transfer_type"
                                                   : "placement.ep_out.transfer_type";
        if (ep->transfer_type == 0U) {
            set_error(MIMUSB_ERR_UNSET_FIELD, field,
                      "transfer_type is required: a HID endpoint is MIMUSB_TRANSFER_INTERRUPT.");
            return MIMUSB_ERR_UNSET_FIELD;
        }
        if (ep->transfer_type != MIMUSB_TRANSFER_INTERRUPT) {
            set_error(MIMUSB_ERR_UNSUPPORTED, field,
                      "The endpoints of a HID Node are interrupt endpoints.");
            return MIMUSB_ERR_UNSUPPORTED;
        }
    }
    if (effective.ep_in.policy.on_full == 0U || effective.ep_in.policy.depth == 0U) {
        set_error(MIMUSB_ERR_UNSET_FIELD, "placement.ep_in.policy",
                  "The IN endpoint needs a policy: on_full and depth are required.");
        return MIMUSB_ERR_UNSET_FIELD;
    }
    if (effective.ep_in.policy.on_full > MIMUSB_FULL_WAIT) {
        set_error(MIMUSB_ERR_PARAM, "placement.ep_in.policy",
                  "on_full is MIMUSB_FULL_DROP_NEWEST, _DROP_OLDEST or _WAIT.");
        return MIMUSB_ERR_PARAM;
    }
    if (device->board.usb_high_speed &&
        (effective.ep_in.interval > 16U || effective.ep_out.interval > 16U)) {
        // USB 2.0 9.6.6: at high speed bInterval of an interrupt endpoint is an exponent, 1 to 16.
        set_error(MIMUSB_ERR_PARAM, "placement.ep_in.interval",
                  "A high-speed board takes a bInterval of 1 to 16 (USB 2.0 9.6.6).");
        return MIMUSB_ERR_PARAM;
    }
    // The IN endpoint is checked for the board's limits and USB's rules like any other.
    if (effective.ep_in.address == 0U || (effective.ep_in.address & 0x80U) == 0U) {
        set_error(MIMUSB_ERR_PARAM, "placement.ep_in.address",
                  "ep_in.address is 0x81 to 0x8F: bit 7 set, endpoint number 1 to 15.");
        return MIMUSB_ERR_PARAM;
    }
    if (has_out && (effective.ep_out.address & 0x80U) != 0U) {
        set_error(MIMUSB_ERR_PARAM, "placement.ep_out.address",
                  "ep_out.address is 0x01 to 0x0F: bit 7 clear, endpoint number 1 to 15.");
        return MIMUSB_ERR_PARAM;
    }
    // The endpoints and the interface alone, as the composer will see them with the others.
    mimusb::usb::HidNodeInput one;
    one.config = effective;
    one.report_descriptor = &kNoDescriptor;
    std::vector<mimusb::usb::ComposerGroup> groups;
    mimusb::usb::ComposerError error;
    if (mimusb::usb::compose_hid_groups({one}, &groups, &error) != MIMUSB_OK) {
        set_error(error.result, error.field, error.reason, 0, 0, 0, 0, error.offset, error.length);
        return error.result;
    }
    return mimusb::detail::precheck_groups(device, std::move(groups));
}

constexpr std::uint8_t kClassHid = 0x03U;
constexpr std::uint8_t kTypeHid = 0x21U;
constexpr std::uint8_t kTypeHidReport = 0x22U;

const HidSpec& data_of(const mimusb_spec& spec) {
    return *static_cast<const HidSpec*>(spec.data.get());
}

// What the composer and the rules need of a HID Node.
mimusb::usb::HidNodeInput input_of(const mimusb_node& node) {
    const HidNode& hid = *static_cast<const HidNode*>(node.runtime.get());
    mimusb::usb::HidNodeInput in;
    in.config = hid.config;
    // The policy of the IN endpoint is the one the Node has now (mimusb_node_endpoint_set_policy).
    for (const mimusb::detail::EndpointRuntime& ep : node.in_endpoints) {
        if (ep.address == in.config.ep_in.address)
            in.config.ep_in.policy = ep.policy;
    }
    mimusb::detail::utf16_from_utf8(hid.interface_string.c_str(), &in.name);
    in.report_descriptor = &hid.spec->descriptor;
    in.has_report_ids = hid.has_report_ids;
    in.report_ids = hid.report_ids;
    in.node_id = public_node_id(&node);
    for (const FeatureEntry& feature : hid.features)
        in.features.push_back(mimusb::usb::HidNodeInput::Feature{feature.report_id, feature.data});
    return in;
}

// The HID descriptor (HID 1.11 6.2.1) of a Node alone on its interface.
std::array<std::uint8_t, 9> hid_descriptor(const mimusb::usb::HidNodeConfig& config,
                                           const std::size_t report_length) {
    return {9U,
            kTypeHid,
            static_cast<std::uint8_t>(config.bcd_hid & 0xFFU),
            static_cast<std::uint8_t>(config.bcd_hid >> 8U),
            config.country_code,
            1U,
            kTypeHidReport,
            static_cast<std::uint8_t>(report_length & 0xFFU),
            static_cast<std::uint8_t>((report_length >> 8U) & 0xFFU)};
}

// The Node on `iface` an event belongs to: the one whose Report IDs have the event's, or the first
// on the interface. An Output report without a Report ID in wValue takes it from its first byte
// when a Node there declares it.
void route(mimusb_device* device, const std::uint8_t iface, mimusb_event* event) {
    event->interface_number = iface;
    std::lock_guard<std::mutex> lock(device->nodes_mutex);
    const auto hid_on = [&](const mimusb_node* node) -> const HidNode* {
        if (node == nullptr || node->closed || node->iface != iface)
            return nullptr;
        return mimusb::detail::hid_node(node);
    };
    const mimusb_node* match = nullptr;
    if (event->kind == MIMUSB_EVENT_OUTPUT_REPORT && event->length > 0U && event->report_id == 0U) {
        const std::uint8_t candidate_id = event->data[0];
        for (const mimusb_node* node : device->slots) {
            const HidNode* hid = hid_on(node);
            if (hid != nullptr && hid->has_report_ids && hid->report_ids.test(candidate_id)) {
                match = node;
                event->report_id = candidate_id;
                event->data += 1;
                event->length -= 1U;
                break;
            }
        }
    }
    if (match == nullptr) {
        for (const mimusb_node* node : device->slots) {
            const HidNode* hid = hid_on(node);
            if (hid == nullptr)
                continue;
            if (event->report_id != 0U && hid->has_report_ids &&
                hid->report_ids.test(event->report_id)) {
                match = node;
                break;
            }
            if (match == nullptr)
                match = node;
        }
    }
    event->node_id = public_node_id(match);
}

// A report the target sent: kind OUTPUT_REPORT or FEATURE_REPORT (HID 1.11 7.2.2: the type in the
// high byte of wValue, the Report ID in the low byte), the Report ID and the bytes after it.
void report_event(const std::uint8_t type, const std::uint8_t id, const std::uint8_t* data,
                  std::size_t length, mimusb_event* event) {
    if (type == 2U && id == 0U) {
        event->kind = MIMUSB_EVENT_OUTPUT_REPORT; // the whole packet: the ID, if any, is in it
    } else {
        // TinyUSB strips the Report ID from the data stage when it matches wValue
        // (src/class/hid/hid_device.c, hidd_control_xfer_cb, SET_REPORT).
        if (id != 0U && length > 1U && data[0] == id) {
            ++data;
            --length;
        }
        event->kind = type == 3U ? MIMUSB_EVENT_FEATURE_REPORT : MIMUSB_EVENT_OUTPUT_REPORT;
        event->report_id = id;
    }
    event->data = data;
    event->length = length;
}

class HidClass final : public mimusb::detail::FunctionClass {
  public:
    std::size_t interface_count(const mimusb_spec&) const override { return 1U; }

    std::size_t roles(const mimusb_spec&, mimusb_role_info* out,
                      const std::size_t capacity) const override {
        const mimusb_role_info all[2] = {{MIMUSB_ROLE_HID_IN, 1U}, {MIMUSB_ROLE_HID_OUT, 0U}};
        for (std::size_t i = 0U; i < 2U && i < capacity; ++i)
            out[i] = all[i];
        return 2U;
    }

    void rule_cost(const mimusb_spec& spec, std::size_t* rules,
                   std::size_t* answer_bytes) const override {
        // The rules the class writes for a Node alone on its interface.
        const HidSpec& hid = data_of(spec);
        mimusb::usb::HidNodeInput input;
        input.config.bcd_hid = hid.function.bcd_hid;
        input.report_descriptor = &hid.descriptor;
        mimusb::detail::collect_report_ids(hid.descriptor, &input.report_ids,
                                           &input.has_report_ids);
        if (std::optional<FeatureEntry> declared = mimusb::detail::declared_feature(hid);
            declared.has_value())
            input.features.push_back({declared->report_id, declared->data});
        for (const mimusb::usb::BoardRule& rule : mimusb::usb::compose_hid_rules({input})) {
            ++*rules;
            if (rule.rule.action == MIMUSB_CONTROL_ANSWER)
                *answer_bytes += rule.data.size();
        }
    }

    void reports(const mimusb_spec& spec, const std::vector<std::uint8_t>** descriptor,
                 const std::vector<mimusb_report_capability>** reports) const override {
        *descriptor = &data_of(spec).descriptor;
        *reports = &data_of(spec).report_capabilities;
    }

    mimusb_result build(mimusb_device* device, mimusb_spec* spec,
                        const mimusb_node_placement& placement,
                        mimusb::detail::NodeBuild* out) const override;

    mimusb_result compose(const std::vector<mimusb_node*>& nodes, mimusb::detail::Composition* out,
                          mimusb::usb::ComposerError* error) const override {
        std::vector<mimusb::usb::HidNodeInput> inputs;
        inputs.reserve(nodes.size());
        for (const mimusb_node* node : nodes)
            inputs.push_back(input_of(*node));
        std::vector<mimusb::usb::ComposerGroup> groups;
        if (const mimusb_result r = mimusb::usb::compose_hid_groups(inputs, &groups, error);
            r != MIMUSB_OK)
            return r;
        for (mimusb::usb::ComposerGroup& group : groups)
            out->groups.push_back(std::move(group));
        for (mimusb::usb::BoardRule& rule : mimusb::usb::compose_hid_rules(inputs))
            out->rules.push_back(std::move(rule));
        return MIMUSB_OK;
    }

    mimusb::detail::Taken on_accepted(mimusb_device* device, const mimusb::usb::BoardRule& rule,
                                      const std::uint8_t* setup,
                                      mimusb_event* event) const override {
        using mimusb::detail::Taken;
        const auto value = static_cast<std::uint16_t>(setup[2] | (setup[3] << 8));
        const auto length = static_cast<std::uint16_t>(setup[6] | (setup[7] << 8));
        const std::uint8_t itf = setup[4];
        if (rule.tag == tag(HidTag::set_idle)) {
            // HID 1.11 7.2.4: the idle rate is the high byte of wValue.
            const auto rate = static_cast<std::uint8_t>(value >> 8U);
            mimusb::detail::set_live_answer(device, this, tag(HidTag::get_idle), itf, {rate});
            event->kind = MIMUSB_EVENT_SET_IDLE;
            event->value = rate;
            route(device, itf, event);
            return Taken::deliver;
        }
        if (rule.tag == tag(HidTag::set_protocol)) {
            // HID 1.11 7.2.6: the protocol is wValue (0 boot, 1 report).
            const auto protocol = static_cast<std::uint8_t>(value & 0xFFU);
            mimusb::detail::set_live_answer(device, this, tag(HidTag::get_protocol), itf,
                                            {protocol});
            event->kind = MIMUSB_EVENT_SET_PROTOCOL;
            event->value = protocol;
            route(device, itf, event);
            return Taken::deliver;
        }
        if (rule.tag == tag(HidTag::set_report)) {
            if (length != 0U)
                return Taken::wait_data; // the report follows as the data stage
            report_event(static_cast<std::uint8_t>(value >> 8U),
                         static_cast<std::uint8_t>(value & 0xFFU), setup, 0U, event);
            route(device, itf, event);
            return Taken::deliver;
        }
        return Taken::pass;
    }

    bool on_control_data(mimusb_device* device, const mimusb::detail::PendingData& pending,
                         const std::uint8_t* data, const std::size_t length,
                         mimusb_event* event) const override {
        report_event(static_cast<std::uint8_t>(pending.value >> 8U),
                     static_cast<std::uint8_t>(pending.value & 0xFFU), data, length, event);
        route(device, pending.iface, event);
        return true;
    }

    bool on_endpoint_data(mimusb_device* device, const std::uint8_t iface,
                          const std::uint8_t address, const std::uint8_t* data,
                          const std::size_t length, mimusb_event* event) const override {
        // HID 1.11 4.4: a packet on the interrupt OUT endpoint is an Output report.
        if ((address & 0x80U) != 0U)
            return false;
        event->kind = MIMUSB_EVENT_OUTPUT_REPORT;
        event->data = data;
        event->length = length;
        route(device, iface, event);
        return true;
    }
};

const HidClass kHid{};

mimusb_result HidClass::build(mimusb_device* device, mimusb_spec* spec,
                              const mimusb_node_placement& placement,
                              mimusb::detail::NodeBuild* out) const {
    const HidSpec& hid = data_of(*spec);
    // The function is the Spec's, the interface and the endpoints of its roles are the
    // placement's.
    mimusb::usb::HidNodeConfig config;
    config.interface_number = placement.first_interface;
    config.subclass = hid.function.subclass;
    config.protocol = hid.function.protocol;
    config.country_code = hid.function.country_code;
    config.bcd_hid = hid.function.bcd_hid;
    config.name = placement.name;
    bool have_in = false;
    bool have_out = false;
    for (std::size_t i = 0U; i < placement.endpoint_count; ++i) {
        const mimusb_placed_endpoint& placed = placement.endpoints[i];
        if (placed.reserved != 0U) {
            set_error(MIMUSB_ERR_PARAM, "placement.endpoints.reserved",
                      "The reserved field must be zero.", 0, 0, 0, 0, static_cast<std::uint32_t>(i),
                      0);
            return MIMUSB_ERR_PARAM;
        }
        bool* seen = placed.role == MIMUSB_ROLE_HID_IN    ? &have_in
                     : placed.role == MIMUSB_ROLE_HID_OUT ? &have_out
                                                          : nullptr;
        if (seen == nullptr || *seen) {
            set_error(MIMUSB_ERR_PARAM, "placement.endpoints.role",
                      seen == nullptr ? "The Spec has no such role (mimusb_spec_roles)."
                                      : "A role is given once.",
                      0, 0, 0, 0, static_cast<std::uint32_t>(i),
                      static_cast<std::uint32_t>(placed.role));
            return MIMUSB_ERR_PARAM;
        }
        *seen = true;
        (placed.role == MIMUSB_ROLE_HID_IN ? config.ep_in : config.ep_out) = placed.endpoint;
    }
    if (!have_in) {
        set_error(MIMUSB_ERR_UNSET_FIELD, "placement.endpoints",
                  "MIMUSB_ROLE_HID_IN is required: the Node's interrupt IN endpoint.");
        return MIMUSB_ERR_UNSET_FIELD;
    }
    if (const mimusb_result v = validate_hid_node_config(device, config); v != MIMUSB_OK)
        return v;

    if (hid.descriptor.empty() || hid.descriptor.size() > device->board.max_answer_bytes) {
        set_error(MIMUSB_ERR_OVERFLOW, "spec.descriptor.board",
                  "The descriptor exceeds the board's answer memory.");
        return MIMUSB_ERR_OVERFLOW;
    }
    struct Limit {
        const char* field;
        const char* reason;
        std::uint64_t allowed;
        std::uint64_t needed;
    };
    const mimusb_linux_hid_policy& linux_hid = device->config.target.linux_hid;
    if (linux_hid.descriptor_bytes == 0U || linux_hid.fields_per_report == 0U ||
        linux_hid.global_stack_depth == 0U || linux_hid.report_size_bits == 0U ||
        linux_hid.usages == 0U || linux_hid.report_data_bits == 0U) {
        set_error(MIMUSB_ERR_UNSET_FIELD, "target.linux_hid",
                  "Every linux_hid limit is required to open a Node of a HID Spec.");
        return MIMUSB_ERR_UNSET_FIELD;
    }
    const mimusb::hid::DescriptorRequirements& needs = hid.requirements;
    const std::array<Limit, 6> limits{{
        {"spec.descriptor.linux_hid", "The descriptor exceeds linux_hid.descriptor_bytes.",
         linux_hid.descriptor_bytes, hid.descriptor.size()},
        {"spec.requirements.maximum_fields_per_report",
         "A report has more fields than linux_hid.fields_per_report.", linux_hid.fields_per_report,
         needs.maximum_fields_per_report},
        {"spec.requirements.maximum_global_stack_depth",
         "The descriptor pushes deeper than linux_hid.global_stack_depth.",
         linux_hid.global_stack_depth, needs.maximum_global_stack_depth},
        {"spec.requirements.maximum_report_size_bits",
         "A Report Size is larger than linux_hid.report_size_bits.", linux_hid.report_size_bits,
         needs.maximum_report_size_bits},
        {"spec.requirements.maximum_usages",
         "The descriptor has more Usages than linux_hid.usages.", linux_hid.usages,
         needs.maximum_usages},
        {"spec.requirements.maximum_report_data_bits",
         "A report is larger than linux_hid.report_data_bits.", linux_hid.report_data_bits,
         needs.maximum_report_data_bits},
    }};
    for (const Limit& limit : limits) {
        if (limit.needed > limit.allowed) {
            set_error(MIMUSB_ERR_OVERFLOW, limit.field, limit.reason, 0, 0, 0, 0,
                      saturate_u32(limit.allowed), saturate_u32(limit.needed));
            return MIMUSB_ERR_OVERFLOW;
        }
    }
    for (const auto& report : hid.report_capabilities) {
        if (report.wire_length > device->maximum_report_bytes) {
            set_error(MIMUSB_ERR_OVERFLOW, "spec.report",
                      "A report exceeds the board's or the caller's report size limit.");
            return MIMUSB_ERR_OVERFLOW;
        }
    }

    // The Node's interface as it is when the Node is alone on it: the HID descriptor, then the
    // endpoints (HID 1.11 7.1). Nodes sharing it are joined at the commit (compose).
    mimusb::detail::InterfacesSpec& layout = out->storage;
    const std::array<std::uint8_t, 9> descriptor = hid_descriptor(config, hid.descriptor.size());
    std::vector<mimusb_usb_item> items;
    mimusb_usb_item item{};
    item.kind = MIMUSB_ITEM_DESCRIPTOR;
    item.descriptor = layout.keep(descriptor.data(), descriptor.size());
    item.descriptor_length = descriptor.size();
    items.push_back(item);
    item = mimusb_usb_item{};
    item.kind = MIMUSB_ITEM_ENDPOINT;
    item.endpoint = config.ep_in;
    items.push_back(item);
    if (config.ep_out.address != 0U || config.ep_out.max_packet_size != 0U) {
        item.endpoint = config.ep_out;
        items.push_back(item);
    }
    layout.items.push_back(std::move(items));
    layout.settings.push_back(
        {mimusb_usb_setting{layout.items.back().data(), layout.items.back().size()}});
    mimusb_usb_interface itf{};
    itf.number = config.interface_number;
    itf.class_code = kClassHid;
    itf.subclass = config.subclass;
    itf.protocol = config.protocol;
    if (placement.name != nullptr) {
        layout.names.emplace_back(placement.name);
        itf.name = layout.names.back().c_str();
    }
    itf.settings = layout.settings.back().data();
    itf.setting_count = 1U;
    layout.interfaces.push_back(itf);
    layout.finish();
    out->config = &layout.view;

    auto runtime = std::make_unique<HidNode>();
    runtime->spec = &hid;
    runtime->config = config;
    runtime->interface_string = placement.name != nullptr ? placement.name : "";
    runtime->config.name = nullptr;
    mimusb::detail::collect_report_ids(hid.descriptor, &runtime->report_ids,
                                       &runtime->has_report_ids);
    if (const mimusb_result r = initialize_node_state(runtime.get()); r != MIMUSB_OK)
        return r;
    // A touch Spec that declares Contact Count Maximum answers the target's Feature read.
    if (std::optional<FeatureEntry> declared = mimusb::detail::declared_feature(hid);
        declared.has_value())
        runtime->features.push_back(std::move(*declared));
    out->runtime = std::move(runtime);
    return MIMUSB_OK;
}

} // namespace

namespace mimusb::detail {

const FunctionClass& hid_class() noexcept { return kHid; }

mimusb_spec* make_hid_spec(std::unique_ptr<HidSpec> spec) {
    auto handle = std::make_unique<mimusb_spec>();
    handle->kind = spec->kind;
    handle->function_class = &kHid;
    handle->data = std::move(spec);
    return handle.release();
}

const HidSpec* hid_spec(const mimusb_spec* spec) noexcept {
    if (spec == nullptr || spec->function_class != &kHid)
        return nullptr;
    return static_cast<const HidSpec*>(spec->data.get());
}

HidSpec* hid_spec(mimusb_spec* spec) noexcept {
    if (spec == nullptr || spec->function_class != &kHid)
        return nullptr;
    return static_cast<HidSpec*>(spec->data.get());
}

HidNode* hid_node(mimusb_node* node) noexcept {
    if (node == nullptr || node->spec == nullptr || node->spec->function_class != &kHid)
        return nullptr;
    return static_cast<HidNode*>(node->runtime.get());
}

const HidNode* hid_node(const mimusb_node* node) noexcept {
    if (node == nullptr || node->spec == nullptr || node->spec->function_class != &kHid)
        return nullptr;
    return static_cast<const HidNode*>(node->runtime.get());
}

HidNode* attach_hid(mimusb_node* node, mimusb_spec* spec) {
    auto runtime = std::make_unique<HidNode>();
    runtime->owner = node;
    runtime->spec = hid_spec(spec);
    if (runtime->spec != nullptr)
        (void)initialize_node_state(runtime.get());
    node->spec = spec;
    node->runtime = std::move(runtime);
    return hid_node(node);
}

mimusb_result HidNode::before_close(const std::uint32_t timeout_ms) {
    return send_neutral(this, timeout_ms);
}

// Collects the Report IDs a descriptor declares (short items only; validated descriptors).
void collect_report_ids(const std::vector<std::uint8_t>& descriptor, std::bitset<256>* ids,
                        bool* has_ids) {
    constexpr std::uint8_t kTypeGlobal = 1U;
    constexpr std::uint8_t kGlobalReportId = 8U;
    std::size_t pos = 0U;
    while (pos < descriptor.size()) {
        const std::uint8_t prefix = descriptor[pos];
        if (prefix == 0xFEU) { // long item: length in the next byte
            pos += pos + 1U < descriptor.size() ? 3U + descriptor[pos + 1U] : descriptor.size();
            continue;
        }
        const std::uint8_t size_code = prefix & 0x03U;
        const std::size_t size = size_code == 3U ? 4U : size_code;
        const std::uint8_t type = (prefix >> 2U) & 0x03U;
        const std::uint8_t item_tag = (prefix >> 4U) & 0x0FU;
        if (type == kTypeGlobal && item_tag == kGlobalReportId && size >= 1U &&
            pos + 1U < descriptor.size()) {
            ids->set(descriptor[pos + 1U]);
            *has_ids = true;
        }
        pos += 1U + size;
    }
}

} // namespace mimusb::detail

extern "C" {

mimusb_result MIMUSB_CALL mimusb_node_set_feature_response(
    mimusb_node* node, const uint8_t report_id,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    const uint8_t* data, const size_t length, const uint32_t control_timeout_ms) try {
    mimusb::detail::clear_error();
    HidNode* hid = mimusb::detail::hid_node(node);
    if (hid == nullptr || node->closed || node->device == nullptr ||
        (data == nullptr && length != 0U) || length > node->device->board.max_control_data_bytes) {
        set_error(MIMUSB_ERR_PARAM, "node.feature",
                  "A live HID Node and a response of at most the board's max_feature_bytes "
                  "are required.");
        return MIMUSB_ERR_PARAM;
    }
    if (const mimusb_result t = require_timeout(control_timeout_ms, "control_timeout_ms");
        t != MIMUSB_OK)
        return t;
    mimusb_device* device = node->device;
    FeatureEntry* entry = nullptr;
    for (auto& existing : hid->features) {
        if (existing.report_id == report_id)
            entry = &existing;
    }
    const bool existed = entry != nullptr;
    const std::size_t old_length = existed ? entry->data.size() : 0U;
    if (entry == nullptr) {
        hid->features.push_back(FeatureEntry{report_id, {}});
        entry = &hid->features.back();
    }
    entry->data.assign(data, data + length);
    if (!device->configured || device->batch_open || node->staged_only) {
        device->dirty = device->dirty || device->configured;
        return MIMUSB_OK; // sent with the next commit
    }
    if (existed && old_length == length) {
        // The rule exists with room for these bytes: only its bytes change. The target reads the
        // Report ID first when there is one (HID 1.11 7.2.1 and 8.1).
        std::vector<std::uint8_t> bytes;
        if (report_id != 0U)
            bytes.push_back(report_id);
        bytes.insert(bytes.end(), entry->data.begin(), entry->data.end());
        std::size_t index = device->table.size();
        {
            std::lock_guard<std::mutex> lock(device->rules_mutex);
            for (std::size_t i = 0U; i < device->table.size(); ++i) {
                const mimusb::usb::BoardRule& rule = device->table[i];
                if (rule.owner == &kHid && rule.tag == tag(HidTag::feature) &&
                    rule.iface == node->iface && rule.rule.action == MIMUSB_CONTROL_ANSWER &&
                    rule.rule.request_value == 0x01U &&
                    rule.rule.value_value == static_cast<std::uint16_t>(0x0300U | report_id))
                    index = i;
            }
        }
        if (index < device->table.size()) {
            const mimusb_result sent = mimusb::detail::send_rule_data(
                device, index, bytes.data(), bytes.size(), control_timeout_ms);
            if (sent == MIMUSB_OK) {
                std::lock_guard<std::mutex> lock(device->rules_mutex);
                if (index < device->table.size())
                    device->table[index].data = std::move(bytes);
            }
            return sent;
        }
    }
    // A new Feature report or another length: the table is made again.
    return mimusb::detail::rebuild_rules(device, control_timeout_ms);
}
MIMUSB_C_RESULT_CATCH("node.set_feature_response")

} // extern "C"
