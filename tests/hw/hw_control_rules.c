// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
// An HID interface written by hand (a vendor usage page, so the session gets no input) on a real
// board whose target is this machine, to see every control rule action answered to a real host:
// ANSWER, ANSWER_LAST_SENT, ACCEPT, ASK, a stall for a request no rule matches, a rule's bytes
// replaced, and an Output report on the OUT endpoint. The target side is hidraw and libusb.
#include "example_values.h"
#include <dirent.h>
#include <fcntl.h>
#include <libusb.h>
#include <linux/hidraw.h>
#include <mimusb.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static const uint8_t rd[] = {0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01, 0x15, 0x00, 0x26, 0xFF,
                             0x00, 0x75, 0x08, 0x95, 0x08, 0x09, 0x01, 0x81, 0x02, 0x09, 0x02,
                             0x91, 0x02, 0x95, 0x04, 0x09, 0x03, 0xB1, 0x02, 0xC0};
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static mimusb_device* g_device;
static int g_setup_accepted, g_data_events, g_out_events, g_asked;
static uint8_t g_last_setup[8], g_last_data[64], g_last_out[64];
static size_t g_last_data_len, g_last_out_len;

static void on_event(void* user, const mimusb_event* e) {
    (void)user;
    pthread_mutex_lock(&g_lock);
    if (e->kind == MIMUSB_EVENT_CONTROL_SETUP) {
        if (e->length >= 8)
            memcpy(g_last_setup, e->data, 8);
        if (e->control_flags & MIMUSB_CONTROL_FLAG_ACCEPTED)
            g_setup_accepted++;
        if (e->control_flags & MIMUSB_CONTROL_FLAG_HELD) {
            g_asked++;
            static const uint8_t answer[] = "answered by the program";
            mimusb_device_control_reply(g_device, e->control_token, MIMUSB_CONTROL_ACTION_ANSWER,
                                        answer, sizeof(answer), 100);
        }
    } else if (e->kind == MIMUSB_EVENT_CONTROL_DATA) {
        g_data_events++;
        g_last_data_len = e->length < 64 ? e->length : 64;
        memcpy(g_last_data, e->data, g_last_data_len);
    } else if (e->kind == MIMUSB_EVENT_ENDPOINT_DATA) {
        g_out_events++;
        g_last_out_len = e->length < 64 ? e->length : 64;
        memcpy(g_last_out, e->data, g_last_out_len);
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
        } else {                                                                                   \
            printf("  ok: " __VA_ARGS__);                                                          \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

static int open_hidraw(void) {
    DIR* d = opendir("/sys/class/hidraw");
    if (!d)
        return -1;
    struct dirent* e;
    int fd = -1;
    while ((e = readdir(d)) != NULL && fd < 0) {
        if (strncmp(e->d_name, "hidraw", 6) != 0)
            continue;
        char p[512], line[256];
        snprintf(p, sizeof(p), "/sys/class/hidraw/%s/device/uevent", e->d_name);
        FILE* f = fopen(p, "r");
        if (!f)
            continue;
        int match = 0;
        while (fgets(line, sizeof(line), f))
            if (strstr(line, "HID_ID=0003:00001209:00000001"))
                match = 1;
        fclose(f);
        if (!match)
            continue;
        snprintf(p, sizeof(p), "/dev/%s", e->d_name);
        fd = open(p, O_RDWR | O_NONBLOCK);
        if (fd >= 0)
            printf("hidraw %s\n", p);
    }
    closedir(d);
    return fd;
}
static void hex(const char* what, const uint8_t* b, int n) {
    printf("    %s:", what);
    for (int i = 0; i < n; i++)
        printf(" %02x", b[i]);
    printf("\n");
}

int main(void) {
    mimusb_discovery* found = NULL;
    mimusb_result r = mimusb_discover(3000, &found);
    if (r != MIMUSB_OK || mimusb_discovery_count(found) == 0) {
        printf("no board\n");
        return 1;
    }
    mimusb_device_config config;
    example_fill_device_config(&config, mimusb_discovery_get(found, 0)->baud_rate,
                               "HW control test");
    config.transport.event_sink = on_event;
    r = mimusb_device_open(mimusb_discovery_get(found, 0)->port_name, &config, &g_device);
    mimusb_discovery_destroy(found);
    if (r != MIMUSB_OK) {
        printf("open: %s\n", mimusb_result_name(r));
        return 1;
    }
    mimusb_device* device = g_device;

    /* HID 1.11 6.2.1: the HID descriptor, with the report descriptor as its one subordinate. */
    const uint8_t hidd[9] = {
        9, 0x21, 0x11, 0x01, 0, 1, 0x22, (uint8_t)(sizeof(rd) & 0xFF), (uint8_t)(sizeof(rd) >> 8)};
    mimusb_usb_endpoint eps[2];
    memset(eps, 0, sizeof(eps));
    eps[0].address = 0x81;
    eps[0].transfer_type = MIMUSB_TRANSFER_INTERRUPT;
    eps[0].max_packet_size = 8;
    eps[0].interval = 1;
    eps[0].policy.on_full = MIMUSB_FULL_DROP_OLDEST;
    eps[0].policy.depth = 4;
    eps[1].address = 0x01;
    eps[1].transfer_type = MIMUSB_TRANSFER_INTERRUPT;
    eps[1].max_packet_size = 8;
    eps[1].interval = 1;
    mimusb_usb_interface itf;
    memset(&itf, 0, sizeof(itf));
    itf.number = 0;
    itf.class_code = 0x03;
    itf.name = "Vendor HID";
    const mimusb_usb_item items[3] = {mimusb_usb_item_make_descriptor(hidd, 9),
                                      mimusb_usb_item_make_endpoint(eps[0]),
                                      mimusb_usb_item_make_endpoint(eps[1])};
    const mimusb_usb_setting setting = {items, 3};
    itf.settings = &setting;
    itf.setting_count = 1;
    static uint8_t feature[4] = {1, 2, 3, 4};
    mimusb_control_rule rules[6];
    memset(rules, 0, sizeof(rules));
    /* GET_DESCRIPTOR (USB 2.0 9.4.3) of the report descriptor (HID 1.11 7.1.1) on interface 0. */
    rules[0].request_type_mask = 0xFF;
    rules[0].request_type_value = 0x81;
    rules[0].request_mask = 0xFF;
    rules[0].request_value = 0x06;
    rules[0].value_mask = 0xFF00;
    rules[0].value_value = 0x2200;
    rules[0].index_mask = 0xFFFF;
    rules[0].index_value = 0;
    rules[0].action = MIMUSB_CONTROL_ANSWER;
    rules[0].data = rd;
    rules[0].length = sizeof(rd);
#define RULE(i, bm, req, vmask, v)                                                                 \
    rules[i].request_type_mask = 0xFF;                                                             \
    rules[i].request_type_value = bm;                                                              \
    rules[i].request_mask = 0xFF;                                                                  \
    rules[i].request_value = req;                                                                  \
    rules[i].value_mask = vmask;                                                                   \
    rules[i].value_value = v
    RULE(1, 0xA1, 0x01, 0xFF00, 0x0100);
    rules[1].index_mask = 0xFFFF;
    rules[1].action = MIMUSB_CONTROL_ANSWER_LAST_SENT;
    rules[1].endpoint_address = 0x81;
    RULE(2, 0xA1, 0x01, 0xFF00, 0x0300);
    rules[2].index_mask = 0xFFFF;
    rules[2].action = MIMUSB_CONTROL_ANSWER;
    rules[2].data = feature;
    rules[2].length = 4;
    RULE(3, 0x21, 0x09, 0, 0);
    rules[3].action = MIMUSB_CONTROL_ACCEPT;
    RULE(4, 0x21, 0x0A, 0, 0);
    rules[4].action = MIMUSB_CONTROL_ACCEPT;
    RULE(5, 0xC0, 0x42, 0, 0);
    rules[5].action = MIMUSB_CONTROL_ASK;
    rules[5].timeout_ms = 500;
    rules[5].when_late = MIMUSB_CONTROL_ACTION_STALL;
    mimusb_node_config nc;
    mimusb_node_config_init(&nc);
    nc.interfaces = &itf;
    nc.interface_count = 1;
    nc.rules = rules;
    nc.rule_count = 6;
    mimusb_commit_timeouts to;
    example_fill_commit_timeouts(&to);
    mimusb_node* node = NULL;
    r = example_node_open_interfaces(device, &nc, &to, &node);
    if (r != MIMUSB_OK) {
        printf("node_open: %s (%s: %s)\n", mimusb_result_name(r), mimusb_last_error()->field,
               mimusb_last_error()->reason);
        return 1;
    }
    r = mimusb_device_wait_ready(device, 5000);
    if (r != MIMUSB_OK) {
        printf("wait_ready: %s\n", mimusb_result_name(r));
        return 1;
    }
    int fd = -1;
    for (int i = 0; i < 100 && fd < 0; i++) {
        fd = open_hidraw();
        if (fd < 0)
            sleep_ms(50);
    }
    if (fd < 0) {
        printf("no hidraw node (the report descriptor was not accepted, or permissions)\n");
        return 1;
    }
    uint8_t b[128];
    int n;

    printf("ANSWER (the report descriptor): the kernel bound usbhid and made a hidraw node\n");
    struct hidraw_report_descriptor d;
    memset(&d, 0, sizeof(d));
    int sz = 0;
    ioctl(fd, HIDIOCGRDESCSIZE, &sz);
    d.size = sz;
    ioctl(fd, HIDIOCGRDESC, &d);
    CHECK(sz == (int)sizeof(rd) && memcmp(d.value, rd, sizeof(rd)) == 0,
          "the report descriptor the kernel read is the one given (%d bytes)", sz);
    sleep_ms(100);
    pthread_mutex_lock(&g_lock);
    printf("  (SET_IDLE seen during enumeration: %d accepted setup(s), last setup %02x %02x)\n",
           g_setup_accepted, g_last_setup[0], g_last_setup[1]);
    pthread_mutex_unlock(&g_lock);

    printf("ANSWER_LAST_SENT (GET_REPORT Input)\n");
    memset(b, 0, sizeof(b));
    n = ioctl(fd, HIDIOCGINPUT(9), b);
    CHECK(n < 0, "before any report: the request fails (stalled), ioctl returned %d", n);
    const uint8_t r1[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    r = mimusb_node_endpoint_write(node, 0x81, r1, 8, 100);
    CHECK(r == MIMUSB_OK, "an input report is sent: %s", mimusb_result_name(r));
    struct pollfd pf = {fd, POLLIN, 0};
    poll(&pf, 1, 500);
    n = (int)read(fd, b, sizeof(b));
    CHECK(n == 8 && memcmp(b, r1, 8) == 0,
          "the target reads it from the interrupt endpoint (%d bytes)", n);
    memset(b, 0, sizeof(b));
    n = ioctl(fd, HIDIOCGINPUT(9), b);
    CHECK(n >= 8 && (memcmp(b + 1, r1, 8) == 0 || memcmp(b, r1, 8) == 0),
          "GET_REPORT returns the report last sent (ioctl %d)", n);
    if (n > 0)
        hex("returned", b, n);

    printf("ANSWER (GET_REPORT Feature), then its bytes replaced\n");
    memset(b, 0, sizeof(b));
    n = ioctl(fd, HIDIOCGFEATURE(5), b);
    CHECK(n >= 4 && (memcmp(b + 1, feature, 4) == 0 || memcmp(b, feature, 4) == 0),
          "the Feature report is the rule's bytes (ioctl %d)", n);
    if (n > 0)
        hex("returned", b, n);
    const uint8_t f2[4] = {0xA0, 0xA1, 0xA2, 0xA3};
    r = mimusb_node_set_rule_data(node, 2, f2, 4, 500);
    CHECK(r == MIMUSB_OK, "set_rule_data: %s", mimusb_result_name(r));
    memset(b, 0, sizeof(b));
    n = ioctl(fd, HIDIOCGFEATURE(5), b);
    CHECK(n >= 4 && (memcmp(b + 1, f2, 4) == 0 || memcmp(b, f2, 4) == 0),
          "the Feature report is the new bytes (ioctl %d)", n);

    printf("ACCEPT (SET_REPORT Feature with a data stage)\n");
    pthread_mutex_lock(&g_lock);
    int a0 = g_setup_accepted, d0 = g_data_events;
    pthread_mutex_unlock(&g_lock);
    uint8_t sf[5] = {0, 0xC1, 0xC2, 0xC3, 0xC4};
    n = ioctl(fd, HIDIOCSFEATURE(5), sf);
    CHECK(n >= 0, "the target's SET_REPORT completes (ioctl %d)", n);
    sleep_ms(100);
    pthread_mutex_lock(&g_lock);
    CHECK(g_setup_accepted == a0 + 1 && g_last_setup[0] == 0x21 && g_last_setup[1] == 0x09,
          "the program hears of it: setup %02x %02x, accepted", g_last_setup[0], g_last_setup[1]);
    CHECK(g_data_events == d0 + 1 && g_last_data_len == 4 && memcmp(g_last_data, sf + 1, 4) == 0,
          "and gets its data stage (%zu bytes)", g_last_data_len);
    pthread_mutex_unlock(&g_lock);

    printf("OUT endpoint (an Output report written by the target)\n");
    uint8_t out[9] = {0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8};
    n = (int)write(fd, out, 9);
    CHECK(n == 9, "write to hidraw: %d", n);
    sleep_ms(100);
    pthread_mutex_lock(&g_lock);
    CHECK(g_out_events >= 1 && g_last_out_len == 8 && memcmp(g_last_out, out + 1, 8) == 0,
          "the program gets it as ENDPOINT_DATA (%zu bytes)", g_last_out_len);
    pthread_mutex_unlock(&g_lock);

    printf("ASK and STALL (vendor requests to the device)\n");
    libusb_context* ctx = NULL;
    libusb_init(&ctx);
    libusb_device_handle* h = libusb_open_device_with_vid_pid(ctx, 0x1209, 0x0001);
    if (!h) {
        CHECK(0, "libusb cannot open the device");
    } else {
        memset(b, 0, sizeof(b));
        n = libusb_control_transfer(h, 0xC0, 0x42, 0x1234, 0, b, 64, 1000);
        CHECK(n == (int)sizeof("answered by the program") &&
                  strcmp((char*)b, "answered by the program") == 0,
              "a held request is answered by the program: %d bytes \"%s\"", n,
              n > 0 ? (char*)b : "");
        pthread_mutex_lock(&g_lock);
        CHECK(g_asked == 1, "it was asked once (%d)", g_asked);
        pthread_mutex_unlock(&g_lock);
        n = libusb_control_transfer(h, 0xC0, 0x43, 0, 0, b, 64, 1000);
        CHECK(n == LIBUSB_ERROR_PIPE, "a request no rule matches is stalled: %s",
              libusb_error_name(n));
        libusb_close(h);
    }
    libusb_exit(ctx);
    close(fd);
    r = mimusb_node_close(node, &to, 1000);
    CHECK(r == MIMUSB_OK, "node_close: %s", mimusb_result_name(r));
    r = mimusb_device_close(device, 500, 1000);
    CHECK(r == MIMUSB_OK, "device_close: %s", mimusb_result_name(r));
    printf("%s (%d failure(s))\n", fails ? "FAILED" : "PASSED", fails);
    return fails != 0;
}
