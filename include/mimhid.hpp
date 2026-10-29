// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#pragma once

/* Every function of the C ABI in mimhid.h, forwarded unchanged under namespace mimhid without the
 * mimhid_ prefix; types, arguments, results and documentation are those of mimhid.h. On top of
 * them, at the end of this file: functions that return initialized options, and owning handles for
 * the two objects whose release cannot fail. */

#include "mimhid.h"

#include <memory>

namespace mimhid {

[[nodiscard]] inline const mimhid_error_detail* last_error() noexcept {
    return mimhid_last_error();
}
[[nodiscard]] inline const char* result_name(mimhid_result result) noexcept {
    return mimhid_result_name(result);
}
[[nodiscard]] inline const char* frame_type_name(int32_t frame_type) noexcept {
    return mimhid_frame_type_name(frame_type);
}
[[nodiscard]] inline uint32_t version() noexcept { return mimhid_version(); }
[[nodiscard]] inline size_t discovery_count(const mimhid_discovery* discovery) noexcept {
    return mimhid_discovery_count(discovery);
}
[[nodiscard]] inline const mimhid_device_info* discovery_get(const mimhid_discovery* discovery,
                                                             size_t index) noexcept {
    return mimhid_discovery_get(discovery, index);
}
inline void discovery_destroy(mimhid_discovery* discovery) noexcept {
    mimhid_discovery_destroy(discovery);
}
[[nodiscard]] inline mimhid_result discover(uint32_t timeout_ms,
                                            mimhid_discovery** out_discovery) noexcept {
    return mimhid_discover(timeout_ms, out_discovery);
}
[[nodiscard]] inline mimhid_result device_open(const char* port_path,
                                               const mimhid_device_config* config,
                                               mimhid_device** out_device) noexcept {
    return mimhid_device_open(port_path, config, out_device);
}
[[nodiscard]] inline mimhid_result
device_set_usb_identity(mimhid_device* device, const mimhid_usb_device* usb_device,
                        const mimhid_usb_config* usb_config,
                        const mimhid_commit_timeouts* timeouts) noexcept {
    return mimhid_device_set_usb_identity(device, usb_device, usb_config, timeouts);
}
[[nodiscard]] inline mimhid_result device_commit(mimhid_device* device, uint32_t reconnect_delay_ms,
                                                 const mimhid_commit_timeouts* timeouts) noexcept {
    return mimhid_device_commit(device, reconnect_delay_ms, timeouts);
}
[[nodiscard]] inline mimhid_result device_connect(mimhid_device* device,
                                                  const mimhid_commit_timeouts* timeouts) noexcept {
    return mimhid_device_connect(device, timeouts);
}
[[nodiscard]] inline mimhid_result device_disconnect(mimhid_device* device,
                                                     uint32_t control_timeout_ms) noexcept {
    return mimhid_device_disconnect(device, control_timeout_ms);
}
[[nodiscard]] inline int32_t device_is_ready(const mimhid_device* device) noexcept {
    return mimhid_device_is_ready(device);
}
[[nodiscard]] inline mimhid_result device_wait_ready(mimhid_device* device,
                                                     uint32_t timeout_ms) noexcept {
    return mimhid_device_wait_ready(device, timeout_ms);
}
[[nodiscard]] inline mimhid_result device_batch_begin(mimhid_device* device) noexcept {
    return mimhid_device_batch_begin(device);
}
[[nodiscard]] inline mimhid_result
device_batch_end(mimhid_device* device, const mimhid_commit_timeouts* timeouts) noexcept {
    return mimhid_device_batch_end(device, timeouts);
}
[[nodiscard]] inline mimhid_result device_remote_wakeup(mimhid_device* device,
                                                        uint32_t control_timeout_ms) noexcept {
    return mimhid_device_remote_wakeup(device, control_timeout_ms);
}
[[nodiscard]] inline mimhid_result device_board_info(const mimhid_device* device,
                                                     mimhid_device_info* out_info) noexcept {
    return mimhid_device_board_info(device, out_info);
}
[[nodiscard]] inline mimhid_result device_limits(const mimhid_device* device,
                                                 mimhid_board_limits* out_limits) noexcept {
    return mimhid_device_limits(device, out_limits);
}
[[nodiscard]] inline mimhid_result
device_set_control_routing(mimhid_device* device, uint32_t report_all,
                           const mimhid_control_rule* rules, size_t rule_count,
                           uint32_t control_timeout_ms) noexcept {
    return mimhid_device_set_control_routing(device, report_all, rules, rule_count,
                                             control_timeout_ms);
}
[[nodiscard]] inline mimhid_result device_set_loss_reporting(mimhid_device* device,
                                                             uint32_t enabled, uint32_t interval_ms,
                                                             uint32_t control_timeout_ms) noexcept {
    return mimhid_device_set_loss_reporting(device, enabled, interval_ms, control_timeout_ms);
}
[[nodiscard]] inline mimhid_result device_control_reply(mimhid_device* device, uint32_t token,
                                                        mimhid_control_action action,
                                                        const uint8_t* data, size_t length,
                                                        uint32_t send_timeout_ms) noexcept {
    return mimhid_device_control_reply(device, token, action, data, length, send_timeout_ms);
}
[[nodiscard]] inline mimhid_result node_set_feature_response(mimhid_node* node, uint8_t report_id,
                                                             const uint8_t* data, size_t length,
                                                             uint32_t control_timeout_ms) noexcept {
    return mimhid_node_set_feature_response(node, report_id, data, length, control_timeout_ms);
}
[[nodiscard]] inline mimhid_result node_set_queue_depth(mimhid_node* node, uint32_t depth,
                                                        uint32_t control_timeout_ms) noexcept {
    return mimhid_node_set_queue_depth(node, depth, control_timeout_ms);
}
[[nodiscard]] inline mimhid_result device_close(mimhid_device* device, uint32_t control_timeout_ms,
                                                uint32_t drain_timeout_ms) noexcept {
    return mimhid_device_close(device, control_timeout_ms, drain_timeout_ms);
}
[[nodiscard]] inline mimhid_result device_latched_error(mimhid_device* device) noexcept {
    return mimhid_device_latched_error(device);
}
[[nodiscard]] inline mimhid_result spec_create_keyboard(const mimhid_keyboard_options* options,
                                                        mimhid_spec** out_spec) noexcept {
    return mimhid_spec_create_keyboard(options, out_spec);
}
[[nodiscard]] inline mimhid_result spec_create_mouse(const mimhid_mouse_options* options,
                                                     mimhid_spec** out_spec) noexcept {
    return mimhid_spec_create_mouse(options, out_spec);
}
[[nodiscard]] inline mimhid_result spec_create_toggle(const mimhid_toggle_options* options,
                                                      mimhid_spec** out_spec) noexcept {
    return mimhid_spec_create_toggle(options, out_spec);
}
[[nodiscard]] inline mimhid_result spec_create_gamepad(const mimhid_gamepad_options* options,
                                                       mimhid_spec** out_spec) noexcept {
    return mimhid_spec_create_gamepad(options, out_spec);
}
[[nodiscard]] inline mimhid_result
spec_create_touchscreen(const mimhid_touchscreen_options* options,
                        mimhid_spec** out_spec) noexcept {
    return mimhid_spec_create_touchscreen(options, out_spec);
}
[[nodiscard]] inline mimhid_result spec_create_touchpad(const mimhid_touchpad_options* options,
                                                        mimhid_spec** out_spec) noexcept {
    return mimhid_spec_create_touchpad(options, out_spec);
}
[[nodiscard]] inline mimhid_result spec_create_pen(const mimhid_pen_options* options,
                                                   mimhid_spec** out_spec) noexcept {
    return mimhid_spec_create_pen(options, out_spec);
}
[[nodiscard]] inline mimhid_result spec_create_digitizer(const mimhid_digitizer_options* options,
                                                         mimhid_spec** out_spec) noexcept {
    return mimhid_spec_create_digitizer(options, out_spec);
}
[[nodiscard]] inline mimhid_result spec_create_raw(const mimhid_raw_options* options,
                                                   mimhid_spec** out_spec) noexcept {
    return mimhid_spec_create_raw(options, out_spec);
}
inline void spec_retain(mimhid_spec* spec) noexcept { mimhid_spec_retain(spec); }
inline void spec_release(mimhid_spec* spec) noexcept { mimhid_spec_release(spec); }
[[nodiscard]] inline mimhid_result spec_descriptor(const mimhid_spec* spec, const uint8_t** bytes,
                                                   size_t* length) noexcept {
    return mimhid_spec_descriptor(spec, bytes, length);
}
[[nodiscard]] inline mimhid_result spec_manifest(const mimhid_spec* spec,
                                                 mimhid_capability_manifest* manifest) noexcept {
    return mimhid_spec_manifest(spec, manifest);
}
[[nodiscard]] inline mimhid_result node_open(mimhid_device* device, mimhid_spec* spec,
                                             const mimhid_node_config* config,
                                             const mimhid_commit_timeouts* timeouts,
                                             mimhid_node** out_node) noexcept {
    return mimhid_node_open(device, spec, config, timeouts, out_node);
}
[[nodiscard]] inline mimhid_result node_open_raw(mimhid_device* device, const uint8_t* descriptor,
                                                 size_t descriptor_length,
                                                 const mimhid_node_config* config,
                                                 const mimhid_commit_timeouts* timeouts,
                                                 mimhid_node** out_node) noexcept {
    return mimhid_node_open_raw(device, descriptor, descriptor_length, config, timeouts, out_node);
}
[[nodiscard]] inline mimhid_result node_close(mimhid_node* node,
                                              const mimhid_commit_timeouts* timeouts,
                                              uint32_t drain_timeout_ms) noexcept {
    return mimhid_node_close(node, timeouts, drain_timeout_ms);
}
[[nodiscard]] inline uint16_t node_id(const mimhid_node* node) noexcept {
    return mimhid_node_id(node);
}
[[nodiscard]] inline mimhid_result node_manifest(const mimhid_node* node,
                                                 mimhid_capability_manifest* manifest) noexcept {
    return mimhid_node_manifest(node, manifest);
}
[[nodiscard]] inline mimhid_result node_submit(mimhid_node* node,
                                               uint32_t send_timeout_ms) noexcept {
    return mimhid_node_submit(node, send_timeout_ms);
}
[[nodiscard]] inline mimhid_result node_submit_blocking(mimhid_node* node,
                                                        uint32_t timeout_ms) noexcept {
    return mimhid_node_submit_blocking(node, timeout_ms);
}
[[nodiscard]] inline mimhid_result keyboard_key(mimhid_node* node, uint16_t usage,
                                                uint32_t down) noexcept {
    return mimhid_keyboard_key(node, usage, down);
}
[[nodiscard]] inline mimhid_result mouse_move(mimhid_node* node, int32_t dx, int32_t dy) noexcept {
    return mimhid_mouse_move(node, dx, dy);
}
[[nodiscard]] inline mimhid_result mouse_scroll(mimhid_node* node, int32_t wheel,
                                                int32_t pan) noexcept {
    return mimhid_mouse_scroll(node, wheel, pan);
}
[[nodiscard]] inline mimhid_result mouse_button(mimhid_node* node, uint32_t button,
                                                uint32_t down) noexcept {
    return mimhid_mouse_button(node, button, down);
}
[[nodiscard]] inline mimhid_result toggle_usage(mimhid_node* node, uint16_t usage,
                                                uint32_t down) noexcept {
    return mimhid_toggle_usage(node, usage, down);
}
[[nodiscard]] inline mimhid_result gamepad_button(mimhid_node* node, uint32_t button,
                                                  uint32_t down) noexcept {
    return mimhid_gamepad_button(node, button, down);
}
[[nodiscard]] inline mimhid_result gamepad_axis(mimhid_node* node, size_t axis_index,
                                                int32_t value) noexcept {
    return mimhid_gamepad_axis(node, axis_index, value);
}
[[nodiscard]] inline mimhid_result gamepad_dpad(mimhid_node* node, uint32_t up, uint32_t down,
                                                uint32_t right, uint32_t left) noexcept {
    return mimhid_gamepad_dpad(node, up, down, right, left);
}
[[nodiscard]] inline mimhid_result
touchscreen_contact(mimhid_node* node, uint32_t contact_id, uint32_t down, int32_t x, int32_t y,
                    const mimhid_touchscreen_extra* extra) noexcept {
    return mimhid_touchscreen_contact(node, contact_id, down, x, y, extra);
}
[[nodiscard]] inline mimhid_result touchpad_contact(mimhid_node* node, uint32_t contact_id,
                                                    uint32_t down, int32_t x, int32_t y,
                                                    const mimhid_touchpad_extra* extra) noexcept {
    return mimhid_touchpad_contact(node, contact_id, down, x, y, extra);
}
[[nodiscard]] inline mimhid_result touchpad_button(mimhid_node* node, uint32_t button,
                                                   uint32_t down) noexcept {
    return mimhid_touchpad_button(node, button, down);
}
[[nodiscard]] inline mimhid_result pen_update(mimhid_node* node,
                                              const mimhid_pen_sample* sample) noexcept {
    return mimhid_pen_update(node, sample);
}
[[nodiscard]] inline mimhid_result
digitizer_update(mimhid_node* node, const mimhid_digitizer_sample* sample) noexcept {
    return mimhid_digitizer_update(node, sample);
}
[[nodiscard]] inline mimhid_result battery_update(mimhid_node* node, int32_t strength) noexcept {
    return mimhid_battery_update(node, strength);
}
[[nodiscard]] inline mimhid_result raw_submit(mimhid_node* node, const uint8_t* report,
                                              size_t length, uint32_t send_timeout_ms) noexcept {
    return mimhid_raw_submit(node, report, length, send_timeout_ms);
}
[[nodiscard]] inline mimhid_result report_send(mimhid_node* node, const uint8_t* report_bytes,
                                               size_t report_length,
                                               uint32_t send_timeout_ms) noexcept {
    return mimhid_report_send(node, report_bytes, report_length, send_timeout_ms);
}

// ---- Initialized options ----------------------------------------------------------------
// Each returns what the C *_init function of the same name fills in:
//   auto config = mimhid::device_config();
[[nodiscard]] inline mimhid_device_config device_config() noexcept {
    mimhid_device_config value;
    mimhid_device_config_init(&value);
    return value;
}
[[nodiscard]] inline mimhid_commit_timeouts commit_timeouts() noexcept {
    mimhid_commit_timeouts value;
    mimhid_commit_timeouts_init(&value);
    return value;
}
[[nodiscard]] inline mimhid_node_config node_config(uint8_t interface_number) noexcept {
    mimhid_node_config value;
    mimhid_node_config_init(&value, interface_number);
    return value;
}
[[nodiscard]] inline mimhid_board_limits board_limits() noexcept {
    mimhid_board_limits value;
    mimhid_board_limits_init(&value);
    return value;
}
[[nodiscard]] inline mimhid_keyboard_options keyboard_options() noexcept {
    mimhid_keyboard_options value;
    mimhid_keyboard_options_init(&value);
    return value;
}
[[nodiscard]] inline mimhid_mouse_options mouse_options() noexcept {
    mimhid_mouse_options value;
    mimhid_mouse_options_init(&value);
    return value;
}
[[nodiscard]] inline mimhid_toggle_options toggle_options() noexcept {
    mimhid_toggle_options value;
    mimhid_toggle_options_init(&value);
    return value;
}
[[nodiscard]] inline mimhid_gamepad_options gamepad_options() noexcept {
    mimhid_gamepad_options value;
    mimhid_gamepad_options_init(&value);
    return value;
}
[[nodiscard]] inline mimhid_touchscreen_options touchscreen_options() noexcept {
    mimhid_touchscreen_options value;
    mimhid_touchscreen_options_init(&value);
    return value;
}
[[nodiscard]] inline mimhid_touchpad_options touchpad_options() noexcept {
    mimhid_touchpad_options value;
    mimhid_touchpad_options_init(&value);
    return value;
}
[[nodiscard]] inline mimhid_pen_options pen_options() noexcept {
    mimhid_pen_options value;
    mimhid_pen_options_init(&value);
    return value;
}
[[nodiscard]] inline mimhid_digitizer_options digitizer_options() noexcept {
    mimhid_digitizer_options value;
    mimhid_digitizer_options_init(&value);
    return value;
}
[[nodiscard]] inline mimhid_raw_options raw_options() noexcept {
    mimhid_raw_options value;
    mimhid_raw_options_init(&value);
    return value;
}
[[nodiscard]] inline mimhid_capability_manifest capability_manifest() noexcept {
    mimhid_capability_manifest value;
    mimhid_capability_manifest_init(&value);
    return value;
}

// ---- Owning handles ---------------------------------------------------------------------
struct SpecRelease {
    void operator()(mimhid_spec* spec) const noexcept { mimhid_spec_release(spec); }
};
struct DiscoveryDestroy {
    void operator()(mimhid_discovery* discovery) const noexcept {
        mimhid_discovery_destroy(discovery);
    }
};
// Holds one reference: mimhid_spec* raw; mimhid::spec_create_mouse(&o, &raw); SpecPtr spec{raw};
using SpecPtr = std::unique_ptr<mimhid_spec, SpecRelease>;
using DiscoveryPtr = std::unique_ptr<mimhid_discovery, DiscoveryDestroy>;

} // namespace mimhid
