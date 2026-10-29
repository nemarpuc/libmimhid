// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
// The board presents a keyboard, reports every control transfer of the target, and answers
// vendor request 0x42 itself.
#include <mimhid.h>

#include <inttypes.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms)*1000)
#endif

static const uint8_t vendor_answer[] = {0x01, 0x02, 0x03, 0x04};

static int fail(const char* what, mimhid_result r) {
    fprintf(stderr, "%s: %s (%s)\n", what, mimhid_result_name(r), mimhid_last_error()->reason);
    return 1;
}

static void on_event(void* user, const mimhid_event* event) {
    mimhid_device* device = *(mimhid_device**)user;
    if (event->kind != MIMHID_EVENT_CONTROL_SETUP)
        return;
    printf("setup: bmRequestType=0x%02x bRequest=0x%02x token=%" PRIu32 "%s%s\n", event->data[0],
           event->data[1], event->control_token,
           (event->control_flags & MIMHID_CONTROL_FLAG_HELD) ? " held" : "",
           (event->control_flags & MIMHID_CONTROL_FLAG_FIXED) ? " fixed" : "");
    if (event->control_flags & MIMHID_CONTROL_FLAG_HELD)
        mimhid_device_control_reply(device, event->control_token, MIMHID_CONTROL_ACTION_ANSWER,
                                    vendor_answer, sizeof(vendor_answer), MIMHID_SEND_TIMEOUT_MS);
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
    mimhid_device_config config;
    mimhid_device_config_init(&config);
    // pid.codes test IDs, for testing in-house only: a device you hand out needs IDs of its own.
    config.usb_device.vendor_id = 0x1209;
    config.usb_device.product_id = 0x0001;
    config.usb_device.product = "Control transfers";
    config.transport.baud_rate = mimhid_discovery_get(found, 0)->baud_rate;
    config.transport.event_sink = on_event;
    config.transport.event_user = &device;

    r = mimhid_device_open(mimhid_discovery_get(found, 0)->port_name, &config, &device);
    mimhid_discovery_destroy(found);
    if (r != MIMHID_OK)
        return fail("device_open", r);

    mimhid_keyboard_options keyboard_options;
    mimhid_keyboard_options_init(&keyboard_options);
    mimhid_spec* keyboard_spec = NULL;
    if ((r = mimhid_spec_create_keyboard(&keyboard_options, &keyboard_spec)) != MIMHID_OK)
        return fail("spec_create", r);

    mimhid_commit_timeouts timeouts;
    mimhid_commit_timeouts_init(&timeouts);
    mimhid_node_config node_config;
    mimhid_node_config_init(&node_config, 0);
    mimhid_node* keyboard = NULL;
    r = mimhid_node_open(device, keyboard_spec, &node_config, &timeouts, &keyboard);
    mimhid_spec_release(keyboard_spec);
    if (r != MIMHID_OK || (r = mimhid_device_wait_ready(device, 5000)) != MIMHID_OK)
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
    if ((r = mimhid_device_set_control_routing(device, 1, &rule, 1, MIMHID_CONTROL_TIMEOUT_MS)) !=
        MIMHID_OK)
        return fail("set_control_routing", r);

    printf(
        "Listening for 10 s; send vendor request 0x42 (read) from the target to get an answer.\n");
    sleep_ms(10000);

    // Closing the Device closes the Node and takes the rules off the board.
    r = mimhid_device_close(device, MIMHID_CONTROL_TIMEOUT_MS, MIMHID_DRAIN_TIMEOUT_MS);
    return r == MIMHID_OK ? 0 : fail("device_close", r);
}
