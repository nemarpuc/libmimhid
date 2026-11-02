// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Touchpad profile: same fixed multi-touch contact shape as Touchscreen, plus
 * one physical click button. button_count may be zero for a buttonless
 * clickpad; this example declares one button to exercise mimhid_touchpad_button. */
#include "profile_example.h"

static const char* const k_send_sequence[] = {
    "mimhid_touchpad_contact(node, 0, 1, 500, 500, NULL)   contact id 0, tip down",
    "mimhid_node_submit(node, MIMHID_SEND_TIMEOUT_MS)                     Contact Count and Scan "
    "Time are derived",
    "mimhid_touchpad_button(node, 1, 1)                    physical click button pressed",
    "mimhid_node_submit(node, MIMHID_SEND_TIMEOUT_MS)                     button report",
    "mimhid_touchpad_button(node, 1, 0)                    button released",
    "mimhid_touchpad_contact(node, 0, 0, 500, 500, NULL)   tip up keeps the same stable id",
    "mimhid_node_submit(node, MIMHID_SEND_TIMEOUT_MS)                     closes the frame",
    NULL};

int main(void) {
    mimhid_touchpad_options options;
    const mimhid_integer_field byte_range = {0, 255, 8U, {0, 0, 0, 0, 0}};
    mimhid_spec* spec = NULL;
    mimhid_result result;

    mimhid_touchpad_options_init(&options);
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

    result = mimhid_spec_create_touchpad(&options, &spec);
    return mimhid_example_run("touchpad", MIMHID_PROFILE_TOUCHPAD, result, spec, k_send_sequence);
}
