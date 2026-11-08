// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Drags one finger back and forth across the screen five times, so a person
 * watching the target can confirm real touch input actually arrives.
 *
 * See docs/QUICKSTART.md for the full setup this assumes. Any screen can be
 * on when you run this; no text field is needed. Ctrl+C to stop early. */
#include "verify_common.h"

int main(void) {
    mimhid_device* device = NULL;
    if (verify_open_first_board(&device) != 0)
        return 1;

    /* A single fixed-slot Multi-Touch contact. The logical range 0..32767 is
     * unitless on the wire; the target's input stack maps it onto the actual
     * screen extent, so this drags fully across the screen regardless of its
     * physical size. */
    mimhid_touchscreen_options options;
    example_fill_touchscreen_options(&options);
    options.maximum_contacts = 1U;
    options.contacts_per_report = 1U;
    options.contact_count = mimhid_integer_field_make(0, 1, 1U);

    mimhid_spec* spec = NULL;
    mimhid_result result = mimhid_spec_create_touchscreen(&options, &spec);
    if (result != MIMHID_OK) {
        mimhid_device_close(device, EXAMPLE_CONTROL_TIMEOUT_MS, EXAMPLE_DRAIN_TIMEOUT_MS);
        return verify_fail("touchscreen spec create", result);
    }

    mimhid_node* node = NULL;
    result = verify_open_node(device, spec, 0U, &node);
    mimhid_spec_release(spec);
    if (result != MIMHID_OK) {
        mimhid_device_close(device, EXAMPLE_CONTROL_TIMEOUT_MS, EXAMPLE_DRAIN_TIMEOUT_MS);
        return verify_fail("touchscreen node open", result);
    }
    /* The target may still be binding its driver to the new HID; input sent at once
     * can be lost (docs/API.md). */
    verify_sleep_ms(100);

    printf("dragging across the screen 5 times over about 6 seconds. Watch the target.\n");

    const int32_t y = 16000;
    const int steps = 40;
    for (int cycle = 0; cycle < 5; ++cycle) {
        const int32_t start_x = (cycle % 2 == 0) ? 3000 : 29000;
        const int32_t end_x = (cycle % 2 == 0) ? 29000 : 3000;

        result = mimhid_touchscreen_contact(node, 0U, 1U, start_x, y, NULL);
        if (result != MIMHID_OK) {
            mimhid_device_close(device, EXAMPLE_CONTROL_TIMEOUT_MS, EXAMPLE_DRAIN_TIMEOUT_MS);
            return verify_fail("touch down", result);
        }
        result = verify_submit(node);
        if (result != MIMHID_OK) {
            mimhid_device_close(device, EXAMPLE_CONTROL_TIMEOUT_MS, EXAMPLE_DRAIN_TIMEOUT_MS);
            return verify_fail("submit touch down", result);
        }

        for (int i = 1; i <= steps; ++i) {
            const int32_t x = start_x + (int32_t)((int64_t)(end_x - start_x) * i / steps);
            result = mimhid_touchscreen_contact(node, 0U, 1U, x, y, NULL);
            if (result != MIMHID_OK) {
                mimhid_device_close(device, EXAMPLE_CONTROL_TIMEOUT_MS, EXAMPLE_DRAIN_TIMEOUT_MS);
                return verify_fail("touch move", result);
            }
            result = verify_submit(node);
            if (result != MIMHID_OK) {
                mimhid_device_close(device, EXAMPLE_CONTROL_TIMEOUT_MS, EXAMPLE_DRAIN_TIMEOUT_MS);
                return verify_fail("submit touch move", result);
            }
            verify_sleep_ms(30);
        }

        result = mimhid_touchscreen_contact(node, 0U, 0U, end_x, y, NULL);
        if (result != MIMHID_OK) {
            mimhid_device_close(device, EXAMPLE_CONTROL_TIMEOUT_MS, EXAMPLE_DRAIN_TIMEOUT_MS);
            return verify_fail("touch up", result);
        }
        result = verify_submit(node);
        if (result != MIMHID_OK) {
            mimhid_device_close(device, EXAMPLE_CONTROL_TIMEOUT_MS, EXAMPLE_DRAIN_TIMEOUT_MS);
            return verify_fail("submit touch up", result);
        }
        verify_sleep_ms(300);
    }

    printf("done. Expected result: 5 left-right drags visible on screen.\n");
    mimhid_device_close(device, EXAMPLE_CONTROL_TIMEOUT_MS, EXAMPLE_DRAIN_TIMEOUT_MS);
    return 0;
}
