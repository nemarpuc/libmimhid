// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Battery report on a profile: the keyboard of keyboard.c with a second Input
 * report, Battery Strength, in the same Application Collection. Linux
 * registers a battery only with an input device, so there is no battery
 * profile of its own; every generated profile takes the same two fields. */
#include "profile_example.h"

static const char* const k_send_sequence[] = {
    "mimhid_battery_update(node, 80)   set the level; nothing is sent yet",
    "mimhid_node_submit(node, MIMHID_SEND_TIMEOUT_MS)  no key is pending, so this sends the "
    "battery report",
    "mimhid_keyboard_key(node, 0x04, 1) + mimhid_battery_update(node, 79)",
    "mimhid_node_submit(node, MIMHID_SEND_TIMEOUT_MS)  the key report goes first",
    "mimhid_node_submit(node, MIMHID_SEND_TIMEOUT_MS)  then the battery report; the same level "
    "again sends "
    "nothing",
    NULL};

int main(void) {
    mimhid_keyboard_options options;
    mimhid_spec* spec = NULL;
    mimhid_result result;

    mimhid_keyboard_options_init(&options);
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

    result = mimhid_spec_create_keyboard(&options, &spec);
    return mimhid_example_run("keyboard_battery", MIMHID_PROFILE_KEYBOARD, result, spec,
                              k_send_sequence);
}
