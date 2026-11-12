// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* Gamepad profile: two 16-bit axes, twelve buttons from Button 1, and a Hat Switch (logical 0 to 7
 * in four bits).
 */
#include "profile_example.h"

static const mimusb_integer_field k_stick = {-32767, 32767, 16U, {0, 0, 0, 0, 0}};

static const char* const k_send_sequence[] = {
    "mimusb_gamepad_axis(node, 0, 0)   axis index 0, caller-declared range",
    "mimusb_gamepad_dpad(node, 1, 0, 0, 0)         booleans become a canonical Hat value",
    "mimusb_gamepad_button(node, 1, 1)     buttons count from 1",
    "mimusb_node_submit(node, send_timeout_ms)     opposite D-pad pairs are rejected", NULL};

int main(void) {
    const mimusb_gamepad_axis_option axes[] = {{MIMUSB_AXIS_X, 0x01U, 0x30U, k_stick, 0, "ABS_X"},
                                               {MIMUSB_AXIS_Y, 0x01U, 0x31U, k_stick, 0, "ABS_Y"}};
    mimusb_gamepad_options options;
    mimusb_spec* spec = NULL;
    mimusb_result result;

    mimusb_gamepad_options_init(&options);
    options.function.bcd_hid = 0x0111; /* HID 1.11 */
    options.axes = axes;
    options.axis_count = sizeof axes / sizeof axes[0];
    options.button_count = 12U;
    options.button_usage_minimum = 1U;
    options.dpad_representation = MIMUSB_DPAD_HAT;
    options.hat_logical_minimum = 0;
    options.hat_logical_maximum = 7;
    options.hat_bit_width = 4U;

    result = mimusb_spec_create_gamepad(&options, &spec);
    return mimusb_example_run("gamepad", MIMUSB_PROFILE_GAMEPAD, result, spec, k_send_sequence);
}
