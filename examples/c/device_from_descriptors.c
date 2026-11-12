// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
// The first board found presents a whole USB device given as the bytes the target reads: a device
// descriptor, a configuration with one vendor interface (a bulk IN and a bulk OUT endpoint) and
// two strings. This is how a copy of a real device is presented, from the bytes read off it. The
// library reads no class and changes none of the bytes; every value below is this program's. The
// program writes one message to the target and prints what the target sends back for five
// seconds.
#include <mimhid.h>

#include "example_values.h"

#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms)*1000)
#endif

static int fail(const char* what, mimhid_result r) {
    const mimhid_error_detail* d = mimhid_last_error();
    fprintf(stderr, "%s: %s (%s: %s, offset %u)\n", what, mimhid_result_name(r), d->field, d->reason,
            (unsigned)d->offset);
    return 1;
}

// The data of an OUT endpoint of a Device given as bytes arrives with node_id 0.
static void MIMHID_CALL on_event(void* user, const mimhid_event* event) {
    (void)user;
    if (event->kind == MIMHID_EVENT_ENDPOINT_DATA) {
        printf("target sent %zu bytes on endpoint 0x%02X (interface %u):", event->length,
               (unsigned)event->endpoint_address, (unsigned)event->interface_number);
        for (size_t i = 0; i < event->length; ++i)
            printf(" %02X", event->data[i]);
        printf("\n");
    }
}

// USB 2.0 9.6.1: 18 bytes. bcdUSB 2.00, class at the interface, 64-byte control endpoint, the pid.codes
// test IDs, strings 1 (manufacturer) and 2 (product), no serial number, one configuration.
static const uint8_t kDevice[18] = {18,   1,    0x00, 0x02, 0,    0,    0,    64,  0x09,
                                    0x12, 0x01, 0x00, 0x00, 0x01, 1,    2,    0,   1};

// USB 2.0 9.6.3, 9.6.5, 9.6.6: 9 + 9 + 7 + 7 = 32 bytes. One vendor-class interface, number 0,
// with a bulk IN endpoint 0x81 and a bulk OUT endpoint 0x01 of 64-byte packets (full speed,
// USB 2.0 5.8.3).
static const uint8_t kConfiguration[32] = {
    9, 2, 32, 0, 1, 1, 0, 0x80, 50,       // configuration: 1 interface, value 1, bus powered, 100 mA
    9, 4, 0, 0, 2, 0xFF, 1, 2, 0,         // interface 0: 2 endpoints, vendor class
    7, 5, 0x81, 2, 64, 0, 0,              // bulk IN
    7, 5, 0x01, 2, 64, 0, 0};             // bulk OUT

// String descriptors go on the bus as UTF-16 code units (USB 2.0 9.6.7).
static const uint16_t kManufacturer[] = {'A', 'c', 'm', 'e'};
static const uint16_t kProduct[] = {'W', 'i', 'd', 'g', 'e', 't'};

int main(void) {
    mimhid_discovery* found = NULL;
    mimhid_result r = mimhid_discover(50, &found);
    if (r != MIMHID_OK || mimhid_discovery_count(found) == 0) {
        mimhid_discovery_destroy(found);
        fprintf(stderr, "No mimhid board found.\n");
        return 1;
    }
    mimhid_device_config config;
    example_fill_device_config(&config, mimhid_discovery_get(found, 0)->baud_rate, "unused");
    config.transport.event_sink = on_event;
    mimhid_device* device = NULL;
    r = mimhid_device_open(mimhid_discovery_get(found, 0)->port_name, &config, &device);
    mimhid_discovery_destroy(found);
    if (r != MIMHID_OK)
        return fail("device_open", r);

    // Before the descriptors: the strings, and the policy of every IN endpoint. The IN endpoint
    // waits instead of dropping: a write blocks while 4 transfers are unfinished.
    if ((r = mimhid_device_set_string(device, 1, 0x0409, kManufacturer, 4)) != MIMHID_OK)
        return fail("set_string", r);
    if ((r = mimhid_device_set_string(device, 2, 0x0409, kProduct, 6)) != MIMHID_OK)
        return fail("set_string", r);
    mimhid_endpoint_policy policy;
    policy.on_full = MIMHID_FULL_WAIT;
    policy.depth = 4;
    if ((r = mimhid_device_endpoint_set_policy(device, 0x81, &policy, EXAMPLE_CONTROL_TIMEOUT_MS)) !=
        MIMHID_OK)
        return fail("endpoint_set_policy", r);

    mimhid_device_descriptors descriptors;
    descriptors.struct_size = sizeof(descriptors);
    descriptors.reserved = 0;
    descriptors.device = kDevice;
    descriptors.device_length = sizeof(kDevice);
    descriptors.configuration = kConfiguration;
    descriptors.configuration_length = sizeof(kConfiguration);
    descriptors.bos = NULL; // no BOS descriptor: see winusb_bulk.c
    descriptors.bos_length = 0;
    mimhid_commit_timeouts timeouts;
    example_fill_commit_timeouts(&timeouts);
    if ((r = mimhid_device_set_descriptors(device, &descriptors, &timeouts)) != MIMHID_OK)
        return fail("set_descriptors", r);
    if ((r = mimhid_device_wait_ready(device, 5000)) != MIMHID_OK)
        return fail("wait_ready", r);

    const uint8_t message[] = "hello from the board";
    if ((r = mimhid_device_endpoint_write(device, 0x81, message, sizeof(message), 1000)) !=
        MIMHID_OK)
        return fail("endpoint_write", r);
    sleep_ms(5000); // the events above arrive while this waits

    if ((r = mimhid_device_clear_descriptors(device, &timeouts)) != MIMHID_OK)
        return fail("clear_descriptors", r);
    if ((r = mimhid_device_close(device, 500, 1000)) != MIMHID_OK)
        return fail("device_close", r);
    return 0;
}
