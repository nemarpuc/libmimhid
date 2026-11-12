// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Proves the public header and exported entry points are usable from C; USB
 * transport behavior is covered by the integration suite. */
#include "../fill_values.h"
#include "mimhid.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* These assertions are part of the ABI gate, not platform-behaviour claims.
 * They remain true even when a consumer enables a short-enum compiler mode. */
_Static_assert(sizeof(mimhid_result) == sizeof(int32_t), "mimhid_result ABI width");
_Static_assert(sizeof(mimhid_event_kind) == sizeof(int32_t), "mimhid_event_kind ABI width");
_Static_assert(sizeof(mimhid_log_level) == sizeof(int32_t), "mimhid_log_level ABI width");
_Static_assert(sizeof(mimhid_profile_kind) == sizeof(int32_t), "mimhid_profile_kind ABI width");
_Static_assert(sizeof(mimhid_axis_role) == sizeof(int32_t), "mimhid_axis_role ABI width");
_Static_assert(sizeof(mimhid_dpad_representation) == sizeof(int32_t),
               "mimhid_dpad_representation ABI width");
_Static_assert(sizeof(mimhid_usage_semantic) == sizeof(int32_t), "mimhid_usage_semantic ABI width");

/* Structures without pointers have the same layout on every supported ABI (LP64, LLP64, ILP32).
 * Members that precede the first pointer have fixed offsets too. */
_Static_assert(sizeof(mimhid_physical_properties) == 20, "physical_properties layout");
_Static_assert(sizeof(mimhid_integer_field) == 32, "integer_field layout");
_Static_assert(sizeof(mimhid_report_id_option) == 8, "report_id_option layout");
_Static_assert(sizeof(mimhid_battery_report) == 40, "battery_report layout");
_Static_assert(sizeof(mimhid_device_info) == 176, "device_info layout");
_Static_assert(offsetof(mimhid_device_info, baud_rate) == 140, "device_info.baud_rate");
_Static_assert(offsetof(mimhid_device_info, unique_id) == 155, "device_info.unique_id");
_Static_assert(offsetof(mimhid_device_info, version_major) == 128, "device_info.version_major");

_Static_assert(sizeof(mimhid_usb_endpoint) == 10, "usb_endpoint layout");
_Static_assert(offsetof(mimhid_usb_endpoint, address) == 0, "usb_endpoint.address");
_Static_assert(offsetof(mimhid_usb_endpoint, transfer_type) == 1, "usb_endpoint.transfer_type");
_Static_assert(offsetof(mimhid_usb_endpoint, interval) == 2, "usb_endpoint.interval");
_Static_assert(offsetof(mimhid_usb_endpoint, max_packet_size) == 4, "usb_endpoint.max_packet_size");
_Static_assert(offsetof(mimhid_usb_endpoint, policy) == 8, "usb_endpoint.policy");
_Static_assert(sizeof(mimhid_endpoint_policy) == 2, "endpoint_policy layout");

_Static_assert(sizeof(mimhid_usb_interface) == 48, "usb_interface layout");
_Static_assert(offsetof(mimhid_usb_interface, number) == 0, "usb_interface.number");
_Static_assert(offsetof(mimhid_usb_interface, class_code) == 1, "usb_interface.class_code");
_Static_assert(offsetof(mimhid_usb_interface, name) == 8, "usb_interface.name");
_Static_assert(offsetof(mimhid_usb_interface, endpoints) == 32, "usb_interface.endpoints");

_Static_assert(sizeof(mimhid_hid_node_config) == 48, "hid_node_config layout");
_Static_assert(offsetof(mimhid_hid_node_config, bcd_hid) == 12, "hid_node_config.bcd_hid");
_Static_assert(offsetof(mimhid_hid_node_config, ep_in) == 24, "hid_node_config.ep_in");

_Static_assert(offsetof(mimhid_usb_config, configuration_value) == 0,
               "usb_config.configuration_value");
_Static_assert(offsetof(mimhid_usb_config, bm_attributes) == 1, "usb_config.bm_attributes");
_Static_assert(offsetof(mimhid_usb_config, bmax_power) == 2, "usb_config.bmax_power");

_Static_assert(offsetof(mimhid_usb_device, bcd_usb) == 0, "usb_device.bcd_usb");
_Static_assert(offsetof(mimhid_usb_device, vendor_id) == 6, "usb_device.vendor_id");
_Static_assert(offsetof(mimhid_usb_device, product_id) == 8, "usb_device.product_id");
_Static_assert(offsetof(mimhid_usb_device, bcd_device) == 10, "usb_device.bcd_device");

_Static_assert(offsetof(mimhid_node_config, struct_size) == 0, "node_config.struct_size");
_Static_assert(sizeof(mimhid_node_config) == 56, "node_config layout");
_Static_assert(offsetof(mimhid_device_config, struct_size) == 0, "device_config.struct_size");
_Static_assert(offsetof(mimhid_commit_timeouts, struct_size) == 0, "commit_timeouts.struct_size");
_Static_assert(offsetof(mimhid_commit_timeouts, reserved) == 4, "commit_timeouts.reserved");
_Static_assert(sizeof(mimhid_commit_timeouts) == 16, "commit_timeouts layout");
_Static_assert(offsetof(mimhid_node_config, reserved) == 4, "node_config.reserved");
_Static_assert(offsetof(mimhid_device_config, reserved) == 4, "device_config.reserved");
_Static_assert(offsetof(mimhid_board_limits, reserved) == 4, "board_limits.reserved");

_Static_assert(sizeof(mimhid_linux_hid_policy) == 32, "linux_hid_policy layout");
_Static_assert(offsetof(mimhid_linux_hid_policy, usages) == 16, "linux_hid_policy.usages");
_Static_assert(offsetof(mimhid_event, kind) == 8, "event.kind");
_Static_assert(offsetof(mimhid_event, value) == 16, "event.value");

/* Every factory accepts what its initializer fills in (the Toggle allow-list is the caller's). */
static int initial_specs_build(void) {
    static const uint16_t usages[1] = {0xCD}; /* Play/Pause */
    static const mimhid_usage_semantic semantics[1] = {MIMHID_USAGE_ONE_SHOT};
    static const char* const types[1] = {"EV_KEY"};
    static const char* const codes[1] = {"KEY_PLAYPAUSE"};
    mimhid_keyboard_options keyboard;
    mimhid_mouse_options mouse;
    mimhid_toggle_options toggle;
    mimhid_gamepad_options gamepad;
    mimhid_touchscreen_options touchscreen;
    mimhid_touchpad_options touchpad;
    mimhid_pen_options pen;
    mimhid_digitizer_options digitizer;
    mimhid_spec* specs[8] = {NULL};
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
    failed |= mimhid_spec_create_keyboard(&keyboard, &specs[0]) != MIMHID_OK;
    failed |= mimhid_spec_create_mouse(&mouse, &specs[1]) != MIMHID_OK;
    failed |= mimhid_spec_create_toggle(&toggle, &specs[2]) != MIMHID_OK;
    failed |= mimhid_spec_create_gamepad(&gamepad, &specs[3]) != MIMHID_OK;
    failed |= mimhid_spec_create_touchscreen(&touchscreen, &specs[4]) != MIMHID_OK;
    failed |= mimhid_spec_create_touchpad(&touchpad, &specs[5]) != MIMHID_OK;
    failed |= mimhid_spec_create_pen(&pen, &specs[6]) != MIMHID_OK;
    failed |= mimhid_spec_create_digitizer(&digitizer, &specs[7]) != MIMHID_OK;
    for (size_t i = 0; i < 8; ++i) {
        mimhid_capability_manifest manifest;
        mimhid_capability_manifest_init(&manifest);
        /* 64 bytes is the report maximum test_fill_device_config sets. */
        failed |= specs[i] == NULL || mimhid_spec_manifest(specs[i], &manifest) != MIMHID_OK ||
                  manifest.reports[0].wire_length > 64U;
        mimhid_spec_release(specs[i]);
    }
    return failed;
}

/* A required field left zero is refused, never filled in. */
static int unset_fields_refused(void) {
    mimhid_keyboard_options keyboard;
    mimhid_spec* spec = NULL;
    mimhid_device_config config;
    mimhid_device* device = NULL;
    mimhid_discovery* discovery = NULL;
    int failed = 0;
    test_fill_keyboard(&keyboard);
    keyboard.usage_maximum = 0;
    failed |= mimhid_spec_create_keyboard(&keyboard, &spec) == MIMHID_OK || spec != NULL;
    test_fill_device_config(&config);
    config.usb_device.bcd_usb = 0; /* a required value left zero */
    failed |=
        mimhid_device_open("unused", &config, &device) != MIMHID_ERR_UNSET_FIELD || device != NULL;
    test_fill_device_config(&config);
    config.usb_device.vendor_id = 0; /* zero stands for a vendor ID nobody set */
    failed |=
        mimhid_device_open("unused", &config, &device) != MIMHID_ERR_UNSET_FIELD || device != NULL;
    test_fill_device_config(&config);
    config.usb_device.product_id = 0; /* any product ID is valid, zero included: not UNSET_FIELD */
    failed |= mimhid_device_open("unused", &config, &device) == MIMHID_ERR_UNSET_FIELD;
    mimhid_device_close(device, 1U, 1U);
    device = NULL;
    /* An initialized configuration is all zero, so it is refused: nothing is filled in. */
    mimhid_device_config_init(&config);
    failed |=
        mimhid_device_open("unused", &config, &device) != MIMHID_ERR_UNSET_FIELD || device != NULL;
    failed |= mimhid_discover(0, &discovery) != MIMHID_ERR_UNSET_FIELD || discovery != NULL;
    return failed;
}

static int names(void) {
    int failed = 0;
    failed |= strcmp(mimhid_frame_type_name(0x13), "COMMIT") != 0 ||
              mimhid_last_error()->code != MIMHID_OK;
    failed |= strcmp(mimhid_frame_type_name(0x45), "EP_IN") != 0;
    failed |= strcmp(mimhid_frame_type_name(0x7F), "UNKNOWN") != 0 ||
              mimhid_last_error()->code != MIMHID_ERR_PARAM;
    failed |= strcmp(mimhid_result_name(MIMHID_ERR_TIMEOUT), "MIMHID_ERR_TIMEOUT") != 0;
    failed |= mimhid_version() !=
              ((MIMHID_VERSION_MAJOR << 24) | (MIMHID_VERSION_MINOR << 16) | MIMHID_VERSION_PATCH);
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
    INIT_IS_ZERO(mimhid_device_config, mimhid_device_config_init);
    INIT_IS_ZERO(mimhid_commit_timeouts, mimhid_commit_timeouts_init);
    INIT_IS_ZERO(mimhid_node_config, mimhid_node_config_init);
    INIT_IS_ZERO(mimhid_board_limits, mimhid_board_limits_init);
    INIT_IS_ZERO(mimhid_link_counters, mimhid_link_counters_init);
    INIT_IS_ZERO(mimhid_keyboard_options, mimhid_keyboard_options_init);
    INIT_IS_ZERO(mimhid_mouse_options, mimhid_mouse_options_init);
    INIT_IS_ZERO(mimhid_toggle_options, mimhid_toggle_options_init);
    INIT_IS_ZERO(mimhid_gamepad_options, mimhid_gamepad_options_init);
    INIT_IS_ZERO(mimhid_touchscreen_options, mimhid_touchscreen_options_init);
    INIT_IS_ZERO(mimhid_touchpad_options, mimhid_touchpad_options_init);
    INIT_IS_ZERO(mimhid_pen_options, mimhid_pen_options_init);
    INIT_IS_ZERO(mimhid_digitizer_options, mimhid_digitizer_options_init);
    INIT_IS_ZERO(mimhid_raw_options, mimhid_raw_options_init);
    INIT_IS_ZERO(mimhid_capability_manifest, mimhid_capability_manifest_init);
    return failed;
}

/* A spec cannot be made from an initialized (all zero) profile: its fields are the caller's, and
 * the library refuses what it is not given (MIMHID_ERR_PARAM or MIMHID_ERR_UNSET_FIELD). */
static int init_options_are_refused(void) {
    mimhid_mouse_options mouse;
    mimhid_spec* spec = NULL;
    mimhid_mouse_options_init(&mouse);
    return mimhid_spec_create_mouse(&mouse, &spec) == MIMHID_OK || spec != NULL;
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
