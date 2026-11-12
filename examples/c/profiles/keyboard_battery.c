// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* Battery report on a profile: the keyboard of keyboard.c with a second Input
 * report, Battery Strength, in the same Application Collection. Linux
 * registers a battery only with an input device, so there is no battery
 * profile of its own; every generated profile takes the same two fields. */
#include "profile_example.h"

static const char* const k_send_sequence[] = {
    "mimusb_battery_update(node, 80)   set the level; nothing is sent yet",
    "mimusb_node_submit(node, send_timeout_ms)  no key is pending, so this sends the "
    "battery report",
    "mimusb_keyboard_key(node, 0x04, 1) + mimusb_battery_update(node, 79)",
    "mimusb_node_submit(node, send_timeout_ms)  the key report goes first",
    "mimusb_node_submit(node, send_timeout_ms)  then the battery report; the same level "
    "again sends "
    "nothing",
    NULL};

int main(void) {
    mimusb_keyboard_options options;
    mimusb_spec* spec = NULL;
    mimusb_result result;

    mimusb_keyboard_options_init(&options);
    options.function.bcd_hid = 0x0111; /* HID 1.11 */
    options.usage_minimum = 0x04U;
    options.usage_maximum = 0xDDU;
    /* Both reports need a Report ID, and the two must differ. */
    options.report_id.enabled = 1U;
    options.report_id.value = 1U;
    options.enable_battery = 1U;
    options.battery.report_id.enabled = 1U;
    options.battery.report_id.value = 2U;
    /* 1..100 rather than 0..100: Linux up to v6.12 ignores a raw value of 0. */
    options.battery.strength.logical_minimum = 1;
    options.battery.strength.logical_maximum = 100;
    options.battery.strength.bit_width = 8U;

    result = mimusb_spec_create_keyboard(&options, &spec);
    return mimusb_example_run("keyboard_battery", MIMUSB_PROFILE_KEYBOARD, result, spec,
                              k_send_sequence);
}
