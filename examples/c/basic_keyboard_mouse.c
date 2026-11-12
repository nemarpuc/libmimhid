// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
// The first board found presents a keyboard and a mouse, types "a" and clicks once.
#include <mimusb.h>

#include "example_values.h"

#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms)*1000)
#endif

static int fail(const char* what, mimusb_result r) {
    // Read the record first: mimusb_result_name is a call too and replaces it.
    const char* reason = mimusb_last_error()->reason;
    fprintf(stderr, "%s: %s (%s)\n", what, mimusb_result_name(r), reason);
    return 1;
}

int main(void) {
    mimusb_discovery* found = NULL;
    mimusb_result r = mimusb_discover(50, &found);
    if (r != MIMUSB_OK || mimusb_discovery_count(found) == 0) {
        mimusb_discovery_destroy(found);
        fprintf(stderr, "No mimusb board found.\n");
        return 1;
    }

    // example_values.h: every value of the Device written out, with the pid.codes test IDs
    // (for testing in-house only: a device you hand out needs IDs of its own).
    mimusb_device_config config;
    example_fill_device_config(&config, mimusb_discovery_get(found, 0)->baud_rate,
                               "Keyboard and Mouse");

    mimusb_device* device = NULL;
    r = mimusb_device_open(mimusb_discovery_get(found, 0)->port_name, &config, &device);
    mimusb_discovery_destroy(found);
    if (r != MIMUSB_OK)
        return fail("device_open", r);

    mimusb_keyboard_options keyboard_options;
    example_fill_keyboard_options(&keyboard_options);
    mimusb_mouse_options mouse_options;
    example_fill_mouse_options(&mouse_options);
    mimusb_spec* keyboard_spec = NULL;
    mimusb_spec* mouse_spec = NULL;
    if ((r = mimusb_spec_create_keyboard(&keyboard_options, &keyboard_spec)) != MIMUSB_OK ||
        (r = mimusb_spec_create_mouse(&mouse_options, &mouse_spec)) != MIMUSB_OK)
        return fail("spec_create", r);

    // One interface each, opened in one batch so the board enumerates once.
    mimusb_commit_timeouts timeouts;
    example_fill_commit_timeouts(&timeouts);
    example_placement keyboard_placement;
    example_fill_placement(&keyboard_placement, 0);
    example_placement mouse_placement;
    example_fill_placement(&mouse_placement, 1);
    mimusb_node* keyboard = NULL;
    mimusb_node* mouse = NULL;
    mimusb_device_batch_begin(device);
    if ((r = mimusb_node_open(device, keyboard_spec, &keyboard_placement.placement, &timeouts,
                              &keyboard)) != MIMUSB_OK ||
        (r = mimusb_node_open(device, mouse_spec, &mouse_placement.placement, &timeouts, &mouse)) !=
            MIMUSB_OK)
        return fail("node_open", r);
    mimusb_spec_release(keyboard_spec); // each Node keeps its own reference
    mimusb_spec_release(mouse_spec);
    if ((r = mimusb_device_batch_end(device, &timeouts)) != MIMUSB_OK ||
        (r = mimusb_device_wait_ready(device, 5000)) != MIMUSB_OK)
        return fail("enumeration", r);

    mimusb_keyboard_key(keyboard, 0x04, 1); // Keyboard a
    mimusb_node_submit(keyboard, EXAMPLE_SEND_TIMEOUT_MS);
    sleep_ms(50);
    mimusb_keyboard_key(keyboard, 0x04, 0);
    mimusb_node_submit(keyboard, EXAMPLE_SEND_TIMEOUT_MS);

    mimusb_mouse_move(mouse, 50, 50);
    mimusb_mouse_button(mouse, 1, 1);
    mimusb_node_submit(mouse, EXAMPLE_SEND_TIMEOUT_MS);
    sleep_ms(30);
    mimusb_mouse_button(mouse, 1, 0);
    mimusb_node_submit(mouse, EXAMPLE_SEND_TIMEOUT_MS);

    // Taking the USB device off the target drops reports still queued on the board, so the
    // target gets time to poll the last one first.
    sleep_ms(50);
    // Closing the Device closes its Nodes too.
    r = mimusb_device_close(device, EXAMPLE_CONTROL_TIMEOUT_MS, EXAMPLE_DRAIN_TIMEOUT_MS);
    return r == MIMUSB_OK ? 0 : fail("device_close", r);
}
