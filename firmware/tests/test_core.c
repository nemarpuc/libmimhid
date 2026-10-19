// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
// Host-side test of the portable firmware core with a fake HAL and TinyUSB stubs.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "tusb.h"
#include "config.h"
#include "core.h"
#include "hal.h"

uint8_t const* tud_descriptor_device_cb(void);
uint8_t const* tud_descriptor_configuration_cb(uint8_t index);
uint8_t const* tud_hid_descriptor_report_cb(uint8_t instance);
uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid);
uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen);
void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize);
void tud_hid_set_protocol_cb(uint8_t instance, uint8_t protocol);
bool tud_hid_set_idle_cb(uint8_t instance, uint8_t idle_rate);
void tud_suspend_cb(bool remote_wakeup_en);
void tud_resume_cb(void);

// ---- fake HAL ----
static uint8_t  g_in[8192];
static size_t   g_in_len, g_in_pos;
static uint8_t  g_out[8192];
static size_t   g_out_len;
static uint32_t g_now;
static int      g_has_id = 1;
static uint32_t g_baud = MIMHID_BOOT_BAUD;
static int      g_baud_changes;

void hal_init(void) {}
size_t hal_uart_read(uint8_t* dst, size_t max) {
    size_t n = g_in_len - g_in_pos;
    if (n > max) n = max;
    memcpy(dst, g_in + g_in_pos, n);
    g_in_pos += n;
    return n;
}
void hal_uart_write(const uint8_t* d, size_t n) { memcpy(g_out + g_out_len, d, n); g_out_len += n; }
uint32_t hal_millis(void) { return g_now; }
bool hal_uart_baud_ok(uint32_t baud) { return baud <= 3000000u; }
void hal_uart_set_baud(uint32_t baud) { g_baud = baud; g_baud_changes++; }
uint8_t hal_chip(void) { return MIMHID_CHIP_RP2040; }
size_t hal_unique_id(uint8_t* out, size_t max) {
    if (!g_has_id) return 0;
    static const uint8_t id[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    size_t n = max < sizeof(id) ? max : sizeof(id);
    memcpy(out, id, n);
    return n;
}

// ---- TinyUSB stubs ----
static bool g_mounted = true, g_suspended;
static int  g_disconnects, g_connects, g_wakeups;
static struct { uint8_t iface, len, data[64]; } g_sent[512];
static int  g_nsent;
static bool g_busy[8];

bool tusb_rhport_init(uint8_t rhport, const tusb_rhport_init_t* init) { (void)rhport; (void)init; return true; }
void tud_task_ext(uint32_t timeout_ms, bool in_isr) { (void)timeout_ms; (void)in_isr; }
bool tud_mounted(void) { return g_mounted; }
bool tud_suspended(void) { return g_suspended; }
bool tud_disconnect(void) { g_disconnects++; return true; }
bool tud_connect(void) { g_connects++; return true; }
bool tud_remote_wakeup(void) { g_wakeups++; return true; }
bool tud_hid_n_ready(uint8_t itf) { return !g_busy[itf]; }
bool tud_hid_n_report(uint8_t itf, uint8_t rid, void const* r, uint16_t len) {
    assert(rid == 0); // reports carry their own Report ID
    g_sent[g_nsent].iface = itf;
    g_sent[g_nsent].len = (uint8_t)len;
    memcpy(g_sent[g_nsent].data, r, len);
    g_nsent++;
    return true;
}

// ---- frame helpers ----
static uint8_t g_seq;

static void put_frame(uint8_t type, const uint8_t* p, size_t n) {
    uint8_t x = (uint8_t)(type ^ n);
    g_in[g_in_len++] = MIMHID_SYNC;
    g_in[g_in_len++] = type;
    g_in[g_in_len++] = (uint8_t)n;
    for (size_t i = 0; i < n; i++) { g_in[g_in_len++] = p[i]; x ^= p[i]; }
    g_in[g_in_len++] = x;
}

static void put_cmd(uint8_t type, const uint8_t* p, size_t n) {
    uint8_t buf[256];
    buf[0] = ++g_seq;
    memcpy(buf + 1, p, n);
    put_frame(type, buf, n + 1);
}

static void reset_io(void) {
    g_in_len = g_in_pos = g_out_len = 0;
    g_nsent = 0;
}

static void pump(void) {
    for (int i = 0; i < 4; i++) {
        mimhid_rx_poll();
        mimhid_usb_poll();
    }
}

// Reads the next board frame at *pos; returns its type, or 0 when none is left.
static uint8_t next_out(size_t* pos, uint8_t* payload, uint8_t* len) {
    if (*pos + 4 > g_out_len) return 0;
    assert(g_out[*pos] == MIMHID_SYNC);
    uint8_t t = g_out[*pos + 1], n = g_out[*pos + 2];
    assert(*pos + 4 + n <= g_out_len);
    uint8_t x = (uint8_t)(t ^ n);
    for (uint8_t i = 0; i < n; i++) { payload[i] = g_out[*pos + 3 + i]; x ^= payload[i]; }
    assert(x == g_out[*pos + 3 + n]);
    *pos += 4u + n;
    *len = n;
    return t;
}

// Result of the last command: 1 = ACK, 0 = NAK (code in g_nak_code), -1 = no reply.
static int g_nak_code;

static int result_of(uint8_t type) {
    size_t pos = 0;
    uint8_t t, p[256], n;
    int result = -1;
    while ((t = next_out(&pos, p, &n)) != 0) {
        if ((t == MIMHID_T_ACK || t == MIMHID_T_NAK) && p[1] == type) {
            assert(p[0] == g_seq);
            result = t == MIMHID_T_ACK;
            if (t == MIMHID_T_NAK) g_nak_code = p[2];
        }
    }
    return result;
}

static int command(uint8_t type, const uint8_t* p, size_t n) {
    g_out_len = 0;
    put_cmd(type, p, n);
    pump();
    return result_of(type);
}

static int device_config(uint8_t flags, const char* serial) {
    uint8_t p[160];
    size_t n = 0;
    p[n++] = 0x34; p[n++] = 0x12;          // vid
    p[n++] = 0x78; p[n++] = 0x56;          // pid
    p[n++] = 0x00; p[n++] = 0x02;          // bcd
    p[n++] = 0xEF; p[n++] = 0x02; p[n++] = 0x01;
    p[n++] = 0x09; p[n++] = 0x04;          // language
    p[n++] = flags;
    p[n++] = 50;                           // 100 mA
    const char* strs[4] = {"Maker", "Product", serial, "Config"};
    for (int i = 0; i < 4; i++) {
        p[n++] = (uint8_t)strlen(strs[i]);
        memcpy(p + n, strs[i], strlen(strs[i]));
        n += strlen(strs[i]);
    }
    return command(MIMHID_T_DEVICE_CONFIG, p, n);
}

static const uint8_t k_desc_a[] = {  // a small valid keyboard-like descriptor
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0xC0};
static const uint8_t k_desc_b[] = {  // mouse-like
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02, 0x09, 0x30, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08,
    0x95, 0x01, 0x81, 0x06, 0xC0};

static int node_begin(uint8_t id, uint8_t iface, uint16_t desc_len, uint8_t poll, uint8_t ep, uint8_t boot,
                      const char* str) {
    uint8_t p[64];
    size_t n = 0;
    p[n++] = id; p[n++] = iface;
    p[n++] = (uint8_t)(desc_len & 0xFF); p[n++] = (uint8_t)(desc_len >> 8);
    p[n++] = poll; p[n++] = ep;
    p[n++] = 0x11; p[n++] = 0x01;          // bcdHID 1.11
    p[n++] = 0;                            // country
    p[n++] = boot;
    p[n++] = 0;                            // idle
    p[n++] = 0;                            // flags
    p[n++] = (uint8_t)strlen(str);
    memcpy(p + n, str, strlen(str));
    n += strlen(str);
    return command(MIMHID_T_NODE_BEGIN, p, n);
}

static int node_data(uint8_t id, uint16_t off, const uint8_t* d, size_t len) {
    uint8_t p[256];
    p[0] = id; p[1] = (uint8_t)(off & 0xFF); p[2] = (uint8_t)(off >> 8);
    memcpy(p + 3, d, len);
    return command(MIMHID_T_NODE_DATA, p, len + 3);
}

static int commit(void) {
    return command(MIMHID_T_COMMIT, NULL, 0);
}

static void report(uint8_t node, const uint8_t* d, size_t n) {
    put_frame((uint8_t)(MIMHID_T_REPORT_BASE | node), d, n);
}

static void expect_nak(int r, int code) {
    assert(r == 0);
    assert(g_nak_code == code);
}

static void test_descriptors_basic(void) {
    // device with two nodes, each on its own interface
    assert(device_config(MIMHID_DEVFLAG_REMOTE_WAKEUP, "SN-1") == 1);
    assert(node_begin(0, 0, sizeof(k_desc_a), 4, 16, 1, "Keys") == 1);
    assert(node_data(0, 0, k_desc_a, sizeof(k_desc_a)) == 1);
    assert(node_begin(1, 1, sizeof(k_desc_b), 1, 64, 0, "") == 1);
    assert(node_data(1, 0, k_desc_b, sizeof(k_desc_b)) == 1);
    g_disconnects = g_connects = 0;
    assert(commit() == 1);
    assert(g_disconnects == 0 && g_connects == 1);
    assert(mimhid_active_interfaces() == 2);

    const tusb_desc_device_t* d = (const tusb_desc_device_t*)tud_descriptor_device_cb();
    assert(d->idVendor == 0x1234 && d->idProduct == 0x5678 && d->bcdDevice == 0x0200);
    assert(d->bDeviceClass == 0xEF && d->bDeviceSubClass == 2 && d->bDeviceProtocol == 1);
    assert(d->iManufacturer == 1 && d->iProduct == 2 && d->iSerialNumber == 3);

    const uint8_t* c = tud_descriptor_configuration_cb(0);
    uint16_t total = (uint16_t)(c[2] | (c[3] << 8));
    assert(total == 9 + 2 * 25 && c[4] == 2 && c[6] == 4);
    assert(c[7] == (0x80 | 0x20) && c[8] == 50);
    // interface 0: boot keyboard, string 5, bcdHID 1.11, report descriptor length, EP 0x81, 16 bytes, 4 ms
    const uint8_t* i0 = c + 9;
    assert(i0[1] == 4 && i0[2] == 0 && i0[5] == 3 && i0[6] == 1 && i0[7] == 1 && i0[8] == 5);
    assert(i0[11] == 0x11 && i0[12] == 0x01 && i0[16] == (sizeof(k_desc_a) & 0xFF));
    assert(i0[20] == 0x81 && i0[21] == 3 && i0[22] == 16 && i0[24] == 4);
    // interface 1: no string, 64-byte endpoint defaults, 1 ms
    const uint8_t* i1 = c + 9 + 25;
    assert(i1[2] == 1 && i1[6] == 0 && i1[8] == 0 && i1[20] == 0x82 && i1[22] == 64 && i1[24] == 1);

    assert(memcmp(tud_hid_descriptor_report_cb(0), k_desc_a, sizeof(k_desc_a)) == 0);
    assert(memcmp(tud_hid_descriptor_report_cb(1), k_desc_b, sizeof(k_desc_b)) == 0);
    assert(tud_hid_descriptor_report_cb(2) == NULL);

    const uint16_t* s = tud_descriptor_string_cb(1, 0x0409);
    assert((s[0] & 0xFF) == 2 + 2 * 5 && s[1] == 'M');
    s = tud_descriptor_string_cb(3, 0x0409);
    assert(s[1] == 'S' && s[4] == '1');
    s = tud_descriptor_string_cb(4, 0x0409);
    assert(s[1] == 'C');
    s = tud_descriptor_string_cb(5, 0x0409);
    assert(s[1] == 'K');
    assert(tud_descriptor_string_cb(7, 0x0409) == NULL);
    assert(tud_descriptor_string_cb(0, 0)[1] == 0x0409);
}

static void test_reports(void) {
    reset_io();
    const uint8_t a[] = {1, 0x02};
    const uint8_t b[] = {2, 0x05};
    report(0, a, sizeof(a));
    report(1, b, sizeof(b));
    report(5, a, sizeof(a));              // unknown node: dropped
    report(0, a, 0 + 0 + 2);
    pump();
    assert(g_nsent == 3);
    assert(g_sent[0].iface == 0 && g_sent[0].len == 2 && g_sent[0].data[1] == 0x02);
    assert(g_sent[1].iface == 1 && g_sent[1].data[0] == 2);

    // bad XOR is skipped and the next frame is still seen
    reset_io();
    put_frame(MIMHID_T_REPORT_BASE, a, sizeof(a));
    g_in[g_in_len - 1] ^= 0xFF;
    const uint8_t junk[] = {0x00, MIMHID_SYNC, MIMHID_SYNC, 0x13};
    memcpy(g_in + g_in_len, junk, sizeof(junk));
    g_in_len += sizeof(junk);
    report(1, b, sizeof(b));
    pump();
    // the junk looks like a header with a long length; the idle timeout abandons it
    assert(g_nsent == 0);
    g_now += 11;
    pump();
    assert(g_nsent == 1 && g_sent[0].iface == 1);

    // a frame split across polls is reassembled; a 64-byte report is accepted
    reset_io();
    uint8_t big[64];
    for (int i = 0; i < 64; i++) big[i] = (uint8_t)i;
    report(0, big, sizeof(big));
    size_t full = g_in_len;
    g_in_len = 10;
    pump();
    assert(g_nsent == 0);
    g_in_len = full;
    g_now += 1;
    pump();
    assert(g_nsent == 1 && g_sent[0].len == 64 && memcmp(g_sent[0].data, big, 64) == 0);

    // a 65-byte report is rejected
    reset_io();
    uint8_t toolong[65] = {0};
    report(0, toolong, sizeof(toolong));
    pump();
    assert(g_nsent == 0);

    // a full queue drops the newest report and keeps order
    reset_io();
    g_busy[0] = true;
    for (int i = 0; i < 30; i++) {
        uint8_t r[2] = {1, (uint8_t)i};
        report(0, r, 2);
    }
    pump();
    assert(g_nsent == 0);
    g_busy[0] = false;
    for (int i = 0; i < 6; i++) pump();
    assert(g_nsent == 15 && g_sent[0].data[1] == 0 && g_sent[14].data[1] == 14);

    // RESET drops what is queued
    reset_io();
    g_busy[0] = true;
    report(0, a, sizeof(a));
    pump();
    uint8_t none = 0;
    (void)none;
    assert(command(MIMHID_T_RESET, NULL, 0) == 1);
    g_busy[0] = false;
    g_nsent = 0;
    pump();
    assert(g_nsent == 0);
}

static void test_ping_and_events(void) {
    reset_io();
    const uint8_t v = MIMHID_PROTO_VERSION;
    put_frame(MIMHID_T_PING, &v, 1);
    pump();
    size_t pos = 0;
    uint8_t p[256], n;
    assert(next_out(&pos, p, &n) == MIMHID_T_PONG && n == MIMHID_PONG_MIN_LEN + 8);
    assert(p[0] == FW_VERSION_MAJOR && p[1] == FW_VERSION_MINOR && p[2] == FW_VERSION_PATCH && p[3] == 1);
    assert(p[4] == MIMHID_PROTO_VERSION && p[5] == MIMHID_MAX_NODES && p[6] == MIMHID_HID_ITFS);
    assert(p[7] == 64 && (p[8] | (p[9] << 8)) == MIMHID_MAX_DESC_BYTES && p[10] == MIMHID_PONG_FLAG_UNIQUE_ID);
    assert(p[11] == MIMHID_CHIP_RP2040 && p[12] == 0 && p[13] == 0);
    // 4,000,000 (index 8) is past what this HAL makes.
    assert((p[14] | (p[15] << 8)) == 0x00FF);
    assert(p[16] == 8 && p[17] == 0x01 && p[24] == 0xEF);

    reset_io();
    tud_mount_cb();
    tud_umount_cb();
    tud_suspend_cb(true);
    tud_resume_cb();
    const uint8_t sr[] = {0xAA, 0xBB};
    tud_hid_set_report_cb(1, 3, HID_REPORT_TYPE_OUTPUT, sr, sizeof(sr));
    tud_hid_set_protocol_cb(0, 0);
    assert(tud_hid_set_idle_cb(1, 8));
    pos = 0;
    assert(next_out(&pos, p, &n) == MIMHID_T_EVENT && p[0] == MIMHID_EV_MOUNTED);
    assert(next_out(&pos, p, &n) == MIMHID_T_EVENT && p[0] == MIMHID_EV_UNMOUNTED);
    assert(next_out(&pos, p, &n) == MIMHID_T_EVENT && p[0] == MIMHID_EV_SUSPEND && p[1] == 1);
    assert(next_out(&pos, p, &n) == MIMHID_T_EVENT && p[0] == MIMHID_EV_RESUME);
    assert(next_out(&pos, p, &n) == MIMHID_T_EVENT && p[0] == MIMHID_EV_SET_REPORT && p[1] == 1 &&
           p[2] == HID_REPORT_TYPE_OUTPUT && p[3] == 3 && p[4] == 0xAA && p[5] == 0xBB);
    assert(next_out(&pos, p, &n) == MIMHID_T_EVENT && p[0] == MIMHID_EV_SET_PROTOCOL && p[1] == 0 && p[2] == 0);
    assert(next_out(&pos, p, &n) == MIMHID_T_EVENT && p[0] == MIMHID_EV_SET_IDLE && p[1] == 1 && p[2] == 8);

    // remote wakeup only while suspended
    g_suspended = false;
    assert(command(MIMHID_T_REMOTE_WAKEUP, NULL, 0) == 0 && g_nak_code == MIMHID_NAK_STATE);
    g_suspended = true;
    assert(command(MIMHID_T_REMOTE_WAKEUP, NULL, 0) == 1 && g_wakeups == 1);
    g_suspended = false;
}

static void test_set_baud(void) {
    reset_io();
    const uint8_t v = MIMHID_PROTO_VERSION;
    // A rate the board cannot make, and one past the table, are refused.
    uint8_t index = 8;
    assert(command(MIMHID_T_SET_BAUD, &index, 1) == 0 && g_nak_code == MIMHID_NAK_PARAM);
    index = MIMHID_BAUD_COUNT;
    assert(command(MIMHID_T_SET_BAUD, &index, 1) == 0 && g_nak_code == MIMHID_NAK_PARAM);
    assert(g_baud == MIMHID_BOOT_BAUD && g_baud_changes == 0);

    // The ACK goes out before the switch; a valid frame at the new rate keeps it.
    index = 7;
    assert(command(MIMHID_T_SET_BAUD, &index, 1) == 1 && g_baud == 3000000u);
    g_now += MIMHID_BAUD_CONFIRM_MS - 1;
    reset_io();
    put_frame(MIMHID_T_PING, &v, 1);
    pump();
    size_t pos = 0;
    uint8_t p[256], n;
    assert(next_out(&pos, p, &n) == MIMHID_T_PONG && p[13] == 7);
    g_now += 10 * MIMHID_BAUD_CONFIRM_MS;
    pump();
    assert(g_baud == 3000000u);

    // Back to the boot rate needs no confirmation.
    index = 0;
    assert(command(MIMHID_T_SET_BAUD, &index, 1) == 1 && g_baud == MIMHID_BOOT_BAUD);
    g_now += 10 * MIMHID_BAUD_CONFIRM_MS;
    pump();
    assert(g_baud == MIMHID_BOOT_BAUD);

    // Nothing valid at the new rate in time: back to the boot rate.
    index = 3;
    assert(command(MIMHID_T_SET_BAUD, &index, 1) == 1 && g_baud == 921600u);
    reset_io();
    const uint8_t junk[] = {0x00, 0xFF, 0x13, 0x37};
    memcpy(g_in, junk, sizeof(junk));
    g_in_len = sizeof(junk);
    g_now += MIMHID_BAUD_CONFIRM_MS - 1;
    pump();
    assert(g_baud == 921600u);
    g_now += 1;
    pump();
    assert(g_baud == MIMHID_BOOT_BAUD);
    reset_io();
    put_frame(MIMHID_T_PING, &v, 1);
    pump();
    pos = 0;
    assert(next_out(&pos, p, &n) == MIMHID_T_PONG && p[13] == 0);
}

static void test_feature(void) {
    // the committed configuration still has nodes 0 and 1; stage it again with a feature report
    assert(device_config(0, "") == 1);
    assert(node_begin(0, 0, sizeof(k_desc_a), 1, 0, 0, "") == 1);
    assert(node_data(0, 0, k_desc_a, sizeof(k_desc_a)) == 1);
    const uint8_t f[] = {0, 0x07, 0x0A};     // node 0, report id 7, data 0x0A
    assert(command(MIMHID_T_FEATURE, f, sizeof(f)) == 1);
    const uint8_t bad[] = {3, 0x07, 0x0A};   // node 3 was never begun
    expect_nak(command(MIMHID_T_FEATURE, bad, sizeof(bad)), MIMHID_NAK_STATE);
    assert(commit() == 1);
    uint8_t buf[8];
    assert(tud_hid_get_report_cb(0, 7, HID_REPORT_TYPE_FEATURE, buf, sizeof(buf)) == 1 && buf[0] == 0x0A);
    assert(tud_hid_get_report_cb(0, 8, HID_REPORT_TYPE_FEATURE, buf, sizeof(buf)) == 0);
    assert(tud_hid_get_report_cb(0, 7, HID_REPORT_TYPE_INPUT, buf, sizeof(buf)) == 0);

    // serial from the chip id when the host gives none; empty strings leave indices at zero
    assert(device_config(MIMHID_DEVFLAG_SERIAL_FROM_ID, "") == 1);
    assert(node_begin(0, 0, sizeof(k_desc_a), 1, 0, 0, "") == 1);
    assert(node_data(0, 0, k_desc_a, sizeof(k_desc_a)) == 1);
    assert(commit() == 1);
    const tusb_desc_device_t* d = (const tusb_desc_device_t*)tud_descriptor_device_cb();
    assert(d->iSerialNumber == 3);
    const uint16_t* s = tud_descriptor_string_cb(3, 0x0409);
    assert((s[0] & 0xFF) == 2 + 2 * 16 && s[1] == '0' && s[2] == '1' && s[15] == 'E' && s[16] == 'F');
}

static void test_detach_begin_and_live_feature(void) {
    assert(device_config(0, "") == 1);
    assert(node_begin(0, 0, sizeof(k_desc_a), 1, 0, 0, "") == 1);
    assert(node_begin(0, 0, sizeof(k_desc_a), 1, 0, 0, "") == 1);    // repeated begin is acknowledged
    assert(node_data(0, 0, k_desc_a, sizeof(k_desc_a)) == 1);
    const uint8_t f1[] = {0, 0x07, 0x01};
    assert(command(MIMHID_T_FEATURE, f1, sizeof(f1)) == 1);
    assert(commit() == 1);
    assert(mimhid_active_interfaces() == 1);
    uint8_t buf[8];
    assert(tud_hid_get_report_cb(0, 7, HID_REPORT_TYPE_FEATURE, buf, sizeof(buf)) == 1 && buf[0] == 0x01);

    // A feature answer changes on the running device without a COMMIT.
    const int connects = g_connects, disconnects = g_disconnects;
    const uint8_t f2[] = {0, 0x07, 0x02};
    assert(command(MIMHID_T_FEATURE, f2, sizeof(f2)) == 1);
    assert(tud_hid_get_report_cb(0, 7, HID_REPORT_TYPE_FEATURE, buf, sizeof(buf)) == 1 && buf[0] == 0x02);
    assert(g_connects == connects && g_disconnects == disconnects);

    // COMMIT with the detach flag leaves the bus at once and forgets the configuration.
    const uint8_t detach[] = {1};
    assert(command(MIMHID_T_COMMIT, detach, sizeof(detach)) == 1);
    assert(g_disconnects == disconnects + 1 && g_connects == connects);
    assert(mimhid_active_interfaces() == 0);
}

static void test_errors(void) {
    // NODE_BEGIN needs a DEVICE_CONFIG first
    uint8_t clear[1] = {0};
    (void)clear;
    assert(device_config(0, "x") == 1);
    expect_nak(node_begin(9, 0, 4, 1, 0, 0, ""), MIMHID_NAK_PARAM);           // node id out of range
    expect_nak(node_begin(0, 0, 0, 1, 0, 0, ""), MIMHID_NAK_PARAM);           // empty descriptor
    expect_nak(node_begin(0, 0, 4, 1, 65, 0, ""), MIMHID_NAK_PARAM);           // endpoint size above 64
    expect_nak(node_begin(0, 0, 4, 1, 0, 3, ""), MIMHID_NAK_PARAM);           // bad boot protocol
    expect_nak(node_begin(0, MIMHID_HID_ITFS, 4, 1, 0, 0, ""), MIMHID_NAK_OVERFLOW);
    expect_nak(node_begin(0, 0, MIMHID_MAX_DESC_BYTES + 1, 1, 0, 0, ""), MIMHID_NAK_OVERFLOW);
    assert(node_begin(0, 0, sizeof(k_desc_a), 1, 0, 0, "") == 1);
    expect_nak(node_begin(0, 0, 4, 1, 0, 0, ""), MIMHID_NAK_STATE);           // already begun
    expect_nak(node_data(0, 5, k_desc_a, 4), MIMHID_NAK_PARAM);               // gap in the data
    expect_nak(node_data(0, 0, k_desc_a, sizeof(k_desc_a) + 1), MIMHID_NAK_PARAM);
    expect_nak(node_data(4, 0, k_desc_a, 4), MIMHID_NAK_STATE);
    // a COMMIT with an incomplete descriptor is refused and does not disconnect
    g_disconnects = 0;
    assert(node_data(0, 0, k_desc_a, 10) == 1);
    expect_nak(command(MIMHID_T_COMMIT, NULL, 0), MIMHID_NAK_DESCRIPTOR);
    assert(g_disconnects == 0);
    // a repeated chunk is accepted and completes the descriptor
    assert(node_data(0, 0, k_desc_a, 10) == 1);
    assert(node_data(0, 10, k_desc_a + 10, sizeof(k_desc_a) - 10) == 1);
    // a gap in the interface indices is refused
    assert(node_begin(1, MIMHID_HID_ITFS > 2 ? 2 : 1, sizeof(k_desc_b), 1, 0, 0, "") == 1);
    assert(node_data(1, 0, k_desc_b, sizeof(k_desc_b)) == 1);
    if (MIMHID_HID_ITFS > 2) {
        expect_nak(command(MIMHID_T_COMMIT, NULL, 0), MIMHID_NAK_DESCRIPTOR);
    }
    // a COMMIT completes immediately and synchronizes
    assert(device_config(0, "") == 1);
    assert(node_begin(0, 0, sizeof(k_desc_a), 1, 0, 0, "") == 1);
    assert(node_data(0, 0, k_desc_a, sizeof(k_desc_a)) == 1);
    assert(command(MIMHID_T_COMMIT, NULL, 0) == 1);
}

static void test_packing(void) {
    // two nodes share interface 0; reports of both reach one queue in order
    assert(device_config(0, "") == 1);
    assert(node_begin(0, 0, sizeof(k_desc_a), 1, 0, 0, "") == 1);
    assert(node_data(0, 0, k_desc_a, sizeof(k_desc_a)) == 1);
    assert(node_begin(1, 0, sizeof(k_desc_b), 1, 0, 0, "") == 1);
    assert(node_data(1, 0, k_desc_b, sizeof(k_desc_b)) == 1);
    assert(commit() == 1);
    assert(mimhid_active_interfaces() == 1);
    const uint8_t* rd = tud_hid_descriptor_report_cb(0);
    assert(memcmp(rd, k_desc_a, sizeof(k_desc_a)) == 0);
    assert(memcmp(rd + sizeof(k_desc_a), k_desc_b, sizeof(k_desc_b)) == 0);
    const uint8_t* c = tud_descriptor_configuration_cb(0);
    assert((c[2] | (c[3] << 8)) == 9 + 25 && c[9 + 16] == sizeof(k_desc_a) + sizeof(k_desc_b));
    reset_io();
    const uint8_t a[] = {1, 1}, b[] = {2, 2};
    report(0, a, 2);
    report(1, b, 2);
    report(0, a, 2);
    pump();
    assert(g_nsent == 3 && g_sent[0].data[0] == 1 && g_sent[1].data[0] == 2 && g_sent[2].data[0] == 1);
    assert(g_sent[0].iface == 0 && g_sent[1].iface == 0);
}

int main(void) {
    printf("firmware core, %d HID interfaces\n", MIMHID_HID_ITFS);
    mimhid_core_init();
    test_ping_and_events();
    test_set_baud();
    test_descriptors_basic();
    test_reports();
    test_feature();
    test_detach_begin_and_live_feature();
    test_errors();
    test_packing();
    printf("firmware core OK\n");
    return 0;
}
