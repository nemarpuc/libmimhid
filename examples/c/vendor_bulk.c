// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
// The first board found presents a vendor-class device (class 0xFF) with a bulk IN and a bulk OUT
// endpoint, writes one message to the target, and prints what the target sends back for five
// seconds. The class is the program's: the library places the bytes it is given and reads none of
// them. Nothing here is a default; every value is one this program decides.
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

static int fail(const char* what, mimusb_result r) {
    const char* reason = mimusb_last_error()->reason;
    fprintf(stderr, "%s: %s (%s)\n", what, mimusb_result_name(r), reason);
    return 1;
}

// Called on the library's reader thread: do little here.
static void MIMUSB_CALL on_event(void* user, const mimusb_event* event) {
    (void)user;
    if (event->kind == MIMUSB_EVENT_ENDPOINT_DATA) {
        printf("target sent %zu bytes on endpoint 0x%02X:", event->length,
               (unsigned)event->endpoint_address);
        for (size_t i = 0; i < event->length; ++i)
            printf(" %02X", event->data[i]);
        printf("\n");
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
    example_fill_device_config(&config, mimusb_discovery_get(found, 0)->baud_rate, "Vendor Bulk");
    config.transport.event_sink = on_event;
    mimusb_device* device = NULL;
    r = mimusb_device_open(mimusb_discovery_get(found, 0)->port_name, &config, &device);
    mimusb_discovery_destroy(found);
    if (r != MIMUSB_OK)
        return fail("device_open", r);

    // Bulk endpoints of a full-speed device have packets of 8, 16, 32 or 64 bytes (USB 2.0 5.8.3).
    // The IN endpoint waits instead of dropping: a write blocks while 4 transfers are unfinished.
    mimusb_usb_endpoint endpoints[2];
    memset(endpoints, 0, sizeof(endpoints));
    endpoints[0].address = 0x81;
    endpoints[0].transfer_type = MIMUSB_TRANSFER_BULK;
    endpoints[0].max_packet_size = 64;
    endpoints[0].policy.on_full = MIMUSB_FULL_WAIT;
    endpoints[0].policy.depth = 4;
    endpoints[1].address = 0x01;
    endpoints[1].transfer_type = MIMUSB_TRANSFER_BULK;
    endpoints[1].max_packet_size = 64;

    mimusb_usb_interface interface_config;
    memset(&interface_config, 0, sizeof(interface_config));
    interface_config.number = 0;
    interface_config.class_code = 0xFF; // vendor specific
    interface_config.name = "Data";
    const mimusb_usb_item items[2] = {mimusb_usb_item_make_endpoint(endpoints[0]),
                                      mimusb_usb_item_make_endpoint(endpoints[1])};
    const mimusb_usb_setting setting = {items, 2};
    interface_config.settings = &setting;
    interface_config.setting_count = 1;

    mimusb_node_config node_config;
    mimusb_node_config_init(&node_config);
    node_config.interfaces = &interface_config;
    node_config.interface_count = 1;

    mimusb_commit_timeouts timeouts;
    example_fill_commit_timeouts(&timeouts);
    mimusb_node* node = NULL;
    if ((r = mimusb_node_open(device, &node_config, &timeouts, &node)) != MIMUSB_OK)
        return fail("node_open", r);
    if ((r = mimusb_device_wait_ready(device, 5000)) != MIMUSB_OK)
        return fail("wait_ready", r);

    const uint8_t message[] = "hello from the board";
    if ((r = mimusb_node_endpoint_write(node, 0x81, message, sizeof(message), 1000)) != MIMUSB_OK)
        return fail("endpoint_write", r);

    sleep_ms(5000); // the events above arrive while this waits

    if ((r = mimusb_node_close(node, &timeouts, 1000)) != MIMUSB_OK)
        return fail("node_close", r);
    if ((r = mimusb_device_close(device, 500, 1000)) != MIMUSB_OK)
        return fail("device_close", r);
    return 0;
}
