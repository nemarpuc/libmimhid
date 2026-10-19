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

static const uint8_t vendor_answer[] = {'h', 'e', 'l', 'l', 'o'};

static int fail(const char* what, mimhid_result r) {
    fprintf(stderr, "%s: %s (%s)\n", what, mimhid_result_name(r), mimhid_last_error()->reason);
    return 1;
}

// Runs on the reader thread, so it stays short. Replying from here is how a short rule time is met.
static void on_event(void* user, const mimhid_event* event) {
    mimhid_device* device = *(mimhid_device**)user;
    if (event->kind != MIMHID_EVENT_CONTROL_SETUP || event->length != 8)
        return;
    const uint8_t* s = event->data;
    printf("setup %02x %02x value %04x index %04x length %u%s%s\n", s[0], s[1], s[2] | (s[3] << 8),
           s[4] | (s[5] << 8), s[6] | (s[7] << 8), (event->control_flags & MIMHID_CONTROL_FLAG_HELD) ? " held" : "",
           (event->control_flags & MIMHID_CONTROL_FLAG_FIXED) ? " fixed" : "");
    if (event->control_flags & MIMHID_CONTROL_FLAG_HELD)
        mimhid_device_control_reply(device, event->control_token, MIMHID_CONTROL_ACTION_ANSWER,
                                    vendor_answer, sizeof(vendor_answer));
}

int main(void) {
    mimhid_discovery* found = NULL;
    mimhid_result r = mimhid_discover(50, &found);
    if (r != MIMHID_OK || mimhid_discovery_count(found) == 0) {
        mimhid_discovery_destroy(found);
        fprintf(stderr, "No mimhid board found.\n");
        return 1;
    }

    mimhid_device* device = NULL;
    mimhid_device_options options = {0};
    options.struct_size = sizeof(options);
    options.vendor_id = 0x046D;
    options.product_id = 0xC52B;
    options.manufacturer = "Example";
    options.product = "Control transfers";
    options.serial_from_chip_id = 1;
    options.baud_rate = mimhid_discovery_get(found, 0)->baud_rate;
    options.control_timeout_ms = 500;
    options.send_timeout_ms = 500;
    options.reconnect_timeout_ms = 3000;
    options.close_drain_timeout_ms = 1000;
    options.language_id = 0x0409;
    options.bcd_device = 0x0100;
    options.max_power_ma = 100;
    options.linux_descriptor_policy_bytes = 4096;
    options.linux_hid_fields_per_report_policy = 256;
    options.linux_hid_global_stack_depth_policy = 4;
    options.linux_hid_usages_policy = 1024;
    options.linux_hid_report_data_policy_bits = 8192;
    options.linux_hid_report_size_policy_bits = 32;
    options.event_sink = on_event;
    options.event_user = &device;

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
    mimhid_spec* keyboard_spec = NULL;
    if ((r = mimhid_spec_create_keyboard(&keyboard_options, &keyboard_spec)) != MIMHID_OK)
        return fail("spec_create", r);

    mimhid_node_options node_options = {0};
    node_options.struct_size = sizeof(node_options);
    node_options.interface_number = 0;
    node_options.poll_interval_ms = 1;
    node_options.ep_in_size = 64;
    node_options.bcd_hid = 0x0111;
    mimhid_node* keyboard = NULL;
    if ((r = mimhid_node_open(device, keyboard_spec, &node_options, &keyboard)) != MIMHID_OK ||
        (r = mimhid_device_wait_ready(device, 5000)) != MIMHID_OK)
        return fail("enumeration", r);

    // Report every request, and hold vendor requests 0x42 that read from the device: the sink
    // answers them. If no answer came within 100 ms the target would see a STALL.
    mimhid_control_rule rule = {0};
    rule.request_type_mask = 0xFF;
    rule.request_type_value = 0xC0; // device to host, vendor, device
    rule.request_mask = 0xFF;
    rule.request_value = 0x42;
    rule.timeout_ms = 100;
    rule.when_late = MIMHID_CONTROL_ACTION_STALL;
    if ((r = mimhid_device_set_control_routing(device, 1, &rule, 1)) != MIMHID_OK)
        return fail("set_control_routing", r);

    printf("Listening for 10 s; send vendor request 0x42 (read) from the target to get an answer.\n");
    sleep_ms(10000);

    mimhid_node_close(keyboard);
    mimhid_device_close(device); // takes the rules off the board
    mimhid_spec_release(keyboard_spec);
    return 0;
}
