// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
// A CDC-ACM function (mimusb_cdc_acm_descriptors_make, mimusb_cdc_acm_rules) on a real board whose
// target is this machine: Linux binds its cdc_acm driver and makes a /dev/ttyACM* node, which this
// program opens as any terminal program would.
//
//   hw_cdc
//
// A: the tty appears; the line coding the program gave is what the driver reads.
// B: setting the baud rate on the tty reaches the program (SET_LINE_CODING, with its data stage).
// C: bytes written to the tty arrive as MIMUSB_EVENT_ENDPOINT_DATA; bytes the program writes on
//    the bulk IN endpoint are read from the tty.
#include "example_values.h"
#include <dirent.h>
#include <fcntl.h>
#include <mimusb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t g_data[256];
static size_t g_data_len;
static uint8_t g_line[16];
static size_t g_line_len;

static void on_event(void* user, const mimusb_event* e) {
    (void)user;
    pthread_mutex_lock(&g_lock);
    if (e->kind == MIMUSB_EVENT_ENDPOINT_DATA && e->endpoint_address == 0x04 &&
        g_data_len + e->length <= sizeof(g_data)) {
        memcpy(g_data + g_data_len, e->data, e->length);
        g_data_len += e->length;
    } else if (e->kind == MIMUSB_EVENT_CONTROL_DATA && e->length <= sizeof(g_line)) {
        memcpy(g_line, e->data, e->length);
        g_line_len = e->length;
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

int main(void) {
    mimusb_discovery* found = NULL;
    mimusb_result r = mimusb_discover(3000, &found);
    if (r != MIMUSB_OK || mimusb_discovery_count(found) == 0) {
        printf("no board\n");
        return 1;
    }
    mimusb_device_config config;
    example_fill_device_config(&config, mimusb_discovery_get(found, 0)->baud_rate, "HW CDC test");
    config.transport.event_sink = on_event;
    mimusb_device* device = NULL;
    r = mimusb_device_open(mimusb_discovery_get(found, 0)->port_name, &config, &device);
    mimusb_discovery_destroy(found);
    if (r != MIMUSB_OK) {
        printf("open: %s %s\n", mimusb_result_name(r), mimusb_last_error()->reason);
        return 1;
    }

    mimusb_cdc_acm_descriptors cdc;
    r = mimusb_cdc_acm_descriptors_make(0x0120, 0, 0x00, 0x02, &cdc);
    CHECK(r == MIMUSB_OK, "descriptors: %s", mimusb_result_name(r));
    const uint8_t line_coding[7] = {0x00, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x08}; /* 115200 8N1 */
    mimusb_control_rule rules[3];
    r = mimusb_cdc_acm_rules(0, line_coding, rules);
    CHECK(r == MIMUSB_OK, "rules: %s", mimusb_result_name(r));

    mimusb_usb_endpoint notify, bulk_in, bulk_out;
    memset(&notify, 0, sizeof(notify));
    notify.address = 0x83;
    notify.transfer_type = MIMUSB_TRANSFER_INTERRUPT;
    notify.max_packet_size = 16;
    notify.interval = 16;
    notify.policy.on_full = MIMUSB_FULL_DROP_NEWEST;
    notify.policy.depth = 2;
    memset(&bulk_in, 0, sizeof(bulk_in));
    bulk_in.address = 0x84;
    bulk_in.transfer_type = MIMUSB_TRANSFER_BULK;
    bulk_in.max_packet_size = 64;
    bulk_in.policy.on_full = MIMUSB_FULL_WAIT;
    bulk_in.policy.depth = 4;
    memset(&bulk_out, 0, sizeof(bulk_out));
    bulk_out.address = 0x04;
    bulk_out.transfer_type = MIMUSB_TRANSFER_BULK;
    bulk_out.max_packet_size = 64;
    const mimusb_usb_item comm_items[5] = {
        mimusb_usb_item_make_descriptor(cdc.header, sizeof(cdc.header)),
        mimusb_usb_item_make_descriptor(cdc.call_management, sizeof(cdc.call_management)),
        mimusb_usb_item_make_descriptor(cdc.acm, sizeof(cdc.acm)),
        mimusb_usb_item_make_descriptor(cdc.union_, sizeof(cdc.union_)),
        mimusb_usb_item_make_endpoint(notify)};
    const mimusb_usb_item data_items[2] = {mimusb_usb_item_make_endpoint(bulk_in),
                                           mimusb_usb_item_make_endpoint(bulk_out)};
    const mimusb_usb_setting comm_setting = {comm_items, 5};
    const mimusb_usb_setting data_setting = {data_items, 2};
    mimusb_usb_interface itfs[2];
    memset(itfs, 0, sizeof(itfs));
    itfs[0].number = 0;
    itfs[0].class_code = 0x02;
    itfs[0].subclass = 0x02;
    itfs[0].protocol = 0x01;
    itfs[0].name = "HW CDC control";
    itfs[0].settings = &comm_setting;
    itfs[0].setting_count = 1;
    itfs[1].number = 1;
    itfs[1].class_code = 0x0A;
    itfs[1].name = "HW CDC data";
    itfs[1].settings = &data_setting;
    itfs[1].setting_count = 1;
    mimusb_node_config nc;
    mimusb_node_config_init(&nc);
    nc.interfaces = itfs;
    nc.interface_count = 2;
    nc.association = cdc.association;
    nc.association_length = sizeof(cdc.association);
    nc.rules = rules;
    nc.rule_count = 3;
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

    printf("A: the tty of the cdc_acm driver\n");
    char path[64] = "";
    for (int i = 0; i < 50 && !path[0]; i++) {
        DIR* d = opendir("/dev");
        struct dirent* ent;
        while (d && (ent = readdir(d)) != NULL)
            if (!strncmp(ent->d_name, "ttyACM", 6))
                snprintf(path, sizeof(path), "/dev/%s", ent->d_name);
        if (d)
            closedir(d);
        if (!path[0])
            sleep_ms(100);
    }
    CHECK(path[0], "no /dev/ttyACM* appeared");
    if (!path[0])
        return 1;
    sleep_ms(300);
    int fd = -1;
    for (int i = 0; i < 20 && fd < 0; i++) {
        fd = open(path, O_RDWR | O_NOCTTY);
        if (fd < 0)
            sleep_ms(100);
    }
    if (fd < 0) {
        printf("cannot open %s (permissions: uucp group)\n", path);
        return 1;
    }
    struct termios tio;
    CHECK(tcgetattr(fd, &tio) == 0, "tcgetattr");
    printf("  %s opened; line coding read by the driver: baud speed code %u\n", path,
           (unsigned)cfgetospeed(&tio));
    cfmakeraw(&tio);
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);
    CHECK(tcsetattr(fd, TCSANOW, &tio) == 0, "tcsetattr 115200");

    printf("B: SET_LINE_CODING from the tty\n");
    cfsetispeed(&tio, B19200);
    cfsetospeed(&tio, B19200);
    pthread_mutex_lock(&g_lock);
    g_line_len = 0;
    pthread_mutex_unlock(&g_lock);
    CHECK(tcsetattr(fd, TCSANOW, &tio) == 0, "tcsetattr 19200");
    sleep_ms(300);
    pthread_mutex_lock(&g_lock);
    const uint32_t rate =
        g_line_len >= 4
            ? (uint32_t)(g_line[0] | g_line[1] << 8 | g_line[2] << 16 | (uint32_t)g_line[3] << 24)
            : 0;
    printf("  the program heard dwDTERate %u (%zu bytes)\n", rate, g_line_len);
    CHECK(g_line_len == 7 && rate == 19200, "SET_LINE_CODING data: %zu bytes, rate %u", g_line_len,
          rate);
    pthread_mutex_unlock(&g_lock);

    printf("C: data both ways\n");
    const char* hello = "AT+HELLO\r\n";
    CHECK(write(fd, hello, strlen(hello)) == (ssize_t)strlen(hello), "tty write");
    sleep_ms(300);
    pthread_mutex_lock(&g_lock);
    CHECK(g_data_len == strlen(hello) && memcmp(g_data, hello, g_data_len) == 0,
          "the program got %zu bytes", g_data_len);
    printf("  program received %zu bytes from the tty\n", g_data_len);
    pthread_mutex_unlock(&g_lock);
    const char* reply = "OK 0.5.0\r\n";
    r = mimusb_node_endpoint_write(node, 0x84, (const uint8_t*)reply, strlen(reply), 1000);
    CHECK(r == MIMUSB_OK, "endpoint_write: %s", mimusb_result_name(r));
    char rx[64];
    size_t got = 0;
    for (int i = 0; i < 30 && got < strlen(reply); i++) {
        ssize_t n = read(fd, rx + got, sizeof(rx) - got);
        if (n > 0)
            got += (size_t)n;
        else
            sleep_ms(50);
    }
    CHECK(got == strlen(reply) && memcmp(rx, reply, got) == 0, "tty read %zu bytes", got);
    printf("  tty read %zu bytes\n", got);

    mimusb_link_counters counters;
    memset(&counters, 0, sizeof(counters));
    counters.struct_size = sizeof(counters);
    r = mimusb_device_link_counters(device, 0, 500, &counters);
    CHECK(r == MIMUSB_OK && counters.bad_crc == 0 && counters.framing == 0 &&
              counters.uart_overrun == 0 && counters.ring_overrun == 0,
          "link errors");
    close(fd);
    printf("node_close: %s\n", mimusb_result_name(mimusb_node_close(node, &to, 1000)));
    printf("device_close: %s\n", mimusb_result_name(mimusb_device_close(device, 500, 1000)));
    printf(fails ? "FAILED (%d failure(s))\n" : "PASSED (%d failure(s))\n", fails);
    return fails ? 1 : 0;
}
