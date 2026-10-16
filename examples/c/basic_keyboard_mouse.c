// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#include <mimhid.h>

#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms) * 1000)
#endif

static int fail(const char* what, mimhid_result r) {
    fprintf(stderr, "%s: %s (%s)\n", what, mimhid_result_name(r), mimhid_last_error()->reason);
    return 1;
}

int main(void) {
    mimhid_discovery* found = NULL;
    mimhid_result r = mimhid_discover(500, &found);
    if (r != MIMHID_OK || mimhid_discovery_count(found) == 0) {
        mimhid_discovery_destroy(found);
        fprintf(stderr, "No mimhid board found.\n");
        return 1;
    }

    mimhid_device_options options = {0};
    options.struct_size = sizeof(options);
    options.vendor_id = 0x046D;
    options.product_id = 0xC52B;
    options.manufacturer = "Example";
    options.product = "Keyboard and Mouse";
    options.serial_from_chip_id = 1;
    // Limits of the target's HID parser; choose the values of the system you attach to.
    options.linux_descriptor_policy_bytes = 4096;
    options.linux_hid_fields_per_report_policy = 256;
    options.linux_hid_global_stack_depth_policy = 4;
    options.linux_hid_usages_policy = 1024;
    options.linux_hid_report_data_policy_bits = 8192;
    options.linux_hid_report_size_policy_bits = 32;

    mimhid_device* device = NULL;
    r = mimhid_device_open(mimhid_discovery_get(found, 0)->port_name, &options, &device);
    mimhid_discovery_destroy(found);
    if (r != MIMHID_OK)
        return fail("device_open", r);

    mimhid_keyboard_options keyboard_options = {0};
    keyboard_options.struct_size = sizeof(keyboard_options);
    keyboard_options.report_id.enabled = 1;
    keyboard_options.report_id.value = 1;
    keyboard_options.usage_minimum = 0x04;
    keyboard_options.usage_maximum = 0x65;
    mimhid_mouse_options mouse_options = {0};
    mouse_options.struct_size = sizeof(mouse_options);
    mouse_options.report_id.enabled = 1;
    mouse_options.report_id.value = 2;
    mouse_options.button_count = 3;
    mouse_options.x.logical_minimum = -127;
    mouse_options.x.logical_maximum = 127;
    mouse_options.x.bit_width = 8;
    mouse_options.y = mouse_options.x;

    mimhid_spec* keyboard_spec = NULL;
    mimhid_spec* mouse_spec = NULL;
    if ((r = mimhid_spec_create_keyboard(&keyboard_options, &keyboard_spec)) != MIMHID_OK ||
        (r = mimhid_spec_create_mouse(&mouse_options, &mouse_spec)) != MIMHID_OK)
        return fail("spec_create", r);

    // One batch: the board re-enumerates once for both nodes.
    mimhid_node_options node_options = {0};
    node_options.struct_size = sizeof(node_options);
    node_options.interface_number = 0;
    node_options.poll_interval_ms = 1;
    node_options.ep_in_size = 64;
    node_options.bcd_hid = 0x0111;
    mimhid_node* keyboard = NULL;
    mimhid_node* mouse = NULL;
    mimhid_device_batch_begin(device);
    r = mimhid_node_open(device, keyboard_spec, &node_options, &keyboard);
    if (r == MIMHID_OK) {
        node_options.interface_number = 1;
        r = mimhid_node_open(device, mouse_spec, &node_options, &mouse);
    }
    if (r != MIMHID_OK)
        return fail("node_open", r);
    if ((r = mimhid_device_batch_end(device)) != MIMHID_OK ||
        (r = mimhid_device_wait_ready(device, 5000)) != MIMHID_OK)
        return fail("enumeration", r);

    mimhid_keyboard_key(keyboard, 0x04, 1);
    mimhid_node_submit(keyboard);
    sleep_ms(50);
    mimhid_keyboard_key(keyboard, 0x04, 0);
    mimhid_node_submit(keyboard);

    mimhid_mouse_move(mouse, 50, 50);
    mimhid_mouse_button(mouse, 1, 1);
    mimhid_node_submit(mouse);
    sleep_ms(30);
    mimhid_mouse_button(mouse, 1, 0);
    mimhid_node_submit(mouse);

    mimhid_node_close(mouse);
    mimhid_node_close(keyboard);
    mimhid_device_close(device);
    mimhid_spec_release(mouse_spec);
    mimhid_spec_release(keyboard_spec);
    return 0;
}
