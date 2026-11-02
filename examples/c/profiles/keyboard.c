// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Keyboard profile: a full-NKRO one-bit Variable bitmap over one contiguous
 * Usage range. Every simultaneously pressed key gets its own bit, so there is
 * no array slot count and no ErrorRollOver overflow to configure. */
#include "profile_example.h"

static const char* const k_send_sequence[] = {
    "mimhid_keyboard_key(node, 0x04, 1)   press Keyboard a/A; the library sets its bitmap bit",
    "mimhid_node_submit(node, MIMHID_SEND_TIMEOUT_MS)  one accepted edge, one request 57",
    "mimhid_keyboard_key(node, 0x04, 0)   release it; a held edge must reach one report",
    "mimhid_node_submit(node, MIMHID_SEND_TIMEOUT_MS)  submitting an opposite edge too early is "
    "BUSY",
    NULL};

int main(void) {
    mimhid_keyboard_options options;
    mimhid_spec* spec = NULL;
    mimhid_result result;

    mimhid_keyboard_options_init(&options);

    result = mimhid_spec_create_keyboard(&options, &spec);
    return mimhid_example_run("keyboard", MIMHID_PROFILE_KEYBOARD, result, spec, k_send_sequence);
}
