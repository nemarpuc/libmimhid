// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* Proves the public header and exported entry points are usable from C; USB
 * transport behavior is covered by the integration suite. */
#include "../fill_values.h"
#include "mimusb.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* These assertions are part of the ABI gate, not platform-behaviour claims.
 * They remain true even when a consumer enables a short-enum compiler mode. */
_Static_assert(sizeof(mimusb_result) == sizeof(int32_t), "mimusb_result ABI width");
_Static_assert(sizeof(mimusb_event_kind) == sizeof(int32_t), "mimusb_event_kind ABI width");
_Static_assert(sizeof(mimusb_log_level) == sizeof(int32_t), "mimusb_log_level ABI width");
_Static_assert(sizeof(mimusb_profile_kind) == sizeof(int32_t), "mimusb_profile_kind ABI width");
_Static_assert(sizeof(mimusb_axis_role) == sizeof(int32_t), "mimusb_axis_role ABI width");
_Static_assert(sizeof(mimusb_dpad_representation) == sizeof(int32_t),
               "mimusb_dpad_representation ABI width");
_Static_assert(sizeof(mimusb_usage_semantic) == sizeof(int32_t), "mimusb_usage_semantic ABI width");

/* Structures without pointers have the same layout on every supported ABI (LP64, LLP64, ILP32).
 * Members that precede the first pointer have fixed offsets too. */
_Static_assert(sizeof(mimusb_physical_properties) == 20, "physical_properties layout");
_Static_assert(sizeof(mimusb_integer_field) == 32, "integer_field layout");
_Static_assert(sizeof(mimusb_report_id_option) == 8, "report_id_option layout");
_Static_assert(sizeof(mimusb_battery_report) == 40, "battery_report layout");
_Static_assert(sizeof(mimusb_device_info) == 176, "device_info layout");
_Static_assert(offsetof(mimusb_device_info, baud_rate) == 140, "device_info.baud_rate");
_Static_assert(offsetof(mimusb_device_info, unique_id) == 155, "device_info.unique_id");
_Static_assert(offsetof(mimusb_device_info, version_major) == 128, "device_info.version_major");

_Static_assert(offsetof(mimusb_usb_endpoint, address) == 0, "usb_endpoint.address");
_Static_assert(offsetof(mimusb_usb_endpoint, transfer_type) == 1, "usb_endpoint.transfer_type");
_Static_assert(offsetof(mimusb_usb_endpoint, interval) == 2, "usb_endpoint.interval");
_Static_assert(offsetof(mimusb_usb_endpoint, max_packet_size) == 4, "usb_endpoint.max_packet_size");
_Static_assert(offsetof(mimusb_usb_endpoint, audio_form) == 3, "usb_endpoint.audio_form");
_Static_assert(offsetof(mimusb_usb_endpoint, refresh) == 6, "usb_endpoint.refresh");
_Static_assert(offsetof(mimusb_usb_endpoint, synch_address) == 7, "usb_endpoint.synch_address");
_Static_assert(offsetof(mimusb_usb_endpoint, sync_type) == 8, "usb_endpoint.sync_type");
_Static_assert(offsetof(mimusb_usb_endpoint, usage_type) == 9, "usb_endpoint.usage_type");
_Static_assert(offsetof(mimusb_usb_endpoint, policy) == 10, "usb_endpoint.policy");
_Static_assert(sizeof(mimusb_usb_endpoint) == 12, "usb_endpoint layout");
_Static_assert(sizeof(mimusb_usb_item) == 40, "usb_item layout");
_Static_assert(offsetof(mimusb_usb_item, endpoint) == 8, "usb_item.endpoint");
_Static_assert(offsetof(mimusb_usb_item, descriptor) == 24, "usb_item.descriptor");
_Static_assert(sizeof(mimusb_usb_setting) == 16, "usb_setting layout");
_Static_assert(sizeof(mimusb_endpoint_policy) == 2, "endpoint_policy layout");

_Static_assert(sizeof(mimusb_usb_interface) == 32, "usb_interface layout");
_Static_assert(offsetof(mimusb_usb_interface, number) == 0, "usb_interface.number");
_Static_assert(offsetof(mimusb_usb_interface, class_code) == 1, "usb_interface.class_code");
_Static_assert(offsetof(mimusb_usb_interface, name) == 8, "usb_interface.name");
_Static_assert(offsetof(mimusb_usb_interface, settings) == 16, "usb_interface.settings");
_Static_assert(offsetof(mimusb_usb_interface, setting_count) == 24, "usb_interface.setting_count");

_Static_assert(sizeof(mimusb_node_placement) == 40, "node_placement layout");
_Static_assert(offsetof(mimusb_node_placement, first_interface) == 8,
               "node_placement.first_interface");
_Static_assert(offsetof(mimusb_node_placement, name) == 16, "node_placement.name");
_Static_assert(offsetof(mimusb_node_placement, endpoints) == 24, "node_placement.endpoints");
_Static_assert(offsetof(mimusb_node_placement, endpoint_count) == 32,
               "node_placement.endpoint_count");
_Static_assert(sizeof(mimusb_placed_endpoint) == 20, "placed_endpoint layout");
_Static_assert(offsetof(mimusb_placed_endpoint, endpoint) == 8, "placed_endpoint.endpoint");
_Static_assert(sizeof(mimusb_hid_function) == 8, "hid_function layout");
_Static_assert(offsetof(mimusb_hid_function, country_code) == 4, "hid_function.country_code");

_Static_assert(offsetof(mimusb_usb_config, configuration_value) == 0,
               "usb_config.configuration_value");
_Static_assert(offsetof(mimusb_usb_config, bm_attributes) == 1, "usb_config.bm_attributes");
_Static_assert(offsetof(mimusb_usb_config, bmax_power) == 2, "usb_config.bmax_power");

_Static_assert(offsetof(mimusb_usb_device, bcd_usb) == 0, "usb_device.bcd_usb");
_Static_assert(offsetof(mimusb_usb_device, vendor_id) == 6, "usb_device.vendor_id");
_Static_assert(offsetof(mimusb_usb_device, product_id) == 8, "usb_device.product_id");
_Static_assert(offsetof(mimusb_usb_device, bcd_device) == 10, "usb_device.bcd_device");

_Static_assert(offsetof(mimusb_node_config, struct_size) == 0, "node_config.struct_size");
_Static_assert(sizeof(mimusb_node_config) == 56, "node_config layout");
_Static_assert(offsetof(mimusb_device_config, struct_size) == 0, "device_config.struct_size");
_Static_assert(offsetof(mimusb_commit_timeouts, struct_size) == 0, "commit_timeouts.struct_size");
_Static_assert(offsetof(mimusb_commit_timeouts, reserved) == 4, "commit_timeouts.reserved");
_Static_assert(sizeof(mimusb_commit_timeouts) == 16, "commit_timeouts layout");
_Static_assert(offsetof(mimusb_node_config, reserved) == 4, "node_config.reserved");
_Static_assert(offsetof(mimusb_device_config, reserved) == 4, "device_config.reserved");
_Static_assert(offsetof(mimusb_board_limits, reserved) == 4, "board_limits.reserved");

_Static_assert(sizeof(mimusb_linux_hid_policy) == 32, "linux_hid_policy layout");
_Static_assert(offsetof(mimusb_linux_hid_policy, usages) == 16, "linux_hid_policy.usages");
_Static_assert(offsetof(mimusb_event, kind) == 8, "event.kind");
_Static_assert(offsetof(mimusb_event, value) == 16, "event.value");

/* Every factory accepts what its initializer fills in (the Toggle allow-list is the caller's). */
static int initial_specs_build(void) {
    static const uint16_t usages[1] = {0xCD}; /* Play/Pause */
    static const mimusb_usage_semantic semantics[1] = {MIMUSB_USAGE_ONE_SHOT};
    static const char* const types[1] = {"EV_KEY"};
    static const char* const codes[1] = {"KEY_PLAYPAUSE"};
    mimusb_keyboard_options keyboard;
    mimusb_mouse_options mouse;
    mimusb_toggle_options toggle;
    mimusb_gamepad_options gamepad;
    mimusb_touchscreen_options touchscreen;
    mimusb_touchpad_options touchpad;
    mimusb_pen_options pen;
    mimusb_digitizer_options digitizer;
    mimusb_spec* specs[8] = {NULL};
    int failed = 0;
    test_fill_keyboard(&keyboard);
    test_fill_mouse(&mouse);
    test_fill_toggle(&toggle);
    toggle.allowed_usages = usages;
    toggle.allowed_usage_count = 1;
    toggle.usage_semantics = semantics;
    toggle.expected_linux_event_types = types;
    toggle.expected_linux_codes = codes;
    test_fill_gamepad(&gamepad);
    test_fill_touchscreen(&touchscreen);
    test_fill_touchpad(&touchpad);
    test_fill_pen(&pen);
    test_fill_digitizer(&digitizer);
    failed |= mimusb_spec_create_keyboard(&keyboard, &specs[0]) != MIMUSB_OK;
    failed |= mimusb_spec_create_mouse(&mouse, &specs[1]) != MIMUSB_OK;
    failed |= mimusb_spec_create_toggle(&toggle, &specs[2]) != MIMUSB_OK;
    failed |= mimusb_spec_create_gamepad(&gamepad, &specs[3]) != MIMUSB_OK;
    failed |= mimusb_spec_create_touchscreen(&touchscreen, &specs[4]) != MIMUSB_OK;
    failed |= mimusb_spec_create_touchpad(&touchpad, &specs[5]) != MIMUSB_OK;
    failed |= mimusb_spec_create_pen(&pen, &specs[6]) != MIMUSB_OK;
    failed |= mimusb_spec_create_digitizer(&digitizer, &specs[7]) != MIMUSB_OK;
    for (size_t i = 0; i < 8; ++i) {
        mimusb_capability_manifest manifest;
        mimusb_capability_manifest_init(&manifest);
        /* 64 bytes is the report maximum test_fill_device_config sets. */
        failed |= specs[i] == NULL || mimusb_spec_manifest(specs[i], &manifest) != MIMUSB_OK ||
                  manifest.reports[0].wire_length > 64U;
        mimusb_spec_release(specs[i]);
    }
    return failed;
}

/* A required field left zero is refused, never filled in. */
static int unset_fields_refused(void) {
    mimusb_keyboard_options keyboard;
    mimusb_spec* spec = NULL;
    mimusb_device_config config;
    mimusb_device* device = NULL;
    mimusb_discovery* discovery = NULL;
    int failed = 0;
    test_fill_keyboard(&keyboard);
    keyboard.usage_maximum = 0;
    failed |= mimusb_spec_create_keyboard(&keyboard, &spec) == MIMUSB_OK || spec != NULL;
    test_fill_device_config(&config);
    config.usb_device.bcd_usb = 0; /* a required value left zero */
    failed |=
        mimusb_device_open("unused", &config, &device) != MIMUSB_ERR_UNSET_FIELD || device != NULL;
    test_fill_device_config(&config);
    config.usb_device.vendor_id = 0; /* zero stands for a vendor ID nobody set */
    failed |=
        mimusb_device_open("unused", &config, &device) != MIMUSB_ERR_UNSET_FIELD || device != NULL;
    test_fill_device_config(&config);
    config.usb_device.product_id = 0; /* any product ID is valid, zero included: not UNSET_FIELD */
    failed |= mimusb_device_open("unused", &config, &device) == MIMUSB_ERR_UNSET_FIELD;
    mimusb_device_close(device, 1U, 1U);
    device = NULL;
    /* An initialized configuration is all zero, so it is refused: nothing is filled in. */
    mimusb_device_config_init(&config);
    failed |=
        mimusb_device_open("unused", &config, &device) != MIMUSB_ERR_UNSET_FIELD || device != NULL;
    failed |= mimusb_discover(0, &discovery) != MIMUSB_ERR_UNSET_FIELD || discovery != NULL;
    return failed;
}

static int names(void) {
    int failed = 0;
    failed |= strcmp(mimusb_frame_type_name(0x13), "COMMIT") != 0 ||
              mimusb_last_error()->code != MIMUSB_OK;
    failed |= strcmp(mimusb_frame_type_name(0x45), "EP_IN") != 0;
    failed |= strcmp(mimusb_frame_type_name(0x7F), "UNKNOWN") != 0 ||
              mimusb_last_error()->code != MIMUSB_ERR_PARAM;
    failed |= strcmp(mimusb_result_name(MIMUSB_ERR_TIMEOUT), "MIMUSB_ERR_TIMEOUT") != 0;
    failed |= mimusb_version() !=
              ((MIMUSB_VERSION_MAJOR << 24) | (MIMUSB_VERSION_MINOR << 16) | MIMUSB_VERSION_PATCH);
    return failed;
}

/* True when every byte of the structure after struct_size is zero and struct_size is its size. */
static int zeroed_with_size(const void* value, size_t size) {
    const unsigned char* bytes = (const unsigned char*)value;
    uint32_t struct_size = 0;
    memcpy(&struct_size, bytes, sizeof(struct_size));
    if (struct_size != (uint32_t)size)
        return 0;
    for (size_t i = sizeof(struct_size); i < size; ++i) {
        if (bytes[i] != 0)
            return 0;
    }
    return 1;
}

#define INIT_IS_ZERO(type, init)                                                                   \
    do {                                                                                           \
        type value;                                                                                \
        memset(&value, 0xA5, sizeof(value));                                                       \
        init(&value);                                                                              \
        failed |= !zeroed_with_size(&value, sizeof(value));                                        \
    } while (0)

/* Every *_init function zeroes the structure and sets struct_size; none fills in a value. */
static int init_fills_nothing(void) {
    int failed = 0;
    INIT_IS_ZERO(mimusb_device_config, mimusb_device_config_init);
    INIT_IS_ZERO(mimusb_commit_timeouts, mimusb_commit_timeouts_init);
    INIT_IS_ZERO(mimusb_node_config, mimusb_node_config_init);
    INIT_IS_ZERO(mimusb_board_limits, mimusb_board_limits_init);
    INIT_IS_ZERO(mimusb_link_counters, mimusb_link_counters_init);
    INIT_IS_ZERO(mimusb_keyboard_options, mimusb_keyboard_options_init);
    INIT_IS_ZERO(mimusb_mouse_options, mimusb_mouse_options_init);
    INIT_IS_ZERO(mimusb_toggle_options, mimusb_toggle_options_init);
    INIT_IS_ZERO(mimusb_gamepad_options, mimusb_gamepad_options_init);
    INIT_IS_ZERO(mimusb_touchscreen_options, mimusb_touchscreen_options_init);
    INIT_IS_ZERO(mimusb_touchpad_options, mimusb_touchpad_options_init);
    INIT_IS_ZERO(mimusb_pen_options, mimusb_pen_options_init);
    INIT_IS_ZERO(mimusb_digitizer_options, mimusb_digitizer_options_init);
    INIT_IS_ZERO(mimusb_raw_options, mimusb_raw_options_init);
    INIT_IS_ZERO(mimusb_capability_manifest, mimusb_capability_manifest_init);
    return failed;
}

/* A spec cannot be made from an initialized (all zero) profile: its fields are the caller's, and
 * the library refuses what it is not given (MIMUSB_ERR_PARAM or MIMUSB_ERR_UNSET_FIELD). */
static int init_options_are_refused(void) {
    mimusb_mouse_options mouse;
    mimusb_spec* spec = NULL;
    mimusb_mouse_options_init(&mouse);
    return mimusb_spec_create_mouse(&mouse, &spec) == MIMUSB_OK || spec != NULL;
}

int main(void) {
    int failed = 0;
    int result;
#define RUN(check)                                                                                 \
    do {                                                                                           \
        result = check();                                                                          \
        if (result != 0)                                                                           \
            fprintf(stderr, "test_c_abi: %s failed\n", #check);                                    \
        failed |= result;                                                                          \
    } while (0)
    RUN(initial_specs_build);
    RUN(unset_fields_refused);
    RUN(names);
    RUN(init_fills_nothing);
    RUN(init_options_are_refused);
    return failed;
}
