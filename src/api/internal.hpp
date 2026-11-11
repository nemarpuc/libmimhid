// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#pragma once

/*
 * This file owns the private C-handle representations and shared invariants.
 * It contains no public ABI declarations beyond the opaque tag definitions.
 */

#include "hid/report_layout.hpp"
#include "hid/validator.hpp"
#include "mimhid.h"
#include "transport/link.hpp"

#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

namespace mimhid::detail {

struct KeyboardConfig {
    mimhid_keyboard_options options{};
};

struct MouseConfig {
    mimhid_mouse_options options{};
};

struct ConsumerConfig {
    mimhid_toggle_options options{};
    std::vector<std::uint16_t> allowed_usages;
    std::vector<mimhid_usage_semantic> usage_semantics;
    std::vector<std::string> expected_linux_event_types;
    std::vector<std::string> expected_linux_codes;
};

struct GamepadAxisConfig {
    mimhid_gamepad_axis_option axis{};
    std::string expected_linux_code;
};

struct GamepadConfig {
    mimhid_gamepad_options options{};
    std::vector<GamepadAxisConfig> axes;
};

/* Profile-agnostic contact-field representation shared by Touchscreen and
 * Touchpad. button_count is always here; Touchscreen always passes zero, so
 * the button state machine below needs no profile_kind branch. */
struct TouchFields {
    mimhid_report_id_option report_id{};
    std::uint32_t maximum_contacts{};
    std::uint32_t contacts_per_report{};
    mimhid_integer_field contact_id{};
    mimhid_integer_field x{};
    mimhid_integer_field y{};
    mimhid_integer_field contact_count{};
    std::uint32_t enable_pressure{};
    mimhid_integer_field pressure{};
    std::uint32_t enable_width{};
    mimhid_integer_field width{};
    std::uint32_t enable_height{};
    mimhid_integer_field height{};
    std::uint32_t enable_azimuth{};
    mimhid_integer_field azimuth{};
    std::uint32_t enable_scan_time{};
    mimhid_integer_field scan_time{};
    std::uint32_t scan_time_unit_100us{};
    std::uint32_t enable_contact_count_maximum_feature_declaration{};
    std::uint32_t enable_multi_packet_frames{};
    std::uint32_t button_count{};
};

struct TouchConfig {
    TouchFields fields{};
};

struct PenConfig {
    mimhid_pen_options options{};
    std::vector<std::uint16_t> barrel_usages;
};

struct RawConfig {
    std::vector<mimhid_raw_report> reports;
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
    mimhid_pen_sample desired{};
    mimhid_pen_sample emitted{};
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

void set_error(mimhid_result result, const char* field, const char* reason,
               std::int32_t transport_status = 0, std::int32_t frame_type = 0,
               std::uint16_t node_id = 0, std::uint16_t report_id = 0, std::uint32_t offset = 0,
               std::uint32_t length = 0) noexcept;
void clear_error() noexcept;
mimhid_result finish_result(mimhid_result result, const char* field, const char* reason,
                            std::int32_t transport_status = 0, std::int32_t frame_type = 0,
                            std::uint16_t node_id = 0, std::uint16_t report_id = 0,
                            std::uint32_t offset = 0, std::uint32_t length = 0) noexcept;
const char* result_name(mimhid_result result) noexcept;
const char* frame_type_name(std::int32_t frame_type) noexcept;
void restore_error(const mimhid_error_detail& detail) noexcept;
bool retain_spec(struct mimhid_spec* spec) noexcept;
bool release_spec(struct mimhid_spec* spec) noexcept;
void log_message(const struct mimhid_device* device, mimhid_log_level level,
                 const char* message) noexcept;

bool valid_boolean(std::uint32_t value) noexcept;
// Defined inline so that static analysis sees that a null pointer never passes.
inline bool valid_struct(const void* value, const std::uint32_t actual_size,
                         const std::uint32_t expected_size, const char* field) noexcept {
    if (value == nullptr) {
        set_error(MIMHID_ERR_PARAM, field, "The required structure pointer is null.");
        return false;
    }
    if (actual_size != expected_size) {
        set_error(MIMHID_ERR_PARAM, field,
                  "struct_size must equal the size of the ABI structure used by this release.");
        return false;
    }
    return true;
}

mimhid_result serialize_node(struct mimhid_node* node, std::uint8_t* report, std::size_t capacity,
                             std::size_t* report_length) noexcept;
mimhid_result validate_serialized_report(const struct mimhid_spec* spec, const std::uint8_t* report,
                                         std::size_t report_length) noexcept;
mimhid_result validate_serialized_report(const struct mimhid_spec* spec,
                                         const mimhid::hid::ReportLayout& layout,
                                         const std::uint8_t* report,
                                         std::size_t report_length) noexcept;
mimhid_result serialize_battery(const struct mimhid_node* node, std::uint8_t* report,
                                std::size_t capacity, std::size_t* report_length) noexcept;
void transfer_complete(void* user, mimhid_result result, std::int32_t native_status) noexcept;
// MIMHID_OK, or why a node cannot send now (serial link gone, batch open, re-enumerating).
mimhid_result link_available(const struct mimhid_node* node, const char* field) noexcept;

} // namespace mimhid::detail

namespace mimhid::detail {

// One interface of a Node opened with mimhid_node_open, copied from the caller's configuration.
struct GenericInterface {
    std::uint8_t number{};
    std::uint8_t class_code{};
    std::uint8_t subclass{};
    std::uint8_t protocol{};
    std::string name;
    std::vector<std::uint8_t> class_descriptors;
    std::vector<mimhid_usb_endpoint> endpoints;
};

// What the library knows of an IN endpoint of such a Node while it is open. The counters are
// guarded by the Device's endpoint_mutex.
struct EndpointRuntime {
    std::uint8_t address{};
    std::uint16_t max_packet_size{};
    mimhid_endpoint_policy policy{};
    std::uint32_t outstanding{}; // frames written minus frames the board reported finished
    std::uint64_t epoch{};       // counts the resets of `outstanding`, which wake writers
};

} // namespace mimhid::detail

struct mimhid_spec {
    std::atomic<std::uint32_t> references{1U};
    mimhid_profile_kind kind{};
    std::vector<std::uint8_t> descriptor;
    mimhid::hid::ReportLayout layout;
    mimhid::hid::DescriptorRequirements requirements;
    std::vector<mimhid_report_capability> report_capabilities;
    mimhid::detail::SpecConfig config;
    // The optional battery report: a second Input report with its own layout.
    bool has_battery{};
    mimhid_battery_report battery{};
    mimhid::hid::ReportLayout battery_layout;
};

struct mimhid_discovery {
    std::vector<mimhid_device_info> entries;
};

struct mimhid_device {
    mimhid_device_config config{};
    mimhid_device_info info{};
    mimhid::transport::BoardInfo board{};
    mimhid::transport::Link link;
    std::uint32_t maximum_report_bytes{};
    std::string manufacturer;
    std::string product;
    std::string serial;
    std::string configuration;
    // Strings the caller added (mimhid_device_add_string), by index, in UTF-8.
    std::map<std::uint8_t, std::string> user_strings;

    // The wait policy's counts (EndpointRuntime) and the writers that wait on them. A writer holds
    // the mutex only to look at a count, never while it waits for the board or writes.
    std::mutex endpoint_mutex;
    std::condition_variable endpoint_cv;

    // Nodes by node id (the index). A closed Node leaves a null entry that the next Node takes.
    // Event delivery reads this from the reader thread.
    std::mutex nodes_mutex;
    std::vector<struct mimhid_node*> slots;

    bool batch_open{false};
    bool configured{false};                  // the board holds a committed configuration
    bool dirty{false};                       // staged changes not yet committed
    std::atomic<bool> loss_reported{false};  // the board reports dropped reports
    std::atomic<bool> control_routed{false}; // the board holds control routing rules or reports
    std::atomic<bool> reconfiguring{false};
    std::atomic<bool> closing{false};
    std::atomic<mimhid_result> latched{MIMHID_OK};
};

struct mimhid_node {
    mimhid_device* device{};
    mimhid_spec* spec{};
    mimhid::detail::NodeState state;
    // A Node opened with mimhid_node_open (generic) has no Spec: its interfaces are in `interfaces`
    // and `config` is not used. A profile Node (mimhid_node_open_profile, _raw) has a Spec and
    // `config`, and the board's HID driver serves it.
    bool generic{false};
    std::vector<mimhid::detail::GenericInterface> interfaces;
    std::vector<std::uint8_t> association;
    std::vector<mimhid::detail::EndpointRuntime> in_endpoints; // generic Nodes only
    mimhid_hid_node_config config{};
    std::string interface_string;
    std::uint16_t node_id{};
    std::uint8_t iface{}; // config.interface_number: the bInterfaceNumber the Node has on the bus
    struct FeatureEntry {
        std::uint8_t report_id{};
        std::vector<std::uint8_t> data;
    };
    std::vector<FeatureEntry> features; // answers to the target's Feature report reads
    bool has_report_ids{false};         // the descriptor uses Report IDs
    std::bitset<256> report_ids;        // every Report ID the descriptor declares
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
    mimhid::detail::BatteryState battery{};
    bool battery_dirty{false};
    bool submitted_battery{false};
    bool closed{false};
    bool staged_only{false}; // opened inside a batch that has not ended
    // The report just written; the state machine settles it in transfer_complete().
    std::atomic<bool> transfer_inflight{false};
    std::atomic<mimhid_result> completion_result{MIMHID_OK};
    std::atomic<std::int32_t> completion_native_status{0};
    std::atomic<std::uint32_t> completion_report_length{0U};
    std::atomic<std::uint16_t> completion_report_id{0U};
};
