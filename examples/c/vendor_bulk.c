// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
// The first board found presents a vendor-class device (class 0xFF) with a bulk IN and a bulk OUT
// endpoint, writes one message to the target, and prints what the target sends back for five
// seconds. The class is the program's: the library places the bytes it is given and reads none of
// them. Nothing here is a default; every value is one this program decides.
#include <mimhid.h>

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

static int fail(const char* what, mimhid_result r) {
    const char* reason = mimhid_last_error()->reason;
    fprintf(stderr, "%s: %s (%s)\n", what, mimhid_result_name(r), reason);
    return 1;
}

// Called on the library's reader thread: do little here.
static void MIMHID_CALL on_event(void* user, const mimhid_event* event) {
    (void)user;
    if (event->kind == MIMHID_EVENT_ENDPOINT_DATA) {
        printf("target sent %zu bytes on endpoint 0x%02X:", event->length,
               (unsigned)event->endpoint_address);
        for (size_t i = 0; i < event->length; ++i)
            printf(" %02X", event->data[i]);
        printf("\n");
    }
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
    example_fill_device_config(&config, mimhid_discovery_get(found, 0)->baud_rate, "Vendor Bulk");
    config.transport.event_sink = on_event;
    mimhid_device* device = NULL;
    r = mimhid_device_open(mimhid_discovery_get(found, 0)->port_name, &config, &device);
    mimhid_discovery_destroy(found);
    if (r != MIMHID_OK)
        return fail("device_open", r);

    // Bulk endpoints of a full-speed device have packets of 8, 16, 32 or 64 bytes (USB 2.0 5.8.3).
    // The IN endpoint waits instead of dropping: a write blocks while 4 transfers are unfinished.
    mimhid_usb_endpoint endpoints[2];
    memset(endpoints, 0, sizeof(endpoints));
    endpoints[0].address = 0x81;
    endpoints[0].transfer_type = MIMHID_TRANSFER_BULK;
    endpoints[0].max_packet_size = 64;
    endpoints[0].policy.on_full = MIMHID_FULL_WAIT;
    endpoints[0].policy.depth = 4;
    endpoints[1].address = 0x01;
    endpoints[1].transfer_type = MIMHID_TRANSFER_BULK;
    endpoints[1].max_packet_size = 64;

    mimhid_usb_interface interface_config;
    memset(&interface_config, 0, sizeof(interface_config));
    interface_config.number = 0;
    interface_config.class_code = 0xFF; // vendor specific
    interface_config.name = "Data";
    interface_config.endpoints = endpoints;
    interface_config.endpoint_count = 2;

    mimhid_node_config node_config;
    mimhid_node_config_init(&node_config);
    node_config.interfaces = &interface_config;
    node_config.interface_count = 1;

    mimhid_commit_timeouts timeouts;
    example_fill_commit_timeouts(&timeouts);
    mimhid_node* node = NULL;
    if ((r = mimhid_node_open(device, &node_config, &timeouts, &node)) != MIMHID_OK)
        return fail("node_open", r);
    if ((r = mimhid_device_wait_ready(device, 5000)) != MIMHID_OK)
        return fail("wait_ready", r);

    const uint8_t message[] = "hello from the board";
    if ((r = mimhid_node_endpoint_write(node, 0x81, message, sizeof(message), 1000)) != MIMHID_OK)
        return fail("endpoint_write", r);

    sleep_ms(5000); // the events above arrive while this waits

    if ((r = mimhid_node_close(node, &timeouts, 1000)) != MIMHID_OK)
        return fail("node_close", r);
    if ((r = mimhid_device_close(device, 500, 1000)) != MIMHID_OK)
        return fail("device_close", r);
    return 0;
}
