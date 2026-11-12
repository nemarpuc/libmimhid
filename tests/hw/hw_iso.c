// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
// A Node with an isochronous IN endpoint in alternate setting 1 (setting 0 has none) on a real
// board whose target is this machine: the host side is libmimusb, the target side is libusb.
//
//   hw_iso
//
// A: setting 0 has no endpoint; the target selects setting 1 and the program hears
//    MIMUSB_EVENT_INTERFACE_ALTERNATE with the Node's id.
// B: 100 packets written by the program arrive as isochronous packets in order and whole.
// C: back to setting 0 (the endpoint is gone), and to setting 1 again.
#include "example_values.h"
#include <libusb.h>
#include <mimusb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PACKET 192
#define PACKETS 100
#define FRAMES 400

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_alt_events, g_last_alt = -1, g_last_node = -1;

static void on_event(void* user, const mimusb_event* e) {
    (void)user;
    pthread_mutex_lock(&g_lock);
    if (e->kind == MIMUSB_EVENT_INTERFACE_ALTERNATE) {
        g_alt_events++;
        g_last_alt = (int)e->value;
        g_last_node = e->node_id;
    }
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
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

static volatile int g_done;
static void LIBUSB_CALL on_transfer(struct libusb_transfer* t) {
    (void)t;
    g_done = 1;
}

static void* feeder(void* arg) {
    mimusb_node* node = arg;
    for (int i = 0; i < PACKETS; i++) {
        uint8_t m[PACKET];
        memset(m, i + 1, sizeof(m));
        m[0] = (uint8_t)(i + 1);
        mimusb_result r = mimusb_node_endpoint_write(node, 0x82, m, sizeof(m), 2000);
        if (r != MIMUSB_OK) {
            printf("  feeder: write %d: %s\n", i, mimusb_result_name(r));
            fails++;
            break;
        }
    }
    return NULL;
}

int main(void) {
    mimusb_discovery* found = NULL;
    mimusb_result r = mimusb_discover(3000, &found);
    if (r != MIMUSB_OK || mimusb_discovery_count(found) == 0) {
        printf("no board\n");
        return 1;
    }
    mimusb_device_config config;
    example_fill_device_config(&config, mimusb_discovery_get(found, 0)->baud_rate, "HW iso test");
    config.transport.event_sink = on_event;
    mimusb_device* device = NULL;
    r = mimusb_device_open(mimusb_discovery_get(found, 0)->port_name, &config, &device);
    mimusb_discovery_destroy(found);
    if (r != MIMUSB_OK) {
        printf("open: %s %s\n", mimusb_result_name(r), mimusb_last_error()->reason);
        return 1;
    }
    mimusb_usb_endpoint ep;
    memset(&ep, 0, sizeof(ep));
    ep.address = 0x82;
    ep.transfer_type = MIMUSB_TRANSFER_ISOCHRONOUS;
    ep.max_packet_size = PACKET;
    ep.interval = 1;
    ep.sync_type = MIMUSB_ISO_SYNC_ASYNCHRONOUS;
    ep.policy.on_full = MIMUSB_FULL_WAIT;
    ep.policy.depth = 8;
    const mimusb_usb_item items1[1] = {mimusb_usb_item_make_endpoint(ep)};
    const mimusb_usb_setting settings[2] = {{NULL, 0}, {items1, 1}};
    mimusb_usb_interface itf;
    memset(&itf, 0, sizeof(itf));
    itf.number = 0;
    itf.class_code = 0xFF;
    itf.name = "Stream";
    itf.settings = settings;
    itf.setting_count = 2;
    mimusb_node_config nc;
    mimusb_node_config_init(&nc);
    nc.interfaces = &itf;
    nc.interface_count = 1;
    mimusb_commit_timeouts to;
    example_fill_commit_timeouts(&to);
    mimusb_node* node = NULL;
    r = mimusb_node_open(device, &nc, &to, &node);
    if (r != MIMUSB_OK) {
        printf("node_open: %s %s\n", mimusb_result_name(r), mimusb_last_error()->reason);
        return 1;
    }
    if (mimusb_device_wait_ready(device, 5000) != MIMUSB_OK) {
        printf("wait_ready failed\n");
        return 1;
    }
    libusb_context* ctx = NULL;
    libusb_init(&ctx);
    libusb_device_handle* h = NULL;
    for (int i = 0; i < 50 && !h; i++) {
        h = libusb_open_device_with_vid_pid(ctx, 0x1209, 0x0001);
        if (!h)
            sleep_ms(100);
    }
    if (!h) {
        printf("libusb: cannot open 1209:0001 (permissions?)\n");
        return 1;
    }
    int e = libusb_claim_interface(h, 0);
    if (e) {
        printf("claim: %s\n", libusb_error_name(e));
        return 1;
    }

    printf("A: setting 0 has no endpoint, setting 1 has the isochronous one\n");
    struct libusb_config_descriptor* cd = NULL;
    libusb_get_active_config_descriptor(libusb_get_device(h), &cd);
    int eps0 = -1, eps1 = -1;
    for (int a = 0; cd && a < cd->interface[0].num_altsetting; a++) {
        const struct libusb_interface_descriptor* d = &cd->interface[0].altsetting[a];
        if (d->bAlternateSetting == 0)
            eps0 = d->bNumEndpoints;
        if (d->bAlternateSetting == 1) {
            eps1 = d->bNumEndpoints;
            CHECK(d->bNumEndpoints == 1 && d->endpoint[0].bEndpointAddress == 0x82 &&
                      (d->endpoint[0].bmAttributes & 3) == 1 &&
                      ((d->endpoint[0].bmAttributes >> 2) & 3) == MIMUSB_ISO_SYNC_ASYNCHRONOUS &&
                      d->endpoint[0].wMaxPacketSize == PACKET && d->endpoint[0].bInterval == 1,
                  "setting 1 endpoint is not as given");
        }
    }
    CHECK(eps0 == 0 && eps1 == 1, "endpoints per setting: %d and %d", eps0, eps1);
    printf("  settings: %d endpoints in 0, %d in 1\n", eps0, eps1);
    if (cd)
        libusb_free_config_descriptor(cd);

    e = libusb_set_interface_alt_setting(h, 0, 1);
    CHECK(e == 0, "set alt 1: %s", libusb_error_name(e));
    sleep_ms(100);
    pthread_mutex_lock(&g_lock);
    CHECK(g_last_alt == 1 && g_last_node == mimusb_node_id(node),
          "INTERFACE_ALTERNATE: alt %d node %d (want 1 and %d)", g_last_alt, g_last_node,
          mimusb_node_id(node));
    pthread_mutex_unlock(&g_lock);

    printf("B: %d packets of %d bytes\n", PACKETS, PACKET);
    struct libusb_transfer* t = libusb_alloc_transfer(FRAMES);
    static uint8_t buf[FRAMES * PACKET];
    libusb_fill_iso_transfer(t, h, 0x82, buf, sizeof(buf), FRAMES, on_transfer, NULL, 5000);
    libusb_set_iso_packet_lengths(t, PACKET);
    g_done = 0;
    pthread_t th;
    e = libusb_submit_transfer(t);
    CHECK(e == 0, "submit: %s", libusb_error_name(e));
    pthread_create(&th, NULL, feeder, node);
    while (!g_done)
        libusb_handle_events(ctx);
    pthread_join(th, NULL);
    int seen = 0, in_order = 1, whole = 1, last = 0;
    for (int i = 0; i < FRAMES; i++) {
        struct libusb_iso_packet_descriptor* p = &t->iso_packet_desc[i];
        if (p->actual_length == 0)
            continue;
        const uint8_t* d = libusb_get_iso_packet_buffer_simple(t, i);
        if (p->actual_length != PACKET || d[1] != d[0] || d[PACKET - 1] != d[0])
            whole = 0;
        if (d[0] <= last)
            in_order = 0;
        last = d[0];
        seen++;
    }
    printf("  %d of %d packets seen, last number %d\n", seen, PACKETS, last);
    CHECK(seen == PACKETS && last == PACKETS && in_order && whole,
          "seen %d (want %d), last %d, order %d, whole %d", seen, PACKETS, last, in_order, whole);
    libusb_free_transfer(t);

    printf("C: back to setting 0 and to setting 1 again\n");
    e = libusb_set_interface_alt_setting(h, 0, 0);
    CHECK(e == 0, "set alt 0: %s", libusb_error_name(e));
    sleep_ms(100);
    pthread_mutex_lock(&g_lock);
    CHECK(g_last_alt == 0, "INTERFACE_ALTERNATE after alt 0: %d", g_last_alt);
    pthread_mutex_unlock(&g_lock);
    int got = 0;
    uint8_t probe[PACKET];
    e = libusb_bulk_transfer(h, 0x82, probe, sizeof(probe), &got, 100);
    CHECK(e != 0, "the endpoint answers in setting 0");
    e = libusb_set_interface_alt_setting(h, 0, 1);
    CHECK(e == 0, "set alt 1 again: %s", libusb_error_name(e));
    sleep_ms(100);
    pthread_mutex_lock(&g_lock);
    CHECK(g_last_alt == 1 && g_alt_events >= 3, "events: %d, last alt %d", g_alt_events,
          g_last_alt);
    pthread_mutex_unlock(&g_lock);

    mimusb_link_counters counters;
    memset(&counters, 0, sizeof(counters));
    counters.struct_size = sizeof(counters);
    r = mimusb_device_link_counters(device, 0, 500, &counters);
    printf("link counters: %s bad_crc=%u framing=%u uart_overrun=%u ring_overrun=%u\n",
           mimusb_result_name(r), counters.bad_crc, counters.framing, counters.uart_overrun,
           counters.ring_overrun);
    CHECK(r == MIMUSB_OK && counters.bad_crc == 0 && counters.framing == 0 &&
              counters.uart_overrun == 0 && counters.ring_overrun == 0,
          "link errors");
    libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    printf("node_close: %s\n", mimusb_result_name(mimusb_node_close(node, &to, 1000)));
    printf("device_close: %s\n", mimusb_result_name(mimusb_device_close(device, 500, 1000)));
    printf(fails ? "FAILED (%d failure(s))\n" : "PASSED (%d failure(s))\n", fails);
    return fails ? 1 : 0;
}
