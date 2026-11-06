// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
// Host-side test of the portable firmware core with a fake HAL and TinyUSB stubs.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "tusb.h"
#include "device/dcd.h"
#include "config.h"
#include "control.h"
#include "core.h"
#include "ep0.h"
#include "device/usbd_pvt.h"
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
void __wrap_dcd_event_handler(dcd_event_t const* event, bool in_isr);
bool __wrap_dcd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t* buffer, uint16_t total_bytes);

// ---- fake HAL ----
static uint8_t  g_in[16384];
static size_t   g_in_len, g_in_pos;
static uint8_t  g_out[16384];
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
static uint32_t g_now_step;
uint32_t hal_millis(void) { g_now += g_now_step; return g_now; }
static uint32_t g_max_rate = 12000000u;
static uint32_t g_rate_error_ppm; // the fake divider's error, applied to every rate
static uint8_t  g_errors;         // HAL_UART_ERR_* bits the next hal_uart_errors returns
uint32_t hal_uart_baud_actual(uint32_t baud) {
    if (baud == 0 || baud > g_max_rate) return 0;
    return (uint32_t)((uint64_t)baud * (1000000u + g_rate_error_ppm) / 1000000u);
}
uint32_t hal_uart_max_baud(void) { return g_max_rate; }
bool hal_uart_has_break(void) { return true; }
uint8_t hal_uart_errors(void) { uint8_t e = g_errors; g_errors = 0; return e; }
void hal_uart_set_baud(uint32_t baud) { g_baud = baud; g_baud_changes++; }
uint8_t hal_chip(void) { return MIMHID_CHIP_RP2040; }
size_t hal_unique_id(uint8_t* out, size_t max) {
    if (!g_has_id) return 0;
    static const uint8_t id[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    size_t n = max < sizeof(id) ? max : sizeof(id);
    memcpy(out, id, n);
    return n;
}
void hal_control_silent_drop(void) {}
void __real_dcd_edpt_stall(uint8_t rhport, uint8_t ep_addr) { (void)rhport; (void)ep_addr; }

// ---- TinyUSB stubs ----
static bool g_mounted = true, g_suspended;
static int  g_disconnects, g_connects, g_wakeups;
static struct { uint8_t iface; uint16_t len; uint8_t data[1024]; } g_sent[64];
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
    g_sent[g_nsent].len = len;
    memcpy(g_sent[g_nsent].data, r, len);
    g_nsent++;
    return true;
}

// What the stack would get or do for control transfers.
static dcd_event_t g_forwarded[32];
static int g_nforwarded;
static struct {
    int kind; // 0 none, 1 data stage, 2 status only
    uint8_t* buffer;
    uint16_t length;
    uint16_t w_length; // of the request given with it
    uint8_t copy[1100];
} g_xfer;

void __real_dcd_event_handler(dcd_event_t const* event, bool in_isr) {
    (void)in_isr;
    assert(g_nforwarded < 32);
    g_forwarded[g_nforwarded++] = *event;
}
void dcd_int_disable(uint8_t rhport) { (void)rhport; }
void dcd_int_enable(uint8_t rhport) { (void)rhport; }
bool tud_control_xfer(uint8_t rhport, tusb_control_request_t const* request, void* buffer, uint16_t len) {
    (void)rhport;
    g_xfer.kind = 1;
    g_xfer.w_length = request->wLength;
    g_xfer.buffer = buffer;
    g_xfer.length = len;
    memcpy(g_xfer.copy, buffer, len);
    return true;
}
// What the controller is given on its endpoints, and the call the stack's task would make.
static struct {
    uint8_t ep_addr;
    uint8_t* buffer;
    uint16_t length;
} g_dcd[32];
static int g_ndcd;
static osal_task_func_t g_deferred;
static void* g_deferred_param;

bool __real_dcd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t* buffer, uint16_t total_bytes) {
    (void)rhport;
    assert(g_ndcd < 32);
    g_dcd[g_ndcd].ep_addr = ep_addr;
    g_dcd[g_ndcd].buffer = buffer;
    g_dcd[g_ndcd].length = total_bytes;
    g_ndcd++;
    return true;
}
void usbd_defer_func(osal_task_func_t func, void* param, bool in_isr) {
    (void)in_isr;
    assert(g_deferred == NULL);
    g_deferred = func;
    g_deferred_param = param;
}
bool tud_control_status(uint8_t rhport, tusb_control_request_t const* request) {
    (void)rhport;
    (void)request;
    g_xfer.kind = 2;
    return true;
}

// ---- frame helpers ----
static uint32_t u32at(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint8_t g_seq;

static void put_frame(uint8_t type, const uint8_t* p, size_t n) {
    uint16_t crc = mimhid_crc16_update(MIMHID_CRC16_INIT, type);
    g_in[g_in_len++] = MIMHID_SYNC;
    g_in[g_in_len++] = type;
    if (n < 0x80) {
        g_in[g_in_len++] = (uint8_t)n;
        crc = mimhid_crc16_update(crc, (uint8_t)n);
    } else {
        const uint8_t low = (uint8_t)(0x80 | (n & 0x7F)), high = (uint8_t)(n >> 7);
        g_in[g_in_len++] = low;
        g_in[g_in_len++] = high;
        crc = mimhid_crc16_update(crc, low);
        crc = mimhid_crc16_update(crc, high);
    }
    for (size_t i = 0; i < n; i++) { g_in[g_in_len++] = p[i]; crc = mimhid_crc16_update(crc, p[i]); }
    g_in[g_in_len++] = (uint8_t)(crc & 0xFF);
    g_in[g_in_len++] = (uint8_t)(crc >> 8);
}

static void put_cmd(uint8_t type, const uint8_t* p, size_t n) {
    uint8_t buf[2048];
    buf[0] = ++g_seq;
    memcpy(buf + 1, p, n);
    put_frame(type, buf, n + 1);
}

static void reset_io(void) {
    g_in_len = g_in_pos = g_out_len = 0;
    g_nsent = 0;
    g_nforwarded = 0;
    memset(&g_xfer, 0, sizeof(g_xfer));
}

static void pump(void) {
    for (int i = 0; i < 4; i++) {
        mimhid_rx_poll();
        mimhid_usb_poll();
    }
}

// Reads the next board frame at *pos; returns its type, or 0 when none is left.
static uint8_t next_out(size_t* pos, uint8_t* payload, uint16_t* len) {
    if (*pos + 5 > g_out_len) return 0;
    assert(g_out[*pos] == MIMHID_SYNC);
    uint8_t t = g_out[*pos + 1];
    uint8_t low = g_out[*pos + 2], high = 0;
    size_t header = 3;
    uint16_t n = low;
    if (low & 0x80) {
        high = g_out[*pos + 3];
        n = (uint16_t)((low & 0x7F) | (high << 7));
        header = 4;
    }
    assert(*pos + header + n + 2 <= g_out_len);
    uint16_t crc = mimhid_crc16_update(MIMHID_CRC16_INIT, t);
    crc = mimhid_crc16_update(crc, low);
    if (header == 4) crc = mimhid_crc16_update(crc, high);
    for (uint16_t i = 0; i < n; i++) { payload[i] = g_out[*pos + header + i]; crc = mimhid_crc16_update(crc, payload[i]); }
    assert(g_out[*pos + header + n] == (uint8_t)(crc & 0xFF));
    assert(g_out[*pos + header + n + 1] == (uint8_t)(crc >> 8));
    *pos += header + n + 2u;
    *len = n;
    return t;
}

// The dropped reports the board has reported so far in g_out.
static int lost_reported(void) {
    size_t pos = 0;
    uint8_t p[16];
    uint16_t n = 0;
    uint8_t t;
    int lost = 0;
    while ((t = next_out(&pos, p, &n)) != 0) {
        if (t == MIMHID_T_EVENT && p[0] == MIMHID_EV_REPORT_LOST) {
            assert(n == 4 && p[1] == 0);
            lost += p[2] | (p[3] << 8);
        }
    }
    return lost;
}

// Result of the last command: 1 = ACK, 0 = NAK (code in g_nak_code), -1 = no reply.
static int g_nak_code;
static uint8_t g_ack_data[32];
static uint16_t g_ack_data_len;

static int result_of(uint8_t type) {
    size_t pos = 0;
    uint8_t t, p[2048];
    uint16_t n;
    int result = -1;
    while ((t = next_out(&pos, p, &n)) != 0) {
        if ((t == MIMHID_T_ACK || t == MIMHID_T_NAK) && p[1] == type) {
            assert(p[0] == g_seq);
            result = t == MIMHID_T_ACK;
            if (t == MIMHID_T_NAK) g_nak_code = p[2];
            if (t == MIMHID_T_ACK) {
                g_ack_data_len = (uint16_t)(n - 2);
                memcpy(g_ack_data, &p[2], g_ack_data_len);
            }
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

static uint8_t g_ep0_size = 64;
static uint8_t g_config_value = 1;
static uint8_t g_bm_attributes = 0xA0;
static uint8_t g_device_class = 0xEF;
static uint8_t g_device_subclass = 0x02;
// Endpoint addresses NODE_BEGIN carries instead of the ones it derives from the interface; -1: none.
static int g_force_in = -1;
static int g_force_out = -1;

// A string on the wire: a count of UTF-16 code units, then the units, little endian.
static size_t put_units(uint8_t* p, size_t n, const uint16_t* units, size_t count) {
    p[n++] = (uint8_t)count;
    for (size_t i = 0; i < count; i++) {
        p[n++] = (uint8_t)(units[i] & 0xFF);
        p[n++] = (uint8_t)(units[i] >> 8);
    }
    return n;
}

static size_t put_ascii(uint8_t* p, size_t n, const char* text) {
    uint16_t units[MIMHID_STRING_MAX];
    size_t count = strlen(text);
    if (count > MIMHID_STRING_MAX) count = MIMHID_STRING_MAX;
    for (size_t i = 0; i < count; i++) units[i] = (uint8_t)text[i];
    return put_units(p, n, units, count);
}

static int device_config_units(uint8_t flags, const char* serial, const uint16_t* maker,
                               size_t maker_count) {
    uint8_t p[512];
    size_t n = 0;
    p[n++] = 0x34; p[n++] = 0x12;          // vid
    p[n++] = 0x78; p[n++] = 0x56;          // pid
    p[n++] = 0x00; p[n++] = 0x02;          // bcd
    p[n++] = g_device_class; p[n++] = g_device_subclass; p[n++] = 0x01;
    p[n++] = 0x09; p[n++] = 0x04;          // language
    p[n++] = 0x00; p[n++] = 0x02;          // bcdUSB 0x0200
    p[n++] = g_ep0_size;                   // ep0_max_packet_size
    p[n++] = g_config_value;               // configuration_value
    p[n++] = g_bm_attributes;              // bm_attributes (0x80 | 0x20 remote wakeup)
    p[n++] = 50;                           // bmax_power (100 mA)
    p[n++] = flags;                        // flags
    const char* strs[4] = {"Maker", "Product", serial, "Config"};
    for (int i = 0; i < 4; i++) {
        n = (i == 0 && maker != NULL) ? put_units(p, n, maker, maker_count) : put_ascii(p, n, strs[i]);
    }
    return command(MIMHID_T_DEVICE_CONFIG, p, n);
}

static int device_config(uint8_t flags, const char* serial) {
    return device_config_units(flags, serial, NULL, 0);
}

static const uint8_t k_desc_a[] = {  // a small valid keyboard-like descriptor
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0xC0};
static const uint8_t k_desc_b[] = {  // mouse-like
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02, 0x09, 0x30, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08,
    0x95, 0x01, 0x81, 0x06, 0xC0};

static int node_begin_full(uint8_t id, uint8_t iface, uint16_t desc_len, uint8_t poll, uint16_t ep,
                           uint16_t ep_out, uint8_t cls, uint8_t subclass, uint8_t protocol,
                           const char* str) {
    uint8_t p[24 + 1 + 2 * MIMHID_STRING_MAX];
    size_t n = 0;
    p[n++] = id; p[n++] = iface;
    p[n++] = (uint8_t)(desc_len & 0xFF); p[n++] = (uint8_t)(desc_len >> 8);
    p[n++] = cls;                          // interface_class
    p[n++] = subclass;                     // interface_subclass
    p[n++] = protocol;                     // interface_protocol
    p[n++] = 0x11; p[n++] = 0x01;          // bcdHID 1.11
    p[n++] = 0;                            // country
    p[n++] = g_force_in >= 0 ? (uint8_t)g_force_in : (uint8_t)(0x81 + iface); // ep_in_addr
    p[n++] = (uint8_t)(ep & 0xFF); p[n++] = (uint8_t)(ep >> 8); // ep_size
    p[n++] = poll;                         // poll_interval
    p[n++] = g_force_out >= 0 ? (uint8_t)g_force_out : ep_out ? (uint8_t)(0x01 + iface) : 0; // ep_out_addr
    p[n++] = (uint8_t)(ep_out & 0xFF); p[n++] = (uint8_t)(ep_out >> 8); // ep_out_size
    p[n++] = ep_out ? poll : 0;            // poll_interval_out
    p[n++] = 0;                            // idle_rate
    p[n++] = 0;                            // flags
    n = put_ascii(p, n, str);
    return command(MIMHID_T_NODE_BEGIN, p, n);
}

static int node_begin_out(uint8_t id, uint8_t iface, uint16_t desc_len, uint8_t poll, uint16_t ep,
                          uint16_t ep_out, uint8_t boot, const char* str) {
    return node_begin_full(id, iface, desc_len, poll, ep, ep_out, 0x03, boot ? 1 : 0, boot, str);
}

static int node_begin(uint8_t id, uint8_t iface, uint16_t desc_len, uint8_t poll, uint16_t ep, uint8_t boot,
                      const char* str) {
    return node_begin_out(id, iface, desc_len, poll, ep, 0, boot, str);
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
    assert(device_config(0, "SN-1") == 1);
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

// Strings are UTF-16: a Japanese name and a surrogate pair reach the string descriptor unchanged;
// a zero unit and a count above the limit are refused.
static void test_strings_utf16(void) {
    static const uint16_t maker[] = {0x65E5, 0x672C, 0xD83D, 0xDE00, 'A'}; // two kanji, U+1F600, A
    assert(device_config_units(0, "", maker, 5) == 1);
    assert(node_begin(0, 0, sizeof(k_desc_a), 1, 0, 0, "") == 1);
    assert(node_data(0, 0, k_desc_a, sizeof(k_desc_a)) == 1);
    assert(commit() == 1);
    const uint16_t* s = tud_descriptor_string_cb(1, 0x0409);
    assert((s[0] & 0xFF) == 2 + 2 * 5);
    for (int i = 0; i < 5; i++) assert(s[1 + i] == maker[i]);

    static const uint16_t with_zero[] = {'a', 0, 'b'};
    assert(device_config_units(0, "", with_zero, 3) == 0);
    uint16_t longest[MIMHID_STRING_MAX + 1];
    for (size_t i = 0; i < MIMHID_STRING_MAX + 1; i++) longest[i] = 0x3042;
    assert(device_config_units(0, "", longest, MIMHID_STRING_MAX) == 1);
    assert(device_config_units(0, "", longest, MIMHID_STRING_MAX + 1) == 0);
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

    // a bad CRC is skipped and the next frame is still seen
    reset_io();
    put_frame(MIMHID_T_REPORT_BASE, a, sizeof(a));
    g_in[g_in_len - 1] ^= 0xFF;
    const uint8_t junk[] = {0x00, MIMHID_SYNC, MIMHID_SYNC, 0x13, 0x7F};
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
    // loss reporting is off until asked: nothing is said about the 15 that did not fit
    g_now += 25;
    pump();
    assert(lost_reported() == 0);
    g_busy[0] = false;
    for (int i = 0; i < 6; i++) pump();
    assert(g_nsent == 15 && g_sent[0].data[1] == 0 && g_sent[14].data[1] == 14);

    // LOSS_CONFIG checks its payload: enabled is 0 or 1, and the interval is 0 while disabled
    {
        const uint8_t bad_flag[3] = {2, 0, 0};
        expect_nak(command(MIMHID_T_LOSS_CONFIG, bad_flag, 3), MIMHID_NAK_PARAM);
        const uint8_t off_with_interval[3] = {0, 5, 0};
        expect_nak(command(MIMHID_T_LOSS_CONFIG, off_with_interval, 3), MIMHID_NAK_PARAM);
        const uint8_t short_payload[2] = {1, 0};
        expect_nak(command(MIMHID_T_LOSS_CONFIG, short_payload, 2), MIMHID_NAK_PARAM);
    }

    // enabled with a 50 ms interval: drops are gathered and reported once per interval
    reset_io();
    {
        const uint8_t on[3] = {1, 50, 0};
        assert(command(MIMHID_T_LOSS_CONFIG, on, 3) == 1);
    }
    reset_io();
    g_busy[0] = true;
    for (int i = 0; i < 20; i++) {
        uint8_t r[2] = {1, (uint8_t)i};
        report(0, r, 2);
    }
    pump();
    assert(lost_reported() == 5); // the first report after enabling goes out at once
    g_out_len = 0;
    for (int i = 0; i < 3; i++) {
        uint8_t r[2] = {1, 0};
        report(0, r, 2);
    }
    g_now += 10;
    pump();
    assert(lost_reported() == 0); // still inside the interval
    g_now += 45;
    pump();
    assert(lost_reported() == 3);

    // disabled again: silent
    {
        const uint8_t off[3] = {0, 0, 0};
        assert(command(MIMHID_T_LOSS_CONFIG, off, 3) == 1);
    }
    g_out_len = 0;
    report(0, (const uint8_t[]){1, 0}, 2);
    g_now += 100;
    pump();
    assert(lost_reported() == 0);

    // a command sent again with the same sequence number is answered again, not run twice: the
    // second payload (off) is not applied, so a drop is still reported
    {
        const uint8_t on[3] = {1, 0, 0};
        const uint8_t off[3] = {0, 0, 0};
        assert(command(MIMHID_T_LOSS_CONFIG, on, 3) == 1);
        g_seq--;
        assert(command(MIMHID_T_LOSS_CONFIG, off, 3) == 1);
        g_out_len = 0;
        report(0, (const uint8_t[]){1, 0}, 2);
        pump();
        assert(lost_reported() == 1);
        // a PING is a new host, whose numbers start anew: the same number runs the command
        const uint8_t v = MIMHID_PROTO_VERSION;
        put_frame(MIMHID_T_PING, &v, 1);
        pump();
        g_seq--;
        assert(command(MIMHID_T_LOSS_CONFIG, off, 3) == 1);
        g_out_len = 0;
        report(0, (const uint8_t[]){1, 0}, 2);
        g_now += 1;
        pump();
        assert(lost_reported() == 0);
    }
    g_busy[0] = false;

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
    uint8_t p[2048];
    uint16_t n;
    assert(next_out(&pos, p, &n) == MIMHID_T_PONG && n == MIMHID_PONG_MIN_LEN + 8);
    assert(p[0] == FW_VERSION_MAJOR && p[1] == FW_VERSION_MINOR && p[2] == FW_VERSION_PATCH && p[3] == 1);
    assert(p[4] == MIMHID_PROTO_VERSION && p[5] == MIMHID_MAX_NODES && p[6] == MIMHID_HID_ITFS);
    assert(p[7] == MIMHID_PONG_FLAG_UNIQUE_ID && p[8] == MIMHID_CHIP_RP2040 && p[9] == 0);
    assert(u32at(&p[10]) == MIMHID_BOOT_BAUD && u32at(&p[14]) == g_max_rate);
    assert(u32at(&p[18]) == MIMHID_PONG_UART_BREAK);
    assert((p[22] | (p[23] << 8)) == MIMHID_MAX_DESC_BYTES);
    assert((p[24] | (p[25] << 8)) == MIMHID_MAX_REPORT_BYTES && MIMHID_MAX_REPORT_BYTES == 64);
    assert((p[26] | (p[27] << 8)) == MIMHID_FEATURE_MAX && (p[28] | (p[29] << 8)) == 64);
    assert((p[30] | (p[31] << 8)) == MIMHID_MAX_FRAME_PAYLOAD && MIMHID_MAX_FRAME_PAYLOAD >= MIMHID_MIN_FRAME_PAYLOAD);
    assert((p[32] | (p[33] << 8)) == MIMHID_CONTROL_DATA_MAX && p[34] == MIMHID_CONTROL_RULES);
    assert(p[35] == 8 && p[36] == 0x01 && p[43] == 0xEF);

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

static int set_baud_cmd(uint32_t baud) {
    uint8_t b[4];
    put32(b, baud);
    return command(MIMHID_T_SET_BAUD, b, 4);
}

static void ping_pong(uint8_t* p) {
    const uint8_t v = MIMHID_PROTO_VERSION;
    reset_io();
    put_frame(MIMHID_T_PING, &v, 1);
    pump();
    size_t pos = 0;
    uint16_t n;
    assert(next_out(&pos, p, &n) == MIMHID_T_PONG);
}

static void test_set_baud(void) {
    reset_io();
    uint8_t p[2048];
    // Zero, a rate above the board's maximum, and a rate the divider misses by over 2 % are refused.
    assert(set_baud_cmd(0) == 0 && g_nak_code == MIMHID_NAK_PARAM);
    assert(set_baud_cmd(g_max_rate + 1) == 0 && g_nak_code == MIMHID_NAK_PARAM);
    g_rate_error_ppm = 30000;
    assert(set_baud_cmd(1000000u) == 0 && g_nak_code == MIMHID_NAK_PARAM);
    // A wrong payload length is refused too.
    assert(command(MIMHID_T_SET_BAUD, (const uint8_t*)"\1", 1) == 0 && g_nak_code == MIMHID_NAK_PARAM);
    assert(g_baud == MIMHID_BOOT_BAUD && g_baud_changes == 0);

    // The ACK carries the rate the divider makes (1.5 % off here) and goes out before the switch.
    g_rate_error_ppm = 15000;
    assert(set_baud_cmd(1000000u) == 1 && g_baud == 1000000u);
    assert(g_ack_data_len == 4 && u32at(g_ack_data) == 1015000u);
    g_rate_error_ppm = 0;
    // An odd rate is as good as a round one.
    assert(set_baud_cmd(1234567u) == 1 && g_baud == 1234567u);
    assert(set_baud_cmd(g_max_rate) == 1 && g_baud == g_max_rate);

    // A valid frame at the new rate keeps it.
    g_now += MIMHID_BAUD_CONFIRM_MS - 1;
    ping_pong(p);
    assert(u32at(&p[10]) == g_max_rate);
    g_now += 10 * MIMHID_BAUD_CONFIRM_MS;
    pump();
    assert(g_baud == g_max_rate);

    // Back to the boot rate needs no confirmation.
    assert(set_baud_cmd(MIMHID_BOOT_BAUD) == 1 && g_baud == MIMHID_BOOT_BAUD);
    g_now += 10 * MIMHID_BAUD_CONFIRM_MS;
    pump();
    assert(g_baud == MIMHID_BOOT_BAUD);

    // Nothing valid at the new rate in time: back to the boot rate.
    assert(set_baud_cmd(921600u) == 1 && g_baud == 921600u);
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
    ping_pong(p);
    assert(u32at(&p[10]) == MIMHID_BOOT_BAUD);
}

// A BREAK returns the board to the boot rate, confirmed or not, and drops what it holds.
static void test_break(void) {
    uint8_t p[2048];
    assert(set_baud_cmd(3000000u) == 1);
    g_now += 1;
    ping_pong(p); // confirmed by a valid frame
    g_now += 10 * MIMHID_BAUD_CONFIRM_MS;
    pump();
    assert(g_baud == 3000000u);
    reset_io();
    const uint8_t v = MIMHID_PROTO_VERSION;
    put_frame(MIMHID_T_PING, &v, 1); // sent at the old rate, still unread when the BREAK is seen
    g_errors = HAL_UART_ERR_BREAK;
    pump();
    assert(g_baud == MIMHID_BOOT_BAUD);
    assert(g_out_len == 0); // the ping was discarded
    ping_pong(p);
    assert(u32at(&p[10]) == MIMHID_BOOT_BAUD);
    // A BREAK at the boot rate changes nothing but the discard.
    int changes = g_baud_changes;
    g_errors = HAL_UART_ERR_BREAK;
    pump();
    assert(g_baud_changes == changes);
}

// The counters, PROBE and the CRC.
static void test_counters_and_probe(void) {
    uint8_t reset = MIMHID_COUNTERS_F_RESET, keep = 0;
    assert(command(MIMHID_T_COUNTERS, &reset, 1) == 1); // start from zero
    uint8_t pat[100];
    for (unsigned i = 0; i < sizeof(pat); i++) pat[i] = MIMHID_PROBE_BYTE(i);
    reset_io();
    for (int i = 0; i < 3; i++) put_frame(MIMHID_T_PROBE, pat, 100);
    pump();
    assert(g_out_len == 0); // a probe is not answered
    // a probe with a wrong pattern, a frame with a wrong CRC, a framing error and an overrun
    pat[5] ^= 1;
    put_frame(MIMHID_T_PROBE, pat, 100);
    put_frame(MIMHID_T_PROBE, pat, 100);
    g_in[g_in_len - 1] ^= 0x40;
    g_errors = HAL_UART_ERR_FRAMING | HAL_UART_ERR_OVERRUN;
    pump();
    assert(command(MIMHID_T_COUNTERS, &keep, 1) == 1 && g_ack_data_len == 10);
    assert((g_ack_data[0] | (g_ack_data[1] << 8)) == 3);        // probes_good
    assert((g_ack_data[2] | (g_ack_data[3] << 8)) == 2);        // bad_crc: wrong pattern + wrong CRC
    assert((g_ack_data[4] | (g_ack_data[5] << 8)) == 1);        // framing
    assert((g_ack_data[6] | (g_ack_data[7] << 8)) == 1);        // uart_overrun
    assert((g_ack_data[8] | (g_ack_data[9] << 8)) == 0);        // ring_overrun
    // Read without reset keeps them; read with reset returns them once more and then zeroes.
    assert(command(MIMHID_T_COUNTERS, &reset, 1) == 1 && (g_ack_data[0] | (g_ack_data[1] << 8)) == 3);
    assert(command(MIMHID_T_COUNTERS, &keep, 1) == 1);
    for (int i = 0; i < 10; i++) assert(g_ack_data[i] == 0);
    // The counters stop at 65535.
    for (int i = 0; i < 70000 / 1000; i++) {
        reset_io();
        for (int k = 0; k < 1000; k++) g_errors |= 0;
        g_errors = HAL_UART_ERR_FRAMING;
        pump();
    }
    assert(command(MIMHID_T_COUNTERS, &keep, 1) == 1);
    assert((g_ack_data[4] | (g_ack_data[5] << 8)) == 70);

    // Two flips in the same bit position cancel in an XOR but not in a CRC.
    reset_io();
    uint8_t d[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    put_cmd(MIMHID_T_QUEUE_DEPTH, d, 2);
    g_in[g_in_len - 5] ^= 0x10; // flips one byte of the payload...
    g_in[g_in_len - 4] ^= 0x10; // ...and the same bit of the next
    g_out_len = 0;
    pump();
    assert(result_of(MIMHID_T_QUEUE_DEPTH) == -1);
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
    expect_nak(node_begin(MIMHID_MAX_NODES, 0, 4, 1, 0, 0, ""), MIMHID_NAK_PARAM); // node id out of range
    expect_nak(node_begin(0, 0, 0, 1, 0, 0, ""), MIMHID_NAK_PARAM);           // empty descriptor
    expect_nak(node_begin(0, 0, 4, 1, 65, 0, ""), MIMHID_NAK_PARAM);           // endpoint size above 64
    expect_nak(node_begin(0, 0, 4, 1, 0, 3, ""), MIMHID_NAK_PARAM);           // bad boot protocol
    // HID 1.11 4.2, 4.3: subclass 0 or 1, protocol 0, 1 or 2.
    expect_nak(node_begin_full(0, 0, 4, 1, 0, 0, 0x03, 2, 0, ""), MIMHID_NAK_PARAM);
    expect_nak(node_begin_full(0, 0, 4, 1, 0, 0, 0x03, 1, 3, ""), MIMHID_NAK_PARAM);
    // USB 2.0 9.6.6: endpoint number 1 to 15, bits 6..4 zero, bit 7 the direction.
    assert(device_config(0, "x") == 1);
    static const int bad_in_list[5] = {0x80, 0x90, 0xF1, 0x01, 0x41};
    for (size_t bad_in_i = 0; bad_in_i < 5; ++bad_in_i) {
        const int bad_in = bad_in_list[bad_in_i];
        g_force_in = bad_in;
        expect_nak(node_begin(0, 0, 4, 1, 0, 0, ""), MIMHID_NAK_PARAM);
    }
    g_force_in = 0x8F;
    assert(node_begin(0, 0, sizeof(k_desc_a), 1, 0, 0, "") == 1);
    g_force_in = -1;
    assert(device_config(0, "x") == 1);
    static const int bad_out_list[4] = {0x80, 0x81, 0x10, 0x40};
    for (size_t bad_out_i = 0; bad_out_i < 4; ++bad_out_i) {
        const int bad_out = bad_out_list[bad_out_i];
        g_force_out = bad_out;
        expect_nak(node_begin_out(0, 0, 4, 1, 8, 8, 0, ""), MIMHID_NAK_PARAM);
    }
    g_force_out = 0x0F;
    assert(node_begin_out(0, 0, sizeof(k_desc_a), 1, 8, 8, 0, "") == 1);
    g_force_out = -1;
    assert(device_config(0, "x") == 1);
    // The stack opens HID interfaces only; any other class would leave the device unconfigured.
    expect_nak(node_begin_full(0, 0, 4, 1, 0, 0, 0x02, 0, 0, ""), MIMHID_NAK_UNSUPPORTED);
    expect_nak(node_begin_full(0, 0, 4, 1, 0, 0, 0xFF, 0, 0, ""), MIMHID_NAK_UNSUPPORTED);
    expect_nak(node_begin_full(0, 0, 4, 1, 0, 0, 0x00, 0, 0, ""), MIMHID_NAK_UNSUPPORTED);
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

// Packet sizes are taken as given, a Feature answer may be longer than a packet, and a frame
// longer than 127 bytes carries a two-byte length in both directions.
static void test_wide_values(void) {
    assert(device_config(0, "") == 1);
    assert(node_begin_out(0, 0, sizeof(k_desc_a), 1, 6, 5, 0, "") == 1);
    assert(node_data(0, 0, k_desc_a, sizeof(k_desc_a)) == 1);
    expect_nak(node_begin_out(1, 1, sizeof(k_desc_b), 1, MIMHID_MAX_ENDPOINT_BYTES + 1, 0, 0, ""), MIMHID_NAK_PARAM);
    expect_nak(node_begin_out(1, 1, sizeof(k_desc_b), 1, 8, MIMHID_MAX_ENDPOINT_BYTES + 1, 0, ""), MIMHID_NAK_PARAM);

    uint8_t f[2 + 644];
    f[0] = 0;
    f[1] = 0x07;
    for (size_t i = 0; i < 644; i++) f[2 + i] = (uint8_t)(i * 7 + 3);
    assert(command(MIMHID_T_FEATURE, f, sizeof(f)) == 1);
    static uint8_t too_long[2 + MIMHID_FEATURE_MAX + 1];
    too_long[0] = 0;
    too_long[1] = 0x08;
    expect_nak(command(MIMHID_T_FEATURE, too_long, sizeof(too_long)), MIMHID_NAK_PARAM);
    assert(commit() == 1);

    // The interface's IN endpoint (after the configuration and interface descriptors and the
    // HID descriptor) and its OUT endpoint.
    const uint8_t* c = tud_descriptor_configuration_cb(0);
    assert(c[27 + 2] == 0x81 && c[27 + 4] == 6 && c[27 + 5] == 0);
    assert(c[34 + 2] == 0x01 && c[34 + 4] == 5 && c[34 + 5] == 0);

    static uint8_t out[MIMHID_FEATURE_MAX];
    assert(tud_hid_get_report_cb(0, 7, HID_REPORT_TYPE_FEATURE, out, sizeof(out)) == 644 &&
           memcmp(out, f + 2, 644) == 0);

    // The target writes 300 bytes: the event frame has a two-byte length.
    reset_io();
    static uint8_t written[300];
    for (size_t i = 0; i < sizeof(written); i++) written[i] = (uint8_t)(i + 1);
    tud_hid_set_report_cb(0, 5, HID_REPORT_TYPE_OUTPUT, written, sizeof(written));
    size_t pos = 0;
    static uint8_t p[2048];
    uint16_t n;
    assert(next_out(&pos, p, &n) == MIMHID_T_EVENT && n == 1 + 3 + 300 && p[0] == MIMHID_EV_SET_REPORT &&
           p[1] == 0 && p[3] == 5 && memcmp(p + 4, written, 300) == 0);
    reset_io();
    tud_hid_set_report_cb(0, 0, HID_REPORT_TYPE_OUTPUT, written, sizeof(written));
    pos = 0;
    assert(next_out(&pos, p, &n) == MIMHID_T_EVENT && p[0] == MIMHID_EV_OUTPUT_REPORT && p[1] == 0 &&
           (p[2] | (p[3] << 8)) == 300 && memcmp(p + 4, written, 300) == 0);
}

// ---- control transfers ----
typedef struct {
    uint8_t bm_mask, bm_value, request_mask, request_value;
    uint16_t value_mask, value_value, index_mask, index_value;
    uint16_t timeout_ms;
    uint8_t action;
} trule_t;

static int control_config(uint8_t flags, const trule_t* r, int n) {
    uint8_t p[2 + MIMHID_CONTROL_RULES * MIMHID_CONTROL_RULE_LEN + 15];
    size_t k = 0;
    p[k++] = flags;
    p[k++] = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        p[k++] = r[i].bm_mask; p[k++] = r[i].bm_value; p[k++] = r[i].request_mask; p[k++] = r[i].request_value;
        p[k++] = (uint8_t)r[i].value_mask; p[k++] = (uint8_t)(r[i].value_mask >> 8);
        p[k++] = (uint8_t)r[i].value_value; p[k++] = (uint8_t)(r[i].value_value >> 8);
        p[k++] = (uint8_t)r[i].index_mask; p[k++] = (uint8_t)(r[i].index_mask >> 8);
        p[k++] = (uint8_t)r[i].index_value; p[k++] = (uint8_t)(r[i].index_value >> 8);
        p[k++] = (uint8_t)r[i].timeout_ms; p[k++] = (uint8_t)(r[i].timeout_ms >> 8);
        p[k++] = r[i].action;
    }
    return command(MIMHID_T_CONTROL_CONFIG, p, k);
}

// A rule for one request, and any value and index.
static trule_t rule_for(uint8_t bm, uint8_t request, uint16_t timeout_ms, uint8_t action) {
    trule_t r = {0xFF, bm, 0xFF, request, 0, 0, 0, 0, timeout_ms, action};
    return r;
}

static dcd_event_t setup_event(uint8_t bm, uint8_t request, uint16_t value, uint16_t index, uint16_t length) {
    const uint8_t raw[8] = {bm, request, (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)index,
                            (uint8_t)(index >> 8), (uint8_t)length, (uint8_t)(length >> 8)};
    dcd_event_t e;
    memset(&e, 0, sizeof(e));
    e.rhport = 0;
    e.event_id = DCD_EVENT_SETUP_RECEIVED;
    memcpy(&e.setup_received, raw, 8);
    return e;
}

static void setup(uint8_t bm, uint8_t request, uint16_t value, uint16_t index, uint16_t length) {
    const dcd_event_t e = setup_event(bm, request, value, index, length);
    __wrap_dcd_event_handler(&e, true);
}

// The next CONTROL_SETUP event of the output; *token and *flags and the 8 bytes are filled.
static bool next_control_event(size_t* pos, uint8_t* token, uint8_t* flags, uint8_t* raw) {
    uint8_t t, p[2048];
    uint16_t n;
    while ((t = next_out(pos, p, &n)) != 0) {
        if (t == MIMHID_T_EVENT && p[0] == MIMHID_EV_CONTROL_SETUP) {
            assert(n == 1 + 2 + 8);
            *token = p[1];
            *flags = p[2];
            memcpy(raw, &p[3], 8);
            return true;
        }
    }
    return false;
}

static int reply(uint8_t token, uint8_t action, const uint8_t* data, size_t n) {
    uint8_t p[2 + 1100];
    p[0] = token;
    p[1] = action;
    if (n) memcpy(&p[2], data, n);
    return command(MIMHID_T_CONTROL_REPLY, p, 2 + n);
}

// What the board answers by itself when no rule holds a request, where the specifications fix the
// answer: GetStatus() to an interface (USB 2.0 9.4.5) and Get_Report of an Input report (HID 1.11 7.2.1).
static void test_specified_answers(void) {
    assert(device_config(0, "") == 1);
    assert(node_begin(0, 0, sizeof(k_desc_a), 4, 16, 0, "") == 1);
    assert(node_data(0, 0, k_desc_a, sizeof(k_desc_a)) == 1);
    assert(commit() == 1);
    g_mounted = true;
    reset_io();

    // GetStatus() to the interface that exists: two bytes of zero, sent through the vendor callback.
    setup(0x81, TUSB_REQ_GET_STATUS, 0, 0, 2);
    assert(g_nforwarded == 1);
    tusb_control_request_t req = g_forwarded[0].setup_received;
    assert(req.bmRequestType == 0xC1 && req.bRequest == TUSB_REQ_GET_STATUS && req.wLength == 2);
    assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &req));
    assert(g_xfer.kind == 1 && g_xfer.length == 2 && g_xfer.copy[0] == 0 && g_xfer.copy[1] == 0);
    assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_DATA, &req));
    assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_ACK, &req));
    // Answered once: the same setup stage again is refused.
    assert(!tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &req));

    // Everything the specification leaves open goes to TinyUSB as it came.
    reset_io();
    setup(0x81, TUSB_REQ_GET_STATUS, 0, 1, 2); // an interface that does not exist: a Request Error
    setup(0x81, TUSB_REQ_GET_STATUS, 0, 0, 1); // wLength other than 2: not specified
    setup(0x81, TUSB_REQ_GET_STATUS, 1, 0, 2); // wValue other than 0: not specified
    setup(0x80, TUSB_REQ_GET_STATUS, 0, 0, 2); // the device: TinyUSB answers
    setup(0x82, TUSB_REQ_GET_STATUS, 0, 0x81, 2); // an endpoint: TinyUSB answers
    assert(g_nforwarded == 5);
    for (int i = 0; i < 5; i++) assert(g_forwarded[i].setup_received.bmRequestType != 0xC1);
    g_mounted = false; // address state: a Request Error
    reset_io();
    setup(0x81, TUSB_REQ_GET_STATUS, 0, 0, 2);
    assert(g_nforwarded == 1 && g_forwarded[0].setup_received.bmRequestType == 0x81);
    g_mounted = true;

    // A rule that matches it is stronger: the request is held for the host.
    trule_t held = rule_for(0x81, TUSB_REQ_GET_STATUS, 100, MIMHID_CONTROL_STALL);
    assert(control_config(0, &held, 1) == 1);
    reset_io();
    setup(0x81, TUSB_REQ_GET_STATUS, 0, 0, 2);
    assert(g_nforwarded == 0);
    pump();
    size_t pos = 0;
    uint8_t token, flags, raw[8];
    assert(next_control_event(&pos, &token, &flags, raw) && (flags & MIMHID_CONTROL_F_HELD) && raw[0] == 0x81);
    assert(control_config(0, NULL, 0) == 1);

    // Get_Report of an Input report is the last report sent on the interface, and only that.
    uint8_t buf[8];
    assert(tud_hid_get_report_cb(0, 0, HID_REPORT_TYPE_INPUT, buf, sizeof(buf)) == 0); // none sent yet
    const uint8_t a[] = {1, 0x02, 0x03};
    const uint8_t b[] = {1, 0x05, 0x06};
    reset_io();
    report(0, a, sizeof(a));
    pump();
    assert(g_nsent == 1);
    assert(tud_hid_get_report_cb(0, 1, HID_REPORT_TYPE_INPUT, buf, sizeof(buf)) == 2 && buf[0] == 0x02 && buf[1] == 0x03);
    assert(tud_hid_get_report_cb(0, 2, HID_REPORT_TYPE_INPUT, buf, sizeof(buf)) == 0); // another Report ID
    assert(tud_hid_get_report_cb(0, 0, HID_REPORT_TYPE_INPUT, buf, sizeof(buf)) == 3 && buf[0] == 1 && buf[2] == 0x03);
    assert(tud_hid_get_report_cb(0, 1, HID_REPORT_TYPE_INPUT, buf, 1) == 1 && buf[0] == 0x02); // cut to wLength
    report(0, b, sizeof(b));
    pump();
    assert(tud_hid_get_report_cb(0, 1, HID_REPORT_TYPE_INPUT, buf, sizeof(buf)) == 2 && buf[0] == 0x05);
    assert(tud_hid_get_report_cb(1, 1, HID_REPORT_TYPE_INPUT, buf, sizeof(buf)) == 0); // another interface
    assert(tud_hid_get_report_cb(MIMHID_HID_ITFS, 0, HID_REPORT_TYPE_INPUT, buf, sizeof(buf)) == 0);

    // A new configuration forgets it.
    assert(device_config(0, "") == 1);
    assert(node_begin(0, 0, sizeof(k_desc_a), 4, 16, 0, "") == 1);
    assert(node_data(0, 0, k_desc_a, sizeof(k_desc_a)) == 1);
    assert(commit() == 1);
    assert(tud_hid_get_report_cb(0, 1, HID_REPORT_TYPE_INPUT, buf, sizeof(buf)) == 0);
}

static void test_control(void) {
    assert(device_config(0, "") == 1);
    assert(node_begin(0, 0, sizeof(k_desc_a), 4, 16, 0, "") == 1);
    assert(node_data(0, 0, k_desc_a, sizeof(k_desc_a)) == 1);
    assert(commit() == 1);
    reset_io();

    // A board that was not configured for it passes everything on and says nothing.
    setup(0x80, TUSB_REQ_GET_DESCRIPTOR, 0x0100, 0, 18);
    pump();
    assert(g_nforwarded == 1 && g_out_len == 0);

    // Every transfer reported, none held.
    assert(control_config(1, NULL, 0) == 1);
    reset_io();
    setup(0x80, TUSB_REQ_GET_DESCRIPTOR, 0x0302, 0x0409, 255);
    setup(0x00, TUSB_REQ_SET_ADDRESS, 7, 0, 0);
    pump();
    size_t pos = 0;
    uint8_t token, flags, raw[8], token2;
    assert(next_control_event(&pos, &token, &flags, raw) && token != 0 && flags == 0);
    assert(raw[0] == 0x80 && raw[1] == TUSB_REQ_GET_DESCRIPTOR && raw[2] == 0x02 && raw[3] == 0x03 &&
           raw[4] == 0x09 && raw[5] == 0x04 && raw[6] == 255 && raw[7] == 0);
    assert(next_control_event(&pos, &token2, &flags, raw) && token2 != token && flags == MIMHID_CONTROL_F_FIXED);
    assert(g_nforwarded == 2);

    // Too many for the queue: the next one carries the loss.
    reset_io();
    for (int i = 0; i < 12; i++) setup(0x80, TUSB_REQ_GET_STATUS, 0, 0, 2);
    assert(g_nforwarded == 12);
    pump();
    pos = 0;
    int seen = 0;
    while (next_control_event(&pos, &token, &flags, raw)) seen++;
    assert(seen == 7); // one slot of eight is the full mark
    reset_io();
    setup(0x80, TUSB_REQ_GET_STATUS, 0, 0, 2);
    pump();
    pos = 0;
    assert(next_control_event(&pos, &token, &flags, raw) && (flags & MIMHID_CONTROL_F_LOST));

    // A host-to-device data stage longer than the board takes is never held; with the report
    // flag it is reported as too long.
    trule_t any_class = {0x60, 0x20, 0, 0, 0, 0, 0, 0, 50, MIMHID_CONTROL_STALL};
    assert(control_config(0, &any_class, 1) == 1);
    reset_io();
    setup(0x21, HID_REQ_CONTROL_SET_REPORT, 0x0201, 0, MIMHID_CONTROL_DATA_MAX + 1);
    pump();
    assert(g_nforwarded == 1 && g_out_len == 0);
    assert(control_config(1, &any_class, 1) == 1);
    reset_io();
    setup(0x21, HID_REQ_CONTROL_SET_REPORT, 0x0201, 0, MIMHID_CONTROL_DATA_MAX + 1);
    pump();
    pos = 0;
    assert(g_nforwarded == 1 && next_control_event(&pos, &token, &flags, raw) &&
           flags == MIMHID_CONTROL_F_TOO_LONG);

    // An answer with data (device to host).
    trule_t vendor_in = rule_for(0xC0, 0x42, 100, MIMHID_CONTROL_STALL);
    assert(control_config(0, &vendor_in, 1) == 1);
    reset_io();
    setup(0xC0, 0x42, 5, 6, 64);
    assert(g_nforwarded == 0); // held
    pump();
    pos = 0;
    assert(next_control_event(&pos, &token, &flags, raw) && flags == MIMHID_CONTROL_F_HELD);
    assert(raw[0] == 0xC0 && raw[1] == 0x42);
    expect_nak(reply((uint8_t)(token + 1), MIMHID_CONTROL_ANSWER, NULL, 0), MIMHID_NAK_STATE);
    const uint8_t answer[4] = {1, 2, 3, 4};
    assert(reply(token, MIMHID_CONTROL_ANSWER, answer, sizeof(answer)) == 1);
    assert(g_nforwarded == 1);
    assert(g_forwarded[0].setup_received.bmRequestType == 0xC0); // already a vendor request
    {
        const tusb_control_request_t req = g_forwarded[0].setup_received;
        assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &req));
        assert(g_xfer.kind == 1 && g_xfer.length == 4 && memcmp(g_xfer.copy, answer, 4) == 0);
        assert(g_xfer.w_length == 64); // shorter than asked: the stack keeps wLength for its ZLP
        assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_DATA, &req));
        assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_ACK, &req));
        // The same request again, with nobody having been asked, is refused as before.
        assert(!tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &req));
    }
    expect_nak(reply(token, MIMHID_CONTROL_ANSWER, answer, 1), MIMHID_NAK_STATE); // too late

    // A longer answer than wLength is sent whole, as a device that overruns the host's length does.
    reset_io();
    setup(0xC0, 0x42, 0, 0, 2);
    pump();
    pos = 0;
    assert(next_control_event(&pos, &token, &flags, raw));
    assert(reply(token, MIMHID_CONTROL_ANSWER, answer, sizeof(answer)) == 1);
    {
        const tusb_control_request_t req = g_forwarded[0].setup_received;
        assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &req));
        assert(g_xfer.kind == 1 && g_xfer.length == 4);
        assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_ACK, &req));
    }

    // With wLength 0 an answer with data still starts the data stage, as the real device does; an
    // empty answer is the status stage alone.
    reset_io();
    setup(0xC0, 0x42, 0, 0, 0);
    pump();
    pos = 0;
    assert(next_control_event(&pos, &token, &flags, raw));
    assert(reply(token, MIMHID_CONTROL_ANSWER, answer, 2) == 1);
    assert(g_nforwarded == 1 && g_forwarded[0].setup_received.bmRequestType == 0xC0);
    {
        const tusb_control_request_t req = g_forwarded[0].setup_received;
        assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &req));
        assert(g_xfer.kind == 1 && g_xfer.length == 2 && memcmp(g_xfer.copy, answer, 2) == 0);
        assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_ACK, &req));
    }
    reset_io();
    setup(0xC0, 0x42, 0, 0, 0);
    pump();
    pos = 0;
    assert(next_control_event(&pos, &token, &flags, raw));
    assert(reply(token, MIMHID_CONTROL_ANSWER, NULL, 0) == 1);
    assert(g_nforwarded == 1 && g_forwarded[0].setup_received.bmRequestType == 0x40); // direction dropped
    {
        const tusb_control_request_t req = g_forwarded[0].setup_received;
        assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &req));
        assert(g_xfer.kind == 2);
        assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_ACK, &req));
    }

    // A stall.
    reset_io();
    setup(0xC0, 0x42, 0, 0, 2);
    pump();
    pos = 0;
    assert(next_control_event(&pos, &token, &flags, raw));
    assert(reply(token, MIMHID_CONTROL_STALL, NULL, 0) == 1);
    {
        const tusb_control_request_t req = g_forwarded[0].setup_received;
        assert(!tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &req));
    }

    // No reply in time: the rule's action. STALL first, then ANSWER with nothing, then LEAVE.
    reset_io();
    setup(0xC0, 0x42, 0, 0, 2);
    pump();
    assert(g_nforwarded == 0);
    g_now += 99;
    pump();
    assert(g_nforwarded == 0);
    g_now += 1;
    pump();
    assert(g_nforwarded == 1);
    {
        const tusb_control_request_t req = g_forwarded[0].setup_received;
        assert(!tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &req));
    }

    trule_t std_get = rule_for(0x80, TUSB_REQ_GET_DESCRIPTOR, 30, MIMHID_CONTROL_ANSWER);
    assert(control_config(0, &std_get, 1) == 1);
    reset_io();
    setup(0x80, TUSB_REQ_GET_DESCRIPTOR, 0x0300, 0, 4);
    pump();
    g_now += 30;
    pump();
    assert(g_nforwarded == 1 && g_forwarded[0].setup_received.bmRequestType == 0xC0); // retyped
    {
        const tusb_control_request_t req = g_forwarded[0].setup_received;
        assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &req));
        assert(g_xfer.kind == 1 && g_xfer.length == 0); // an empty data stage
        assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_ACK, &req));
    }

    trule_t std_leave = rule_for(0x80, TUSB_REQ_GET_DESCRIPTOR, 30, MIMHID_CONTROL_LEAVE);
    assert(control_config(0, &std_leave, 1) == 1);
    reset_io();
    setup(0x80, TUSB_REQ_GET_DESCRIPTOR, 0x0300, 0, 4);
    pump();
    pos = 0;
    assert(next_control_event(&pos, &token, &flags, raw));
    assert(reply(token, MIMHID_CONTROL_LEAVE, NULL, 0) == 1);
    assert(g_nforwarded == 1 && g_forwarded[0].setup_received.bmRequestType == 0x80); // as it came
    assert(g_forwarded[0].setup_received.bRequest == TUSB_REQ_GET_DESCRIPTOR);

    // A request without a data stage: ANSWER is a plain acknowledgement.
    trule_t no_data = rule_for(0x00, TUSB_REQ_SET_FEATURE, 30, MIMHID_CONTROL_STALL);
    assert(control_config(0, &no_data, 1) == 1);
    reset_io();
    setup(0x00, TUSB_REQ_SET_FEATURE, 1, 0, 0);
    pump();
    pos = 0;
    assert(next_control_event(&pos, &token, &flags, raw));
    assert(reply(token, MIMHID_CONTROL_ANSWER, NULL, 0) == 1);
    {
        const tusb_control_request_t req = g_forwarded[0].setup_received;
        assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &req));
        assert(g_xfer.kind == 2);
        assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_ACK, &req));
    }

    // SET_ADDRESS and SET_CONFIGURATION are never held, whatever the rules say.
    trule_t everything = {0, 0, 0, 0, 0, 0, 0, 0, 100, MIMHID_CONTROL_STALL};
    assert(control_config(0, &everything, 1) == 1);
    reset_io();
    setup(0x00, TUSB_REQ_SET_ADDRESS, 9, 0, 0);
    setup(0x00, TUSB_REQ_SET_CONFIGURATION, 1, 0, 0);
    assert(g_nforwarded == 2);
    setup(0x80, TUSB_REQ_GET_DESCRIPTOR, 0x0100, 0, 18);
    assert(g_nforwarded == 2); // this one is held

    // A new request, a bus reset, and a new configuration each end a hold.
    reset_io();
    pump();
    pos = 0;
    assert(next_control_event(&pos, &token, &flags, raw));
    setup(0x80, TUSB_REQ_GET_DESCRIPTOR, 0x0200, 0, 9); // held in turn
    expect_nak(reply(token, MIMHID_CONTROL_ANSWER, NULL, 0), MIMHID_NAK_STATE);
    {
        dcd_event_t bus = {.rhport = 0, .event_id = DCD_EVENT_BUS_RESET};
        __wrap_dcd_event_handler(&bus, true);
    }
    g_now += 1000;
    pump();
    assert(g_nforwarded == 1 && g_forwarded[0].event_id == DCD_EVENT_BUS_RESET); // nothing else went through

    reset_io();
    setup(0x80, TUSB_REQ_GET_DESCRIPTOR, 0x0100, 0, 18);
    assert(control_config(0, NULL, 0) == 1); // the transfer is given to the stack as it came
    assert(g_nforwarded == 1 && g_forwarded[0].setup_received.bmRequestType == 0x80);
    reset_io();
    setup(0x80, TUSB_REQ_GET_DESCRIPTOR, 0x0100, 0, 18);
    assert(g_nforwarded == 1); // no rules any more

    // Bad configurations are refused and change nothing.
    trule_t bad = rule_for(0xC0, 0x42, 0, MIMHID_CONTROL_STALL);
    expect_nak(control_config(0, &bad, 1), MIMHID_NAK_PARAM); // no time
    // USB 2.0 9.2.6.1: 5 seconds is the most any command may take.
    assert(MIMHID_CONTROL_TIMEOUT_MAX == 5000);
    bad = rule_for(0xC0, 0x42, MIMHID_CONTROL_TIMEOUT_MAX, MIMHID_CONTROL_STALL);
    assert(control_config(0, &bad, 1) == 1);
    assert(control_config(0, NULL, 0) == 1);
    bad = rule_for(0xC0, 0x42, MIMHID_CONTROL_TIMEOUT_MAX + 1, MIMHID_CONTROL_STALL);
    expect_nak(control_config(0, &bad, 1), MIMHID_NAK_PARAM);
    bad = rule_for(0xC0, 0x42, 10, 4);
    expect_nak(control_config(0, &bad, 1), MIMHID_NAK_PARAM);
    bad = rule_for(0xC0, 0x42, 10, MIMHID_CONTROL_STALL);
    bad.value_value = 1; // outside its (zero) mask
    expect_nak(control_config(0, &bad, 1), MIMHID_NAK_PARAM);
    expect_nak(control_config(2, NULL, 0), MIMHID_NAK_PARAM); // a reserved flag
    {
        uint8_t p[2] = {0, 1}; // a rule is announced but absent
        expect_nak(command(MIMHID_T_CONTROL_CONFIG, p, 2), MIMHID_NAK_PARAM);
        uint8_t q[2] = {0, MIMHID_CONTROL_RULES + 1};
        expect_nak(command(MIMHID_T_CONTROL_CONFIG, q, 2), MIMHID_NAK_PARAM);
    }
    reset_io();
    setup(0xC0, 0x42, 0, 0, 2);
    assert(g_nforwarded == 1); // still no rules

    // Host to device with a data stage: the data is shown and the host gives the verdict.
    mimhid_control_set_single_context(true);
    trule_t out_rule = rule_for(0x21, HID_REQ_CONTROL_SET_REPORT, 100, MIMHID_CONTROL_STALL);
    assert(control_config(0, &out_rule, 1) == 1);
    reset_io();
    setup(0x21, HID_REQ_CONTROL_SET_REPORT, 0x0302, 0, 3);
    pump();
    pos = 0;
    assert(next_control_event(&pos, &token, &flags, raw) && flags == MIMHID_CONTROL_F_HELD);
    assert(reply(token, MIMHID_CONTROL_ANSWER, NULL, 0) == 1);
    {
        const tusb_control_request_t req = g_forwarded[0].setup_received;
        assert(req.bmRequestType == 0x41); // retyped as vendor, direction and recipient kept
        assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &req));
        assert(g_xfer.kind == 1 && g_xfer.length == 3);
        const uint8_t sent[3] = {9, 8, 7};
        memcpy(g_xfer.buffer, sent, 3);
        // The host refuses it.
        reset_io();
        uint8_t v[2] = {token, MIMHID_CONTROL_STALL};
        put_cmd(MIMHID_T_CONTROL_REPLY, v, 2);
        assert(!tud_vendor_control_xfer_cb(0, CONTROL_STAGE_DATA, &req));
        size_t q = 0;
        uint8_t t, p[2048];
        uint16_t n;
        bool saw_data = false;
        while ((t = next_out(&q, p, &n)) != 0) {
            if (t == MIMHID_T_EVENT && p[0] == MIMHID_EV_CONTROL_DATA) {
                assert(n == 1 + 1 + 3 && p[1] == token && memcmp(&p[2], sent, 3) == 0);
                saw_data = true;
            }
        }
        assert(saw_data);
        assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_ACK, &req));
    }

    // No verdict in time: the rule's action again (a stall).
    reset_io();
    setup(0x21, HID_REQ_CONTROL_SET_REPORT, 0x0302, 0, 3);
    pump();
    pos = 0;
    assert(next_control_event(&pos, &token, &flags, raw));
    assert(reply(token, MIMHID_CONTROL_ANSWER, NULL, 0) == 1);
    {
        const tusb_control_request_t req = g_forwarded[0].setup_received;
        assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &req));
        g_now_step = 10; // each look at the clock is 10 ms later
        assert(!tud_vendor_control_xfer_cb(0, CONTROL_STAGE_DATA, &req));
        g_now_step = 0;
    }

    // Leaving the control routing: back to nothing.
    assert(control_config(0, NULL, 0) == 1);
    mimhid_control_set_single_context(false);
}

// ---- endpoint 0 in the announced packet size ----
// The controller has finished the packet it was given last, with `got` bytes.
static void ep0_done(uint16_t got) {
    dcd_event_t e;
    memset(&e, 0, sizeof(e));
    e.event_id = DCD_EVENT_XFER_COMPLETE;
    e.xfer_complete.ep_addr = g_dcd[g_ndcd - 1].ep_addr;
    e.xfer_complete.len = got;
    e.xfer_complete.result = XFER_RESULT_SUCCESS;
    __wrap_dcd_event_handler(&e, true);
}

static void run_deferred(void) {
    assert(g_deferred != NULL);
    const osal_task_func_t f = g_deferred;
    g_deferred = NULL;
    f(g_deferred_param);
}

// The one completion the stack was given: its length.
static uint32_t ep0_whole(void) {
    assert(g_nforwarded == 1 && g_forwarded[0].event_id == DCD_EVENT_XFER_COMPLETE);
    g_nforwarded = 0;
    return g_forwarded[0].xfer_complete.len;
}

static void ep0_begin(uint8_t bm, uint16_t length) {
    setup(bm, TUSB_REQ_GET_DESCRIPTOR, 0x0100, 0, length);
    g_nforwarded = 0;
    g_ndcd = 0;
    assert(g_deferred == NULL);
}

static void test_ep0(void) {
    uint8_t buf[64];
    for (int i = 0; i < 64; i++) buf[i] = (uint8_t)i;
    reset_io();

    // USB 2.0 9.6.3: bmAttributes bits 4..0 are reserved. 9.6.1: a subclass needs a class.
    g_bm_attributes = 0x81;
    expect_nak(device_config(0, ""), MIMHID_NAK_PARAM);
    g_bm_attributes = 0x9F;
    expect_nak(device_config(0, ""), MIMHID_NAK_PARAM);
    g_bm_attributes = 0xA0;
    g_device_class = 0;
    expect_nak(device_config(0, ""), MIMHID_NAK_PARAM); // subclass 2 under class 0
    g_device_subclass = 0;
    assert(device_config(0, "") == 1);
    g_device_class = 0xEF;
    g_device_subclass = 0x02;

    // The stack maps SET_CONFIGURATION(n) to the n-th configuration; the board has one.
    g_config_value = 2;
    expect_nak(device_config(0, ""), MIMHID_NAK_UNSUPPORTED);
    g_config_value = 255;
    expect_nak(device_config(0, ""), MIMHID_NAK_UNSUPPORTED);
    g_config_value = 0;
    expect_nak(device_config(0, ""), MIMHID_NAK_UNSUPPORTED);
    g_config_value = 1;

    // The board refuses a size USB does not have.
    g_ep0_size = 48;
    expect_nak(device_config(0, ""), MIMHID_NAK_PARAM);
    g_ep0_size = 0;
    expect_nak(device_config(0, ""), MIMHID_NAK_PARAM);

    // The size comes with the descriptor: 8, and the device descriptor reads 8.
    g_ep0_size = 8;
    assert(device_config(0, "") == 1);
    assert(node_begin(0, 0, sizeof(k_desc_a), 1, 8, 0, "") == 1);
    assert(node_data(0, 0, k_desc_a, sizeof(k_desc_a)) == 1);
    assert(commit() == 1);
    assert(tud_descriptor_device_cb()[7] == 8);

    // 18 bytes asked as 64: packets of 8, 8 and 2, the last one short.
    ep0_begin(0x80, 64);
    assert(__wrap_dcd_edpt_xfer(0, 0x80, buf, 18));
    assert(g_ndcd == 1 && g_dcd[0].length == 8 && g_dcd[0].buffer == buf);
    ep0_done(8);
    assert(g_nforwarded == 0 && g_ndcd == 1); // the next packet waits for the task
    run_deferred();
    assert(g_ndcd == 2 && g_dcd[1].length == 8 && g_dcd[1].buffer == buf + 8);
    ep0_done(8);
    run_deferred();
    assert(g_ndcd == 3 && g_dcd[2].length == 2 && g_dcd[2].buffer == buf + 16);
    ep0_done(2);
    assert(ep0_whole() == 18 && g_deferred == NULL);

    // 16 of 64: two full packets, then the zero-length one.
    ep0_begin(0x80, 64);
    assert(__wrap_dcd_edpt_xfer(0, 0x80, buf, 16));
    ep0_done(8);
    run_deferred();
    ep0_done(8);
    assert(g_nforwarded == 0);
    run_deferred();
    assert(g_ndcd == 3 && g_dcd[2].length == 0);
    ep0_done(0);
    assert(ep0_whole() == 16);

    // 16 of 16: the target has all it asked for, no zero-length packet.
    ep0_begin(0x80, 16);
    assert(__wrap_dcd_edpt_xfer(0, 0x80, buf, 16));
    ep0_done(8);
    run_deferred();
    ep0_done(8);
    assert(ep0_whole() == 16 && g_deferred == NULL && g_ndcd == 2);

    // 64 of a longer answer: the stack goes on itself; its zero-length packet goes through as is.
    ep0_begin(0x80, 255);
    assert(__wrap_dcd_edpt_xfer(0, 0x80, buf, 64));
    for (int i = 0; i < 7; i++) {
        ep0_done(8);
        run_deferred();
    }
    assert(g_ndcd == 8 && g_dcd[7].buffer == buf + 56);
    ep0_done(8);
    assert(ep0_whole() == 64 && g_deferred == NULL);
    assert(__wrap_dcd_edpt_xfer(0, 0x80, buf, 8)); // the stack's next 8 bytes of 72
    ep0_done(8);
    assert(g_nforwarded == 0);
    run_deferred(); // 72 of 255 ends on a full packet
    assert(g_dcd[g_ndcd - 1].length == 0);
    ep0_done(0);
    assert(ep0_whole() == 8);

    // Host to device: 20 bytes come as 8, 8 and 4; a short packet ends the data stage early.
    ep0_begin(0x00, 20);
    assert(__wrap_dcd_edpt_xfer(0, 0x00, buf, 20));
    ep0_done(8);
    run_deferred();
    ep0_done(8);
    run_deferred();
    assert(g_dcd[2].length == 4);
    ep0_done(4);
    assert(ep0_whole() == 20);
    ep0_begin(0x00, 20);
    assert(__wrap_dcd_edpt_xfer(0, 0x00, buf, 20));
    ep0_done(8);
    run_deferred();
    ep0_done(3);
    assert(ep0_whole() == 11 && g_deferred == NULL);

    // The status stage.
    assert(__wrap_dcd_edpt_xfer(0, 0x80, NULL, 0));
    assert(g_dcd[g_ndcd - 1].length == 0);
    ep0_done(0);
    assert(ep0_whole() == 0);

    // A new setup packet: the packet the task was about to start is not sent.
    ep0_begin(0x80, 64);
    assert(__wrap_dcd_edpt_xfer(0, 0x80, buf, 18));
    ep0_done(8);
    setup(0x80, TUSB_REQ_GET_DESCRIPTOR, 0x0100, 0, 64);
    g_nforwarded = 0;
    run_deferred();
    assert(g_ndcd == 1);

    // Other endpoints are not touched.
    assert(__wrap_dcd_edpt_xfer(0, 0x81, buf, 40));
    assert(g_ndcd == 2 && g_dcd[1].length == 40);

    // 64: everything goes through as it came.
    reset_io();
    g_ep0_size = 64;
    assert(device_config(0, "") == 1);
    assert(node_begin(0, 0, sizeof(k_desc_a), 1, 8, 0, "") == 1);
    assert(node_data(0, 0, k_desc_a, sizeof(k_desc_a)) == 1);
    assert(commit() == 1);
    ep0_begin(0x80, 255);
    assert(__wrap_dcd_edpt_xfer(0, 0x80, buf, 64));
    assert(g_ndcd == 1 && g_dcd[0].length == 64);
    ep0_done(64);
    assert(ep0_whole() == 64 && g_deferred == NULL);
}

// The wire format names a node in four bits, so all 16 nodes can be used, and each keeps its own
// interface through a commit.
static void test_sixteen_nodes(void) {
    assert(MIMHID_MAX_NODES == 16);
    assert(device_config(0, "") == 1);
    for (uint8_t id = 0; id < MIMHID_MAX_NODES; id++) {
        const uint8_t iface = (uint8_t)(id % MIMHID_HID_ITFS);
        assert(node_begin(id, iface, sizeof(k_desc_a), 1, 8, 0, "") == 1);
        assert(node_data(id, 0, k_desc_a, sizeof(k_desc_a)) == 1);
    }
    expect_nak(node_begin(MIMHID_MAX_NODES, 0, sizeof(k_desc_a), 1, 8, 0, ""), MIMHID_NAK_PARAM);
    assert(commit() == 1);
    for (uint8_t id = 0; id < MIMHID_MAX_NODES; id++)
        assert(mimhid_active_node_interface(id) == (uint8_t)(id % MIMHID_HID_ITFS));
    assert(mimhid_active_node_interface(MIMHID_MAX_NODES) == 0xFF);
    // A detach forgets them all again.
    uint8_t detach[1] = {1};
    assert(command(MIMHID_T_COMMIT, detach, 1) == 1);
    for (uint8_t id = 0; id < MIMHID_MAX_NODES; id++)
        assert(mimhid_active_node_interface(id) == 0xFF);
}

#if TUD_OPT_HIGH_SPEED
// A high-speed build: the rules of USB 2.0 that only a high-speed device has. The tests above
// assume full-speed packet sizes, so this build runs these alone.
static void test_high_speed(void) {
    assert(MIMHID_MAX_ENDPOINT_BYTES == 1024);
    // 9.6.1: the control endpoint of a high-speed device is 64 bytes, and only that.
    assert(device_config(0, "") == 1);
    g_ep0_size = 8;
    expect_nak(device_config(0, ""), MIMHID_NAK_PARAM);
    g_ep0_size = 32;
    expect_nak(device_config(0, ""), MIMHID_NAK_PARAM);
    g_ep0_size = 64;
    assert(device_config(0, "") == 1);
    // 9.6.6: bInterval of an interrupt endpoint is an exponent, 1 to 16.
    expect_nak(node_begin(0, 0, sizeof(k_desc_a), 0, 8, 0, ""), MIMHID_NAK_PARAM);
    expect_nak(node_begin(0, 0, sizeof(k_desc_a), 17, 8, 0, ""), MIMHID_NAK_PARAM);
    expect_nak(node_begin(0, 0, sizeof(k_desc_a), 255, 8, 0, ""), MIMHID_NAK_PARAM);
    assert(node_begin(0, 0, sizeof(k_desc_a), 1, 8, 0, "") == 1);
    assert(node_begin(1, 1, sizeof(k_desc_a), 16, 1024, 0, "") == 1); // the largest endpoint packet
    expect_nak(node_begin(2, 2, sizeof(k_desc_a), 16, 1025, 0, ""), MIMHID_NAK_PARAM);
}
#endif

int main(void) {
    printf("firmware core, %d HID interfaces\n", MIMHID_HID_ITFS);
    mimhid_core_init();
#if TUD_OPT_HIGH_SPEED
    test_high_speed();
    printf("firmware core (high speed) OK\n");
    return 0;
#endif
    test_ping_and_events();
    test_set_baud();
    test_break();
    test_counters_and_probe();
    test_strings_utf16();
    test_descriptors_basic();
    test_reports();
    test_feature();
    test_detach_begin_and_live_feature();
    test_errors();
    test_sixteen_nodes();
    test_packing();
    test_wide_values();
    test_specified_answers();
    test_control();
    test_ep0();
    printf("firmware core OK\n");
    return 0;
}
