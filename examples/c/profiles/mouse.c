// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* Mouse profile: five buttons, 16-bit relative X and Y, and an 8-bit Wheel and AC Pan. */
#include "profile_example.h"

static const char* const k_send_sequence[] = {
    "mimusb_mouse_move(node, 10, -5)   pending totals are 64-bit and signed",
    "mimusb_mouse_scroll(node, 1, 0)   Wheel and AC Pan are independent",
    "mimusb_mouse_button(node, 1, 1)   buttons count from 1",
    "mimusb_node_submit(node, send_timeout_ms)  only the submitted fragment is consumed", NULL};

int main(void) {
    mimusb_mouse_options options;
    mimusb_spec* spec = NULL;
    mimusb_result result;

    mimusb_mouse_options_init(&options);
    options.button_count = 5U;
    options.x = mimusb_integer_field_make(-32767, 32767, 16U);
    options.y = options.x;
    options.enable_wheel = 1U;
    options.wheel = mimusb_integer_field_make(-127, 127, 8U);
    options.enable_pan = 1U;
    options.pan = options.wheel;

    result = mimusb_spec_create_mouse(&options, &spec);
    return mimusb_example_run("mouse", MIMUSB_PROFILE_MOUSE, result, spec, k_send_sequence);
}
