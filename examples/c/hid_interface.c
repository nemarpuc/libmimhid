// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
// The first board found presents a keyboard whose HID interface this program writes itself, with
// mimusb_node_open: the HID descriptor, the report descriptor the target asks for, the answer to
// GET_REPORT, the requests that are accepted, and the reports sent with mimusb_node_endpoint_write.
// Nothing is a default: every byte below is this program's. The library places them and reads none.
// For the same keyboard from a profile, with all of this written for you, see
// basic_keyboard_mouse.c.
#include <mimusb.h>

#include "example_values.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms)*1000)
#endif

// The report descriptor of a boot keyboard without Output items: HID 1.11 Appendix B.1, minus its
// LED report. One 8 byte Input report: the modifiers (E0..E7), a reserved byte, six key codes.
static const uint8_t report_descriptor[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25,
    0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0};

static int fail(const char* what, mimusb_result r) {
    // Read the record first: mimusb_result_name is a call too and replaces it.
    const char* reason = mimusb_last_error()->reason;
    fprintf(stderr, "%s: %s (%s)\n", what, mimusb_result_name(r), reason);
    return 1;
}

// Called on the library's reader thread: do little here. A request an ACCEPT rule took arrives as
// a control setup with the ACCEPTED flag; the setup packet is 8 bytes (USB 2.0 9.3).
static void MIMUSB_CALL on_event(void* user, const mimusb_event* event) {
    (void)user;
    if (event->kind == MIMUSB_EVENT_CONTROL_SETUP &&
        (event->control_flags & MIMUSB_CONTROL_FLAG_ACCEPTED) != 0U) {
        printf("target sent class request 0x%02X, wValue 0x%02X%02X\n", event->data[1],
               event->data[3], event->data[2]);
    }
}

int main(void) {
    mimusb_discovery* found = NULL;
    mimusb_result r = mimusb_discover(50, &found);
    if (r != MIMUSB_OK || mimusb_discovery_count(found) == 0) {
        mimusb_discovery_destroy(found);
        fprintf(stderr, "No mimusb board found.\n");
        return 1;
    }
    mimusb_device_config config;
    example_fill_device_config(&config, mimusb_discovery_get(found, 0)->baud_rate,
                               "Hand written HID");
    config.transport.event_sink = on_event;
    mimusb_device* device = NULL;
    r = mimusb_device_open(mimusb_discovery_get(found, 0)->port_name, &config, &device);
    mimusb_discovery_destroy(found);
    if (r != MIMUSB_OK)
        return fail("device_open", r);

    // The HID descriptor (HID 1.11 6.2.1) goes after the Interface descriptor as class bytes; the
    // helper writes its 9 bytes. 0x0111 is HID 1.11, country code 0 is "not localized".
    uint8_t hid_descriptor[9];
    if ((r = mimusb_hid_descriptor_bytes(0x0111, 0, sizeof(report_descriptor), hid_descriptor)) !=
        MIMUSB_OK)
        return fail("hid_descriptor_bytes", r);

    // One interrupt IN endpoint of 8 bytes, polled every 10 ms (full speed: bInterval in frames,
    // USB 2.0 9.6.6). A newer report replaces one that has not left yet: a keyboard sends its whole
    // state each time.
    mimusb_usb_endpoint endpoint;
    memset(&endpoint, 0, sizeof(endpoint));
    endpoint.address = 0x81;
    endpoint.transfer_type = MIMUSB_TRANSFER_INTERRUPT;
    endpoint.max_packet_size = 8;
    endpoint.interval = 10;
    endpoint.policy.on_full = MIMUSB_FULL_DROP_OLDEST;
    endpoint.policy.depth = 4;

    mimusb_usb_interface interface_config;
    memset(&interface_config, 0, sizeof(interface_config));
    interface_config.number = 0;
    interface_config.class_code = 0x03; // HID
    interface_config.subclass = 1;      // boot interface (HID 1.11 4.2)
    interface_config.protocol = 1;      // keyboard (HID 1.11 4.3)
    interface_config.name = "Keyboard";
    interface_config.class_descriptors = hid_descriptor;
    interface_config.class_descriptors_length = sizeof(hid_descriptor);
    interface_config.endpoints = &endpoint;
    interface_config.endpoint_count = 1;

    // What the board does with the target's control requests for this interface. They are tried in
    // order, the first that matches wins, and a request no rule matches is stalled.
    mimusb_control_rule rules[4];
    memset(rules, 0, sizeof(rules));
    // 0: the report descriptor, GET_DESCRIPTOR type 0x22 (HID 1.11 7.1.1).
    if ((r = mimusb_hid_descriptor_rule(0, 0x22, report_descriptor, sizeof(report_descriptor),
                                        &rules[0])) != MIMUSB_OK)
        return fail("hid_descriptor_rule", r);
    // 1: GET_REPORT of an Input report (HID 1.11 7.2.1): the last report sent on endpoint 0x81. A
    // request before the first report is stalled, as the board has nothing to answer with.
    rules[1].request_type_mask = 0xFF;
    rules[1].request_type_value = 0xA1; // device to host, class, interface
    rules[1].request_mask = 0xFF;
    rules[1].request_value = 0x01; // GET_REPORT
    rules[1].value_mask = 0xFF00;
    rules[1].value_value = 0x0100; // report type Input
    rules[1].index_mask = 0xFFFF;
    rules[1].index_value = 0;
    rules[1].action = MIMUSB_CONTROL_ANSWER_LAST_SENT;
    rules[1].endpoint_address = 0x81;
    // 2 and 3: SET_IDLE and SET_PROTOCOL (HID 1.11 7.2.4, 7.2.6) are taken, and reported above.
    for (int i = 2; i <= 3; ++i) {
        rules[i].request_type_mask = 0xFF;
        rules[i].request_type_value = 0x21; // host to device, class, interface
        rules[i].request_mask = 0xFF;
        rules[i].request_value = (uint8_t)(i == 2 ? 0x0A : 0x0B);
        rules[i].index_mask = 0xFFFF;
        rules[i].index_value = 0;
        rules[i].action = MIMUSB_CONTROL_ACCEPT;
    }

    mimusb_node_config node_config;
    mimusb_node_config_init(&node_config);
    node_config.interfaces = &interface_config;
    node_config.interface_count = 1;
    node_config.rules = rules;
    node_config.rule_count = 4;

    mimusb_commit_timeouts timeouts;
    example_fill_commit_timeouts(&timeouts);
    mimusb_node* node = NULL;
    if ((r = mimusb_node_open(device, &node_config, &timeouts, &node)) != MIMUSB_OK)
        return fail("node_open", r);
    if ((r = mimusb_device_wait_ready(device, 5000)) != MIMUSB_OK)
        return fail("wait_ready", r);

    // Press and release the "a" key (usage 0x04 of the Keyboard page): 8 bytes each.
    const uint8_t press[8] = {0, 0, 0x04, 0, 0, 0, 0, 0};
    const uint8_t release[8] = {0};
    if ((r = mimusb_node_endpoint_write(node, 0x81, press, sizeof(press), 1000)) != MIMUSB_OK)
        return fail("endpoint_write", r);
    sleep_ms(50);
    if ((r = mimusb_node_endpoint_write(node, 0x81, release, sizeof(release), 1000)) != MIMUSB_OK)
        return fail("endpoint_write", r);

    sleep_ms(5000); // the events above arrive while this waits

    if ((r = mimusb_node_close(node, &timeouts, 1000)) != MIMUSB_OK)
        return fail("node_close", r);
    if ((r = mimusb_device_close(device, 500, 1000)) != MIMUSB_OK)
        return fail("device_close", r);
    return 0;
}
