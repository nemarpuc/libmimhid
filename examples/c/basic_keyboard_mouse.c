// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
// The first board found presents a keyboard and a mouse, types "a" and clicks once.
#include <mimhid.h>

#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms)*1000)
#endif

static int fail(const char* what, mimhid_result r) {
    fprintf(stderr, "%s: %s (%s)\n", what, mimhid_result_name(r), mimhid_last_error()->reason);
    return 1;
}

int main(void) {
    mimhid_discovery* found = NULL;
    mimhid_result r = mimhid_discover(50, &found);
    if (r != MIMHID_OK || mimhid_discovery_count(found) == 0) {
        mimhid_discovery_destroy(found);
        fprintf(stderr, "No mimhid board found.\n");
        return 1;
    }

    mimhid_device_config config;
    mimhid_device_config_init(&config);
    // pid.codes test IDs, for testing in-house only: a device you hand out needs IDs of its own.
    config.usb_device.vendor_id = 0x1209;
    config.usb_device.product_id = 0x0001;
    config.usb_device.product = "Keyboard and Mouse";
    config.transport.baud_rate = mimhid_discovery_get(found, 0)->baud_rate;

    mimhid_device* device = NULL;
    r = mimhid_device_open(mimhid_discovery_get(found, 0)->port_name, &config, &device);
    mimhid_discovery_destroy(found);
    if (r != MIMHID_OK)
        return fail("device_open", r);

    mimhid_keyboard_options keyboard_options;
    mimhid_keyboard_options_init(&keyboard_options);
    mimhid_mouse_options mouse_options;
    mimhid_mouse_options_init(&mouse_options);
    mimhid_spec* keyboard_spec = NULL;
    mimhid_spec* mouse_spec = NULL;
    if ((r = mimhid_spec_create_keyboard(&keyboard_options, &keyboard_spec)) != MIMHID_OK ||
        (r = mimhid_spec_create_mouse(&mouse_options, &mouse_spec)) != MIMHID_OK)
        return fail("spec_create", r);

    // One interface each, opened in one batch so the board enumerates once.
    mimhid_commit_timeouts timeouts;
    mimhid_commit_timeouts_init(&timeouts);
    mimhid_node_config keyboard_config;
    mimhid_node_config_init(&keyboard_config, 0);
    mimhid_node_config mouse_config;
    mimhid_node_config_init(&mouse_config, 1);
    mimhid_node* keyboard = NULL;
    mimhid_node* mouse = NULL;
    mimhid_device_batch_begin(device);
    if ((r = mimhid_node_open(device, keyboard_spec, &keyboard_config, &timeouts, &keyboard)) !=
            MIMHID_OK ||
        (r = mimhid_node_open(device, mouse_spec, &mouse_config, &timeouts, &mouse)) != MIMHID_OK)
        return fail("node_open", r);
    mimhid_spec_release(keyboard_spec); // each Node keeps its own reference
    mimhid_spec_release(mouse_spec);
    if ((r = mimhid_device_batch_end(device, &timeouts)) != MIMHID_OK ||
        (r = mimhid_device_wait_ready(device, 5000)) != MIMHID_OK)
        return fail("enumeration", r);

    mimhid_keyboard_key(keyboard, 0x04, 1); // Keyboard a
    mimhid_node_submit(keyboard, MIMHID_SEND_TIMEOUT_MS);
    sleep_ms(50);
    mimhid_keyboard_key(keyboard, 0x04, 0);
    mimhid_node_submit(keyboard, MIMHID_SEND_TIMEOUT_MS);

    mimhid_mouse_move(mouse, 50, 50);
    mimhid_mouse_button(mouse, 1, 1);
    mimhid_node_submit(mouse, MIMHID_SEND_TIMEOUT_MS);
    sleep_ms(30);
    mimhid_mouse_button(mouse, 1, 0);
    mimhid_node_submit(mouse, MIMHID_SEND_TIMEOUT_MS);

    // Closing the Device closes its Nodes too.
    r = mimhid_device_close(device, MIMHID_CONTROL_TIMEOUT_MS, MIMHID_DRAIN_TIMEOUT_MS);
    return r == MIMHID_OK ? 0 : fail("device_close", r);
}
