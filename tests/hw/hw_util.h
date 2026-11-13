// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
// What the class tests (hw_dfu, hw_printer, hw_midi) share: finding the one board, opening the
// Device, the libusb handle of the Device the board presents to this machine, and a check macro.
#ifndef HW_UTIL_H
#define HW_UTIL_H

#include "example_values.h"
#include <libusb.h>
#include <mimusb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...)                                                                              \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fails++;                                                                               \
            printf("  FAIL: " __VA_ARGS__);                                                        \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static void sleep_ms(int ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}

// Finds the board and opens it with the event sink and the product string given.
static mimusb_device* hw_open_device(const char* product, mimusb_event_sink sink) {
    mimusb_discovery* found = NULL;
    mimusb_result r = mimusb_discover(3000, &found);
    if (r != MIMUSB_OK || mimusb_discovery_count(found) == 0) {
        printf("no board\n");
        return NULL;
    }
    mimusb_device_config config;
    example_fill_device_config(&config, mimusb_discovery_get(found, 0)->baud_rate, product);
    config.transport.event_sink = sink;
    mimusb_device* device = NULL;
    r = mimusb_device_open(mimusb_discovery_get(found, 0)->port_name, &config, &device);
    mimusb_discovery_destroy(found);
    if (r != MIMUSB_OK) {
        printf("open: %s %s\n", mimusb_result_name(r), mimusb_last_error()->reason);
        return NULL;
    }
    return device;
}

// The Device the board presents, as this machine's libusb sees it (the pid.codes test pair).
static libusb_device_handle* hw_open_target(libusb_context** ctx) {
    libusb_init(ctx);
    libusb_device_handle* h = NULL;
    for (int i = 0; i < 100 && !h; i++) {
        h = libusb_open_device_with_vid_pid(*ctx, 0x1209, 0x0001);
        if (!h)
            sleep_ms(100);
    }
    if (h)
        libusb_set_auto_detach_kernel_driver(h, 1);
    return h;
}

static void hw_link_check(mimusb_device* device) {
    mimusb_link_counters lc;
    memset(&lc, 0, sizeof(lc));
    lc.struct_size = sizeof(lc);
    mimusb_result r = mimusb_device_link_counters(device, 0, 500, &lc);
    printf("link counters: %s bad_crc=%u framing=%u uart_overrun=%u ring_overrun=%u\n",
           mimusb_result_name(r), lc.bad_crc, lc.framing, lc.uart_overrun, lc.ring_overrun);
    CHECK(r == MIMUSB_OK && lc.bad_crc == 0 && lc.framing == 0 && lc.uart_overrun == 0 &&
              lc.ring_overrun == 0,
          "link errors");
}

static mimusb_usb_endpoint hw_bulk_endpoint(uint8_t address, uint32_t depth) {
    mimusb_usb_endpoint ep;
    memset(&ep, 0, sizeof(ep));
    ep.address = address;
    ep.transfer_type = MIMUSB_TRANSFER_BULK;
    ep.max_packet_size = 64;
    if (address & 0x80u) { // the wait policy is for IN endpoints; an OUT endpoint's stays zero
        ep.policy.on_full = MIMUSB_FULL_WAIT;
        ep.policy.depth = depth;
    }
    return ep;
}

#endif
