// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* Test-only. The *_init functions of mimusb.h zero a structure and set struct_size and nothing
 * else, so every test that needs a structure the library accepts fills the values itself. These
 * helpers fill the values the tests have always used (a bus-powered USB 2.0 device with remote
 * wakeup, 64-byte reports, the Linux HID limits of docs/LIMITS.md, the same options as the
 * profiles of the examples); they are not part of the library and mean nothing for a product. */
#ifndef MIMUSB_TEST_FILL_VALUES_H
#define MIMUSB_TEST_FILL_VALUES_H

#include "mimusb.h"

static inline void test_fill_device_config(mimusb_device_config* config) {
    mimusb_device_config_init(config);
    config->transport.baud_rate = 115200U;
    config->transport.handshake_timeout_ms = 50U;
    config->transport.maximum_report_bytes = 64U;
    config->transport.linux_hid.descriptor_bytes = 4096U;
    config->transport.linux_hid.fields_per_report = 256U;
    config->transport.linux_hid.global_stack_depth = 4U;
    config->transport.linux_hid.report_size_bits = 256U;
    config->transport.linux_hid.usages = 12288U;
    config->transport.linux_hid.report_data_bits = 65528U;
    config->transport.log_level = MIMUSB_LOG_DISABLED;
    config->usb_device.bcd_usb = 0x0200U;
    config->usb_device.vendor_id = 0x1209U;
    config->usb_device.product_id = 0x0001U;
    config->usb_device.ep0_max_packet_size = 64U;
    config->usb_device.bcd_device = 0x0100U;
    config->usb_device.language_id = 0x0409U;
    config->usb_device.manufacturer = "libmimusb";
    config->usb_device.product = "libmimusb";
    config->usb_device.serial_from_chip_id = 1U;
    config->usb_config.configuration_value = 1U;
    config->usb_config.bm_attributes = 0xA0U;
    config->usb_config.bmax_power = 50U;
}

static inline void test_fill_commit_timeouts(mimusb_commit_timeouts* timeouts) {
    mimusb_commit_timeouts_init(timeouts);
    timeouts->control_timeout_ms = 500U;
    timeouts->reconnect_timeout_ms = 3000U;
}

/* A HID interface with bInterfaceNumber interface_number, polled every 1 ms through IN endpoint
 * 0x81 + interface_number with 64-byte packets, and no OUT endpoint. */
static inline void test_fill_node_config(mimusb_hid_node_config* config, uint8_t interface_number) {
    mimusb_hid_node_config_init(config);
    config->interface_number = interface_number;
    config->bcd_hid = 0x0111U;
    config->ep_in.address = (uint8_t)(0x81U + interface_number);
    config->ep_in.transfer_type = MIMUSB_TRANSFER_INTERRUPT;
    config->ep_in.interval = 1U;
    config->ep_in.policy.on_full = MIMUSB_FULL_DROP_NEWEST;
    config->ep_in.policy.depth = 7U;
    config->ep_in.max_packet_size = 64U;
}

static inline void test_fill_keyboard(mimusb_keyboard_options* options) {
    mimusb_keyboard_options_init(options);
    options->usage_minimum = 0x04U;
    options->usage_maximum = 0xDDU;
}

static inline void test_fill_mouse(mimusb_mouse_options* options) {
    mimusb_mouse_options_init(options);
    options->button_count = 5U;
    options->x = mimusb_integer_field_make(-32767, 32767, 16U);
    options->y = options->x;
    options->enable_wheel = 1U;
    options->wheel = mimusb_integer_field_make(-127, 127, 8U);
    options->enable_pan = 1U;
    options->pan = options->wheel;
}

static inline void test_fill_toggle(mimusb_toggle_options* options) {
    mimusb_toggle_options_init(options);
    options->application_page = 0x0CU;
    options->application_usage = 0x01U;
    options->field_page = 0x0CU;
}

static inline void test_fill_gamepad(mimusb_gamepad_options* options) {
    static const mimusb_gamepad_axis_option axes[4] = {
        {MIMUSB_AXIS_X, 0x01U, 0x30U, {-127, 127, 8U, {0U, 0, 0, 0, 0U}}, 0, "ABS_X"},
        {MIMUSB_AXIS_Y, 0x01U, 0x31U, {-127, 127, 8U, {0U, 0, 0, 0, 0U}}, 0, "ABS_Y"},
        {MIMUSB_AXIS_Z, 0x01U, 0x32U, {-127, 127, 8U, {0U, 0, 0, 0, 0U}}, 0, "ABS_Z"},
        {MIMUSB_AXIS_RZ, 0x01U, 0x35U, {-127, 127, 8U, {0U, 0, 0, 0, 0U}}, 0, "ABS_RZ"}};
    mimusb_gamepad_options_init(options);
    options->axes = axes;
    options->axis_count = 4U;
    options->button_count = 12U;
    options->button_usage_minimum = 1U;
    options->dpad_representation = MIMUSB_DPAD_HAT;
    options->hat_logical_minimum = 0;
    options->hat_logical_maximum = 7;
    options->hat_bit_width = 4U;
}

static inline void test_fill_touchscreen(mimusb_touchscreen_options* options) {
    mimusb_touchscreen_options_init(options);
    options->maximum_contacts = 10U;
    options->contacts_per_report = 10U;
    options->contact_id = mimusb_integer_field_make(0, 15, 4U);
    options->x = mimusb_integer_field_make(0, 32767, 16U);
    options->y = options->x;
    options->contact_count = mimusb_integer_field_make(0, 10, 4U);
}

static inline void test_fill_touchpad(mimusb_touchpad_options* options) {
    mimusb_touchpad_options_init(options);
    options->maximum_contacts = 5U;
    options->contacts_per_report = 5U;
    options->contact_id = mimusb_integer_field_make(0, 15, 4U);
    options->x = mimusb_integer_field_make(0, 32767, 16U);
    options->y = options->x;
    options->contact_count = mimusb_integer_field_make(0, 5, 4U);
    options->button_count = 1U;
}

static inline void test_fill_pen(mimusb_pen_options* options) {
    static const uint16_t barrel_usages[1] = {0x44U};
    mimusb_pen_options_init(options);
    options->x = mimusb_integer_field_make(0, 32767, 16U);
    options->y = options->x;
    options->enable_pressure = 1U;
    options->pressure = mimusb_integer_field_make(0, 4095, 12U);
    options->barrel_usages = barrel_usages;
    options->barrel_usage_count = 1U;
    options->enable_hover = 1U;
}

static inline void test_fill_digitizer(mimusb_digitizer_options* options) {
    static const uint16_t barrel_usages[1] = {0x44U};
    mimusb_digitizer_options_init(options);
    options->x = mimusb_integer_field_make(0, 32767, 16U);
    options->y = options->x;
    options->enable_pressure = 1U;
    options->pressure = mimusb_integer_field_make(0, 4095, 12U);
    options->barrel_usages = barrel_usages;
    options->barrel_usage_count = 1U;
    options->enable_hover = 1U;
}

#endif /* MIMUSB_TEST_FILL_VALUES_H */
