// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
// A vendor-class device with a bulk IN and a bulk OUT endpoint that Windows binds to its WinUSB
// driver without an INF file: the BOS descriptor carries the Microsoft OS 2.0 platform capability,
// and the board answers the vendor request that capability names with the descriptor set.
// Source: "Microsoft OS 2.0 Descriptors Specification" (Microsoft, Windows hardware
// documentation, USB), which gives the platform capability layout (UUID
// D8DD60DF-4589-4CC7-9CD2-659D9E648A9F), the descriptor set header, the compatible ID feature
// descriptor ("WINUSB") and the request that fetches the set: bmRequestType 0xC0 (vendor, device to
// host, device), bRequest = the vendor code of the capability, wIndex 0x0007. The BOS descriptor
// itself is USB 3.2 9.6.2; a host asks for it only when bcdUSB is 0x0201 or more (USB 2.0 Link
// Power Management ECN).
// NOT RUN ON WINDOWS: this program was checked against the simulated board and on a Linux target,
// where the BOS descriptor and the answer can be read back; no Windows machine has bound WinUSB to
// the device it presents.
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

#define VENDOR_CODE 0x01 // bMS_VendorCode: the request the host sends to get the descriptor set

static int fail(const char* what, mimusb_result r) {
    const mimusb_error_detail* d = mimusb_last_error();
    fprintf(stderr, "%s: %s (%s: %s, offset %u)\n", what, mimusb_result_name(r), d->field,
            d->reason, (unsigned)d->offset);
    return 1;
}

// The MS OS 2.0 descriptor set: a 10-byte set header (wLength 10, type 0 MS_OS_20_SET_HEADER_
// DESCRIPTOR, dwWindowsVersion 0x06030000 = Windows 8.1, wTotalLength 30) and a 20-byte compatible
// ID feature descriptor (wLength 20, type 3, CompatibleID "WINUSB", SubCompatibleID empty).
static const uint8_t kDescriptorSet[30] = {10, 0, 0, 0, 0x00, 0x00, 0x03, 0x06, 30,  0,
                                           20, 0, 3, 0, 'W',  'I',  'N',  'U',  'S', 'B',
                                           0,  0, 0, 0, 0,    0,    0,    0,    0,   0};

// The BOS descriptor: the 5-byte header and one 28-byte platform capability descriptor (bLength 28,
// type 0x10 device capability, capability type 5 platform, reserved 0, the UUID in little-endian
// byte order, dwWindowsVersion, the length of the descriptor set, the vendor code, 0 for
// bAltEnumCode).
static const uint8_t kBos[33] = {5,    0x0F, 33,   0,    1,           28,   0x10, 0x05, 0,
                                 0xDF, 0x60, 0xDD, 0xD8, 0x89,        0x45, 0xC7, 0x4C, 0x9C,
                                 0xD2, 0x65, 0x9D, 0x9E, 0x64,        0x8A, 0x9F, 0x00, 0x00,
                                 0x03, 0x06, 30,   0,    VENDOR_CODE, 0};

static const uint8_t kDevice[18] = {18,   1,    0x10, 0x02, 0,    0, 0, 64, 0x09,
                                    0x12, 0x01, 0x00, 0x00, 0x01, 1, 2, 0,  1};
static const uint8_t kConfiguration[32] = {9,  2, 32, 0,    1, 1,    0, 0x80, 50, 9,    4,
                                           0,  0, 2,  0xFF, 1, 2,    0, 7,    5,  0x81, 2,
                                           64, 0, 0,  7,    5, 0x01, 2, 64,   0,  0};
static const uint16_t kManufacturer[] = {'A', 'c', 'm', 'e'};
static const uint16_t kProduct[] = {'W', 'i', 'n', 'U', 'S', 'B'};

static void MIMUSB_CALL on_event(void* user, const mimusb_event* event) {
    (void)user;
    if (event->kind == MIMUSB_EVENT_ENDPOINT_DATA)
        printf("target sent %zu bytes on endpoint 0x%02X\n", event->length,
               (unsigned)event->endpoint_address);
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
    example_fill_device_config(&config, mimusb_discovery_get(found, 0)->baud_rate, "unused");
    config.transport.event_sink = on_event;
    mimusb_device* device = NULL;
    r = mimusb_device_open(mimusb_discovery_get(found, 0)->port_name, &config, &device);
    mimusb_discovery_destroy(found);
    if (r != MIMUSB_OK)
        return fail("device_open", r);

    if ((r = mimusb_device_set_string(device, 1, 0x0409, kManufacturer, 4)) != MIMUSB_OK)
        return fail("set_string", r);
    if ((r = mimusb_device_set_string(device, 2, 0x0409, kProduct, 6)) != MIMUSB_OK)
        return fail("set_string", r);
    mimusb_endpoint_policy policy;
    policy.on_full = MIMUSB_FULL_WAIT;
    policy.depth = 4;
    if ((r = mimusb_device_endpoint_set_policy(device, 0x81, &policy,
                                               EXAMPLE_CONTROL_TIMEOUT_MS)) != MIMUSB_OK)
        return fail("endpoint_set_policy", r);

    // The rule of the Device that answers the vendor request with the descriptor set. The board
    // answers it by itself, so Windows gets it within the time USB allows.
    mimusb_control_rule rule;
    memset(&rule, 0, sizeof(rule));
    rule.request_type_mask = 0xFF;
    rule.request_type_value = 0xC0;
    rule.request_mask = 0xFF;
    rule.request_value = VENDOR_CODE;
    rule.index_mask = 0xFFFF;
    rule.index_value = 0x0007; // MS_OS_20_DESCRIPTOR_INDEX
    rule.action = MIMUSB_CONTROL_ANSWER;
    rule.data = kDescriptorSet;
    rule.length = sizeof(kDescriptorSet);
    if ((r = mimusb_device_set_control_rules(device, 0, &rule, 1, EXAMPLE_CONTROL_TIMEOUT_MS)) !=
        MIMUSB_OK)
        return fail("set_control_rules", r);

    mimusb_device_descriptors descriptors;
    descriptors.struct_size = sizeof(descriptors);
    descriptors.reserved = 0;
    descriptors.device = kDevice; // bcdUSB 0x0210: a host asks for the BOS descriptor
    descriptors.device_length = sizeof(kDevice);
    descriptors.configuration = kConfiguration;
    descriptors.configuration_length = sizeof(kConfiguration);
    descriptors.bos = kBos;
    descriptors.bos_length = sizeof(kBos);
    mimusb_commit_timeouts timeouts;
    example_fill_commit_timeouts(&timeouts);
    if ((r = mimusb_device_set_descriptors(device, &descriptors, &timeouts)) != MIMUSB_OK)
        return fail("set_descriptors", r);
    if ((r = mimusb_device_wait_ready(device, 5000)) != MIMUSB_OK)
        return fail("wait_ready", r);

    printf("presenting a WinUSB device for 30 seconds\n");
    sleep_ms(30000);

    if ((r = mimusb_device_clear_descriptors(device, &timeouts)) != MIMUSB_OK)
        return fail("clear_descriptors", r);
    if ((r = mimusb_device_close(device, 500, 1000)) != MIMUSB_OK)
        return fail("device_close", r);
    return 0;
}
