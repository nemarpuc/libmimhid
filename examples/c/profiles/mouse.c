// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Mouse profile as mimhid_mouse_options_init sets it: five buttons, 16-bit relative X and Y, and
 * an 8-bit Wheel and AC Pan. */
#include "profile_example.h"

static const char* const k_send_sequence[] = {
    "mimhid_mouse_move(node, 10, -5)   pending totals are 64-bit and signed",
    "mimhid_mouse_scroll(node, 1, 0)   Wheel and AC Pan are independent",
    "mimhid_mouse_button(node, 1, 1)   buttons count from 1",
    "mimhid_node_submit(node, MIMHID_SEND_TIMEOUT_MS)  only the submitted fragment is consumed",
    NULL};

int main(void) {
    mimhid_mouse_options options;
    mimhid_spec* spec = NULL;
    mimhid_result result;

    mimhid_mouse_options_init(&options);

    result = mimhid_spec_create_mouse(&options, &spec);
    return mimhid_example_run("mouse", MIMHID_PROFILE_MOUSE, result, spec, k_send_sequence);
}
