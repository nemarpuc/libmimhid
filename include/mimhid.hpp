// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#pragma once

/* Every function of the C ABI in mimhid.h, forwarded unchanged under namespace
 * mimhid without the mimhid_ prefix. Types, arguments, return values and the
 * documentation are those of mimhid.h; this header adds nothing else. */

#include "mimhid.h"

namespace mimhid {

[[nodiscard]] inline const mimhid_error_detail* last_error() noexcept {
    return mimhid_last_error();
}
[[nodiscard]] inline const char* result_name(mimhid_result result) noexcept {
    return mimhid_result_name(result);
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
[[nodiscard]] inline mimhid_result device_set_usb_identity(mimhid_device* device,
                                                           const mimhid_usb_device* usb_device,
                                                           const mimhid_usb_config* usb_config) noexcept {
    return mimhid_device_set_usb_identity(device, usb_device, usb_config);
}
[[nodiscard]] inline mimhid_result device_commit(mimhid_device* device,
                                                 uint32_t reconnect_delay_ms) noexcept {
    return mimhid_device_commit(device, reconnect_delay_ms);
}
[[nodiscard]] inline mimhid_result device_connect(mimhid_device* device) noexcept {
    return mimhid_device_connect(device);
}
[[nodiscard]] inline mimhid_result device_disconnect(mimhid_device* device) noexcept {
    return mimhid_device_disconnect(device);
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
[[nodiscard]] inline mimhid_result device_batch_end(mimhid_device* device) noexcept {
    return mimhid_device_batch_end(device);
}
[[nodiscard]] inline mimhid_result device_remote_wakeup(mimhid_device* device) noexcept {
    return mimhid_device_remote_wakeup(device);
}
[[nodiscard]] inline mimhid_result device_board_info(const mimhid_device* device,
                                                     mimhid_device_info* out_info) noexcept {
    return mimhid_device_board_info(device, out_info);
}
[[nodiscard]] inline mimhid_result device_limits(const mimhid_device* device,
                                                 mimhid_board_limits* out_limits) noexcept {
    return mimhid_device_limits(device, out_limits);
}
[[nodiscard]] inline mimhid_result device_set_control_routing(mimhid_device* device, uint32_t report_all,
                                                              const mimhid_control_rule* rules,
                                                              size_t rule_count) noexcept {
    return mimhid_device_set_control_routing(device, report_all, rules, rule_count);
}
[[nodiscard]] inline mimhid_result device_control_reply(mimhid_device* device, uint32_t token,
                                                        mimhid_control_action action,
                                                        const uint8_t* data, size_t length) noexcept {
    return mimhid_device_control_reply(device, token, action, data, length);
}
[[nodiscard]] inline mimhid_result node_set_feature_response(mimhid_node* node, uint8_t report_id,
                                                             const uint8_t* data,
                                                             size_t length) noexcept {
    return mimhid_node_set_feature_response(node, report_id, data, length);
}
[[nodiscard]] inline mimhid_result device_close(mimhid_device* device) noexcept {
    return mimhid_device_close(device);
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
                                             mimhid_node** out_node) noexcept {
    return mimhid_node_open(device, spec, config, out_node);
}
[[nodiscard]] inline mimhid_result node_open_raw(mimhid_device* device, const uint8_t* descriptor,
                                                 size_t descriptor_length,
                                                 const mimhid_node_config* config,
                                                 mimhid_node** out_node) noexcept {
    return mimhid_node_open_raw(device, descriptor, descriptor_length, config, out_node);
}
[[nodiscard]] inline mimhid_result node_close(mimhid_node* node) noexcept {
    return mimhid_node_close(node);
}
[[nodiscard]] inline uint16_t node_id(const mimhid_node* node) noexcept {
    return mimhid_node_id(node);
}
[[nodiscard]] inline mimhid_result node_manifest(const mimhid_node* node,
                                                 mimhid_capability_manifest* manifest) noexcept {
    return mimhid_node_manifest(node, manifest);
}
[[nodiscard]] inline mimhid_result node_submit(mimhid_node* node) noexcept {
    return mimhid_node_submit(node);
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
                                              size_t length) noexcept {
    return mimhid_raw_submit(node, report, length);
}
[[nodiscard]] inline mimhid_result report_send(mimhid_node* node, const uint8_t* report_bytes,
                                               size_t report_length) noexcept {
    return mimhid_report_send(node, report_bytes, report_length);
}

} // namespace mimhid
