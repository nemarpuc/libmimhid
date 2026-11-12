// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
// The board presents a keyboard, reports every control transfer of the target, and answers
// vendor request 0x42 itself.
#include <mimusb.h>

#include "example_values.h"

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

static int fail(const char* what, mimusb_result r) {
    // Read the record first: mimusb_result_name is a call too and replaces it.
    const char* reason = mimusb_last_error()->reason;
    fprintf(stderr, "%s: %s (%s)\n", what, mimusb_result_name(r), reason);
    return 1;
}

static void on_event(void* user, const mimusb_event* event) {
    mimusb_device* device = *(mimusb_device**)user;
    if (event->kind != MIMUSB_EVENT_CONTROL_SETUP)
        return;
    printf("setup: bmRequestType=0x%02x bRequest=0x%02x token=%" PRIu32 "%s%s\n", event->data[0],
           event->data[1], event->control_token,
           (event->control_flags & MIMUSB_CONTROL_FLAG_HELD) ? " held" : "",
           (event->control_flags & MIMUSB_CONTROL_FLAG_FIXED) ? " fixed" : "");
    if (event->control_flags & MIMUSB_CONTROL_FLAG_HELD)
        mimusb_device_control_reply(device, event->control_token, MIMUSB_CONTROL_ACTION_ANSWER,
                                    vendor_answer, sizeof(vendor_answer), EXAMPLE_SEND_TIMEOUT_MS);
}

int main(void) {
    mimusb_discovery* found = NULL;
    mimusb_result r = mimusb_discover(50, &found);
    if (r != MIMUSB_OK || mimusb_discovery_count(found) == 0) {
        mimusb_discovery_destroy(found);
        fprintf(stderr, "No mimusb board found.\n");
        return 1;
    }

    mimusb_device* device = NULL;
    // example_values.h: every value of the Device written out, with the pid.codes test IDs
    // (for testing in-house only: a device you hand out needs IDs of its own).
    mimusb_device_config config;
    example_fill_device_config(&config, mimusb_discovery_get(found, 0)->baud_rate,
                               "Control transfers");
    config.transport.event_sink = on_event;
    config.transport.event_user = &device;

    r = mimusb_device_open(mimusb_discovery_get(found, 0)->port_name, &config, &device);
    mimusb_discovery_destroy(found);
    if (r != MIMUSB_OK)
        return fail("device_open", r);

    mimusb_keyboard_options keyboard_options;
    example_fill_keyboard_options(&keyboard_options);
    mimusb_spec* keyboard_spec = NULL;
    if ((r = mimusb_spec_create_keyboard(&keyboard_options, &keyboard_spec)) != MIMUSB_OK)
        return fail("spec_create", r);

    mimusb_commit_timeouts timeouts;
    example_fill_commit_timeouts(&timeouts);
    example_placement node_placement;
    example_fill_placement(&node_placement, 0);
    mimusb_node* keyboard = NULL;
    r = mimusb_node_open(device, keyboard_spec, &node_placement.placement, &timeouts, &keyboard);
    mimusb_spec_release(keyboard_spec);
    if (r != MIMUSB_OK || (r = mimusb_device_wait_ready(device, 5000)) != MIMUSB_OK)
        return fail("enumeration", r);

    // Report every request, and ask the program about vendor requests 0x42 that read from the
    // device: the sink answers them. If no answer came within 100 ms the target would see a STALL.
    mimusb_control_rule rule = {0};
    rule.request_type_mask = 0xFF;
    rule.request_type_value = 0xC0; // device to host, vendor, device
    rule.request_mask = 0xFF;
    rule.request_value = 0x42;
    rule.action = MIMUSB_CONTROL_ASK;
    rule.timeout_ms = 100;
    rule.when_late = MIMUSB_CONTROL_ACTION_STALL;
    if ((r = mimusb_device_set_control_rules(device, 1, &rule, 1, EXAMPLE_CONTROL_TIMEOUT_MS)) !=
        MIMUSB_OK)
        return fail("set_control_rules", r);

    printf(
        "Listening for 10 s; send vendor request 0x42 (read) from the target to get an answer.\n");
    sleep_ms(10000);

    // Closing the Device closes the Node and takes the Device's rules off the board.
    r = mimusb_device_close(device, EXAMPLE_CONTROL_TIMEOUT_MS, EXAMPLE_DRAIN_TIMEOUT_MS);
    return r == MIMUSB_OK ? 0 : fail("device_close", r);
}
