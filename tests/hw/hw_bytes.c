// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
// A device given as bytes (mimhid_device_set_descriptors), and a BOS descriptor, on a real board
// whose target is this machine: the host side is libmimhid, the target side is libusb. Types
// nothing into the session.
//
//   hw_bytes bytes   a vendor device from bytes with a BOS descriptor and two strings; the target
//                    reads back every descriptor, the vendor request that a rule of the Device
//                    answers, bulk IN and OUT.
//   hw_bytes bos     a Device composed from a Node (bcdUSB 0x0210) that is given a BOS with
//                    mimhid_device_set_bos; the target reads it back.
#include "example_values.h"
#include <libusb.h>
#include <mimhid.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t g_out[256];
static size_t g_out_len, g_out_packets;
static int g_out_node = -1, g_out_ep, g_out_itf;
static void on_event(void* user, const mimhid_event* e) {
    (void)user;
    if (e->kind != MIMHID_EVENT_ENDPOINT_DATA)
        return;
    pthread_mutex_lock(&g_lock);
    // One event per packet (USB 2.0 5.8.3): a transfer of 70 bytes is a packet of 64 and one of 6.
    if (g_out_len + e->length <= sizeof(g_out)) {
        memcpy(g_out + g_out_len, e->data, e->length);
        g_out_len += e->length;
    }
    g_out_packets++;
    g_out_node = e->node_id;
    g_out_ep = (int)e->endpoint_address;
    g_out_itf = (int)e->interface_number;
    pthread_mutex_unlock(&g_lock);
}
static void sleep_ms(int ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}
static int fails;
#define CHECK(c, ...)                                                                              \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fails++;                                                                               \
            printf("  FAIL: " __VA_ARGS__);                                                        \
        } else {                                                                                   \
            printf("  ok: " __VA_ARGS__);                                                          \
        }                                                                                          \
        printf("\n");                                                                              \
    } while (0)

// The bytes, as in examples/c/winusb_bulk.c: bcdUSB 0x0210, VID 0x1209 PID 0x0001, strings 1 and 2.
static const uint8_t kDevice[18] = {18,   1,    0x10, 0x02, 0,    0, 0, 64, 0x09,
                                    0x12, 0x01, 0x00, 0x00, 0x01, 1, 2, 0,  1};
static const uint8_t kConfig[32] = {9, 2, 32, 0, 1,    1, 0,  0x80, 50, 9, 4, 0,    0, 2,  0xFF, 1,
                                    2, 0, 7,  5, 0x81, 2, 64, 0,    0,  7, 5, 0x01, 2, 64, 0,    0};
static const uint8_t kBos[33] = {5,    0x0F, 33,   0,    1,    28,   0x10, 0x05, 0,    0xDF, 0x60,
                                 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C, 0x9C, 0xD2, 0x65, 0x9D, 0x9E,
                                 0x64, 0x8A, 0x9F, 0x00, 0x00, 0x03, 0x06, 30,   0,    1,    0};
static const uint8_t kSet[30] = {10, 0, 0, 0, 0x00, 0x00, 0x03, 0x06, 30,  0,
                                 20, 0, 3, 0, 'W',  'I',  'N',  'U',  'S', 'B',
                                 0,  0, 0, 0, 0,    0,    0,    0,    0,   0};
static const uint16_t kManufacturer[] = {'A', 'c', 'm', 'e'};
static const uint16_t kProduct[] = {'B', 'y', 't', 'e', 's'};

static libusb_device_handle* open_target(libusb_context* ctx) {
    libusb_device_handle* h = NULL;
    for (int i = 0; i < 50 && !h; i++) {
        h = libusb_open_device_with_vid_pid(ctx, 0x1209, 0x0001);
        if (!h)
            sleep_ms(100);
    }
    return h;
}

static int get_descriptor(libusb_device_handle* h, uint16_t value, uint16_t index, uint8_t* out,
                          int cap) {
    return libusb_control_transfer(h, 0x80, 6, value, index, out, (uint16_t)cap, 2000);
}

static int run_bytes(mimhid_device* device) {
    mimhid_result r;
    mimhid_commit_timeouts to;
    example_fill_commit_timeouts(&to);
    CHECK((r = mimhid_device_set_string(device, 1, 0x0409, kManufacturer, 4)) == MIMHID_OK,
          "set_string 1: %s", mimhid_result_name(r));
    CHECK((r = mimhid_device_set_string(device, 2, 0x0409, kProduct, 5)) == MIMHID_OK,
          "set_string 2: %s", mimhid_result_name(r));
    mimhid_endpoint_policy p = {MIMHID_FULL_WAIT, 4};
    CHECK((r = mimhid_device_endpoint_set_policy(device, 0x81, &p, 500)) == MIMHID_OK, "policy: %s",
          mimhid_result_name(r));
    mimhid_control_rule rule;
    memset(&rule, 0, sizeof(rule));
    rule.request_type_mask = 0xFF;
    rule.request_type_value = 0xC0;
    rule.request_mask = 0xFF;
    rule.request_value = 1;
    rule.index_mask = 0xFFFF;
    rule.index_value = 7;
    rule.action = MIMHID_CONTROL_ANSWER;
    rule.data = kSet;
    rule.length = sizeof(kSet);
    CHECK((r = mimhid_device_set_control_rules(device, 0, &rule, 1, 500)) == MIMHID_OK, "rule: %s",
          mimhid_result_name(r));
    mimhid_device_descriptors d = {sizeof(d),       0,    kDevice,     sizeof(kDevice), kConfig,
                                   sizeof(kConfig), kBos, sizeof(kBos)};
    r = mimhid_device_set_descriptors(device, &d, &to);
    CHECK(r == MIMHID_OK, "set_descriptors: %s %s", mimhid_result_name(r),
          mimhid_last_error()->reason);
    if (r != MIMHID_OK)
        return 1;
    CHECK((r = mimhid_device_wait_ready(device, 5000)) == MIMHID_OK, "mounted: %s",
          mimhid_result_name(r));

    libusb_context* ctx = NULL;
    libusb_init(&ctx);
    libusb_device_handle* h = open_target(ctx);
    if (!h) {
        printf("libusb: cannot open 1209:0001\n");
        return 1;
    }
    uint8_t b[512];
    int n;
    n = get_descriptor(h, 0x0100, 0, b, 18);
    CHECK(n == 18 && memcmp(b, kDevice, 18) == 0, "device descriptor byte for byte (%d)", n);
    n = get_descriptor(h, 0x0200, 0, b, 255);
    CHECK(n == 32 && memcmp(b, kConfig, 32) == 0, "configuration descriptor byte for byte (%d)", n);
    n = get_descriptor(h, 0x0F00, 0, b, 255);
    CHECK(n == 33 && memcmp(b, kBos, 33) == 0, "BOS descriptor byte for byte (%d)", n);
    n = get_descriptor(h, 0x0301, 0x0409, b, 255);
    CHECK(n == 10 && b[1] == 3 && b[2] == 'A' && b[4] == 'c' && b[8] == 'e', "string 1 (%d)", n);
    n = get_descriptor(h, 0x0302, 0x0409, b, 255);
    CHECK(n == 12 && b[2] == 'B' && b[10] == 's', "string 2 (%d)", n);
    // The vendor request the rule of the Device answers by itself.
    n = libusb_control_transfer(h, 0xC0, 1, 0, 7, b, 255, 2000);
    CHECK(n == 30 && memcmp(b, kSet, 30) == 0, "MS OS 2.0 descriptor set from the rule (%d)", n);
    n = libusb_control_transfer(h, 0xC0, 2, 0, 7, b, 255, 2000);
    CHECK(n < 0, "a request no rule matches is stalled (%d)", n);
    int e = libusb_claim_interface(h, 0);
    CHECK(e == 0, "claim interface 0: %s", libusb_error_name(e));
    // Bulk IN: the host writes, the target reads, with the boundaries kept.
    uint8_t msg[100];
    for (int i = 0; i < 100; i++)
        msg[i] = (uint8_t)(i * 3 + 1);
    int got = 0;
    r = mimhid_device_endpoint_write(device, 0x81, msg, 100, 2000);
    CHECK(r == MIMHID_OK, "endpoint_write: %s", mimhid_result_name(r));
    e = libusb_bulk_transfer(h, 0x81, b, 512, &got, 2000);
    CHECK(e == 0 && got == 100 && memcmp(b, msg, 100) == 0, "bulk IN of 100 bytes (%d, %d)", e,
          got);
    r = mimhid_device_endpoint_write(device, 0x81, msg, 0, 2000);
    e = libusb_bulk_transfer(h, 0x81, b, 512, &got, 2000);
    CHECK(r == MIMHID_OK && e == 0 && got == 0, "zero-length packet (%d, %d)", e, got);
    // Bulk OUT: an event with node id 0.
    int sent = 0;
    e = libusb_bulk_transfer(h, 0x01, msg, 70, &sent, 2000);
    sleep_ms(200);
    pthread_mutex_lock(&g_lock);
    CHECK(e == 0 && sent == 70 && g_out_len == 70 && g_out_packets == 2 &&
              memcmp(g_out, msg, 70) == 0 && g_out_node == 0 && g_out_ep == 1 && g_out_itf == 0,
          "bulk OUT of 70 bytes as two packets, events of node %d, endpoint %d, interface %d "
          "(transfer %s, %d bytes sent, %zu received)",
          g_out_node, g_out_ep, g_out_itf, libusb_error_name(e), sent, g_out_len);
    pthread_mutex_unlock(&g_lock);
    // Halt of an IN endpoint, then clear.
    CHECK((r = mimhid_device_endpoint_stall(device, 0x81, 500)) == MIMHID_OK, "stall: %s",
          mimhid_result_name(r));
    e = libusb_bulk_transfer(h, 0x81, b, 512, &got, 300);
    CHECK(e == LIBUSB_ERROR_PIPE, "a read of the halted endpoint stalls (%s)",
          libusb_error_name(e));
    CHECK((r = mimhid_device_endpoint_clear_stall(device, 0x81, 500)) == MIMHID_OK,
          "clear stall: %s", mimhid_result_name(r));
    libusb_clear_halt(h, 0x81);
    libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    CHECK((r = mimhid_device_clear_descriptors(device, &to)) == MIMHID_OK, "clear_descriptors: %s",
          mimhid_result_name(r));
    return 0;
}

static int run_bos(mimhid_device* device, const char* port, uint32_t baud) {
    mimhid_result r;
    mimhid_commit_timeouts to;
    example_fill_commit_timeouts(&to);
    mimhid_usb_endpoint eps[2];
    memset(eps, 0, sizeof(eps));
    eps[0].address = 0x81;
    eps[0].transfer_type = MIMHID_TRANSFER_BULK;
    eps[0].max_packet_size = 64;
    eps[0].policy.on_full = MIMHID_FULL_WAIT;
    eps[0].policy.depth = 4;
    eps[1].address = 0x01;
    eps[1].transfer_type = MIMHID_TRANSFER_BULK;
    eps[1].max_packet_size = 64;
    mimhid_usb_interface itf;
    memset(&itf, 0, sizeof(itf));
    itf.class_code = 0xFF;
    itf.subclass = 1;
    itf.protocol = 2;
    itf.endpoints = eps;
    itf.endpoint_count = 2;
    mimhid_node_config nc;
    mimhid_node_config_init(&nc);
    nc.interfaces = &itf;
    nc.interface_count = 1;
    CHECK((r = mimhid_device_set_bos(device, kBos, sizeof(kBos))) == MIMHID_OK, "set_bos: %s",
          mimhid_result_name(r));
    {
        // The BOS names the Microsoft OS 2.0 platform capability, so the host asks for the
        // descriptor set: answer it.
        mimhid_control_rule rule;
        memset(&rule, 0, sizeof(rule));
        rule.request_type_mask = 0xFF;
        rule.request_type_value = 0xC0;
        rule.request_mask = 0xFF;
        rule.request_value = 1;
        rule.index_mask = 0xFFFF;
        rule.index_value = 7;
        rule.action = MIMHID_CONTROL_ANSWER;
        rule.data = kSet;
        rule.length = sizeof(kSet);
        mimhid_device_set_control_rules(device, 0, &rule, 1, 500);
    }
    mimhid_node* node = NULL;
    r = mimhid_node_open(device, &nc, &to, &node);
    CHECK(r == MIMHID_OK, "node_open with bcdUSB 0x0210: %s %s", mimhid_result_name(r),
          mimhid_last_error()->reason);
    if (r != MIMHID_OK)
        return 1;
    CHECK((r = mimhid_device_wait_ready(device, 5000)) == MIMHID_OK, "mounted: %s",
          mimhid_result_name(r));
    libusb_context* ctx = NULL;
    libusb_init(&ctx);
    libusb_device_handle* h = open_target(ctx);
    if (!h) {
        printf("libusb: cannot open 1209:0001\n");
        return 1;
    }
    uint8_t b[512];
    int n = get_descriptor(h, 0x0F00, 0, b, 255);
    CHECK(n == 33 && memcmp(b, kBos, 33) == 0,
          "BOS descriptor of a composed Device byte for byte (%d)", n);
    libusb_close(h);
    libusb_exit(ctx);
    // A configuration with bcdUSB 0x0210 and no BOS cannot be enumerated (docs/LIMITS.md), so the
    // BOS is only removed here, for the node_close commit that takes the device off the bus.
    CHECK((r = mimhid_device_set_bos(device, NULL, 0)) == MIMHID_OK, "bos removed: %s",
          mimhid_result_name(r));
    CHECK((r = mimhid_node_close(node, &to, 1000)) == MIMHID_OK, "node_close: %s",
          mimhid_result_name(r));
    (void)port;
    (void)baud;
    return 0;
}

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "bytes";
    mimhid_discovery* found = NULL;
    if (mimhid_discover(3000, &found) != MIMHID_OK || mimhid_discovery_count(found) == 0) {
        printf("no board\n");
        return 1;
    }
    mimhid_device_config config;
    example_fill_device_config(&config, mimhid_discovery_get(found, 0)->baud_rate, "HW bytes test");
    config.transport.event_sink = on_event;
    if (!strcmp(mode, "bos"))
        config.usb_device.bcd_usb = 0x0210; // a host asks for the BOS from 0x0201
    mimhid_device* device = NULL;
    mimhid_result r =
        mimhid_device_open(mimhid_discovery_get(found, 0)->port_name, &config, &device);
    mimhid_discovery_destroy(found);
    if (r != MIMHID_OK) {
        printf("open: %s %s\n", mimhid_result_name(r), mimhid_last_error()->reason);
        return 1;
    }
    int rc = !strcmp(mode, "bos") ? run_bos(device, NULL, 0) : run_bytes(device);
    CHECK((r = mimhid_device_close(device, 500, 1000)) == MIMHID_OK, "close: %s",
          mimhid_result_name(r));
    printf("%s\n", fails == 0 && rc == 0 ? "PASSED" : "FAILED");
    return fails == 0 && rc == 0 ? 0 : 1;
}
