// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Touchscreen profile: fixed multi-touch with three contacts per report and the
 * full optional field set enabled. Azimuth declares degrees through the four
 * HID 1.11 Global physical items rather than assuming a unit. */
#include "profile_example.h"

static const char* const k_send_sequence[] = {
    "mimhid_touchscreen_contact(node, 0, 1, 500, 500, NULL)   contact id 0, tip down",
    "mimhid_node_submit(node, send_timeout_ms)          Contact Count and Scan Time are "
    "derived",
    "mimhid_touchscreen_contact(node, 0, 0, 500, 500, NULL)   tip up keeps the same stable id",
    "mimhid_node_submit(node, send_timeout_ms)          closes the frame", NULL};

int main(void) {
    mimhid_touchscreen_options options;
    const mimhid_integer_field byte_range = {0, 255, 8U, {0, 0, 0, 0, 0}};
    mimhid_spec* spec = NULL;
    mimhid_result result;

    mimhid_touchscreen_options_init(&options);
    options.contact_id = mimhid_integer_field_make(0, 15, 4U);
    options.x = mimhid_integer_field_make(0, 32767, 16U);
    options.y = options.x;
    options.maximum_contacts = 3U;
    options.contacts_per_report = 3U;
    options.contact_count = (mimhid_integer_field){0, 3, 2U, {0, 0, 0, 0, 0}};
    options.enable_pressure = 1U;
    options.pressure = byte_range;
    options.enable_width = 1U;
    options.width = byte_range;
    options.enable_height = 1U;
    options.height = byte_range;
    options.enable_azimuth = 1U;
    options.azimuth = (mimhid_integer_field){0, 36000, 16U, {1U, 0, 360, 0, 0x14U}};
    options.enable_scan_time = 1U;
    options.scan_time = (mimhid_integer_field){0, 65535, 16U, {0, 0, 0, 0, 0}};
    options.scan_time_unit_100us = 1U;

    result = mimhid_spec_create_touchscreen(&options, &spec);
    return mimhid_example_run("touchscreen", MIMHID_PROFILE_TOUCHSCREEN, result, spec,
                              k_send_sequence);
}
