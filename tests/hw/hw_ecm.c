// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
// A network adapter, USB CDC Ethernet Control Model (class 0x02, subclass 0x06; USB CDC ECM 1.2)
// on a real board whose target is this machine. The program is the other end of the wire: it
// answers ARP and ICMP echo for the address 10.9.9.2, and the board knows nothing of Ethernet; it
// only places the two interfaces (an interrupt endpoint for the notification, and a data
// interface with no endpoints in setting 0 and a bulk pair in setting 1) and moves the packets.
//
//   hw_ecm
//
// A: the kernel's cdc_ether binds and makes a network interface with the MAC address the program
//    gave in its string descriptor.
// B: with the kernel's driver set aside, this program is the host with libusb: setting 1 of the
//    data interface, the NETWORK_CONNECTION notification, an ARP request answered, and ICMP echo
//    requests of frame lengths from 60 to 1514 bytes answered with the same bytes, among them
//    lengths that are exact multiples of the 64-byte packet (a frame the zero-length packet ends).
#include "example_values.h"
#include <dirent.h>
#include <libusb.h>
#include <mimusb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { EP_NOTIFY = 0x81, EP_IN = 0x82, EP_OUT = 0x02, FRAME_MAX = 1600, QUEUE = 64 };

static const uint8_t kMac[6] = {0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
static const uint8_t kIp[4] = {10, 9, 9, 2};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static uint8_t g_assembly[FRAME_MAX];
static size_t g_assembly_len;
static uint8_t g_queue[QUEUE][FRAME_MAX];
static size_t g_queue_len[QUEUE];
static unsigned g_head, g_tail;
static int g_stop, g_notify_due;
static unsigned g_frames_in, g_arp, g_echo, g_other, g_oversize;

// A frame ends with a packet shorter than the endpoint's (USB CDC ECM 1.2 3.3.1): a zero-length one
// when its length is a multiple of 64.
static void on_event(void* user, const mimusb_event* e) {
    (void)user;
    pthread_mutex_lock(&g_lock);
    if (e->kind == MIMUSB_EVENT_ENDPOINT_DATA && e->endpoint_address == EP_OUT) {
        if (g_assembly_len + e->length <= FRAME_MAX)
            memcpy(g_assembly + g_assembly_len, e->data, e->length);
        g_assembly_len += e->length;
        if (e->length < 64) {
            if (g_assembly_len <= FRAME_MAX && g_assembly_len > 0 &&
                g_head - g_tail < QUEUE) {
                memcpy(g_queue[g_head % QUEUE], g_assembly, g_assembly_len);
                g_queue_len[g_head % QUEUE] = g_assembly_len;
                g_head++;
                g_frames_in++;
            } else if (g_assembly_len > FRAME_MAX) {
                g_oversize++;
            }
            g_assembly_len = 0;
        }
    } else if (e->kind == MIMUSB_EVENT_INTERFACE_ALTERNATE && e->interface_number == 1 &&
               e->value == 1) {
        g_notify_due = 1;
    }
    pthread_cond_broadcast(&g_cond);
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
static int fails;
#define CHECK(c, ...)                                                                              \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fails++;                                                                               \
            printf("  FAIL: " __VA_ARGS__);                                                        \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

static uint16_t checksum(const uint8_t* p, size_t n) {
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < n; i += 2)
        sum += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (n & 1U)
        sum += (uint32_t)p[n - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFFU) + (sum >> 16);
    return (uint16_t)~sum;
}

static mimusb_node* g_node;

static void send_frame(const uint8_t* frame, size_t n) {
    mimusb_result r = mimusb_node_endpoint_write(g_node, EP_IN, frame, n, 2000);
    if (r == MIMUSB_OK && n % 64 == 0)
        r = mimusb_node_endpoint_write(g_node, EP_IN, NULL, 0, 2000); // the zero-length packet
    if (r != MIMUSB_OK)
        printf("  responder: endpoint_write: %s\n", mimusb_result_name(r));
}

static void send_notification(void) {
    // USB CDC 1.2 6.3.1 NETWORK_CONNECTION: bmRequestType 0xA1, bNotification 0x00, wValue 1
    // (connected), wIndex the control interface, wLength 0.
    const uint8_t note[8] = {0xA1, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    mimusb_result r = mimusb_node_endpoint_write(g_node, EP_NOTIFY, note, sizeof(note), 1000);
    if (r != MIMUSB_OK)
        printf("  responder: notification: %s\n", mimusb_result_name(r));
}

static void answer(const uint8_t* f, size_t n) {
    static uint8_t reply[FRAME_MAX];
    if (n < 14) {
        g_other++;
        return;
    }
    const unsigned type = (unsigned)(f[12] << 8 | f[13]);
    if (type == 0x0806 && n >= 42 && f[20] == 0 && f[21] == 1 && memcmp(f + 38, kIp, 4) == 0) {
        memset(reply, 0, 60);
        memcpy(reply, f + 6, 6); // to the asker
        memcpy(reply + 6, kMac, 6);
        memcpy(reply + 12, f + 12, 2);
        memcpy(reply + 14, f + 14, 6); // hardware and protocol types and sizes
        reply[20] = 0;
        reply[21] = 2; // is-at
        memcpy(reply + 22, kMac, 6);
        memcpy(reply + 28, kIp, 4);
        memcpy(reply + 32, f + 22, 6);
        memcpy(reply + 38, f + 28, 4);
        g_arp++;
        send_frame(reply, 60);
    } else if (type == 0x0800 && n >= 42 && f[14] == 0x45 && f[23] == 1 && f[34] == 8 &&
               memcmp(f + 30, kIp, 4) == 0) {
        const size_t ip_len = (size_t)(f[16] << 8 | f[17]);
        if (14 + ip_len > n || ip_len < 28) {
            g_other++;
            return;
        }
        memcpy(reply, f, 14 + ip_len);
        memcpy(reply, f + 6, 6);
        memcpy(reply + 6, kMac, 6);
        memcpy(reply + 26, f + 30, 4); // source: the one that was asked
        memcpy(reply + 30, f + 26, 4);
        reply[24] = reply[25] = 0;
        const uint16_t ipsum = checksum(reply + 14, 20);
        reply[24] = (uint8_t)(ipsum >> 8);
        reply[25] = (uint8_t)ipsum;
        reply[34] = 0; // echo reply
        reply[36] = reply[37] = 0;
        const uint16_t isum = checksum(reply + 34, ip_len - 20);
        reply[36] = (uint8_t)(isum >> 8);
        reply[37] = (uint8_t)isum;
        g_echo++;
        size_t total = 14 + ip_len;
        if (total < 60) {
            memset(reply + total, 0, 60 - total);
            total = 60;
        }
        send_frame(reply, total);
    } else {
        g_other++;
    }
}

static void* responder(void* arg) {
    (void)arg;
    static uint8_t frame[FRAME_MAX];
    for (;;) {
        size_t n = 0;
        int notify = 0;
        pthread_mutex_lock(&g_lock);
        while (g_head == g_tail && !g_notify_due && !g_stop)
            pthread_cond_wait(&g_cond, &g_lock);
        if (g_stop) {
            pthread_mutex_unlock(&g_lock);
            return NULL;
        }
        if (g_notify_due) {
            g_notify_due = 0;
            notify = 1;
        }
        if (g_head != g_tail) {
            n = g_queue_len[g_tail % QUEUE];
            memcpy(frame, g_queue[g_tail % QUEUE], n);
            g_tail++;
        }
        pthread_mutex_unlock(&g_lock);
        if (notify)
            send_notification();
        if (n)
            answer(frame, n);
    }
}

static int find_net(char* name, size_t cap) {
    DIR* d = opendir("/sys/class/net");
    struct dirent* ent;
    int found = 0;
    while (d && (ent = readdir(d)) != NULL && !found) {
        char path[300], mac[32] = "";
        snprintf(path, sizeof(path), "/sys/class/net/%s/address", ent->d_name);
        FILE* f = fopen(path, "r");
        if (!f)
            continue;
        if (fgets(mac, sizeof(mac), f) && strncmp(mac, "02:aa:bb:cc:dd:ee", 17) == 0) {
            snprintf(name, cap, "%s", ent->d_name);
            found = 1;
        }
        fclose(f);
    }
    if (d)
        closedir(d);
    return found;
}

static libusb_device_handle* g_h;

// Reads one frame from the bulk IN endpoint: packets up to the first one shorter than 64 bytes.
static int read_frame(uint8_t* out, size_t cap, int timeout_ms) {
    size_t total = 0;
    for (;;) {
        uint8_t pkt[512];
        int n = 0;
        int e = libusb_bulk_transfer(g_h, EP_IN, pkt, (int)sizeof(pkt), &n, timeout_ms);
        if (e != 0)
            return -1;
        if (total + (size_t)n > cap)
            return -2;
        memcpy(out + total, pkt, (size_t)n);
        total += (size_t)n;
        if (n < 64)
            return (int)total;
    }
}

static int write_frame(const uint8_t* f, size_t n) {
    int sent = 0;
    int e = libusb_bulk_transfer(g_h, EP_OUT, (uint8_t*)f, (int)n, &sent, 2000);
    if (e != 0 || (size_t)sent != n)
        return -1;
    if (n % 64 == 0) {
        e = libusb_bulk_transfer(g_h, EP_OUT, (uint8_t*)f, 0, &sent, 2000); // zero-length packet
        if (e != 0)
            return -1;
    }
    return 0;
}

// An Ethernet II frame of total length `total` carrying an ICMP echo request to kIp.
static size_t make_echo(uint8_t* f, size_t total, uint16_t seq) {
    const size_t ip_len = total - 14;
    memset(f, 0, total);
    memcpy(f, kMac, 6);
    static const uint8_t host_mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    memcpy(f + 6, host_mac, 6);
    f[12] = 0x08;
    f[13] = 0x00;
    f[14] = 0x45;
    f[16] = (uint8_t)(ip_len >> 8);
    f[17] = (uint8_t)ip_len;
    f[18] = (uint8_t)(seq >> 8);
    f[19] = (uint8_t)seq;
    f[22] = 64;
    f[23] = 1;
    f[26] = 10; f[27] = 9; f[28] = 9; f[29] = 1;
    memcpy(f + 30, kIp, 4);
    const uint16_t ipsum = checksum(f + 14, 20);
    f[24] = (uint8_t)(ipsum >> 8);
    f[25] = (uint8_t)ipsum;
    f[34] = 8; // echo request
    f[38] = (uint8_t)(seq >> 8);
    f[39] = (uint8_t)seq;
    for (size_t i = 42; i < total; i++)
        f[i] = (uint8_t)(i * 5 + seq);
    const uint16_t isum = checksum(f + 34, ip_len - 20);
    f[36] = (uint8_t)(isum >> 8);
    f[37] = (uint8_t)isum;
    return total;
}

int main(void) {
    mimusb_discovery* found = NULL;
    mimusb_result r = mimusb_discover(3000, &found);
    if (r != MIMUSB_OK || mimusb_discovery_count(found) == 0) {
        printf("no board\n");
        return 1;
    }
    mimusb_device_config config;
    example_fill_device_config(&config, mimusb_discovery_get(found, 0)->baud_rate, "HW ECM test");
    config.transport.event_sink = on_event;
    mimusb_device* device = NULL;
    r = mimusb_device_open(mimusb_discovery_get(found, 0)->port_name, &config, &device);
    mimusb_discovery_destroy(found);
    if (r != MIMUSB_OK) {
        printf("open: %s %s\n", mimusb_result_name(r), mimusb_last_error()->reason);
        return 1;
    }

    // iMACAddress: 12 hex digits as a string descriptor (USB CDC ECM 1.2 5.4).
    uint8_t mac_index = 0;
    r = mimusb_device_add_string(device, "02AABBCCDDEE", &mac_index);
    CHECK(r == MIMUSB_OK, "add_string: %s", mimusb_result_name(r));

    // Functional descriptors (USB CDC 1.2 5.2.3): Header, Union, Ethernet Networking.
    const uint8_t header[5] = {5, 0x24, 0x00, 0x20, 0x01}; // bcdCDC 1.20
    const uint8_t union_[5] = {5, 0x24, 0x06, 0, 1};       // control 0, subordinate 1
    const uint8_t ethernet[13] = {13, 0x24, 0x0F, mac_index, 0, 0, 0, 0, 0xEA, 0x05, 0, 0, 0};
    // wMaxSegmentSize 1514 (0x05EA), no multicast filters, no power filters, no statistics.
    const uint8_t association[8] = {8, 0x0B, 0, 2, 0x02, 0x06, 0x00, 0};

    mimusb_usb_endpoint notify, bulk_in, bulk_out;
    memset(&notify, 0, sizeof(notify));
    notify.address = EP_NOTIFY;
    notify.transfer_type = MIMUSB_TRANSFER_INTERRUPT;
    notify.max_packet_size = 16;
    notify.interval = 16;
    notify.policy.on_full = MIMUSB_FULL_DROP_NEWEST;
    notify.policy.depth = 2;
    memset(&bulk_in, 0, sizeof(bulk_in));
    bulk_in.address = EP_IN;
    bulk_in.transfer_type = MIMUSB_TRANSFER_BULK;
    bulk_in.max_packet_size = 64;
    bulk_in.policy.on_full = MIMUSB_FULL_WAIT;
    bulk_in.policy.depth = 8;
    memset(&bulk_out, 0, sizeof(bulk_out));
    bulk_out.address = EP_OUT;
    bulk_out.transfer_type = MIMUSB_TRANSFER_BULK;
    bulk_out.max_packet_size = 64;
    const mimusb_usb_item comm_items[4] = {
        mimusb_usb_item_make_descriptor(header, sizeof(header)),
        mimusb_usb_item_make_descriptor(union_, sizeof(union_)),
        mimusb_usb_item_make_descriptor(ethernet, sizeof(ethernet)),
        mimusb_usb_item_make_endpoint(notify)};
    const mimusb_usb_item data_items[2] = {mimusb_usb_item_make_endpoint(bulk_in),
                                           mimusb_usb_item_make_endpoint(bulk_out)};
    const mimusb_usb_setting comm_setting = {comm_items, 4};
    // Setting 0 of the data interface has no endpoints: the host selects setting 1 to use them.
    const mimusb_usb_setting data_settings[2] = {{NULL, 0}, {data_items, 2}};
    mimusb_usb_interface itfs[2];
    memset(itfs, 0, sizeof(itfs));
    itfs[0].number = 0;
    itfs[0].class_code = 0x02;
    itfs[0].subclass = 0x06;
    itfs[0].protocol = 0x00;
    itfs[0].name = "HW ECM control";
    itfs[0].settings = &comm_setting;
    itfs[0].setting_count = 1;
    itfs[1].number = 1;
    itfs[1].class_code = 0x0A;
    itfs[1].name = "HW ECM data";
    itfs[1].settings = data_settings;
    itfs[1].setting_count = 2;
    // SET_ETHERNET_PACKET_FILTER (class, interface, 0x43) is accepted: the program takes every
    // frame it is sent anyway.
    mimusb_control_rule rule;
    memset(&rule, 0, sizeof(rule));
    rule.request_type_mask = 0xFF;
    rule.request_type_value = 0x21;
    rule.request_mask = 0xFF;
    rule.request_value = 0x43;
    rule.index_mask = 0xFFFF;
    rule.index_value = 0;
    rule.action = MIMUSB_CONTROL_ACCEPT;
    mimusb_node_config nc;
    mimusb_node_config_init(&nc);
    nc.interfaces = itfs;
    nc.interface_count = 2;
    nc.association = association;
    nc.association_length = sizeof(association);
    nc.rules = &rule;
    nc.rule_count = 1;
    mimusb_commit_timeouts to;
    example_fill_commit_timeouts(&to);
    r = example_node_open_interfaces(device, &nc, &to, &g_node);
    if (r != MIMUSB_OK) {
        printf("node_open: %s %s\n", mimusb_result_name(r), mimusb_last_error()->reason);
        return 1;
    }
    pthread_t thread;
    pthread_create(&thread, NULL, responder, NULL);
    if (mimusb_device_wait_ready(device, 5000) != MIMUSB_OK) {
        printf("wait_ready failed\n");
        return 1;
    }

    printf("A: the kernel's cdc_ether and its network interface\n");
    char net[64] = "";
    for (int i = 0; i < 100 && !net[0]; i++)
        if (!find_net(net, sizeof(net)))
            sleep_ms(100);
    CHECK(net[0], "no network interface with the address 02:aa:bb:cc:dd:ee appeared");
    if (net[0]) {
        char path[300], drv[300] = "";
        snprintf(path, sizeof(path), "/sys/class/net/%s/device/driver", net);
        ssize_t n = readlink(path, drv, sizeof(drv) - 1);
        const char* base = n > 0 ? strrchr(drv, '/') : NULL;
        printf("  interface %s, driver %s\n", net, base ? base + 1 : "?");
        CHECK(base && strcmp(base + 1, "cdc_ether") == 0, "the driver is %s", base ? base + 1 : "?");
        unsigned mtu = 0;
        snprintf(path, sizeof(path), "/sys/class/net/%s/mtu", net);
        FILE* f = fopen(path, "r");
        if (f) {
            if (fscanf(f, "%u", &mtu) != 1)
                mtu = 0;
            fclose(f);
        }
        printf("  mtu %u (wMaxSegmentSize 1514 less the 14-byte header: 1500)\n", mtu);
        CHECK(mtu == 1500, "mtu %u", mtu);
    }
    sleep_ms(500);

    printf("B: Ethernet frames with libusb\n");
    libusb_context* ctx = NULL;
    libusb_init(&ctx);
    for (int i = 0; i < 50 && !g_h; i++) {
        g_h = libusb_open_device_with_vid_pid(ctx, 0x1209, 0x0001);
        if (!g_h)
            sleep_ms(100);
    }
    if (!g_h) {
        printf("libusb: cannot open 1209:0001 (permissions?)\n");
        return 1;
    }
    libusb_set_auto_detach_kernel_driver(g_h, 1);
    int e = libusb_claim_interface(g_h, 0);
    CHECK(e == 0, "claim 0: %s", libusb_error_name(e));
    e = libusb_claim_interface(g_h, 1);
    CHECK(e == 0, "claim 1: %s", libusb_error_name(e));
    if (e != 0)
        return 1;
    // Drain what the kernel's driver left in the endpoints.
    {
        uint8_t junk[512];
        int n;
        while (libusb_bulk_transfer(g_h, EP_IN, junk, sizeof(junk), &n, 100) == 0) {
        }
        while (libusb_interrupt_transfer(g_h, EP_NOTIFY, junk, sizeof(junk), &n, 100) == 0) {
        }
    }
    pthread_mutex_lock(&g_lock);
    g_echo = g_arp = g_other = 0;
    pthread_mutex_unlock(&g_lock);

    e = libusb_set_interface_alt_setting(g_h, 1, 1);
    CHECK(e == 0, "alt setting 1 of the data interface: %s", libusb_error_name(e));
    uint8_t note[16];
    int n = 0;
    e = libusb_interrupt_transfer(g_h, EP_NOTIFY, note, sizeof(note), &n, 2000);
    CHECK(e == 0 && n == 8 && note[0] == 0xA1 && note[1] == 0x00 && note[2] == 0x01,
          "NETWORK_CONNECTION notification: %s, %d bytes", libusb_error_name(e), n);

    // ARP: who has 10.9.9.2? (from 10.9.9.1)
    static uint8_t tx[FRAME_MAX], rx[FRAME_MAX];
    static const uint8_t host_mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    memset(tx, 0, 60);
    memset(tx, 0xFF, 6);
    memcpy(tx + 6, host_mac, 6);
    tx[12] = 0x08;
    tx[13] = 0x06;
    tx[15] = 1;
    tx[16] = 0x08;
    tx[18] = 6;
    tx[19] = 4;
    tx[21] = 1;
    memcpy(tx + 22, host_mac, 6);
    tx[28] = 10; tx[29] = 9; tx[30] = 9; tx[31] = 1;
    memcpy(tx + 38, kIp, 4);
    CHECK(write_frame(tx, 60) == 0, "write of the ARP request");
    int got = read_frame(rx, sizeof(rx), 2000);
    CHECK(got == 60 && rx[12] == 0x08 && rx[13] == 0x06 && rx[21] == 2 &&
              memcmp(rx + 22, kMac, 6) == 0 && memcmp(rx + 28, kIp, 4) == 0 &&
              memcmp(rx, host_mac, 6) == 0,
          "ARP reply: %d bytes", got);

    // ICMP echo at frame lengths around and at multiples of the 64-byte packet.
    static const size_t lengths[] = {60, 64, 65, 100, 127, 128, 129, 590, 1000, 1472, 1500, 1514};
    int bad = 0;
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        const size_t len = lengths[i];
        make_echo(tx, len, (uint16_t)(i + 1));
        if (write_frame(tx, len) != 0) {
            bad++;
            CHECK(0, "write of a frame of %zu bytes", len);
            continue;
        }
        got = read_frame(rx, sizeof(rx), 2000);
        const size_t expect = len;
        int ok = got == (int)expect && rx[34] == 0 && memcmp(rx, host_mac, 6) == 0 &&
                 memcmp(rx + 26, kIp, 4) == 0 && checksum(rx + 14, 20) == 0 &&
                 checksum(rx + 34, expect - 34) == 0 &&
                 memcmp(rx + 42, tx + 42, expect - 42) == 0;
        if (!ok) {
            bad++;
            printf("  FAIL: echo of %zu bytes: got %d\n", len, got);
            fails++;
        }
    }
    printf("  %zu frame lengths echoed, %d wrong\n", sizeof(lengths) / sizeof(lengths[0]), bad);

    // Round trips of full frames, for the rate.
    int wrong = 0;
    double t0 = now_ms();
    for (int i = 0; i < 200; i++) {
        make_echo(tx, 1514, (uint16_t)(1000 + i));
        if (write_frame(tx, 1514) != 0 || read_frame(rx, sizeof(rx), 2000) != 1514 ||
            memcmp(rx + 42, tx + 42, 1514 - 42) != 0)
            wrong++;
    }
    double t1 = now_ms();
    CHECK(wrong == 0, "%d of 200 full-size round trips went wrong", wrong);
    printf("  200 round trips of 1514 bytes in %.0f ms (%.1f ms each, %.0f kB/s each way)\n",
           t1 - t0, (t1 - t0) / 200, 200 * 1514 / (t1 - t0));

    pthread_mutex_lock(&g_lock);
    printf("  the program received %u frames: %u ARP, %u echo, %u other, %u oversize\n",
           g_frames_in, g_arp, g_echo, g_other, g_oversize);
    CHECK(g_oversize == 0, "oversize frames");
    pthread_mutex_unlock(&g_lock);

    mimusb_link_counters lc;
    memset(&lc, 0, sizeof(lc));
    lc.struct_size = sizeof(lc);
    r = mimusb_device_link_counters(device, 0, 500, &lc);
    printf("link counters: %s bad_crc=%u framing=%u uart_overrun=%u ring_overrun=%u\n",
           mimusb_result_name(r), lc.bad_crc, lc.framing, lc.uart_overrun, lc.ring_overrun);
    CHECK(r == MIMUSB_OK && lc.bad_crc == 0 && lc.framing == 0 && lc.uart_overrun == 0 &&
              lc.ring_overrun == 0,
          "link errors");

    libusb_release_interface(g_h, 1);
    libusb_release_interface(g_h, 0);
    libusb_close(g_h);
    libusb_exit(ctx);
    pthread_mutex_lock(&g_lock);
    g_stop = 1;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    pthread_join(thread, NULL);
    mimusb_device_close(device, 500, 1000);
    printf(fails ? "FAILED: %d\n" : "OK\n", fails);
    return fails != 0;
}
