// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* Moves the mouse cursor in a circle repeatedly, so a person watching the
 * target can confirm real relative pointer motion actually arrives.
 *
 * Touch-first targets (phones, tablets) may show no visible pointer at all. A
 * clean exit with no error
 * here is still useful evidence that the reports were accepted, even with
 * nothing visible on screen; it does not by itself prove the target
 * consumed the motion. See docs/QUICKSTART.md. Ctrl+C to stop early. */
#include "verify_common.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

int main(void) {
    mimusb_device* device = NULL;
    if (verify_open_first_board(&device) != 0)
        return 1;

    mimusb_mouse_options options;
    example_fill_mouse_options(&options);

    mimusb_spec* spec = NULL;
    mimusb_result result = mimusb_spec_create_mouse(&options, &spec);
    if (result != MIMUSB_OK) {
        mimusb_device_close(device, EXAMPLE_CONTROL_TIMEOUT_MS, EXAMPLE_DRAIN_TIMEOUT_MS);
        return verify_fail("mouse spec create", result);
    }

    mimusb_node* node = NULL;
    result = verify_open_node(device, spec, 0U, &node);
    mimusb_spec_release(spec);
    if (result != MIMUSB_OK) {
        mimusb_device_close(device, EXAMPLE_CONTROL_TIMEOUT_MS, EXAMPLE_DRAIN_TIMEOUT_MS);
        return verify_fail("mouse node open", result);
    }
    /* The target may still be binding its driver to the new HID; input sent at once
     * can be lost (docs/API.md). */
    verify_sleep_ms(100);

    printf("moving the cursor in circles for a few seconds. If nothing is visibly\n");
    printf("moving, that alone does not mean it failed -- see the note above.\n");

    const double radius = 18.0;
    const int steps_per_circle = 36;
    const int circles = 6;
    const double step_angle = 2.0 * M_PI / steps_per_circle;
    double angle = 0.0;
    double prev_x = radius;
    double prev_y = 0.0;

    for (int circle = 0; circle < circles; ++circle) {
        for (int step = 0; step < steps_per_circle; ++step) {
            angle += step_angle;
            const double x = radius * cos(angle);
            const double y = radius * sin(angle);
            const int32_t dx = (int32_t)lround(x - prev_x);
            const int32_t dy = (int32_t)lround(y - prev_y);
            prev_x += dx;
            prev_y += dy;
            if (dx != 0 || dy != 0) {
                result = mimusb_mouse_move(node, dx, dy);
                if (result != MIMUSB_OK) {
                    mimusb_device_close(device, EXAMPLE_CONTROL_TIMEOUT_MS,
                                        EXAMPLE_DRAIN_TIMEOUT_MS);
                    return verify_fail("mouse move", result);
                }
                result = verify_submit(node);
                if (result != MIMUSB_OK) {
                    mimusb_device_close(device, EXAMPLE_CONTROL_TIMEOUT_MS,
                                        EXAMPLE_DRAIN_TIMEOUT_MS);
                    return verify_fail("submit mouse move", result);
                }
            }
            verify_sleep_ms(25);
        }
    }

    printf("done. Expected result: the cursor traced circles, if your target shows one.\n");
    mimusb_device_close(device, EXAMPLE_CONTROL_TIMEOUT_MS, EXAMPLE_DRAIN_TIMEOUT_MS);
    return 0;
}
