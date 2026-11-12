// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* Pen profile: a direct-screen digitizer with pressure, tilt, hover, and the
 * HUT Invert selector for the opposite end. Barrel controls are individual
 * Digitizers-page Usage IDs because HUT does not define them as a range. */
#include "profile_example.h"

static const uint16_t k_barrel_usages[] = {0x44U, 0x5AU};

static const char* const k_send_sequence[] = {
    "mimusb_pen_update(node, &sample)   tip, pressure, and in-range stay consistent",
    "mimusb_node_submit(node, send_timeout_ms)  Away state is normalized on the wire",
    "mimusb_pen_update(node, &sample)   in_range 0 takes the pen away; repeat the last X/Y",
    "mimusb_node_submit(node, send_timeout_ms)  an in-range pen/eraser switch departs first", NULL};

int main(void) {
    mimusb_pen_options options;
    const mimusb_integer_field tilt = {-90, 90, 8U, {0, 0, 0, 0, 0}};
    mimusb_spec* spec = NULL;
    mimusb_result result;

    mimusb_pen_options_init(&options);
    options.function.bcd_hid = 0x0111; /* HID 1.11 */
    options.x = mimusb_integer_field_make(0, 32767, 16U);
    options.y = options.x;
    options.enable_pressure = 1U;
    options.pressure = mimusb_integer_field_make(0, 4095, 12U);
    options.enable_hover = 1U;
    options.enable_tilt = 1U;
    options.tilt_x = tilt;
    options.tilt_y = tilt;
    options.barrel_usages = k_barrel_usages;
    options.barrel_usage_count = sizeof k_barrel_usages / sizeof k_barrel_usages[0];
    options.enable_eraser = 1U;

    result = mimusb_spec_create_pen(&options, &spec);
    return mimusb_example_run("pen", MIMUSB_PROFILE_PEN, result, spec, k_send_sequence);
}
