// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
// A vendor Node with a bulk IN and a bulk OUT endpoint on a real board whose target is this
// machine: the host side is libmimusb, the target side is libusb. Types nothing into the session.
//
//   hw_bulk wait|newest|oldest
//
// wait:   transfers of 1 to 292 bytes with their boundaries, a zero-length packet, long writes
//         without loss, packets from the target, stall and clear.
// newest, oldest: twenty transfers to a target that does not read, then what it reads.
#include "example_values.h"
#include <libusb.h>
#include <mimusb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t g_out[1 << 20];
static size_t g_out_len, g_out_packets;
static int g_halt_set, g_halt_cleared, g_lost;

static void on_event(void* user, const mimusb_event* e) {
    (void)user;
    pthread_mutex_lock(&g_lock);
    if (e->kind == MIMUSB_EVENT_ENDPOINT_DATA) {
        if (g_out_len + e->length <= sizeof(g_out))
            memcpy(g_out + g_out_len, e->data, e->length);
        g_out_len += e->length;
        g_out_packets++;
    } else if (e->kind == MIMUSB_EVENT_ENDPOINT_HALT) {
        if (e->value)
            g_halt_set++;
        else
            g_halt_cleared++;
    } else if (e->kind == MIMUSB_EVENT_TRANSFER_LOST) {
        g_lost += (int)e->value;
    }
    pthread_mutex_unlock(&g_lock);
}
static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static void sleep_ms(int ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}

// The target side reads the IN endpoint all the time, as a driver would.
static libusb_device_handle* g_h;
static volatile int g_reader_on;
static uint8_t g_rx[1 << 21];
static size_t g_rx_len;
static int g_rx_lens[4096], g_rx_transfers;
static void* reader(void* arg) {
    (void)arg;
    static uint8_t b[1 << 16];
    while (g_reader_on) {
        int got = 0;
        // A long timeout on purpose: a read that is cancelled while a transfer is arriving can
        // lose the packets the host controller had already taken (seen with a 50 ms timeout in a
        // VMware guest: the data the board sent was gone, with nothing wrong on the board).
        int e = libusb_bulk_transfer(g_h, 0x81, b, (int)sizeof(b), &got, 1000);
        if (e == 0 || (e == LIBUSB_ERROR_TIMEOUT && got > 0)) {
            pthread_mutex_lock(&g_lock);
            if (g_rx_len + got <= sizeof(g_rx))
                memcpy(g_rx + g_rx_len, b, got);
            g_rx_len += got;
            if (e == 0 && g_rx_transfers < 4096)
                g_rx_lens[g_rx_transfers++] = got;
            pthread_mutex_unlock(&g_lock);
        }
    }
    return NULL;
}
static void reset_rx(void) {
    pthread_mutex_lock(&g_lock);
    g_rx_len = 0;
    g_rx_transfers = 0;
    pthread_mutex_unlock(&g_lock);
}
static void wait_rx(size_t want, int ms) {
    double end = now_ms() + ms;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        size_t have = g_rx_len;
        pthread_mutex_unlock(&g_lock);
        if (have >= want || now_ms() > end)
            return;
        sleep_ms(2);
    }
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

int main(int argc, char** argv) {
    const char* policy_name = argc > 1 ? argv[1] : "wait";
    const uint8_t on_full = !strcmp(policy_name, "oldest")   ? MIMUSB_FULL_DROP_OLDEST
                            : !strcmp(policy_name, "newest") ? MIMUSB_FULL_DROP_NEWEST
                                                             : MIMUSB_FULL_WAIT;
    mimusb_discovery* found = NULL;
    mimusb_result r = mimusb_discover(3000, &found);
    if (r != MIMUSB_OK || mimusb_discovery_count(found) == 0) {
        printf("no board\n");
        return 1;
    }
    mimusb_device_config config;
    example_fill_device_config(&config, mimusb_discovery_get(found, 0)->baud_rate, "HW bulk test");
    config.transport.event_sink = on_event;
    printf("board on %s at %u baud\n", mimusb_discovery_get(found, 0)->port_name,
           mimusb_discovery_get(found, 0)->baud_rate);
    mimusb_device* device = NULL;
    r = mimusb_device_open(mimusb_discovery_get(found, 0)->port_name, &config, &device);
    mimusb_discovery_destroy(found);
    if (r != MIMUSB_OK) {
        printf("open: %s %s\n", mimusb_result_name(r), mimusb_last_error()->reason);
        return 1;
    }
    r = mimusb_device_set_loss_reporting(device, 1, 10, 500);
    if (r != MIMUSB_OK) {
        printf("loss reporting: %s\n", mimusb_result_name(r));
        return 1;
    }

    mimusb_usb_endpoint eps[2];
    memset(eps, 0, sizeof(eps));
    eps[0].address = 0x81;
    eps[0].transfer_type = MIMUSB_TRANSFER_BULK;
    eps[0].max_packet_size = 64;
    eps[0].policy.on_full = on_full;
    eps[0].policy.depth = 4;
    eps[1].address = 0x01;
    eps[1].transfer_type = MIMUSB_TRANSFER_BULK;
    eps[1].max_packet_size = 64;
    mimusb_usb_interface itf;
    memset(&itf, 0, sizeof(itf));
    itf.number = 0;
    itf.class_code = 0xFF;
    itf.name = "Data";
    const mimusb_usb_item items[2] = {mimusb_usb_item_make_endpoint(eps[0]),
                                      mimusb_usb_item_make_endpoint(eps[1])};
    const mimusb_usb_setting setting = {items, 2};
    itf.settings = &setting;
    itf.setting_count = 1;
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
    r = mimusb_device_wait_ready(device, 5000);
    if (r != MIMUSB_OK) {
        printf("wait_ready: %s\n", mimusb_result_name(r));
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
    printf("policy %s; target side opened\n", policy_name);
    uint8_t buf[4096];
    int got = 0;

    if (on_full == MIMUSB_FULL_WAIT) {
        // A: host -> target while the target reads all the time. Each write is one transfer.
        printf("A: IN transfers of 1..300 bytes\n");
        g_h = h;
        g_reader_on = 1;
        pthread_t th;
        pthread_create(&th, NULL, reader, NULL);
        size_t total = 0;
        uint8_t* sent = malloc(1 << 21);
        int n_msgs = 0;
        int lens[512];
        double t0 = now_ms();
        for (int len = 1; len <= 300; len += (len < 70 ? 1 : 37)) {
            if (len % 64 == 0)
                continue;
            uint8_t m[512];
            for (int i = 0; i < len; i++)
                m[i] = (uint8_t)(total + i * 7 + len);
            memcpy(sent + total, m, len);
            total += len;
            lens[n_msgs++] = len;
            r = mimusb_node_endpoint_write(node, 0x81, m, len, 2000);
            CHECK(r == MIMUSB_OK, "write len %d: %s", len, mimusb_result_name(r));
        }
        wait_rx(total, 3000);
        double t1 = now_ms();
        pthread_mutex_lock(&g_lock);
        CHECK(g_rx_len == total && memcmp(sent, g_rx, total) == 0, "stream differs (%zu of %zu)",
              g_rx_len, total);
        int same = g_rx_transfers == n_msgs;
        for (int i = 0; same && i < n_msgs; i++)
            same = g_rx_lens[i] == lens[i];
        CHECK(same, "transfer boundaries differ (%d transfers for %d writes)", g_rx_transfers,
              n_msgs);
        pthread_mutex_unlock(&g_lock);
        printf("  %d transfers, %zu bytes in %.1f ms\n", n_msgs, total, t1 - t0);

        // A2: exactly one packet, then a zero-length packet ends the transfer.
        reset_rx();
        uint8_t full[64];
        for (int i = 0; i < 64; i++)
            full[i] = (uint8_t)(200 + i);
        r = mimusb_node_endpoint_write(node, 0x81, full, 64, 2000);
        CHECK(r == MIMUSB_OK, "write 64");
        r = mimusb_node_endpoint_write(node, 0x81, full, 0, 2000);
        CHECK(r == MIMUSB_OK, "write zlp: %s", mimusb_result_name(r));
        wait_rx(64, 1000);
        sleep_ms(50);
        pthread_mutex_lock(&g_lock);
        CHECK(g_rx_len == 64 && g_rx_transfers == 1 && memcmp(g_rx, full, 64) == 0,
              "64 + zlp: %zu bytes in %d transfers", g_rx_len, g_rx_transfers);
        printf("  64 bytes + zero-length packet: %d transfer of %zu bytes\n", g_rx_transfers,
               g_rx_len);
        pthread_mutex_unlock(&g_lock);

        // A3: single writes of growing size; nothing may be lost with the wait policy.
        static uint8_t src[1 << 20];
        for (size_t i = 0; i < sizeof(src); i++)
            src[i] = (uint8_t)(i * 31 + (i >> 8));
        const size_t sizes[] = {250, 3000, 4090, 20000, 65530, 262100};
        for (unsigned k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
            reset_rx();
            pthread_mutex_lock(&g_lock);
            g_lost = 0;
            pthread_mutex_unlock(&g_lock);
            t0 = now_ms();
            r = mimusb_node_endpoint_write(node, 0x81, src, sizes[k], 5000);
            double tw = now_ms();
            wait_rx(sizes[k], 3000);
            t1 = now_ms();
            sleep_ms(30);
            pthread_mutex_lock(&g_lock);
            int ok = g_rx_len == sizes[k] && memcmp(src, g_rx, sizes[k]) == 0;
            size_t firstbad = 0;
            while (firstbad < g_rx_len && firstbad < sizes[k] && g_rx[firstbad] == src[firstbad])
                firstbad++;
            printf("  write %6zu: %s in %.0f ms; target got %zu (%d transfers) after %.0f ms; same "
                   "up to %zu; lost %d %s\n",
                   sizes[k], mimusb_result_name(r), tw - t0, g_rx_len, g_rx_transfers, t1 - t0,
                   firstbad, g_lost, ok ? "OK" : "<-- BAD");
            if (!ok)
                fails++;
            pthread_mutex_unlock(&g_lock);
            if (!ok) {
                mimusb_node_endpoint_flush(node, 0x81, 500);
                sleep_ms(200);
            }
        }
        g_reader_on = 0;
        pthread_join(th, NULL);
    }

    if (on_full != MIMUSB_FULL_WAIT) {
        // B: the target does not read. The first transfer goes on the bus; more keep arriving.
        printf("B: target not reading, 20 transfers of 8 bytes, depth 4\n");
        for (int i = 0; i < 20; i++) {
            uint8_t m[8];
            memset(m, i, 8);
            r = mimusb_node_endpoint_write(node, 0x81, m, 8, 100);
            CHECK(r == MIMUSB_OK, "write %d: %s", i, mimusb_result_name(r));
            sleep_ms(5);
        }
        sleep_ms(100);
        printf("  target then reads:");
        int first = -1, count = 0, last = -1;
        while (libusb_bulk_transfer(h, 0x81, buf, sizeof(buf), &got, 200) == 0) {
            printf(" %d", buf[0]);
            if (first < 0)
                first = buf[0];
            last = buf[0];
            count++;
            int ok = got == 8;
            for (int i = 1; i < got; i++)
                ok = ok && buf[i] == buf[0];
            CHECK(ok, "transfer %d is not whole (%d bytes)", buf[0], got);
        }
        pthread_mutex_lock(&g_lock);
        int lost = g_lost;
        pthread_mutex_unlock(&g_lock);
        printf("\n  %d read, lost reported %d\n", count, lost);
        CHECK(first == 0, "the transfer on the bus was not the first (%d)", first);
        if (on_full == MIMUSB_FULL_DROP_OLDEST)
            CHECK(last == 19, "the newest was not kept (%d)", last);
        else
            CHECK(last < 19, "the newest was not dropped (%d)", last);
        CHECK(count + lost == 20, "read %d + lost %d != 20", count, lost);
    }

    // C: target -> host, 200 packets; none lost, in order.
    printf("C: OUT stream\n");
    pthread_mutex_lock(&g_lock);
    g_out_len = 0;
    g_out_packets = 0;
    pthread_mutex_unlock(&g_lock);
    const int packets = 200;
    double t0 = now_ms();
    for (int p = 0; p < packets; p++) {
        uint8_t m[64];
        for (int i = 0; i < 64; i++)
            m[i] = (uint8_t)(p * 3 + i);
        e = libusb_bulk_transfer(h, 0x01, m, 64, &got, 2000);
        CHECK(e == 0 && got == 64, "out %d: %s", p, libusb_error_name(e));
    }
    double t1 = now_ms();
    sleep_ms(300);
    pthread_mutex_lock(&g_lock);
    size_t ol = g_out_len, op = g_out_packets;
    int bad = 0;
    for (int p = 0; p < packets && (size_t)(p + 1) * 64 <= ol; p++)
        for (int i = 0; i < 64; i++)
            bad += g_out[p * 64 + i] != (uint8_t)(p * 3 + i);
    pthread_mutex_unlock(&g_lock);
    CHECK(ol == (size_t)packets * 64 && bad == 0, "out stream: %zu bytes in %zu packets, %d bad",
          ol, op, bad);
    printf("  %d packets of 64 bytes in %.1f ms (%.0f kB/s), %zu bytes at the host\n", packets,
           t1 - t0, packets * 64 / (t1 - t0), ol);

    // D: stall from the program, clear from the target.
    printf("D: stall and clear\n");
    r = mimusb_node_endpoint_stall(node, 0x81, 500);
    CHECK(r == MIMUSB_OK, "stall: %s", mimusb_result_name(r));
    e = libusb_bulk_transfer(h, 0x81, buf, sizeof(buf), &got, 500);
    CHECK(e == LIBUSB_ERROR_PIPE, "read of a stalled endpoint: %s", libusb_error_name(e));
    e = libusb_clear_halt(h, 0x81);
    CHECK(e == 0, "clear_halt: %s", libusb_error_name(e));
    sleep_ms(100);
    pthread_mutex_lock(&g_lock);
    int cleared = g_halt_cleared;
    pthread_mutex_unlock(&g_lock);
    CHECK(cleared == 1, "ENDPOINT_HALT cleared events: %d", cleared);
    uint8_t after[5] = {9, 8, 7, 6, 5};
    r = mimusb_node_endpoint_write(node, 0x81, after, 5, 2000);
    CHECK(r == MIMUSB_OK, "write after clear: %s", mimusb_result_name(r));
    e = libusb_bulk_transfer(h, 0x81, buf, sizeof(buf), &got, 2000);
    CHECK(e == 0 && got == 5 && memcmp(buf, after, 5) == 0, "read after clear: %s got %d",
          libusb_error_name(e), got);

    mimusb_link_counters lc;
    memset(&lc, 0, sizeof(lc));
    lc.struct_size = sizeof(lc);
    r = mimusb_device_link_counters(device, 0, 500, &lc);
    printf("link counters: %s bad_crc=%u framing=%u uart_overrun=%u ring_overrun=%u\n",
           mimusb_result_name(r), lc.bad_crc, lc.framing, lc.uart_overrun, lc.ring_overrun);
    libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    r = mimusb_node_close(node, &to, 1000);
    CHECK(r == MIMUSB_OK, "node_close: %s", mimusb_result_name(r));
    r = mimusb_device_close(device, 500, 1000);
    CHECK(r == MIMUSB_OK, "device_close: %s", mimusb_result_name(r));
    printf("%s (%d failure(s))\n", fails ? "FAILED" : "PASSED", fails);
    return fails != 0;
}
