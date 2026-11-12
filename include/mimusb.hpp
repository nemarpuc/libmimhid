// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
#pragma once

/* Every function of the C ABI in mimusb.h, forwarded unchanged under namespace mimusb without the
 * mimusb_ prefix; types, arguments, results and documentation are those of mimusb.h. On top of
 * them, at the end of this file: functions that return initialized options, and owning handles for
 * the two objects whose release cannot fail. */

#include "mimusb.h"

#include <memory>

namespace mimusb {

[[nodiscard]] inline const mimusb_error_detail* last_error() noexcept {
    return mimusb_last_error();
}
[[nodiscard]] inline const char* result_name(mimusb_result result) noexcept {
    return mimusb_result_name(result);
}
[[nodiscard]] inline const char* frame_type_name(int32_t frame_type) noexcept {
    return mimusb_frame_type_name(frame_type);
}
[[nodiscard]] inline uint32_t version() noexcept { return mimusb_version(); }
[[nodiscard]] inline size_t discovery_count(const mimusb_discovery* discovery) noexcept {
    return mimusb_discovery_count(discovery);
}
[[nodiscard]] inline const mimusb_device_info* discovery_get(const mimusb_discovery* discovery,
                                                             size_t index) noexcept {
    return mimusb_discovery_get(discovery, index);
}
inline void discovery_destroy(mimusb_discovery* discovery) noexcept {
    mimusb_discovery_destroy(discovery);
}
[[nodiscard]] inline mimusb_result discover(uint32_t timeout_ms,
                                            mimusb_discovery** out_discovery) noexcept {
    return mimusb_discover(timeout_ms, out_discovery);
}
[[nodiscard]] inline mimusb_result device_open(const char* port_path,
                                               const mimusb_device_config* config,
                                               mimusb_device** out_device) noexcept {
    return mimusb_device_open(port_path, config, out_device);
}
[[nodiscard]] inline mimusb_result
device_set_usb_identity(mimusb_device* device, const mimusb_usb_device* usb_device,
                        const mimusb_usb_config* usb_config,
                        const mimusb_commit_timeouts* timeouts) noexcept {
    return mimusb_device_set_usb_identity(device, usb_device, usb_config, timeouts);
}
[[nodiscard]] inline mimusb_result device_commit(mimusb_device* device, uint32_t reconnect_delay_ms,
                                                 const mimusb_commit_timeouts* timeouts) noexcept {
    return mimusb_device_commit(device, reconnect_delay_ms, timeouts);
}
[[nodiscard]] inline mimusb_result device_connect(mimusb_device* device,
                                                  const mimusb_commit_timeouts* timeouts) noexcept {
    return mimusb_device_connect(device, timeouts);
}
[[nodiscard]] inline mimusb_result device_disconnect(mimusb_device* device,
                                                     uint32_t control_timeout_ms) noexcept {
    return mimusb_device_disconnect(device, control_timeout_ms);
}
[[nodiscard]] inline int32_t device_is_ready(const mimusb_device* device) noexcept {
    return mimusb_device_is_ready(device);
}
[[nodiscard]] inline mimusb_result device_wait_ready(mimusb_device* device,
                                                     uint32_t timeout_ms) noexcept {
    return mimusb_device_wait_ready(device, timeout_ms);
}
[[nodiscard]] inline mimusb_result device_batch_begin(mimusb_device* device) noexcept {
    return mimusb_device_batch_begin(device);
}
[[nodiscard]] inline mimusb_result
device_batch_end(mimusb_device* device, const mimusb_commit_timeouts* timeouts) noexcept {
    return mimusb_device_batch_end(device, timeouts);
}
[[nodiscard]] inline mimusb_result device_remote_wakeup(mimusb_device* device,
                                                        uint32_t control_timeout_ms) noexcept {
    return mimusb_device_remote_wakeup(device, control_timeout_ms);
}
[[nodiscard]] inline mimusb_result device_enter_bootloader(mimusb_device* device,
                                                           uint32_t control_timeout_ms) noexcept {
    return mimusb_device_enter_bootloader(device, control_timeout_ms);
}
[[nodiscard]] inline mimusb_result device_board_info(const mimusb_device* device,
                                                     mimusb_device_info* out_info) noexcept {
    return mimusb_device_board_info(device, out_info);
}
[[nodiscard]] inline mimusb_result device_limits(const mimusb_device* device,
                                                 mimusb_board_limits* out_limits) noexcept {
    return mimusb_device_limits(device, out_limits);
}
[[nodiscard]] inline mimusb_result
device_link_counters(mimusb_device* device, bool reset, uint32_t control_timeout_ms,
                     mimusb_link_counters* out_counters) noexcept {
    return mimusb_device_link_counters(device, reset ? 1U : 0U, control_timeout_ms, out_counters);
}
[[nodiscard]] inline mimusb_result device_set_control_rules(mimusb_device* device,
                                                            uint32_t report_all,
                                                            const mimusb_control_rule* rules,
                                                            size_t rule_count,
                                                            uint32_t control_timeout_ms) noexcept {
    return mimusb_device_set_control_rules(device, report_all, rules, rule_count,
                                           control_timeout_ms);
}
[[nodiscard]] inline mimusb_result device_set_rule_data(mimusb_device* device, size_t rule_index,
                                                        const uint8_t* data, size_t length,
                                                        uint32_t control_timeout_ms) noexcept {
    return mimusb_device_set_rule_data(device, rule_index, data, length, control_timeout_ms);
}
[[nodiscard]] inline mimusb_result node_set_rule_data(mimusb_node* node, size_t rule_index,
                                                      const uint8_t* data, size_t length,
                                                      uint32_t control_timeout_ms) noexcept {
    return mimusb_node_set_rule_data(node, rule_index, data, length, control_timeout_ms);
}
[[nodiscard]] inline mimusb_result hid_descriptor_bytes(uint16_t bcd_hid, uint8_t country_code,
                                                        size_t report_descriptor_length,
                                                        uint8_t out[9]) noexcept {
    return mimusb_hid_descriptor_bytes(bcd_hid, country_code, report_descriptor_length, out);
}
[[nodiscard]] inline mimusb_result hid_descriptor_rule(uint8_t interface_number,
                                                       uint8_t descriptor_type,
                                                       const uint8_t* bytes, size_t length,
                                                       mimusb_control_rule* out_rule) noexcept {
    return mimusb_hid_descriptor_rule(interface_number, descriptor_type, bytes, length, out_rule);
}
[[nodiscard]] inline mimusb_result
cdc_acm_descriptors_make(uint16_t bcd_cdc, uint8_t control_interface,
                         uint8_t call_management_capabilities, uint8_t acm_capabilities,
                         mimusb_cdc_acm_descriptors* out) noexcept {
    return mimusb_cdc_acm_descriptors_make(bcd_cdc, control_interface, call_management_capabilities,
                                           acm_capabilities, out);
}
[[nodiscard]] inline mimusb_result cdc_acm_rules(uint8_t control_interface,
                                                 const uint8_t line_coding[7],
                                                 mimusb_control_rule out_rules[3]) noexcept {
    return mimusb_cdc_acm_rules(control_interface, line_coding, out_rules);
}
[[nodiscard]] inline mimusb_result device_set_loss_reporting(mimusb_device* device,
                                                             uint32_t enabled, uint32_t interval_ms,
                                                             uint32_t control_timeout_ms) noexcept {
    return mimusb_device_set_loss_reporting(device, enabled, interval_ms, control_timeout_ms);
}
[[nodiscard]] inline mimusb_result device_control_reply(mimusb_device* device, uint32_t token,
                                                        mimusb_control_action action,
                                                        const uint8_t* data, size_t length,
                                                        uint32_t send_timeout_ms) noexcept {
    return mimusb_device_control_reply(device, token, action, data, length, send_timeout_ms);
}
[[nodiscard]] inline mimusb_result node_set_feature_response(mimusb_node* node, uint8_t report_id,
                                                             const uint8_t* data, size_t length,
                                                             uint32_t control_timeout_ms) noexcept {
    return mimusb_node_set_feature_response(node, report_id, data, length, control_timeout_ms);
}
[[nodiscard]] inline mimusb_result node_endpoint_set_policy(mimusb_node* node, uint8_t address,
                                                            const mimusb_endpoint_policy* policy,
                                                            uint32_t control_timeout_ms) noexcept {
    return mimusb_node_endpoint_set_policy(node, address, policy, control_timeout_ms);
}
[[nodiscard]] inline mimusb_result node_endpoint_write(mimusb_node* node, uint8_t address,
                                                       const uint8_t* data, size_t length,
                                                       uint32_t timeout_ms) noexcept {
    return mimusb_node_endpoint_write(node, address, data, length, timeout_ms);
}
[[nodiscard]] inline mimusb_result node_endpoint_stall(mimusb_node* node, uint8_t address,
                                                       uint32_t control_timeout_ms) noexcept {
    return mimusb_node_endpoint_stall(node, address, control_timeout_ms);
}
[[nodiscard]] inline mimusb_result node_endpoint_clear_stall(mimusb_node* node, uint8_t address,
                                                             uint32_t control_timeout_ms) noexcept {
    return mimusb_node_endpoint_clear_stall(node, address, control_timeout_ms);
}
[[nodiscard]] inline mimusb_result node_endpoint_flush(mimusb_node* node, uint8_t address,
                                                       uint32_t control_timeout_ms) noexcept {
    return mimusb_node_endpoint_flush(node, address, control_timeout_ms);
}
[[nodiscard]] inline mimusb_result device_add_string(mimusb_device* device, const char* utf8,
                                                     uint8_t* out_index) noexcept {
    return mimusb_device_add_string(device, utf8, out_index);
}
[[nodiscard]] inline mimusb_result device_remove_string(mimusb_device* device,
                                                        uint8_t index) noexcept {
    return mimusb_device_remove_string(device, index);
}
[[nodiscard]] inline mimusb_result device_set_string(mimusb_device* device, uint8_t index,
                                                     uint16_t language_id, const uint16_t* units,
                                                     size_t unit_count) noexcept {
    return mimusb_device_set_string(device, index, language_id, units, unit_count);
}
[[nodiscard]] inline mimusb_result device_clear_string(mimusb_device* device, uint8_t index,
                                                       uint16_t language_id) noexcept {
    return mimusb_device_clear_string(device, index, language_id);
}
[[nodiscard]] inline mimusb_result device_set_bos(mimusb_device* device, const uint8_t* bytes,
                                                  size_t length) noexcept {
    return mimusb_device_set_bos(device, bytes, length);
}
[[nodiscard]] inline mimusb_result
device_set_descriptors(mimusb_device* device, const mimusb_device_descriptors* descriptors,
                       const mimusb_commit_timeouts* timeouts) noexcept {
    return mimusb_device_set_descriptors(device, descriptors, timeouts);
}
[[nodiscard]] inline mimusb_result
device_clear_descriptors(mimusb_device* device, const mimusb_commit_timeouts* timeouts) noexcept {
    return mimusb_device_clear_descriptors(device, timeouts);
}
[[nodiscard]] inline mimusb_result
device_endpoint_set_policy(mimusb_device* device, uint8_t address,
                           const mimusb_endpoint_policy* policy,
                           uint32_t control_timeout_ms) noexcept {
    return mimusb_device_endpoint_set_policy(device, address, policy, control_timeout_ms);
}
[[nodiscard]] inline mimusb_result device_endpoint_write(mimusb_device* device, uint8_t address,
                                                         const uint8_t* data, size_t length,
                                                         uint32_t timeout_ms) noexcept {
    return mimusb_device_endpoint_write(device, address, data, length, timeout_ms);
}
[[nodiscard]] inline mimusb_result device_endpoint_stall(mimusb_device* device, uint8_t address,
                                                         uint32_t control_timeout_ms) noexcept {
    return mimusb_device_endpoint_stall(device, address, control_timeout_ms);
}
[[nodiscard]] inline mimusb_result
device_endpoint_clear_stall(mimusb_device* device, uint8_t address,
                            uint32_t control_timeout_ms) noexcept {
    return mimusb_device_endpoint_clear_stall(device, address, control_timeout_ms);
}
[[nodiscard]] inline mimusb_result device_endpoint_flush(mimusb_device* device, uint8_t address,
                                                         uint32_t control_timeout_ms) noexcept {
    return mimusb_device_endpoint_flush(device, address, control_timeout_ms);
}
[[nodiscard]] inline mimusb_result device_close(mimusb_device* device, uint32_t control_timeout_ms,
                                                uint32_t drain_timeout_ms) noexcept {
    return mimusb_device_close(device, control_timeout_ms, drain_timeout_ms);
}
[[nodiscard]] inline mimusb_result device_latched_error(mimusb_device* device) noexcept {
    return mimusb_device_latched_error(device);
}
[[nodiscard]] inline mimusb_result spec_create_keyboard(const mimusb_keyboard_options* options,
                                                        mimusb_spec** out_spec) noexcept {
    return mimusb_spec_create_keyboard(options, out_spec);
}
[[nodiscard]] inline mimusb_result spec_create_mouse(const mimusb_mouse_options* options,
                                                     mimusb_spec** out_spec) noexcept {
    return mimusb_spec_create_mouse(options, out_spec);
}
[[nodiscard]] inline mimusb_result spec_create_toggle(const mimusb_toggle_options* options,
                                                      mimusb_spec** out_spec) noexcept {
    return mimusb_spec_create_toggle(options, out_spec);
}
[[nodiscard]] inline mimusb_result spec_create_gamepad(const mimusb_gamepad_options* options,
                                                       mimusb_spec** out_spec) noexcept {
    return mimusb_spec_create_gamepad(options, out_spec);
}
[[nodiscard]] inline mimusb_result
spec_create_touchscreen(const mimusb_touchscreen_options* options,
                        mimusb_spec** out_spec) noexcept {
    return mimusb_spec_create_touchscreen(options, out_spec);
}
[[nodiscard]] inline mimusb_result spec_create_touchpad(const mimusb_touchpad_options* options,
                                                        mimusb_spec** out_spec) noexcept {
    return mimusb_spec_create_touchpad(options, out_spec);
}
[[nodiscard]] inline mimusb_result spec_create_pen(const mimusb_pen_options* options,
                                                   mimusb_spec** out_spec) noexcept {
    return mimusb_spec_create_pen(options, out_spec);
}
[[nodiscard]] inline mimusb_result spec_create_digitizer(const mimusb_digitizer_options* options,
                                                         mimusb_spec** out_spec) noexcept {
    return mimusb_spec_create_digitizer(options, out_spec);
}
[[nodiscard]] inline mimusb_result spec_create_raw(const mimusb_raw_options* options,
                                                   mimusb_spec** out_spec) noexcept {
    return mimusb_spec_create_raw(options, out_spec);
}
inline void spec_retain(mimusb_spec* spec) noexcept { mimusb_spec_retain(spec); }
inline void spec_release(mimusb_spec* spec) noexcept { mimusb_spec_release(spec); }
[[nodiscard]] inline mimusb_result spec_descriptor(const mimusb_spec* spec, const uint8_t** bytes,
                                                   size_t* length) noexcept {
    return mimusb_spec_descriptor(spec, bytes, length);
}
[[nodiscard]] inline mimusb_result spec_manifest(const mimusb_spec* spec,
                                                 mimusb_capability_manifest* manifest) noexcept {
    return mimusb_spec_manifest(spec, manifest);
}
[[nodiscard]] inline mimusb_result node_open(mimusb_device* device,
                                             const mimusb_node_config* config,
                                             const mimusb_commit_timeouts* timeouts,
                                             mimusb_node** out_node) noexcept {
    return mimusb_node_open(device, config, timeouts, out_node);
}
[[nodiscard]] inline mimusb_result node_open_profile(mimusb_device* device, mimusb_spec* spec,
                                                     const mimusb_hid_node_config* config,
                                                     const mimusb_commit_timeouts* timeouts,
                                                     mimusb_node** out_node) noexcept {
    return mimusb_node_open_profile(device, spec, config, timeouts, out_node);
}
[[nodiscard]] inline mimusb_result node_close(mimusb_node* node,
                                              const mimusb_commit_timeouts* timeouts,
                                              uint32_t drain_timeout_ms) noexcept {
    return mimusb_node_close(node, timeouts, drain_timeout_ms);
}
[[nodiscard]] inline uint16_t node_id(const mimusb_node* node) noexcept {
    return mimusb_node_id(node);
}
[[nodiscard]] inline mimusb_result node_manifest(const mimusb_node* node,
                                                 mimusb_capability_manifest* manifest) noexcept {
    return mimusb_node_manifest(node, manifest);
}
[[nodiscard]] inline mimusb_result node_submit(mimusb_node* node,
                                               uint32_t send_timeout_ms) noexcept {
    return mimusb_node_submit(node, send_timeout_ms);
}
[[nodiscard]] inline mimusb_result node_submit_blocking(mimusb_node* node,
                                                        uint32_t timeout_ms) noexcept {
    return mimusb_node_submit_blocking(node, timeout_ms);
}
[[nodiscard]] inline mimusb_result keyboard_key(mimusb_node* node, uint16_t usage,
                                                uint32_t down) noexcept {
    return mimusb_keyboard_key(node, usage, down);
}
[[nodiscard]] inline mimusb_result mouse_move(mimusb_node* node, int32_t dx, int32_t dy) noexcept {
    return mimusb_mouse_move(node, dx, dy);
}
[[nodiscard]] inline mimusb_result mouse_scroll(mimusb_node* node, int32_t wheel,
                                                int32_t pan) noexcept {
    return mimusb_mouse_scroll(node, wheel, pan);
}
[[nodiscard]] inline mimusb_result mouse_button(mimusb_node* node, uint32_t button,
                                                uint32_t down) noexcept {
    return mimusb_mouse_button(node, button, down);
}
[[nodiscard]] inline mimusb_result toggle_usage(mimusb_node* node, uint16_t usage,
                                                uint32_t down) noexcept {
    return mimusb_toggle_usage(node, usage, down);
}
[[nodiscard]] inline mimusb_result gamepad_button(mimusb_node* node, uint32_t button,
                                                  uint32_t down) noexcept {
    return mimusb_gamepad_button(node, button, down);
}
[[nodiscard]] inline mimusb_result gamepad_axis(mimusb_node* node, size_t axis_index,
                                                int32_t value) noexcept {
    return mimusb_gamepad_axis(node, axis_index, value);
}
[[nodiscard]] inline mimusb_result gamepad_dpad(mimusb_node* node, uint32_t up, uint32_t down,
                                                uint32_t right, uint32_t left) noexcept {
    return mimusb_gamepad_dpad(node, up, down, right, left);
}
[[nodiscard]] inline mimusb_result
touchscreen_contact(mimusb_node* node, uint32_t contact_id, uint32_t down, int32_t x, int32_t y,
                    const mimusb_touchscreen_extra* extra) noexcept {
    return mimusb_touchscreen_contact(node, contact_id, down, x, y, extra);
}
[[nodiscard]] inline mimusb_result touchpad_contact(mimusb_node* node, uint32_t contact_id,
                                                    uint32_t down, int32_t x, int32_t y,
                                                    const mimusb_touchpad_extra* extra) noexcept {
    return mimusb_touchpad_contact(node, contact_id, down, x, y, extra);
}
[[nodiscard]] inline mimusb_result touchpad_button(mimusb_node* node, uint32_t button,
                                                   uint32_t down) noexcept {
    return mimusb_touchpad_button(node, button, down);
}
[[nodiscard]] inline mimusb_result pen_update(mimusb_node* node,
                                              const mimusb_pen_sample* sample) noexcept {
    return mimusb_pen_update(node, sample);
}
[[nodiscard]] inline mimusb_result
digitizer_update(mimusb_node* node, const mimusb_digitizer_sample* sample) noexcept {
    return mimusb_digitizer_update(node, sample);
}
[[nodiscard]] inline mimusb_result battery_update(mimusb_node* node, int32_t strength) noexcept {
    return mimusb_battery_update(node, strength);
}
[[nodiscard]] inline mimusb_result raw_submit(mimusb_node* node, const uint8_t* report,
                                              size_t length, uint32_t send_timeout_ms) noexcept {
    return mimusb_raw_submit(node, report, length, send_timeout_ms);
}

// ---- Initialized options ----------------------------------------------------------------
// Each returns a zeroed structure with struct_size set, as the C *_init function of the same name
// does:
//   auto config = mimusb::device_config();
[[nodiscard]] inline mimusb_device_config device_config() noexcept {
    mimusb_device_config value;
    mimusb_device_config_init(&value);
    return value;
}
[[nodiscard]] inline mimusb_commit_timeouts commit_timeouts() noexcept {
    mimusb_commit_timeouts value;
    mimusb_commit_timeouts_init(&value);
    return value;
}
[[nodiscard]] inline mimusb_node_config node_config() noexcept {
    mimusb_node_config value;
    mimusb_node_config_init(&value);
    return value;
}
[[nodiscard]] inline mimusb_hid_node_config hid_node_config() noexcept {
    mimusb_hid_node_config value;
    mimusb_hid_node_config_init(&value);
    return value;
}
[[nodiscard]] inline mimusb_board_limits board_limits() noexcept {
    mimusb_board_limits value;
    mimusb_board_limits_init(&value);
    return value;
}
[[nodiscard]] inline mimusb_link_counters link_counters() noexcept {
    mimusb_link_counters value;
    mimusb_link_counters_init(&value);
    return value;
}
[[nodiscard]] inline mimusb_keyboard_options keyboard_options() noexcept {
    mimusb_keyboard_options value;
    mimusb_keyboard_options_init(&value);
    return value;
}
[[nodiscard]] inline mimusb_mouse_options mouse_options() noexcept {
    mimusb_mouse_options value;
    mimusb_mouse_options_init(&value);
    return value;
}
[[nodiscard]] inline mimusb_toggle_options toggle_options() noexcept {
    mimusb_toggle_options value;
    mimusb_toggle_options_init(&value);
    return value;
}
[[nodiscard]] inline mimusb_gamepad_options gamepad_options() noexcept {
    mimusb_gamepad_options value;
    mimusb_gamepad_options_init(&value);
    return value;
}
[[nodiscard]] inline mimusb_touchscreen_options touchscreen_options() noexcept {
    mimusb_touchscreen_options value;
    mimusb_touchscreen_options_init(&value);
    return value;
}
[[nodiscard]] inline mimusb_touchpad_options touchpad_options() noexcept {
    mimusb_touchpad_options value;
    mimusb_touchpad_options_init(&value);
    return value;
}
[[nodiscard]] inline mimusb_pen_options pen_options() noexcept {
    mimusb_pen_options value;
    mimusb_pen_options_init(&value);
    return value;
}
[[nodiscard]] inline mimusb_digitizer_options digitizer_options() noexcept {
    mimusb_digitizer_options value;
    mimusb_digitizer_options_init(&value);
    return value;
}
[[nodiscard]] inline mimusb_raw_options raw_options() noexcept {
    mimusb_raw_options value;
    mimusb_raw_options_init(&value);
    return value;
}
[[nodiscard]] inline mimusb_capability_manifest capability_manifest() noexcept {
    mimusb_capability_manifest value;
    mimusb_capability_manifest_init(&value);
    return value;
}

// ---- Owning handles ---------------------------------------------------------------------
struct SpecRelease {
    void operator()(mimusb_spec* spec) const noexcept { mimusb_spec_release(spec); }
};
struct DiscoveryDestroy {
    void operator()(mimusb_discovery* discovery) const noexcept {
        mimusb_discovery_destroy(discovery);
    }
};
// Holds one reference: mimusb_spec* raw; mimusb::spec_create_mouse(&o, &raw); SpecPtr spec{raw};
using SpecPtr = std::unique_ptr<mimusb_spec, SpecRelease>;
using DiscoveryPtr = std::unique_ptr<mimusb_discovery, DiscoveryDestroy>;

} // namespace mimusb
