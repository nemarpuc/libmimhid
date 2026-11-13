// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
#pragma once

/*
 * The HID class: what a HID Spec keeps (HidSpec: the report descriptor, its layout and the profile
 * the Spec was made for) and what a HID Node keeps while it lives (HidNode: the report state of its
 * profile and the report in flight). The device layer sees neither: it holds a HidSpec as the
 * Spec's SpecData and a HidNode as the Node's NodeClass.
 */

#include "classes/function_class.hpp"
#include "classes/hid/hid_node_config.hpp"
#include "hid/report_layout.hpp"
#include "hid/validator.hpp"
#include "mimusb.h"

#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace mimusb::detail {

struct KeyboardConfig {
    mimusb_keyboard_options options{};
};

struct MouseConfig {
    mimusb_mouse_options options{};
};

struct ConsumerConfig {
    mimusb_toggle_options options{};
    std::vector<std::uint16_t> allowed_usages;
    std::vector<mimusb_usage_semantic> usage_semantics;
    std::vector<std::string> expected_linux_event_types;
    std::vector<std::string> expected_linux_codes;
};

struct GamepadAxisConfig {
    mimusb_gamepad_axis_option axis{};
    std::string expected_linux_code;
};

struct GamepadConfig {
    mimusb_gamepad_options options{};
    std::vector<GamepadAxisConfig> axes;
};

/* Profile-agnostic contact-field representation shared by Touchscreen and
 * Touchpad. button_count is always here; Touchscreen always passes zero, so
 * the button state machine below needs no profile_kind branch. */
struct TouchFields {
    mimusb_report_id_option report_id{};
    std::uint32_t maximum_contacts{};
    std::uint32_t contacts_per_report{};
    mimusb_integer_field contact_id{};
    mimusb_integer_field x{};
    mimusb_integer_field y{};
    mimusb_integer_field contact_count{};
    std::uint32_t enable_pressure{};
    mimusb_integer_field pressure{};
    std::uint32_t enable_width{};
    mimusb_integer_field width{};
    std::uint32_t enable_height{};
    mimusb_integer_field height{};
    std::uint32_t enable_azimuth{};
    mimusb_integer_field azimuth{};
    std::uint32_t enable_scan_time{};
    mimusb_integer_field scan_time{};
    std::uint32_t scan_time_unit_100us{};
    std::uint32_t enable_contact_count_maximum_feature_declaration{};
    std::uint32_t enable_multi_packet_frames{};
    std::uint32_t button_count{};
};

struct TouchConfig {
    TouchFields fields{};
};

struct PenConfig {
    mimusb_pen_options options{};
    std::vector<std::uint16_t> barrel_usages;
};

struct RawConfig {
    std::vector<mimusb_hid_report> reports;
    std::vector<std::uint8_t> trailing_valid_masks;
};

using SpecConfig = std::variant<std::monostate, KeyboardConfig, MouseConfig, ConsumerConfig,
                                GamepadConfig, TouchConfig, PenConfig, RawConfig>;

// One touch contact as the state machine keeps it; not part of the public ABI.
struct TouchContact {
    std::uint32_t contact_id;
    std::int32_t x;
    std::int32_t y;
    std::int32_t pressure;
    std::int32_t width;
    std::int32_t height;
    std::int32_t azimuth;
};

enum class ContactPhase : std::uint8_t { none, down, up };

struct ContactState {
    ContactPhase phase{ContactPhase::none};
    TouchContact sample{};
    /* A Down edge must reach one accepted frame before Up may replace it. */
    bool lifecycle_transition_pending{};
};

struct KeyboardState {
    std::vector<std::uint8_t> pressed_bitmap;
    std::vector<std::uint8_t> key_transitions;
    std::size_t pressed_bitmap_count{};
    std::uint8_t modifiers{};
    std::uint8_t modifier_transitions{};
};

struct MouseState {
    std::vector<std::uint8_t> buttons;
    std::vector<std::uint8_t> button_transitions;
    std::int64_t dx{};
    std::int64_t dy{};
    std::int64_t wheel{};
    std::int64_t pan{};
};

struct ConsumerState {
    // One entry per allowed Usage, in allow-list order.
    std::vector<std::uint8_t> pressed;
    std::vector<std::uint8_t> transitions;
};

struct GamepadState {
    std::vector<std::int32_t> axes;
    std::vector<std::uint8_t> buttons;
    std::vector<std::uint8_t> button_transitions;
    bool hat_has_direction{};
    std::int32_t hat{};
    bool hat_transition_pending{};
};

struct TouchState {
    std::array<ContactState, 16> contacts{};
    std::vector<std::uint8_t> buttons;
    std::vector<std::uint8_t> button_transitions;
    std::uint32_t scan_time{};
    std::size_t packet_cursor{};
    std::chrono::steady_clock::time_point scan_epoch{};
    bool scan_epoch_active{};
};

struct PenState {
    mimusb_pen_sample desired{};
    mimusb_pen_sample emitted{};
    std::uint32_t barrel_transitions{};
    bool tool_switch_departure{};
    bool in_range_transition_pending{};
    bool tip_transition_pending{};
    bool eraser_transition_pending{};
};

struct BatteryState {
    bool has_value{};
    std::int32_t strength{};
};

struct RawState {
    const std::uint8_t* pending{};
    std::size_t pending_length{};
};

using NodeState = std::variant<std::monostate, KeyboardState, MouseState, ConsumerState,
                               GamepadState, TouchState, PenState, RawState>;

// A Spec of the HID class: a generated profile or mimusb_spec_create_hid.
struct HidSpec : SpecData {
    mimusb_profile_kind kind{};
    // The HID function of the Spec (mimusb_hid_function in its options).
    mimusb_hid_function function{};
    std::vector<std::uint8_t> descriptor;
    mimusb::hid::ReportLayout layout;
    mimusb::hid::DescriptorRequirements requirements;
    std::vector<mimusb_report_capability> report_capabilities;
    SpecConfig config;
    // The optional battery report: a second Input report with its own layout.
    bool has_battery{};
    mimusb_battery_report battery{};
    mimusb::hid::ReportLayout battery_layout;
};

// A Feature report a HID Node answers the target's GET_REPORT with.
struct FeatureEntry {
    std::uint8_t report_id{};
    std::vector<std::uint8_t> data;
};

// The runtime of a HID Node.
struct HidNode : NodeClass {
    const HidSpec* spec{};       // the Node's Spec, as the HID class keeps it
    usb::HidNodeConfig config{}; // the function and the endpoints of the placement
    std::string interface_string;
    std::vector<FeatureEntry> features; // answers to the target's Feature report reads
    bool has_report_ids{false};         // the descriptor uses Report IDs
    std::bitset<256> report_ids;        // every Report ID the descriptor declares
    NodeState state;
    std::int64_t submitted_dx{};
    std::int64_t submitted_dy{};
    std::int64_t submitted_wheel{};
    std::int64_t submitted_pan{};
    std::size_t submitted_report_length{};
    std::uint16_t submitted_report_id{};
    std::size_t submitted_touch_count{};
    bool submitted_touch_final_packet{};
    bool submitted_non_neutral{};
    bool emitted_non_neutral{};
    // Registration alone sends nothing; only a real mutation marks the Node.
    bool dirty{false};
    // The battery report of a Spec that has one. It is sent only when there
    // is no input report to send; has_value says a level was ever set.
    BatteryState battery{};
    bool battery_dirty{false};
    bool submitted_battery{false};
    // The report just written; the state machine settles it in transfer_complete().
    std::atomic<bool> transfer_inflight{false};
    std::atomic<mimusb_result> completion_result{MIMUSB_OK};
    std::atomic<std::int32_t> completion_native_status{0};
    std::atomic<std::uint32_t> completion_report_length{0U};
    std::atomic<std::uint16_t> completion_report_id{0U};

    mimusb_result before_close(std::uint32_t timeout_ms) override;
};

// A Spec handle of the HID class holding `spec` (one reference, for the caller).
mimusb_spec* make_hid_spec(std::unique_ptr<HidSpec> spec);
// The HID data of a Spec, or null when the Spec is not of the HID class.
const HidSpec* hid_spec(const mimusb_spec* spec) noexcept;
HidSpec* hid_spec(mimusb_spec* spec) noexcept;
// The HID runtime of a Node, or null when the Node is not of the HID class.
HidNode* hid_node(mimusb_node* node) noexcept;
const HidNode* hid_node(const mimusb_node* node) noexcept;
// Makes `node` a HID Node of `spec` that is attached to no Device (the state machine alone, as the
// unit tests drive it); a public Node is made by mimusb_node_open. Returns its runtime.
HidNode* attach_hid(mimusb_node* node, mimusb_spec* spec);

inline std::uint16_t report_id(const HidNode* node) noexcept {
    return node != nullptr && node->spec != nullptr && node->spec->layout.has_report_id
               ? node->spec->layout.report_id
               : 0U;
}

void collect_report_ids(const std::vector<std::uint8_t>& descriptor, std::bitset<256>* ids,
                        bool* has_ids);
// The Feature report a touch Spec answers by itself (Contact Count Maximum), if it declares one.
std::optional<FeatureEntry> declared_feature(const HidSpec& spec);

mimusb_result serialize_node(HidNode* node, std::uint8_t* report, std::size_t capacity,
                             std::size_t* report_length) noexcept;
mimusb_result validate_serialized_report(const HidSpec* spec, const std::uint8_t* report,
                                         std::size_t report_length) noexcept;
mimusb_result validate_serialized_report(const HidSpec* spec,
                                         const mimusb::hid::ReportLayout& layout,
                                         const std::uint8_t* report,
                                         std::size_t report_length) noexcept;
mimusb_result serialize_battery(const HidNode* node, std::uint8_t* report, std::size_t capacity,
                                std::size_t* report_length) noexcept;
void transfer_complete(HidNode* node, mimusb_result result, std::int32_t native_status) noexcept;
// MIMUSB_OK, or why a node cannot send now (serial link gone, batch open, re-enumerating).
mimusb_result link_available(const HidNode* node, const char* field) noexcept;

bool has_non_neutral_state(const HidNode* node) noexcept;
void make_neutral(HidNode* node) noexcept;
bool touch_frame_in_progress(const HidNode* node) noexcept;
// Sends a Node's pending reports until it is clean, for the close paths.
mimusb_result send_neutral(HidNode* node, std::uint32_t timeout_ms);

} // namespace mimusb::detail
