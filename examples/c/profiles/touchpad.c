// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* Touchpad profile: same fixed multi-touch contact shape as Touchscreen, plus
 * one physical click button. button_count may be zero for a buttonless
 * clickpad; this example declares one button to exercise mimusb_touchpad_button. */
#include "profile_example.h"

static const char* const k_send_sequence[] = {
    "mimusb_touchpad_contact(node, 0, 1, 500, 500, NULL)   contact id 0, tip down",
    "mimusb_node_submit(node, send_timeout_ms)                     Contact Count and Scan "
    "Time are derived",
    "mimusb_touchpad_button(node, 1, 1)                    physical click button pressed",
    "mimusb_node_submit(node, send_timeout_ms)                     button report",
    "mimusb_touchpad_button(node, 1, 0)                    button released",
    "mimusb_touchpad_contact(node, 0, 0, 500, 500, NULL)   tip up keeps the same stable id",
    "mimusb_node_submit(node, send_timeout_ms)                     closes the frame",
    NULL};

int main(void) {
    mimusb_touchpad_options options;
    const mimusb_integer_field byte_range = {0, 255, 8U, {0, 0, 0, 0, 0}};
    mimusb_spec* spec = NULL;
    mimusb_result result;

    mimusb_touchpad_options_init(&options);
    options.function.bcd_hid = 0x0111; /* HID 1.11 */
    options.contact_id = mimusb_integer_field_make(0, 15, 4U);
    options.x = mimusb_integer_field_make(0, 32767, 16U);
    options.y = options.x;
    options.maximum_contacts = 3U;
    options.contacts_per_report = 3U;
    options.contact_count = (mimusb_integer_field){0, 3, 2U, {0, 0, 0, 0, 0}};
    options.enable_pressure = 1U;
    options.pressure = byte_range;
    options.enable_width = 1U;
    options.width = byte_range;
    options.enable_height = 1U;
    options.height = byte_range;
    options.enable_azimuth = 1U;
    options.azimuth = (mimusb_integer_field){0, 36000, 16U, {1U, 0, 360, 0, 0x14U}};
    options.enable_scan_time = 1U;
    options.scan_time = (mimusb_integer_field){0, 65535, 16U, {0, 0, 0, 0, 0}};
    options.scan_time_unit_100us = 1U;
    options.button_count = 1U;

    result = mimusb_spec_create_touchpad(&options, &spec);
    return mimusb_example_run("touchpad", MIMUSB_PROFILE_TOUCHPAD, result, spec, k_send_sequence);
}
