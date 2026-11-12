// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
// Host-side test of the portable firmware core with a fake HAL and TinyUSB stubs.
#include "config.h"
#include "control.h"
#include "core.h"
#include "device/dcd.h"
#include "device/usbd_pvt.h"
#include "ep0.h"
#include "hal.h"
#include "tusb.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t const* tud_descriptor_device_cb(void);
uint8_t const* tud_descriptor_configuration_cb(uint8_t index);
uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid);
void tud_suspend_cb(bool remote_wakeup_en);
void tud_resume_cb(void);
void __wrap_dcd_event_handler(dcd_event_t const* event, bool in_isr);
bool __wrap_dcd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t* buffer, uint16_t total_bytes);

// The class requests of HID 1.11 section 7.2 (the board reads none of them; the tests give them to
// the control rules as any other request).
#define T_HID_DESC_HID 0x21    // the HID descriptor (7.1), a class descriptor to the board
#define T_HID_DESC_REPORT 0x22 // the report descriptor it announces
#define T_HID_GET_REPORT 0x01
#define T_HID_GET_IDLE 0x02
#define T_HID_GET_PROTOCOL 0x03
#define T_HID_SET_REPORT 0x09
#define T_HID_SET_IDLE 0x0A
#define T_HID_SET_PROTOCOL 0x0B
// Report types, the high byte of wValue of GET_REPORT and SET_REPORT (7.2.1).
#define T_HID_INPUT 0x01
#define T_HID_OUTPUT 0x02
#define T_HID_FEATURE 0x03

// ---- fake HAL ----
static uint8_t g_in[16384];
static size_t g_in_len, g_in_pos;
static uint8_t g_out[16384];
static size_t g_out_len;
static uint32_t g_now;
static int g_has_id = 1;
static uint32_t g_baud = MIMUSB_BOOT_BAUD;
static int g_baud_changes;

void hal_init(void) {}
size_t hal_uart_read(uint8_t* dst, size_t max) {
    size_t n = g_in_len - g_in_pos;
    if (n > max)
        n = max;
    memcpy(dst, g_in + g_in_pos, n);
    g_in_pos += n;
    return n;
}
void hal_uart_write(const uint8_t* d, size_t n) {
    memcpy(g_out + g_out_len, d, n);
    g_out_len += n;
}
static uint32_t g_now_step;
uint32_t hal_millis(void) {
    g_now += g_now_step;
    return g_now;
}
static uint32_t g_max_rate = 12000000u;
static uint32_t g_rate_error_ppm; // the fake divider's error, applied to every rate
static uint8_t g_errors;          // HAL_UART_ERR_* bits the next hal_uart_errors returns
uint32_t hal_uart_baud_actual(uint32_t baud) {
    if (baud == 0 || baud > g_max_rate)
        return 0;
    return (uint32_t)((uint64_t)baud * (1000000u + g_rate_error_ppm) / 1000000u);
}
uint32_t hal_uart_max_baud(void) { return g_max_rate; }
bool hal_uart_has_break(void) { return true; }
uint8_t hal_uart_errors(void) {
    uint8_t e = g_errors;
    g_errors = 0;
    return e;
}
void hal_uart_set_baud(uint32_t baud) {
    g_baud = baud;
    g_baud_changes++;
}
uint8_t hal_chip(void) { return MIMUSB_CHIP_RP2040; }
size_t hal_unique_id(uint8_t* out, size_t max) {
    if (!g_has_id)
        return 0;
    static const uint8_t id[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    size_t n = max < sizeof(id) ? max : sizeof(id);
    memcpy(out, id, n);
    return n;
}
static bool g_has_bootloader = true;
static int g_bootloader_entered;
bool hal_has_bootloader(void) { return g_has_bootloader; }
void hal_enter_bootloader(void) { g_bootloader_entered++; }
void hal_control_silent_drop(void) {}
void hal_usb_clear_endpoint(uint8_t address) { (void)address; }
void __real_dcd_edpt_stall(uint8_t rhport, uint8_t ep_addr) {
    (void)rhport;
    (void)ep_addr;
}

// ---- TinyUSB stubs ----
static bool g_mounted = true, g_suspended;
static int g_disconnects, g_connects, g_wakeups;
// The transfers the board started on its IN endpoints, in order (written by the stub of
// usbd_edpt_xfer, read by the tests: one thread). iface is the endpoint number minus 1, which is
// the interface number in the model below.
static struct {
    uint8_t iface;
    uint16_t len;
    uint8_t data[1024];
} g_sent[64];
static int g_nsent;
// An IN endpoint the target does not poll (index: endpoint number - 1): the controller is busy
// with it, so no transfer starts on it. Set and cleared by the tests only.
static bool g_busy[16];
// With this set, pump() completes every transfer the board started, as a target that polls does;
// the tests of the generic driver and of the control rules turn it off to see each one.
static bool g_auto = true;

bool tusb_rhport_init(uint8_t rhport, const tusb_rhport_init_t* init) {
    (void)rhport;
    (void)init;
    return true;
}
void tud_task_ext(uint32_t timeout_ms, bool in_isr) {
    (void)timeout_ms;
    (void)in_isr;
}
bool tud_mounted(void) { return g_mounted; }
bool tud_suspended(void) { return g_suspended; }
bool tud_disconnect(void) {
    g_disconnects++;
    return true;
}
bool tud_connect(void) {
    g_connects++;
    return true;
}
bool tud_remote_wakeup(void) {
    g_wakeups++;
    return true;
}

// The endpoint calls of the stack's device layer (src/device/usbd.c) that the generic driver makes,
// with the state they keep: usbd_edpt_stall sets both stalled and busy, usbd_edpt_clear_stall clears
// both, and usbd_edpt_xfer refuses an endpoint that is busy.
static struct {
    bool opened, busy, stalled;
    uint8_t* buffer;
    uint16_t length;
    int xfers; // transfers started since the endpoint was opened
    uint16_t packet;
} g_ep[2][16]; // [0] OUT, [1] IN
#define EPSTATE(addr) g_ep[((addr) >> 7) & 1][(addr) & 0x0F]

bool usbd_edpt_open(uint8_t rhport, tusb_desc_endpoint_t const* desc) {
    (void)rhport;
    assert(!EPSTATE(desc->bEndpointAddress).opened);
    EPSTATE(desc->bEndpointAddress) = (typeof(g_ep[0][0])){.opened = true,
                                                           .packet = (uint16_t)(desc->wMaxPacketSize & 0x7FF)};
    return true;
}
// The isochronous calls and the stall of endpoint 0 that the generic driver makes (the host test
// board has none: MIMUSB_ISO_BYTES is 0 here, so only the link has to resolve).
bool usbd_edpt_iso_alloc(uint8_t rhport, uint8_t ep_addr, uint16_t largest_packet_size) {
    (void)rhport;
    (void)ep_addr;
    (void)largest_packet_size;
    return true;
}
bool usbd_edpt_iso_activate(uint8_t rhport, tusb_desc_endpoint_t const* desc) {
    (void)rhport;
    (void)desc;
    return true;
}
static int g_ep0_stalls;
void dcd_edpt_stall(uint8_t rhport, uint8_t ep_addr) {
    (void)rhport;
    if ((ep_addr & 0x0F) == 0)
        g_ep0_stalls++;
}
bool usbd_edpt_busy(uint8_t rhport, uint8_t ep_addr) {
    (void)rhport;
    return EPSTATE(ep_addr).busy || ((ep_addr & 0x80) != 0 && g_busy[(ep_addr & 0x0F) - 1]);
}
bool usbd_edpt_stalled(uint8_t rhport, uint8_t ep_addr) {
    (void)rhport;
    return EPSTATE(ep_addr).stalled;
}
bool usbd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t* buffer, uint16_t total_bytes) {
    (void)rhport;
    assert(EPSTATE(ep_addr).opened && !EPSTATE(ep_addr).busy);
    EPSTATE(ep_addr).busy = true;
    EPSTATE(ep_addr).buffer = buffer;
    EPSTATE(ep_addr).length = total_bytes;
    EPSTATE(ep_addr).xfers++;
    if ((ep_addr & 0x80) != 0) {
        assert(g_nsent < 64 && total_bytes <= sizeof(g_sent[0].data));
        g_sent[g_nsent].iface = (uint8_t)((ep_addr & 0x0F) - 1);
        g_sent[g_nsent].len = total_bytes;
        memcpy(g_sent[g_nsent].data, buffer, total_bytes);
        g_nsent++;
    }
    return true;
}
void usbd_edpt_stall(uint8_t rhport, uint8_t ep_addr) {
    (void)rhport;
    EPSTATE(ep_addr).stalled = true;
    EPSTATE(ep_addr).busy = true;
}
void usbd_edpt_clear_stall(uint8_t rhport, uint8_t ep_addr) {
    (void)rhport;
    EPSTATE(ep_addr).stalled = false;
    EPSTATE(ep_addr).busy = false;
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
bool tud_control_xfer(uint8_t rhport, tusb_control_request_t const* request, void* buffer,
                      uint16_t len) {
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
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint8_t g_seq;

static void put_frame(uint8_t type, const uint8_t* p, size_t n) {
    uint16_t crc = mimusb_crc16_update(MIMUSB_CRC16_INIT, type);
    g_in[g_in_len++] = MIMUSB_SYNC;
    g_in[g_in_len++] = type;
    if (n < 0x80) {
        g_in[g_in_len++] = (uint8_t)n;
        crc = mimusb_crc16_update(crc, (uint8_t)n);
    } else {
        const uint8_t low = (uint8_t)(0x80 | (n & 0x7F)), high = (uint8_t)(n >> 7);
        g_in[g_in_len++] = low;
        g_in[g_in_len++] = high;
        crc = mimusb_crc16_update(crc, low);
        crc = mimusb_crc16_update(crc, high);
    }
    for (size_t i = 0; i < n; i++) {
        g_in[g_in_len++] = p[i];
        crc = mimusb_crc16_update(crc, p[i]);
    }
    g_in[g_in_len++] = (uint8_t)(crc & 0xFF);
    g_in[g_in_len++] = (uint8_t)(crc >> 8);
}

static void put_cmd(uint8_t type, const uint8_t* p, size_t n) {
    uint8_t buf[2048];
    buf[0] = ++g_seq;
    if (n != 0)
        memcpy(buf + 1, p, n);
    put_frame(type, buf, n + 1);
}

static void reset_io(void) {
    g_in_len = g_in_pos = g_out_len = 0;
    g_nsent = 0;
    g_nforwarded = 0;
    memset(&g_xfer, 0, sizeof(g_xfer));
}

static const usbd_class_driver_t* gen_driver(void);
static void set_config(void);

// The controller finishes every transfer the board started on an IN endpoint that is not held
// (g_busy), as a target that polls it does.
static void complete_started(void) {
    for (uint8_t number = 1; number <= 15; number++) {
        const uint8_t address = (uint8_t)(0x80 | number);
        if (EPSTATE(address).opened && EPSTATE(address).busy && !EPSTATE(address).stalled &&
            !g_busy[number - 1]) {
            EPSTATE(address).busy = false;
            gen_driver()->xfer_cb(0, address, XFER_RESULT_SUCCESS, EPSTATE(address).length);
        }
    }
}

static void pump(void) {
    for (int i = 0; i < 4; i++) {
        mimusb_rx_poll();
        mimusb_usb_poll();
        if (g_auto)
            complete_started();
    }
}

// Reads the next board frame at *pos; returns its type, or 0 when none is left.
static uint8_t next_out(size_t* pos, uint8_t* payload, uint16_t* len) {
    if (*pos + 5 > g_out_len)
        return 0;
    assert(g_out[*pos] == MIMUSB_SYNC);
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
    uint16_t crc = mimusb_crc16_update(MIMUSB_CRC16_INIT, t);
    crc = mimusb_crc16_update(crc, low);
    if (header == 4)
        crc = mimusb_crc16_update(crc, high);
    for (uint16_t i = 0; i < n; i++) {
        payload[i] = g_out[*pos + header + i];
        crc = mimusb_crc16_update(crc, payload[i]);
    }
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
        if (t == MIMUSB_T_EVENT && p[0] == MIMUSB_EV_TRANSFER_LOST) {
            assert(n == 4 && p[1] == 0x81); // the IN endpoint of interface 0
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
        if ((t == MIMUSB_T_ACK || t == MIMUSB_T_NAK) && p[1] == type) {
            assert(p[0] == g_seq);
            result = t == MIMUSB_T_ACK;
            if (t == MIMUSB_T_NAK)
                g_nak_code = p[2];
            if (t == MIMUSB_T_ACK) {
                g_ack_data_len = (uint16_t)(n - 2);
                memcpy(g_ack_data, &p[2], g_ack_data_len);
            }
        }
    }
    return result;
}

// True while the board is on the bus: an attaching COMMIT was acknowledged and no detach since.
static bool g_up;

static int command(uint8_t type, const uint8_t* p, size_t n) {
    g_out_len = 0;
    if (g_in_pos == g_in_len)
        g_in_pos = g_in_len = 0; // the input is all consumed: start over
    put_cmd(type, p, n);
    pump();
    const int r = result_of(type);
    if (type == MIMUSB_T_COMMIT && r == 1)
        g_up = !(n >= 1 && p[0] == 1);
    return r;
}

static uint8_t g_ep0_size = 64;
static uint16_t g_language = 0x0409;
static uint16_t g_bcd_usb = 0x0200;
static uint8_t g_config_value = 1;
static uint8_t g_bm_attributes = 0xA0;
static uint8_t g_device_class = 0xEF;
static uint8_t g_device_subclass = 0x02;
// Endpoint addresses a node carries instead of the ones derived from its interface; -1: none.
static int g_force_in = -1;
static int g_force_out = -1;

static int ep_config(uint8_t address, uint8_t depth, uint8_t policy) {
    const uint8_t p[3] = {address, depth, policy};
    return command(MIMUSB_T_EP_CONFIG, p, sizeof(p));
}
// The depth of the queue stage() gives every IN endpoint of the model: 15 transfers may wait.
static uint8_t g_depth = 15;

// ---- what the host library does: compose the descriptors and send them as bytes ----
// The tests describe a device the way the old wire did (a device, nodes with interface settings and
// report descriptors); this model turns that into the DESC_* commands the host library sends.
#define T_NODES 16
#define T_SERIAL_FROM_ID 0x01 // device flag of the model: the serial string is the board's

typedef struct {
    bool used;
    uint8_t iface, cls, subclass, protocol;
    uint8_t in_addr, out_addr, poll, poll_out;
    uint16_t in_size, out_size;
    uint16_t desc_len, desc_recv;
    uint8_t rd[300];
    uint16_t name[MIMUSB_STRING_MAX + 2];
    size_t name_len;
} tnode_t;

static tnode_t g_tn[T_NODES];
static uint8_t g_dev_flags;
static uint16_t g_dev_string[4]
                            [MIMUSB_STRING_MAX + 2]; // manufacturer, product, serial, configuration
static size_t g_dev_string_len[4];

static void put_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void set_units(uint16_t* dst, size_t* len, const uint16_t* units, size_t count) {
    assert(count <= MIMUSB_STRING_MAX + 1);
    memcpy(dst, units, count * sizeof(uint16_t));
    *len = count;
}

static void set_ascii(uint16_t* dst, size_t* len, const char* text) {
    size_t count = strlen(text);
    assert(count <= MIMUSB_STRING_MAX);
    for (size_t i = 0; i < count; i++)
        dst[i] = (uint8_t)text[i];
    *len = count;
}

static int desc_reset(void) { return command(MIMUSB_T_DESC_RESET, NULL, 0); }

static int desc_begin_raw(uint8_t kind, uint8_t index, uint16_t language, uint16_t total,
                          uint8_t flags) {
    uint8_t p[7] = {kind,
                    index,
                    (uint8_t)(language & 0xFF),
                    (uint8_t)(language >> 8),
                    (uint8_t)(total & 0xFF),
                    (uint8_t)(total >> 8),
                    flags};
    return command(MIMUSB_T_DESC_BEGIN, p, sizeof(p));
}

static int desc_data_raw(uint8_t kind, uint8_t index, uint16_t language, uint16_t off,
                         const uint8_t* d, size_t len) {
    uint8_t p[6 + MIMUSB_DESC_DATA_CHUNK];
    assert(len <= MIMUSB_DESC_DATA_CHUNK);
    p[0] = kind;
    p[1] = index;
    put_u16(&p[2], language);
    put_u16(&p[4], off);
    memcpy(p + 6, d, len);
    return command(MIMUSB_T_DESC_DATA, p, 6 + len);
}

// Announces a descriptor and sends `have` of its `total` bytes in chunks. Returns 1, or the first
// answer that was not an ACK (0 with the code in g_nak_code, -1 for silence).
static int send_blob_partial(uint8_t kind, uint8_t index, uint16_t language, const uint8_t* d,
                             size_t total, size_t have, uint8_t flags) {
    int r = desc_begin_raw(kind, index, language, (uint16_t)total, flags);
    if (r != 1)
        return r;
    for (size_t off = 0; off < have; off += MIMUSB_DESC_DATA_CHUNK) {
        size_t n = have - off < MIMUSB_DESC_DATA_CHUNK ? have - off : MIMUSB_DESC_DATA_CHUNK;
        r = desc_data_raw(kind, index, language, (uint16_t)off, d + off, n);
        if (r != 1)
            return r;
    }
    return 1;
}

static int send_blob(uint8_t kind, uint8_t index, uint16_t language, const uint8_t* d,
                     size_t total) {
    return send_blob_partial(kind, index, language, d, total, total, 0);
}

// A string descriptor as it is on the bus (USB 2.0 9.6.7).
static size_t string_descriptor(uint8_t* out, const uint16_t* units, size_t count) {
    out[0] = (uint8_t)(2 + 2 * count);
    out[1] = 3;
    for (size_t i = 0; i < count; i++)
        put_u16(&out[2 + 2 * i], units[i]);
    return 2 + 2 * count;
}

static int send_string(uint8_t index, const uint16_t* units, size_t count) {
    uint8_t b[2 + 2 * (MIMUSB_STRING_MAX + 2)];
    assert(count <= MIMUSB_STRING_MAX + 1);
    return send_blob(MIMUSB_DESC_STRING, index, g_language, b, string_descriptor(b, units, count));
}

// The report descriptor of an interface: the nodes of it in node order, back to back. `total` is
// what the nodes announced, the return value is what they actually have.
static size_t joined_report_descriptor(uint8_t iface, uint8_t* out, size_t* total) {
    size_t have = 0;
    *total = 0;
    for (int id = 0; id < T_NODES; id++) {
        if (!g_tn[id].used || g_tn[id].iface != iface)
            continue;
        memcpy(out + have, g_tn[id].rd, g_tn[id].desc_recv);
        have += g_tn[id].desc_recv;
        *total += g_tn[id].desc_len;
    }
    return have;
}

// Sends everything the host library sends before a COMMIT, except the Feature answers and the
// endpoint settings: DESC_RESET, the device and configuration descriptors, the strings and the
// report descriptors. Returns 1, or the first answer that was not an ACK.
static int stage(void) {
    int r = desc_reset();
    if (r != 1)
        return r;

    // The strings take the places the host library gives them: 1 manufacturer, 2 product, 3
    // serial, 4 configuration, 5 + interface number for an interface.
    const bool serial_from_id = (g_dev_flags & T_SERIAL_FROM_ID) != 0 && g_dev_string_len[2] == 0;
    const uint8_t serial_index = (g_dev_string_len[2] != 0 || serial_from_id) ? 3 : 0;
    uint8_t dev[18] = {18, TUSB_DESC_DEVICE};
    put_u16(&dev[2], g_bcd_usb);
    dev[4] = g_device_class;
    dev[5] = g_device_subclass;
    dev[6] = 0x01;
    dev[7] = g_ep0_size;
    put_u16(&dev[8], 0x1234);
    put_u16(&dev[10], 0x5678);
    put_u16(&dev[12], 0x0200);
    dev[14] = g_dev_string_len[0] ? 1 : 0;
    dev[15] = g_dev_string_len[1] ? 2 : 0;
    dev[16] = serial_index;
    dev[17] = 1;
    if ((r = send_blob(MIMUSB_DESC_DEVICE, 0, 0, dev, sizeof(dev))) != 1)
        return r;

    // The interfaces in the order of their numbers; an interface takes its settings from its first
    // node.
    uint8_t cfg[MIMUSB_MAX_CONFIG_BYTES + 64];
    size_t pos = 9;
    uint8_t seen[T_NODES];
    size_t nseen = 0;
    for (int n = 0; n < 256; n++) {
        for (int id = 0; id < T_NODES; id++) {
            if (!g_tn[id].used || g_tn[id].iface != n)
                continue;
            seen[nseen++] = (uint8_t)n;
            break;
        }
    }
    for (size_t k = 0; k < nseen; k++) {
        const tnode_t* t = NULL;
        for (int id = 0; id < T_NODES && t == NULL; id++)
            if (g_tn[id].used && g_tn[id].iface == seen[k])
                t = &g_tn[id];
        size_t total = 0;
        uint8_t rd[T_NODES * 300];
        joined_report_descriptor(seen[k], rd, &total);
        const bool has_out = t->out_addr != 0 || t->out_size != 0;
        const uint8_t itf[] = {9,
                               TUSB_DESC_INTERFACE,
                               seen[k],
                               0,
                               (uint8_t)(has_out ? 2 : 1),
                               t->cls,
                               t->subclass,
                               t->protocol,
                               (uint8_t)(t->name_len ? 5 + seen[k] : 0)};
        memcpy(cfg + pos, itf, sizeof(itf));
        pos += sizeof(itf);
        uint8_t hid[9] = {9, T_HID_DESC_HID, 0x11, 0x01, 0, 1, T_HID_DESC_REPORT};
        put_u16(&hid[7], (uint16_t)total);
        memcpy(cfg + pos, hid, sizeof(hid));
        pos += sizeof(hid);
        uint8_t ep[7] = {7, TUSB_DESC_ENDPOINT, t->in_addr, TUSB_XFER_INTERRUPT};
        put_u16(&ep[4], t->in_size);
        ep[6] = t->poll;
        memcpy(cfg + pos, ep, sizeof(ep));
        pos += sizeof(ep);
        if (has_out) {
            uint8_t eo[7] = {7, TUSB_DESC_ENDPOINT, t->out_addr, TUSB_XFER_INTERRUPT};
            put_u16(&eo[4], t->out_size);
            eo[6] = t->poll_out;
            memcpy(cfg + pos, eo, sizeof(eo));
            pos += sizeof(eo);
        }
    }
    const uint8_t head[9] = {9,
                             TUSB_DESC_CONFIGURATION,
                             0,
                             0,
                             (uint8_t)nseen,
                             g_config_value,
                             (uint8_t)(g_dev_string_len[3] ? 4 : 0),
                             g_bm_attributes,
                             50};
    memcpy(cfg, head, sizeof(head));
    put_u16(&cfg[2], (uint16_t)pos);
    if ((r = send_blob(MIMUSB_DESC_CONFIGURATION, 0, 0, cfg, pos)) != 1)
        return r;

    for (int i = 0; i < 4; i++) {
        if (i == 2 && serial_from_id) {
            if ((r = send_blob_partial(MIMUSB_DESC_STRING, 3, g_language, NULL, 0, 0,
                                       MIMUSB_DESC_F_SERIAL_FROM_ID)) != 1)
                return r;
        } else if (g_dev_string_len[i]) {
            if ((r = send_string((uint8_t)(i + 1), g_dev_string[i], g_dev_string_len[i])) != 1)
                return r;
        }
    }
    for (size_t k = 0; k < nseen; k++) {
        for (int id = 0; id < T_NODES; id++) {
            if (g_tn[id].used && g_tn[id].iface == seen[k]) {
                if (g_tn[id].name_len && (r = send_string((uint8_t)(5 + seen[k]), g_tn[id].name,
                                                          g_tn[id].name_len)) != 1)
                    return r;
                break;
            }
        }
    }
    // The queue of every IN endpoint (the host library sends one for each; a COMMIT refuses an
    // endpoint without). The answer is not looked at: a configuration the walk refuses has none
    // to give, and the COMMIT says so.
    for (size_t k = 0; k < nseen; k++) {
        for (int id = 0; id < T_NODES; id++) {
            if (g_tn[id].used && g_tn[id].iface == seen[k]) {
                (void)ep_config(g_tn[id].in_addr, g_depth, MIMUSB_EP_FULL_DROP_NEWEST);
                break;
            }
        }
    }
    return 1;
}

// A COMMIT. When the board takes it, the target is taken to have enumerated and selected the
// configuration (the stack's SET_CONFIGURATION), so the driver has opened its endpoints.
static int commit_staged(void) {
    // A COMMIT that attaches is refused by a board that is attached, so a test that configures
    // again does what the host library does and detaches first. That detach is the harness's, not
    // the test's: the counts of disconnects and connects the tests read leave it out.
    if (g_up) {
        const int disconnects = g_disconnects, connects = g_connects;
        const uint8_t detach[] = {1};
        assert(command(MIMUSB_T_COMMIT, detach, sizeof(detach)) == 1);
        g_disconnects = disconnects;
        g_connects = connects;
    }
    const int r = command(MIMUSB_T_COMMIT, NULL, 0);
    if (r == 1)
        set_config();
    return r;
}

static int commit(void) {
    int r = stage();
    return r != 1 ? r : commit_staged();
}


// Starts a new device: the strings are "Maker", "Product", the given serial and "Config", and no
// node is left. Nothing is sent until stage() or commit().
static void device_config_units(uint8_t flags, const char* serial, const uint16_t* maker,
                                size_t maker_count) {
    memset(g_tn, 0, sizeof(g_tn));
    g_dev_flags = flags;
    if (maker != NULL)
        set_units(g_dev_string[0], &g_dev_string_len[0], maker, maker_count);
    else
        set_ascii(g_dev_string[0], &g_dev_string_len[0], "Maker");
    set_ascii(g_dev_string[1], &g_dev_string_len[1], "Product");
    set_ascii(g_dev_string[2], &g_dev_string_len[2], serial);
    set_ascii(g_dev_string[3], &g_dev_string_len[3], "Config");
}

static void device_config(uint8_t flags, const char* serial) {
    device_config_units(flags, serial, NULL, 0);
}

static const uint8_t k_desc_a[] = {// a small valid keyboard-like descriptor
                                   0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, 0x05,
                                   0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
                                   0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0xC0};
static const uint8_t k_desc_b[] = {// mouse-like
                                   0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02, 0x09, 0x30, 0x15,
                                   0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xC0};

// A node: its interface settings. The report descriptor follows with node_data(). The endpoints
// are the interface's own (0x81 + interface number for IN, 0x01 + interface number for OUT)
// unless g_force_in / g_force_out say otherwise.
static void node_begin_full(uint8_t id, uint8_t iface, uint16_t desc_len, uint8_t poll, uint16_t ep,
                            uint16_t ep_out, uint8_t cls, uint8_t subclass, uint8_t protocol,
                            const char* str) {
    assert(id < T_NODES && desc_len <= sizeof(g_tn[0].rd));
    tnode_t* t = &g_tn[id];
    memset(t, 0, sizeof(*t));
    t->used = true;
    t->iface = iface;
    t->cls = cls;
    t->subclass = subclass;
    t->protocol = protocol;
    t->in_addr = g_force_in >= 0 ? (uint8_t)g_force_in : (uint8_t)(0x81 + iface);
    t->in_size = ep;
    t->poll = poll;
    t->out_addr = g_force_out >= 0 ? (uint8_t)g_force_out : ep_out ? (uint8_t)(0x01 + iface) : 0;
    t->out_size = ep_out;
    t->poll_out = ep_out ? poll : 0;
    t->desc_len = desc_len;
    set_ascii(t->name, &t->name_len, str);
}

static void node_begin_out(uint8_t id, uint8_t iface, uint16_t desc_len, uint8_t poll, uint16_t ep,
                           uint16_t ep_out, uint8_t boot, const char* str) {
    node_begin_full(id, iface, desc_len, poll, ep, ep_out, 0x03, boot ? 1 : 0, boot, str);
}

static void node_begin(uint8_t id, uint8_t iface, uint16_t desc_len, uint8_t poll, uint16_t ep,
                       uint8_t boot, const char* str) {
    node_begin_out(id, iface, desc_len, poll, ep, 0, boot, str);
}

// The report descriptor of a node, or the part of it from `off` on.
static void node_data(uint8_t id, uint16_t off, const uint8_t* d, size_t len) {
    tnode_t* t = &g_tn[id];
    assert(t->used && off == t->desc_recv && off + len <= t->desc_len);
    memcpy(t->rd + off, d, len);
    t->desc_recv = (uint16_t)(off + len);
}

// One node on interface 0 with a complete descriptor, the base of most refusal tests.
static void base_node(void) {
    node_begin(0, 0, sizeof(k_desc_a), 1, 8, 0, "");
    node_data(0, 0, k_desc_a, sizeof(k_desc_a));
}

// Data for an IN endpoint, by endpoint number.
static void ep_in(uint8_t number, const uint8_t* d, size_t n) {
    put_frame((uint8_t)(MIMUSB_T_EP_IN_BASE | number), d, n);
}

static void expect_nak_at(int r, int code, int line) {
    if (r != 0 || g_nak_code != code) {
        fprintf(stderr, "line %d: expected NAK %d, got result %d, code %d\n", line, code, r,
                g_nak_code);
        abort();
    }
}
#define expect_nak(r, code) expect_nak_at((r), (code), __LINE__)

static void test_descriptors_basic(void) {
    // device with two nodes, each on its own interface
    device_config(0, "SN-1");
    node_begin(0, 0, sizeof(k_desc_a), 4, 16, 1, "Keys");
    node_data(0, 0, k_desc_a, sizeof(k_desc_a));
    node_begin(1, 1, sizeof(k_desc_b), 1, 64, 0, "");
    node_data(1, 0, k_desc_b, sizeof(k_desc_b));
    g_disconnects = g_connects = 0;
    assert(commit() == 1);
    assert(g_disconnects == 0 && g_connects == 1);
    assert(mimusb_active_interfaces() == 2);

    const tusb_desc_device_t* d = (const tusb_desc_device_t*)tud_descriptor_device_cb();
    assert(d->idVendor == 0x1234 && d->idProduct == 0x5678 && d->bcdDevice == 0x0200);
    assert(d->bDeviceClass == 0xEF && d->bDeviceSubClass == 2 && d->bDeviceProtocol == 1);
    assert(d->iManufacturer == 1 && d->iProduct == 2 && d->iSerialNumber == 3);

    const uint8_t* c = tud_descriptor_configuration_cb(0);
    uint16_t total = (uint16_t)(c[2] | (c[3] << 8));
    assert(total == 9 + 2 * 25 && c[4] == 2 && c[6] == 4);
    assert(c[7] == (0x80 | 0x20) && c[8] == 50);
    // interface 0: boot keyboard, string 5, bcdHID 1.11, report descriptor length, EP 0x81, 16
    // bytes, 4 ms
    const uint8_t* i0 = c + 9;
    assert(i0[1] == 4 && i0[2] == 0 && i0[5] == 3 && i0[6] == 1 && i0[7] == 1 && i0[8] == 5);
    assert(i0[11] == 0x11 && i0[12] == 0x01 && i0[16] == (sizeof(k_desc_a) & 0xFF));
    assert(i0[20] == 0x81 && i0[21] == 3 && i0[22] == 16 && i0[24] == 4);
    // interface 1: no string, 64-byte endpoint defaults, 1 ms
    const uint8_t* i1 = c + 9 + 25;
    assert(i1[2] == 1 && i1[6] == 0 && i1[8] == 0 && i1[20] == 0x82 && i1[22] == 64 && i1[24] == 1);

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
// the longest string (254 bytes, USB 2.0 9.6.7) is held, one unit more is refused.
static void test_strings_utf16(void) {
    static const uint16_t maker[] = {0x65E5, 0x672C, 0xD83D, 0xDE00, 'A'}; // two kanji, U+1F600, A
    device_config_units(0, "", maker, 5);
    node_begin(0, 0, sizeof(k_desc_a), 1, 8, 0, "");
    node_data(0, 0, k_desc_a, sizeof(k_desc_a));
    assert(commit() == 1);
    const uint16_t* s = tud_descriptor_string_cb(1, 0x0409);
    assert((s[0] & 0xFF) == 2 + 2 * 5);
    for (int i = 0; i < 5; i++)
        assert(s[1 + i] == maker[i]);

    uint16_t longest[MIMUSB_STRING_MAX + 2];
    for (size_t i = 0; i < MIMUSB_STRING_MAX + 2; i++)
        longest[i] = 0x3042;
    device_config_units(0, "", longest, MIMUSB_STRING_MAX);
    node_begin(0, 0, sizeof(k_desc_a), 1, 8, 0, "");
    node_data(0, 0, k_desc_a, sizeof(k_desc_a));
    assert(commit() == 1);
    s = tud_descriptor_string_cb(1, 0x0409);
    assert((s[0] & 0xFF) == MIMUSB_STRING_DESC_MAX && s[MIMUSB_STRING_MAX] == 0x3042);
    // 127 units do not fit the one byte of bLength: refused when the string is announced.
    device_config_units(0, "", longest, MIMUSB_STRING_MAX + 1);
    node_begin(0, 0, sizeof(k_desc_a), 1, 8, 0, "");
    node_data(0, 0, k_desc_a, sizeof(k_desc_a));
    expect_nak(commit(), MIMUSB_NAK_PARAM);
}

static void test_reports(void) {
    reset_io();
    const uint8_t a[] = {1, 0x02};
    const uint8_t b[] = {2, 0x05};
    ep_in(1, a, sizeof(a));
    ep_in(2, b, sizeof(b));
    ep_in(6, a, sizeof(a)); // an endpoint that is none of the configuration's: dropped
    ep_in(1, a, 0 + 0 + 2);
    pump();
    assert(g_nsent == 3);
    assert(g_sent[0].iface == 0 && g_sent[0].len == 2 && g_sent[0].data[1] == 0x02);
    assert(g_sent[1].iface == 1 && g_sent[1].data[0] == 2);

    // a bad CRC is skipped and the next frame is still seen
    reset_io();
    put_frame(MIMUSB_T_EP_IN_BASE | 1, a, sizeof(a));
    g_in[g_in_len - 1] ^= 0xFF;
    const uint8_t junk[] = {0x00, MIMUSB_SYNC, MIMUSB_SYNC, 0x13, 0x7F};
    memcpy(g_in + g_in_len, junk, sizeof(junk));
    g_in_len += sizeof(junk);
    ep_in(2, b, sizeof(b));
    pump();
    // the junk looks like a header with a long length; the idle timeout abandons it
    assert(g_nsent == 0);
    g_now += 11;
    pump();
    assert(g_nsent == 1 && g_sent[0].iface == 1);

    // a frame split across polls is reassembled; a 64-byte report is accepted
    reset_io();
    uint8_t big[64];
    for (int i = 0; i < 64; i++)
        big[i] = (uint8_t)i;
    ep_in(1, big, sizeof(big));
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
    ep_in(1, toolong, sizeof(toolong));
    pump();
    assert(g_nsent == 0);

    // a full queue drops the newest report and keeps order
    reset_io();
    g_busy[0] = true;
    for (int i = 0; i < 30; i++) {
        uint8_t r[2] = {1, (uint8_t)i};
        ep_in(1, r, 2);
    }
    pump();
    assert(g_nsent == 0);
    // loss reporting is off until asked: nothing is said about the 15 that did not fit
    g_now += 25;
    pump();
    assert(lost_reported() == 0);
    g_busy[0] = false;
    for (int i = 0; i < 6; i++)
        pump();
    assert(g_nsent == 15 && g_sent[0].data[1] == 0 && g_sent[14].data[1] == 14);

    // LOSS_CONFIG checks its payload: enabled is 0 or 1, and the interval is 0 while disabled
    {
        const uint8_t bad_flag[3] = {2, 0, 0};
        expect_nak(command(MIMUSB_T_LOSS_CONFIG, bad_flag, 3), MIMUSB_NAK_PARAM);
        const uint8_t off_with_interval[3] = {0, 5, 0};
        expect_nak(command(MIMUSB_T_LOSS_CONFIG, off_with_interval, 3), MIMUSB_NAK_PARAM);
        const uint8_t short_payload[2] = {1, 0};
        expect_nak(command(MIMUSB_T_LOSS_CONFIG, short_payload, 2), MIMUSB_NAK_PARAM);
    }

    // enabled with a 50 ms interval: drops are gathered and reported once per interval
    reset_io();
    {
        const uint8_t on[3] = {1, 50, 0};
        assert(command(MIMUSB_T_LOSS_CONFIG, on, 3) == 1);
    }
    reset_io();
    g_busy[0] = true;
    for (int i = 0; i < 20; i++) {
        uint8_t r[2] = {1, (uint8_t)i};
        ep_in(1, r, 2);
    }
    pump();
    assert(lost_reported() == 5); // the first report after enabling goes out at once
    g_out_len = 0;
    for (int i = 0; i < 3; i++) {
        uint8_t r[2] = {1, 0};
        ep_in(1, r, 2);
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
        assert(command(MIMUSB_T_LOSS_CONFIG, off, 3) == 1);
    }
    g_out_len = 0;
    ep_in(1, (const uint8_t[]){1, 0}, 2);
    g_now += 100;
    pump();
    assert(lost_reported() == 0);

    // a command sent again with the same sequence number is answered again, not run twice: the
    // second payload (off) is not applied, so a drop is still reported
    {
        const uint8_t on[3] = {1, 0, 0};
        const uint8_t off[3] = {0, 0, 0};
        assert(command(MIMUSB_T_LOSS_CONFIG, on, 3) == 1);
        g_seq--;
        assert(command(MIMUSB_T_LOSS_CONFIG, off, 3) == 1);
        g_out_len = 0;
        ep_in(1, (const uint8_t[]){1, 0}, 2);
        pump();
        assert(lost_reported() == 1);
        // a PING is a new host, whose numbers start anew: the same number runs the command
        put_frame(MIMUSB_T_PING, NULL, 0);
        pump();
        g_seq--;
        assert(command(MIMUSB_T_LOSS_CONFIG, off, 3) == 1);
        g_out_len = 0;
        ep_in(1, (const uint8_t[]){1, 0}, 2);
        g_now += 1;
        pump();
        assert(lost_reported() == 0);
    }
    g_busy[0] = false;

    // RESET drops what is queued
    reset_io();
    g_busy[0] = true;
    ep_in(1, a, sizeof(a));
    pump();
    uint8_t none = 0;
    (void)none;
    assert(command(MIMUSB_T_RESET, NULL, 0) == 1);
    g_busy[0] = false;
    g_nsent = 0;
    pump();
    assert(g_nsent == 0);
}

static void test_ping_and_events(void) {
    reset_io();
    put_frame(MIMUSB_T_PING, NULL, 0);
    pump();
    size_t pos = 0;
    uint8_t p[2048];
    uint16_t n;
    assert(next_out(&pos, p, &n) == MIMUSB_T_PONG && n == MIMUSB_PONG_MIN_LEN + 8);
    assert(p[0] == FW_VERSION_MAJOR && p[1] == FW_VERSION_MINOR && p[2] == FW_VERSION_PATCH &&
           p[3] == 1);
    assert(p[4] == 0 && p[5] == 0); // reserved
    assert(p[6] == MIMUSB_PONG_FLAG_UNIQUE_ID && p[7] == MIMUSB_CHIP_RP2040 && p[8] == 0);
    assert(u32at(&p[9]) == MIMUSB_BOOT_BAUD && u32at(&p[13]) == g_max_rate);
    assert(u32at(&p[17]) == MIMUSB_PONG_UART_BREAK);
    assert((p[21] | (p[22] << 8)) == 0 && (p[25] | (p[26] << 8)) == 0); // reserved
    assert((p[23] | (p[24] << 8)) == MIMUSB_MAX_REPORT_BYTES && MIMUSB_MAX_REPORT_BYTES == 64);
    assert((p[27] | (p[28] << 8)) == 64);
    assert((p[29] | (p[30] << 8)) == MIMUSB_MAX_FRAME_PAYLOAD &&
           MIMUSB_MAX_FRAME_PAYLOAD >= MIMUSB_MIN_FRAME_PAYLOAD);
    assert((p[31] | (p[32] << 8)) == MIMUSB_CONTROL_DATA_MAX && p[33] == MIMUSB_CONTROL_RULES);
    assert(p[34] == MIMUSB_MAX_ENDPOINT_NUMBER);
    assert(p[35] == 8 && p[MIMUSB_PONG_MIN_LEN] == 0x01 && p[MIMUSB_PONG_MIN_LEN + 7] == 0xEF);
    // The limits of the generic driver (config.h), then the unique id.
    assert(p[36] == MIMUSB_MAX_INTERFACES && MIMUSB_MAX_INTERFACES == 15);
    assert((p[37] | (p[38] << 8)) == MIMUSB_MAX_CONFIG_BYTES && p[39] == MIMUSB_MAX_STRINGS);
    assert((p[40] | (p[41] << 8)) == MIMUSB_MAX_TRANSFER_BYTES && MIMUSB_MAX_TRANSFER_BYTES == 64);
    assert((p[42] | (p[43] << 8)) == MIMUSB_EP_POOL_SLOTS && MIMUSB_EP_POOL_SLOTS == 64);
    assert(p[44] == ((1u << TUSB_XFER_BULK) | (1u << TUSB_XFER_INTERRUPT)));
    // The answer memory of the control rules (config.h), the last field before the unique id.
    assert((p[45] | (p[46] << 8)) == MIMUSB_ANSWER_BYTES && MIMUSB_ANSWER_BYTES == 4096);
    // The largest BOS descriptor (config.h), then the unique id.
    assert((p[47] | (p[48] << 8)) == MIMUSB_MAX_BOS_BYTES && MIMUSB_MAX_BOS_BYTES == 256);
    assert(MIMUSB_PONG_MIN_LEN == 49 && p[MIMUSB_PONG_MIN_LEN] == 0x01);

    // The board's own events: mount, unmount, suspend, resume. (What a class does on the control
    // pipe is reported by the control rules, below.)
    device_config(0, "");
    node_begin(0, 0, sizeof(k_desc_a), 1, 8, 0, "");
    node_data(0, 0, k_desc_a, sizeof(k_desc_a));
    node_begin(1, 1, sizeof(k_desc_b), 1, 8, 0, "");
    node_data(1, 0, k_desc_b, sizeof(k_desc_b));
    assert(commit() == 1);
    reset_io();
    tud_mount_cb();
    tud_umount_cb();
    tud_suspend_cb(true);
    tud_resume_cb();
    pos = 0;
    assert(next_out(&pos, p, &n) == MIMUSB_T_EVENT && p[0] == MIMUSB_EV_MOUNTED);
    assert(next_out(&pos, p, &n) == MIMUSB_T_EVENT && p[0] == MIMUSB_EV_UNMOUNTED);
    assert(next_out(&pos, p, &n) == MIMUSB_T_EVENT && p[0] == MIMUSB_EV_SUSPEND && p[1] == 1);
    assert(next_out(&pos, p, &n) == MIMUSB_T_EVENT && p[0] == MIMUSB_EV_RESUME);
    assert(next_out(&pos, p, &n) == 0); // and nothing else

    // remote wakeup only while suspended
    g_suspended = false;
    assert(command(MIMUSB_T_REMOTE_WAKEUP, NULL, 0) == 0 && g_nak_code == MIMUSB_NAK_STATE);
    g_suspended = true;
    assert(command(MIMUSB_T_REMOTE_WAKEUP, NULL, 0) == 1 && g_wakeups == 1);
    g_suspended = false;

    // BOOTLOADER: only with its key, only on a chip that has one; the answer leaves first.
    {
        const uint8_t key[4] = {'B', 'O', 'O', 'T'};
        const uint8_t wrong[4] = {'B', 'O', 'O', 'S'};
        assert(command(MIMUSB_T_BOOTLOADER, wrong, 4) == 0 && g_nak_code == MIMUSB_NAK_PARAM);
        assert(command(MIMUSB_T_BOOTLOADER, key, 3) == 0 && g_nak_code == MIMUSB_NAK_PARAM);
        assert(g_bootloader_entered == 0);
        g_has_bootloader = false;
        assert(command(MIMUSB_T_BOOTLOADER, key, 4) == 0 && g_nak_code == MIMUSB_NAK_UNSUPPORTED);
        assert(g_bootloader_entered == 0);
        g_has_bootloader = true;
        assert(command(MIMUSB_T_BOOTLOADER, key, 4) == 1 && g_bootloader_entered == 1);
    }
}

static int set_baud_cmd(uint32_t baud) {
    uint8_t b[4];
    put32(b, baud);
    return command(MIMUSB_T_SET_BAUD, b, 4);
}

static void ping_pong(uint8_t* p) {
    reset_io();
    put_frame(MIMUSB_T_PING, NULL, 0);
    pump();
    size_t pos = 0;
    uint16_t n;
    assert(next_out(&pos, p, &n) == MIMUSB_T_PONG);
}

static void test_set_baud(void) {
    reset_io();
    uint8_t p[2048];
    // Zero, a rate above the board's maximum, and a rate the divider misses by over 2 % are
    // refused.
    assert(set_baud_cmd(0) == 0 && g_nak_code == MIMUSB_NAK_PARAM);
    assert(set_baud_cmd(g_max_rate + 1) == 0 && g_nak_code == MIMUSB_NAK_PARAM);
    g_rate_error_ppm = 30000;
    assert(set_baud_cmd(1000000u) == 0 && g_nak_code == MIMUSB_NAK_PARAM);
    // A wrong payload length is refused too.
    assert(command(MIMUSB_T_SET_BAUD, (const uint8_t*)"\1", 1) == 0 &&
           g_nak_code == MIMUSB_NAK_PARAM);
    assert(g_baud == MIMUSB_BOOT_BAUD && g_baud_changes == 0);

    // The ACK carries the rate the divider makes (1.5 % off here) and goes out before the switch.
    g_rate_error_ppm = 15000;
    assert(set_baud_cmd(1000000u) == 1 && g_baud == 1000000u);
    assert(g_ack_data_len == 4 && u32at(g_ack_data) == 1015000u);
    g_rate_error_ppm = 0;
    // An odd rate is as good as a round one.
    assert(set_baud_cmd(1234567u) == 1 && g_baud == 1234567u);
    assert(set_baud_cmd(g_max_rate) == 1 && g_baud == g_max_rate);

    // A valid frame at the new rate keeps it.
    g_now += MIMUSB_BAUD_CONFIRM_MS - 1;
    ping_pong(p);
    assert(u32at(&p[9]) == g_max_rate);
    g_now += 10 * MIMUSB_BAUD_CONFIRM_MS;
    pump();
    assert(g_baud == g_max_rate);

    // Back to the boot rate needs no confirmation.
    assert(set_baud_cmd(MIMUSB_BOOT_BAUD) == 1 && g_baud == MIMUSB_BOOT_BAUD);
    g_now += 10 * MIMUSB_BAUD_CONFIRM_MS;
    pump();
    assert(g_baud == MIMUSB_BOOT_BAUD);

    // Nothing valid at the new rate in time: back to the boot rate.
    assert(set_baud_cmd(921600u) == 1 && g_baud == 921600u);
    reset_io();
    const uint8_t junk[] = {0x00, 0xFF, 0x13, 0x37};
    memcpy(g_in, junk, sizeof(junk));
    g_in_len = sizeof(junk);
    g_now += MIMUSB_BAUD_CONFIRM_MS - 1;
    pump();
    assert(g_baud == 921600u);
    g_now += 1;
    pump();
    assert(g_baud == MIMUSB_BOOT_BAUD);
    ping_pong(p);
    assert(u32at(&p[9]) == MIMUSB_BOOT_BAUD);
}

// A BREAK returns the board to the boot rate, confirmed or not, and drops what it holds.
static void test_break(void) {
    uint8_t p[2048];
    assert(set_baud_cmd(3000000u) == 1);
    g_now += 1;
    ping_pong(p); // confirmed by a valid frame
    g_now += 10 * MIMUSB_BAUD_CONFIRM_MS;
    pump();
    assert(g_baud == 3000000u);
    reset_io();
    put_frame(MIMUSB_T_PING, NULL, 0); // sent at the old rate, still unread when the BREAK is seen
    g_errors = HAL_UART_ERR_BREAK;
    pump();
    assert(g_baud == MIMUSB_BOOT_BAUD);
    assert(g_out_len == 0); // the ping was discarded
    ping_pong(p);
    assert(u32at(&p[9]) == MIMUSB_BOOT_BAUD);
    // A BREAK at the boot rate changes nothing but the discard.
    int changes = g_baud_changes;
    g_errors = HAL_UART_ERR_BREAK;
    pump();
    assert(g_baud_changes == changes);
}

// The counters, PROBE and the CRC.
static void test_counters_and_probe(void) {
    uint8_t reset = MIMUSB_COUNTERS_F_RESET, keep = 0;
    assert(command(MIMUSB_T_COUNTERS, &reset, 1) == 1); // start from zero
    uint8_t pat[100];
    for (unsigned i = 0; i < sizeof(pat); i++)
        pat[i] = MIMUSB_PROBE_BYTE(i);
    reset_io();
    for (int i = 0; i < 3; i++)
        put_frame(MIMUSB_T_PROBE, pat, 100);
    pump();
    assert(g_out_len == 0); // a probe is not answered
    // a probe with a wrong pattern, a frame with a wrong CRC, a framing error and an overrun
    pat[5] ^= 1;
    put_frame(MIMUSB_T_PROBE, pat, 100);
    put_frame(MIMUSB_T_PROBE, pat, 100);
    g_in[g_in_len - 1] ^= 0x40;
    g_errors = HAL_UART_ERR_FRAMING | HAL_UART_ERR_OVERRUN;
    pump();
    assert(command(MIMUSB_T_COUNTERS, &keep, 1) == 1 && g_ack_data_len == 10);
    assert((g_ack_data[0] | (g_ack_data[1] << 8)) == 3); // probes_good
    assert((g_ack_data[2] | (g_ack_data[3] << 8)) == 2); // bad_crc: wrong pattern + wrong CRC
    assert((g_ack_data[4] | (g_ack_data[5] << 8)) == 1); // framing
    assert((g_ack_data[6] | (g_ack_data[7] << 8)) == 1); // uart_overrun
    assert((g_ack_data[8] | (g_ack_data[9] << 8)) == 0); // ring_overrun
    // Read without reset keeps them; read with reset returns them once more and then zeroes.
    assert(command(MIMUSB_T_COUNTERS, &reset, 1) == 1 &&
           (g_ack_data[0] | (g_ack_data[1] << 8)) == 3);
    assert(command(MIMUSB_T_COUNTERS, &keep, 1) == 1);
    for (int i = 0; i < 10; i++)
        assert(g_ack_data[i] == 0);
    // The counters stop at 65535.
    for (int i = 0; i < 70000 / 1000; i++) {
        reset_io();
        for (int k = 0; k < 1000; k++)
            g_errors |= 0;
        g_errors = HAL_UART_ERR_FRAMING;
        pump();
    }
    assert(command(MIMUSB_T_COUNTERS, &keep, 1) == 1);
    assert((g_ack_data[4] | (g_ack_data[5] << 8)) == 70);

    // Two flips in the same bit position cancel in an XOR but not in a CRC.
    reset_io();
    uint8_t d[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    put_cmd(MIMUSB_T_EP_CONFIG, d, 2);
    g_in[g_in_len - 5] ^= 0x10; // flips one byte of the payload...
    g_in[g_in_len - 4] ^= 0x10; // ...and the same bit of the next
    g_out_len = 0;
    pump();
    assert(result_of(MIMUSB_T_EP_CONFIG) == -1);
}

// The serial number from the chip id (the board makes the string): its hexadecimal digits, one
// string descriptor; a chip without an id cannot make it; the board's strings take no data.
static void test_serial_from_chip_id(void) {
    device_config(T_SERIAL_FROM_ID, "");
    base_node();
    assert(commit() == 1);
    const tusb_desc_device_t* d = (const tusb_desc_device_t*)tud_descriptor_device_cb();
    assert(d->iSerialNumber == 3);
    const uint16_t* s = tud_descriptor_string_cb(3, 0x0409);
    assert((s[0] & 0xFF) == 2 + 2 * 16 && s[1] == '0' && s[2] == '1' && s[15] == 'E' &&
           s[16] == 'F');

    // Announced again as it was, it is accepted again; with data it is refused; with a length it
    // is another string.
    assert(desc_reset() == 1);
    assert(desc_begin_raw(MIMUSB_DESC_STRING, 3, 0x0409, 0, MIMUSB_DESC_F_SERIAL_FROM_ID) == 1);
    assert(desc_begin_raw(MIMUSB_DESC_STRING, 3, 0x0409, 0, MIMUSB_DESC_F_SERIAL_FROM_ID) == 1);
    expect_nak(desc_data_raw(MIMUSB_DESC_STRING, 3, 0x0409, 0, k_desc_a, 4), MIMUSB_NAK_STATE);
    expect_nak(desc_begin_raw(MIMUSB_DESC_STRING, 3, 0x0409, 0, 0), MIMUSB_NAK_STATE);
    expect_nak(desc_begin_raw(MIMUSB_DESC_STRING, 4, 0x0409, 6, MIMUSB_DESC_F_SERIAL_FROM_ID),
               MIMUSB_NAK_PARAM);

    // no id on the chip: nothing to make the string from
    g_has_id = 0;
    expect_nak(desc_begin_raw(MIMUSB_DESC_STRING, 5, 0x0409, 0, MIMUSB_DESC_F_SERIAL_FROM_ID),
               MIMUSB_NAK_UNSUPPORTED);
    g_has_id = 1;
}

static void test_detach(void) {
    device_config(0, "");
    base_node();
    assert(stage() == 1);
    assert(commit_staged() == 1);
    assert(mimusb_active_interfaces() == 1);
    const int connects = g_connects, disconnects = g_disconnects;

    // COMMIT with the detach flag leaves the bus at once and forgets the configuration.
    const uint8_t detach[] = {1};
    assert(command(MIMUSB_T_COMMIT, detach, sizeof(detach)) == 1);
    assert(g_disconnects == disconnects + 1 && g_connects == connects);
    assert(mimusb_active_interfaces() == 0);
    // what the board presents then is the device without an interface, and no string
    const uint8_t* c = tud_descriptor_configuration_cb(0);
    assert((c[2] | (c[3] << 8)) == 9 && c[4] == 0);
    assert(tud_descriptor_string_cb(0, 0) == NULL && tud_descriptor_string_cb(1, 0x0409) == NULL);
}

static const uint8_t k_zero[64];

// ---- control rules: the commands that stage and apply them ----
// One rule as the host writes it. The masks are zero (any value) unless a test sets them.
typedef struct {
    uint8_t bm_mask, bm_value, request_mask, request_value;
    uint16_t value_mask, value_value, index_mask, index_value;
    uint8_t action; // MIMUSB_RULE_*
    const uint8_t* data; // ANSWER: its bytes
    uint16_t length;     // ANSWER: their number
    uint16_t capacity;   // ANSWER: the answer memory it reserves (rule_answer: its length)
    uint8_t endpoint, filter, first; // ANSWER_LAST_SENT
    uint16_t timeout_ms; // ASK
    uint8_t after;       // ASK: MIMUSB_REPLY_STALL or MIMUSB_REPLY_DROP
} trule_t;

// A rule for one request (type byte and bRequest exactly), any wValue and wIndex.
static trule_t rule_of(uint8_t action, uint8_t bm, uint8_t request) {
    trule_t r;
    memset(&r, 0, sizeof(r));
    r.bm_mask = 0xFF;
    r.bm_value = bm;
    r.request_mask = 0xFF;
    r.request_value = request;
    r.action = action;
    return r;
}
static trule_t rule_answer(uint8_t bm, uint8_t request, const uint8_t* data, uint16_t length) {
    trule_t r = rule_of(MIMUSB_RULE_ANSWER, bm, request);
    r.data = data;
    r.length = length;
    r.capacity = length;
    return r;
}
// ANSWER_LAST_SENT of an IN endpoint; filter 1 keeps only transfers that start with `first`.
static trule_t rule_last(uint8_t bm, uint8_t request, uint8_t endpoint, uint8_t filter,
                         uint8_t first) {
    trule_t r = rule_of(MIMUSB_RULE_ANSWER_LAST_SENT, bm, request);
    r.endpoint = endpoint;
    r.filter = filter;
    r.first = first;
    return r;
}
// ASK with the time the host has and what happens after it.
static trule_t rule_for(uint8_t bm, uint8_t request, uint16_t timeout_ms, uint8_t after) {
    trule_t r = rule_of(MIMUSB_RULE_ASK, bm, request);
    r.timeout_ms = timeout_ms;
    r.after = after;
    return r;
}

static size_t rule_frame(uint8_t index, const trule_t* r, uint8_t* p) {
    size_t k = 0;
    p[k++] = index;
    p[k++] = r->bm_mask;
    p[k++] = r->bm_value;
    p[k++] = r->request_mask;
    p[k++] = r->request_value;
    put_u16(&p[k], r->value_mask);
    put_u16(&p[k + 2], r->value_value);
    put_u16(&p[k + 4], r->index_mask);
    put_u16(&p[k + 6], r->index_value);
    k += 8;
    p[k++] = r->action;
    switch (r->action) {
    case MIMUSB_RULE_ANSWER:
        put_u16(&p[k], r->capacity);
        put_u16(&p[k + 2], r->length);
        k += 4;
        break;
    case MIMUSB_RULE_ANSWER_LAST_SENT:
        p[k++] = r->endpoint;
        p[k++] = r->filter;
        p[k++] = r->first;
        break;
    case MIMUSB_RULE_ASK:
        put_u16(&p[k], r->timeout_ms);
        k += 2;
        p[k++] = r->after;
        break;
    default:
        break;
    }
    return k;
}

// RULE, and for an ANSWER its bytes as DESC_BEGIN and DESC_DATA of the kind ANSWER. 1 or the
// first answer that was not an ACK.
static int rule_stage(uint8_t index, const trule_t* r) {
    uint8_t p[32];
    int res = command(MIMUSB_T_RULE, p, rule_frame(index, r, p));
    if (res != 1 || r->action != MIMUSB_RULE_ANSWER || r->length == 0)
        return res;
    return send_blob(MIMUSB_DESC_ANSWER, index, 0, r->data, r->length);
}

static int rules_apply(uint8_t flags, uint8_t count) {
    const uint8_t p[2] = {flags, count};
    return command(MIMUSB_T_RULES_APPLY, p, sizeof(p));
}

// The table of `n` rules is staged in order and made the active one without a COMMIT.
static int control_config(uint8_t flags, const trule_t* r, int n) {
    for (int i = 0; i < n; i++) {
        const int res = rule_stage((uint8_t)i, &r[i]);
        if (res != 1)
            return res;
    }
    return rules_apply(flags, (uint8_t)n);
}

static int rule_data(uint8_t index, const uint8_t* d, size_t n) {
    uint8_t p[1 + MIMUSB_CONTROL_DATA_MAX + 1];
    assert(n <= MIMUSB_CONTROL_DATA_MAX + 1);
    p[0] = index;
    if (n)
        memcpy(p + 1, d, n);
    return command(MIMUSB_T_RULE_DATA, p, 1 + n);
}


// The staging commands: what they accept again, what they refuse, and what no longer fits.
static void test_desc_commands(void) {
    // DESC_RESET carries nothing but its sequence number.
    expect_nak(command(MIMUSB_T_DESC_RESET, (const uint8_t*)"\1", 1), MIMUSB_NAK_PARAM);
    assert(desc_reset() == 1);

    // DESC_BEGIN: seven bytes, a known kind, and the index, language and flags the kind needs.
    expect_nak(command(MIMUSB_T_DESC_BEGIN, (const uint8_t*)"\1\0\0\0\0", 5), MIMUSB_NAK_PARAM);
    expect_nak(desc_begin_raw(9, 0, 0, 4, 0), MIMUSB_NAK_PARAM); // no such kind
    expect_nak(desc_begin_raw(MIMUSB_DESC_DEVICE, 1, 0, 18, 0),
               MIMUSB_NAK_PARAM); // device: index 0
    expect_nak(desc_begin_raw(MIMUSB_DESC_DEVICE, 0, 0x0409, 18, 0),
               MIMUSB_NAK_PARAM); // device: no language
    expect_nak(desc_begin_raw(MIMUSB_DESC_DEVICE, 0, 0, 18, 1),
               MIMUSB_NAK_PARAM); // device: no flags
    expect_nak(desc_begin_raw(MIMUSB_DESC_DEVICE, 0, 0, 0, 0), MIMUSB_NAK_PARAM); // nothing to send
    expect_nak(desc_begin_raw(MIMUSB_DESC_DEVICE, 0, 0, 19, 0),
               MIMUSB_NAK_OVERFLOW); // holds 18 bytes
    expect_nak(desc_begin_raw(MIMUSB_DESC_CONFIGURATION, 0, 0, MIMUSB_MAX_CONFIG_BYTES + 1, 0),
               MIMUSB_NAK_OVERFLOW);
    expect_nak(desc_begin_raw(MIMUSB_DESC_STRING, 0, 0x0409, 4, 0),
               MIMUSB_NAK_PARAM); // the board makes index 0
    expect_nak(desc_begin_raw(MIMUSB_DESC_STRING, 1, 0, 4, 0),
               MIMUSB_NAK_PARAM); // a language is given
    expect_nak(desc_begin_raw(MIMUSB_DESC_STRING, 1, 0x0409, 3, 0),
               MIMUSB_NAK_PARAM); // an even length
    expect_nak(desc_begin_raw(MIMUSB_DESC_STRING, 1, 0x0409, 0, 0),
               MIMUSB_NAK_PARAM); // at least 2 bytes
    expect_nak(desc_begin_raw(MIMUSB_DESC_STRING, 1, 0x0409, MIMUSB_STRING_DESC_MAX + 2, 0),
               MIMUSB_NAK_PARAM);
    expect_nak(desc_begin_raw(MIMUSB_DESC_STRING, 1, 0x0409, 4, 2),
               MIMUSB_NAK_PARAM); // an unknown flag

    // The kind ANSWER names the bytes of a staged ANSWER rule: the rule is there first, with its
    // length (RULE), the bytes follow as for any descriptor.
    {
        assert(desc_reset() == 1);
        const trule_t a = rule_answer(0xC0, 0x42, k_desc_a, sizeof(k_desc_a));
        uint8_t p[32];
        assert(command(MIMUSB_T_RULE, p, rule_frame(0, &a, p)) == 1);
        const trule_t stall = rule_of(MIMUSB_RULE_STALL, 0xC0, 0x43);
        assert(command(MIMUSB_T_RULE, p, rule_frame(1, &stall, p)) == 1);
        const trule_t empty = rule_answer(0xC0, 0x44, NULL, 0);
        assert(command(MIMUSB_T_RULE, p, rule_frame(2, &empty, p)) == 1);
        expect_nak(desc_begin_raw(MIMUSB_DESC_ANSWER, 0, 0x0409, sizeof(k_desc_a), 0),
                   MIMUSB_NAK_PARAM); // no language
        expect_nak(desc_begin_raw(MIMUSB_DESC_ANSWER, 0, 0, sizeof(k_desc_a), 1),
                   MIMUSB_NAK_PARAM); // no flags
        expect_nak(desc_begin_raw(MIMUSB_DESC_ANSWER, 0, 0, sizeof(k_desc_a) + 1, 0),
                   MIMUSB_NAK_PARAM); // not the length the rule was staged with
        expect_nak(desc_begin_raw(MIMUSB_DESC_ANSWER, 1, 0, 4, 0),
                   MIMUSB_NAK_STATE); // a STALL rule has no bytes
        expect_nak(desc_begin_raw(MIMUSB_DESC_ANSWER, 3, 0, 4, 0), MIMUSB_NAK_STATE); // no rule 3
        // A descriptor announced again as it was is acknowledged; with another length it is refused.
        assert(desc_begin_raw(MIMUSB_DESC_ANSWER, 0, 0, sizeof(k_desc_a), 0) == 1);
        assert(desc_begin_raw(MIMUSB_DESC_ANSWER, 0, 0, sizeof(k_desc_a), 0) == 1);
        expect_nak(desc_begin_raw(MIMUSB_DESC_ANSWER, 0, 0, sizeof(k_desc_a) + 1, 0),
                   MIMUSB_NAK_STATE);
        // DESC_DATA: inside the length, without a gap.
        expect_nak(desc_data_raw(MIMUSB_DESC_ANSWER, 1, 0, 0, k_desc_a, 4), MIMUSB_NAK_STATE);
        expect_nak(desc_data_raw(MIMUSB_DESC_ANSWER, 0, 0, 5, k_desc_a, 4), MIMUSB_NAK_PARAM); // a gap
        expect_nak(desc_data_raw(MIMUSB_DESC_ANSWER, 0, 0, 0, k_zero, sizeof(k_desc_a) + 1),
                   MIMUSB_NAK_PARAM);
        assert(desc_data_raw(MIMUSB_DESC_ANSWER, 0, 0, 0, k_desc_a, 10) == 1);
        assert(desc_data_raw(MIMUSB_DESC_ANSWER, 0, 0, 0, k_desc_a, 10) == 1); // again
        assert(desc_data_raw(MIMUSB_DESC_ANSWER, 0, 0, 10, k_desc_a + 10, sizeof(k_desc_a) - 10) == 1);
        expect_nak(desc_data_raw(MIMUSB_DESC_ANSWER, 0, 0, sizeof(k_desc_a), k_desc_a, 1),
                   MIMUSB_NAK_PARAM);
        // The staged rules are forgotten by DESC_RESET: the bytes of rule 0 have nowhere to go.
        assert(desc_reset() == 1);
        expect_nak(desc_data_raw(MIMUSB_DESC_ANSWER, 0, 0, 0, k_desc_a, 4), MIMUSB_NAK_STATE);
    }

    // DESC_DATA: only for an announced descriptor.
    expect_nak(desc_data_raw(MIMUSB_DESC_STRING, 1, 0x0409, 0, k_desc_a, 4), MIMUSB_NAK_STATE);
    expect_nak(desc_data_raw(MIMUSB_DESC_DEVICE, 0, 0, 0, k_desc_a, 4), MIMUSB_NAK_STATE);
    expect_nak(command(MIMUSB_T_DESC_DATA, (const uint8_t*)"\4\0\0\0\0", 5),
               MIMUSB_NAK_PARAM); // no bytes

    // The string store holds MIMUSB_MAX_STRINGS strings of the longest length, and not one more,
    // whatever its index or language: NAK OVERFLOW.
    assert(desc_reset() == 1);
    for (uint8_t i = 0; i < MIMUSB_MAX_STRINGS; i++)
        assert(desc_begin_raw(MIMUSB_DESC_STRING, (uint8_t)(i + 1), 0x0409, MIMUSB_STRING_DESC_MAX,
                              0) == 1);
    expect_nak(desc_begin_raw(MIMUSB_DESC_STRING, MIMUSB_MAX_STRINGS + 1, 0x0409, 4, 0),
               MIMUSB_NAK_OVERFLOW);
    // the same index in another language is another string
    expect_nak(desc_begin_raw(MIMUSB_DESC_STRING, 1, 0x0407, 4, 0), MIMUSB_NAK_OVERFLOW);
    assert(desc_reset() == 1);
}

// ---- the walk at COMMIT: every refusal, with descriptors written byte by byte ----
// A configuration descriptor under construction.
typedef struct {
    uint8_t b[MIMUSB_MAX_CONFIG_BYTES + 64];
    size_t n;
    uint8_t interfaces;
} tcfg_t;

static void cfg_open(tcfg_t* c) {
    c->n = 9;
    c->interfaces = 0;
}

static void cfg_interface(tcfg_t* c, uint8_t number, uint8_t alt, uint8_t endpoints, uint8_t cls) {
    const uint8_t d[9] = {9, TUSB_DESC_INTERFACE, number, alt, endpoints, cls, 0, 0, 0};
    memcpy(c->b + c->n, d, 9);
    c->n += 9;
    c->interfaces++;
}

static void cfg_hid(tcfg_t* c, uint16_t report_length) {
    uint8_t d[9] = {9, T_HID_DESC_HID, 0x11, 0x01, 0, 1, T_HID_DESC_REPORT};
    put_u16(&d[7], report_length);
    memcpy(c->b + c->n, d, 9);
    c->n += 9;
}

static void cfg_endpoint(tcfg_t* c, uint8_t address, uint8_t type, uint16_t size,
                         uint8_t interval) {
    uint8_t d[7] = {7, TUSB_DESC_ENDPOINT, address, type};
    put_u16(&d[4], size);
    d[6] = interval;
    memcpy(c->b + c->n, d, 7);
    c->n += 7;
}

// One HID interface the board takes: its number, one IN endpoint, and the report descriptor length.
static void cfg_hid_interface(tcfg_t* c, uint8_t number, uint8_t in_number,
                              uint16_t report_length) {
    cfg_interface(c, number, 0, 1, 0x03);
    cfg_hid(c, report_length);
    cfg_endpoint(c, (uint8_t)(0x80 | in_number), TUSB_XFER_INTERRUPT, 8, 1);
}

static void cfg_close(tcfg_t* c) {
    const uint8_t head[9] = {9, TUSB_DESC_CONFIGURATION, 0, 0, c->interfaces, 1, 0, 0xA0, 50};
    memcpy(c->b, head, 9);
    put_u16(&c->b[2], (uint16_t)c->n);
}

static void make_device(uint8_t* d) {
    const uint8_t dev[18] = {
        18, TUSB_DESC_DEVICE, 0x00, 0x02, 0, 0, 0, 64, 0x34, 0x12, 0x78, 0x56, 0x00, 0x01, 0, 0, 0,
        1};
    memcpy(d, dev, 18);
}

// The queue of every IN endpoint a configuration descriptor has, as the host library gives them
// (a COMMIT refuses an IN endpoint without). The answers are not looked at: a configuration the
// walk refuses has none to give.
static void stage_endpoints(const tcfg_t* c) {
    size_t pos = 9;
    while (pos + 2 <= c->n && c->b[pos] >= 2 && pos + c->b[pos] <= c->n) {
        if (c->b[pos + 1] == TUSB_DESC_ENDPOINT && c->b[pos] == 7 && (c->b[pos + 2] & 0x80) != 0)
            (void)ep_config(c->b[pos + 2], 2, MIMUSB_EP_FULL_DROP_NEWEST);
        pos += c->b[pos];
    }
}

// Stages a device and the given configuration, the queues of its IN endpoints, and returns the
// answer to the COMMIT (1, or 0 with the NAK code).
static int commit_raw(const uint8_t* dev, size_t dev_len, tcfg_t* c) {
    assert(desc_reset() == 1);
    uint8_t d[18];
    make_device(d);
    int r = send_blob(MIMUSB_DESC_DEVICE, 0, 0, dev != NULL ? dev : d, dev != NULL ? dev_len : 18);
    if (r != 1)
        return r;
    if ((r = send_blob(MIMUSB_DESC_CONFIGURATION, 0, 0, c->b, c->n)) != 1)
        return r;
    stage_endpoints(c);
    return commit_staged();
}

// A COMMIT of a configuration that does not hold together is refused, nothing is disconnected and
// the active configuration is the one before.
static void expect_refused(int r) { expect_nak(r, MIMUSB_NAK_DESCRIPTOR); }

static void test_commit_walk(void) {
    // Something committed first, which the harness takes off the bus before each of the commits
    // below (as the host does: a COMMIT that attaches is refused while the board is attached, see
    // test_commit_attached_refused).
    device_config(0, "Kept");
    node_begin(0, 0, sizeof(k_desc_a), 4, 16, 1, "Keys");
    node_data(0, 0, k_desc_a, sizeof(k_desc_a));
    assert(commit() == 1);
    g_disconnects = g_connects = 0;

    tcfg_t c;
    const uint16_t rd = sizeof(k_desc_a); // what a HID descriptor says; the board does not read it

    // the base case holds
    cfg_open(&c);
    cfg_hid_interface(&c, 0, 1, rd);
    cfg_close(&c);
    assert(commit_raw(NULL, 0, &c) == 1);
    assert(g_connects == 1);
    g_disconnects = g_connects = 0;

    // the device descriptor: 18 bytes (USB 2.0 9.6.1), its own bLength and type, one configuration,
    // a control endpoint size the controller works with, a bcdUSB
    {
        uint8_t d[19];
        make_device(d);
        d[18] = 0;
        expect_refused(commit_raw(d, 17, &c)); // 17 bytes
        make_device(d);
        d[0] = 17;
        expect_refused(commit_raw(d, 18, &c));
        make_device(d);
        d[1] = TUSB_DESC_CONFIGURATION;
        expect_refused(commit_raw(d, 18, &c));
        make_device(d);
        d[17] = 2;
        expect_refused(commit_raw(d, 18, &c));
        make_device(d);
        d[2] = d[3] = 0;
        expect_refused(commit_raw(d, 18, &c));
        static const uint8_t bad_ep0[] = {0, 7, 48, 128};
        for (size_t i = 0; i < sizeof(bad_ep0); i++) {
            make_device(d);
            d[7] = bad_ep0[i];
            expect_refused(commit_raw(d, 18, &c));
        }
        // announced longer than the 18 bytes the board holds: refused when it is announced
        assert(desc_reset() == 1);
        expect_nak(desc_begin_raw(MIMUSB_DESC_DEVICE, 0, 0, 19, 0), MIMUSB_NAK_OVERFLOW);
        // staged only in part
        make_device(d);
        assert(desc_reset() == 1);
        assert(send_blob_partial(MIMUSB_DESC_DEVICE, 0, 0, d, 18, 10, 0) == 1);
        assert(send_blob(MIMUSB_DESC_CONFIGURATION, 0, 0, c.b, c.n) == 1);
        stage_endpoints(&c);
        expect_refused(commit_staged());
        // no device descriptor at all
        assert(desc_reset() == 1);
        assert(send_blob(MIMUSB_DESC_CONFIGURATION, 0, 0, c.b, c.n) == 1);
        stage_endpoints(&c);
        expect_refused(commit_staged());
        // Through all of those refusals the target saw no connect, and the board, off the bus since
        // the detach before the first of them, serves no configuration.
        assert(g_connects == 0 && g_disconnects == 0);
        assert(mimusb_active_interfaces() == 0);
    }

    // the configuration descriptor: its own header, wTotalLength, one configuration, attributes
    {
        tcfg_t t = c;
        t.b[0] = 8;
        expect_refused(commit_raw(NULL, 0, &t));
        t = c;
        t.b[1] = TUSB_DESC_INTERFACE;
        expect_refused(commit_raw(NULL, 0, &t));
        t = c;
        t.b[2]++; // wTotalLength is not the length of the blob
        expect_refused(commit_raw(NULL, 0, &t));
        t = c;
        t.b[2]--;
        expect_refused(commit_raw(NULL, 0, &t));
        t = c;
        t.b[4] = 2; // bNumInterfaces says two, the walk finds one
        expect_refused(commit_raw(NULL, 0, &t));
        static const uint8_t bad_value[] = {0, 2, 255};
        for (size_t i = 0; i < sizeof(bad_value); i++) {
            t = c;
            t.b[5] = bad_value[i]; // the stack maps SET_CONFIGURATION(1) to the one configuration
            expect_refused(commit_raw(NULL, 0, &t));
        }
        static const uint8_t bad_attr[] = {0x00, 0x20, 0x81,
                                           0x9F}; // D7 clear, or D4..0 set (9.6.3)
        for (size_t i = 0; i < sizeof(bad_attr); i++) {
            t = c;
            t.b[7] = bad_attr[i];
            expect_refused(commit_raw(NULL, 0, &t));
        }
        // a configuration with no interface
        cfg_open(&t);
        cfg_close(&t);
        expect_refused(commit_raw(NULL, 0, &t));
        // the descriptors are cut short: a bLength below 2, and a bLength past the end of the blob
        t = c;
        t.b[9] = 1;
        expect_refused(commit_raw(NULL, 0, &t));
        t = c;
        t.b[9] = 0;
        expect_refused(commit_raw(NULL, 0, &t));
        t = c;
        t.b[9 + 9] = 250; // the class descriptor claims to run past the end
        expect_refused(commit_raw(NULL, 0, &t));
        t = c;
        t.n -= 3;
        put_u16(&t.b[2], (uint16_t)t.n); // the last endpoint descriptor is truncated
        expect_refused(commit_raw(NULL, 0, &t));
        // an interface association descriptor names the interfaces that follow it: this one names
        // two and one follows
        cfg_open(&t);
        {
            const uint8_t iad[8] = {8, 0x0B, 0, 2, 3, 0, 0, 0};
            memcpy(t.b + t.n, iad, 8);
            t.n += 8;
        }
        cfg_hid_interface(&t, 0, 1, rd);
        cfg_close(&t);
        expect_refused(commit_raw(NULL, 0, &t));
        // a configuration too long for the board is refused when it is announced
        assert(desc_reset() == 1);
        expect_nak(desc_begin_raw(MIMUSB_DESC_CONFIGURATION, 0, 0, MIMUSB_MAX_CONFIG_BYTES + 1, 0),
                   MIMUSB_NAK_OVERFLOW);
    }

    // interface numbers: 0, 1, 2, ... in order, alternate setting 0 only
    {
        tcfg_t t;
        cfg_open(&t); // 0 then 2: a gap
        cfg_hid_interface(&t, 0, 1, rd);
        cfg_hid_interface(&t, 2, 2, rd);
        cfg_close(&t);
        expect_refused(commit_raw(NULL, 0, &t));
        cfg_open(&t); // 1 then 0: out of order
        cfg_hid_interface(&t, 1, 1, rd);
        cfg_hid_interface(&t, 0, 2, rd);
        cfg_close(&t);
        expect_refused(commit_raw(NULL, 0, &t));
        cfg_open(&t); // 0 then 0: twice
        cfg_hid_interface(&t, 0, 1, rd);
        cfg_hid_interface(&t, 0, 2, rd);
        cfg_close(&t);
        expect_refused(commit_raw(NULL, 0, &t));
        cfg_open(&t); // starting at 1
        cfg_hid_interface(&t, 1, 1, rd);
        cfg_close(&t);
        expect_refused(commit_raw(NULL, 0, &t));
        cfg_open(&t); // 0 and 1
        cfg_hid_interface(&t, 0, 1, rd);
        cfg_hid_interface(&t, 1, 2, rd);
        cfg_close(&t);
        assert(commit_raw(NULL, 0, &t) == 1);
    }
    {
        tcfg_t t;
        cfg_open(&t); // an alternate setting
        cfg_interface(&t, 0, 1, 1, 0x03);
        cfg_hid(&t, rd);
        cfg_endpoint(&t, 0x81, TUSB_XFER_INTERRUPT, 8, 1);
        cfg_close(&t);
        expect_refused(commit_raw(NULL, 0, &t));
        // Alternate settings: 0, 1, 2, ... in order after setting 0, with nothing between them. A
        // bulk or interrupt endpoint that two settings name is the same in both.
        cfg_open(&t);
        cfg_interface(&t, 0, 0, 1, 0xFF);
        cfg_endpoint(&t, 0x81, TUSB_XFER_BULK, 64, 0);
        cfg_interface(&t, 0, 1, 1, 0xFF);
        t.interfaces--; // a setting is not another interface
        cfg_endpoint(&t, 0x81, TUSB_XFER_BULK, 64, 0);
        cfg_interface(&t, 1, 0, 0, 0xFF);
        cfg_interface(&t, 1, 1, 1, 0xFF);
        t.interfaces--;
        cfg_endpoint(&t, 0x82, TUSB_XFER_INTERRUPT, 8, 1);
        cfg_close(&t);
        assert(commit_raw(NULL, 0, &t) == 1);
        assert(mimusb_active_layout()->count == 2 && mimusb_active_layout()->itf_alts[0] == 2 &&
               mimusb_active_layout()->itf_alts[1] == 2);
        // the span of an interface takes its settings (TinyUSB opens once per interface)
        assert(mimusb_active_generic_span(0) == 9 + 7 + 9 + 7);
        assert(mimusb_active_generic_span(1) == 9 + 9 + 7);
        cfg_open(&t); // setting 2 without 1
        cfg_interface(&t, 0, 0, 0, 0xFF);
        cfg_interface(&t, 0, 2, 0, 0xFF);
        t.interfaces--;
        cfg_close(&t);
        expect_refused(commit_raw(NULL, 0, &t));
        cfg_open(&t); // the endpoint again with another size
        cfg_interface(&t, 0, 0, 1, 0xFF);
        cfg_endpoint(&t, 0x81, TUSB_XFER_BULK, 64, 0);
        cfg_interface(&t, 0, 1, 1, 0xFF);
        t.interfaces--;
        cfg_endpoint(&t, 0x81, TUSB_XFER_BULK, 32, 0);
        cfg_close(&t);
        expect_refused(commit_raw(NULL, 0, &t));
        cfg_open(&t); // the address of another interface
        cfg_interface(&t, 0, 0, 1, 0xFF);
        cfg_endpoint(&t, 0x81, TUSB_XFER_BULK, 64, 0);
        cfg_interface(&t, 1, 0, 1, 0xFF);
        cfg_endpoint(&t, 0x81, TUSB_XFER_BULK, 64, 0);
        cfg_close(&t);
        expect_refused(commit_raw(NULL, 0, &t));
        cfg_open(&t); // an isochronous endpoint on a board that has none
        cfg_interface(&t, 0, 0, 0, 0xFF);
        cfg_interface(&t, 0, 1, 1, 0xFF);
        t.interfaces--;
        cfg_endpoint(&t, 0x81, TUSB_XFER_ISOCHRONOUS, 64, 1);
        cfg_close(&t);
        expect_refused(commit_raw(NULL, 0, &t));
        // The board reads no class: an interface of any class, with or without class descriptors,
        // is the caller's.
        static const uint8_t classes[] = {0x00, 0x02, 0x03, 0x08, 0xFF};
        for (size_t i = 0; i < sizeof(classes); i++) {
            cfg_open(&t);
            cfg_interface(&t, 0, 0, 1, classes[i]);
            cfg_hid(&t, rd);
            cfg_endpoint(&t, 0x81, TUSB_XFER_INTERRUPT, 8, 1);
            cfg_close(&t);
            assert(commit_raw(NULL, 0, &t) == 1);
        }
        // more interfaces than the board has (they need no endpoint)
        cfg_open(&t);
        for (uint8_t i = 0; i <= MIMUSB_MAX_INTERFACES; i++)
            cfg_interface(&t, i, 0, 0, 0xFF);
        cfg_close(&t);
        expect_refused(commit_raw(NULL, 0, &t));
    }

    // endpoints: how many of each direction, in what order and with what descriptors in between
    // is the caller's, as long as the interface announces the ones that follow
    {
        tcfg_t t;
        cfg_open(&t); // no class descriptor
        cfg_interface(&t, 0, 0, 1, 0x03);
        cfg_endpoint(&t, 0x81, TUSB_XFER_INTERRUPT, 8, 1);
        cfg_close(&t);
        assert(commit_raw(NULL, 0, &t) == 1);
        cfg_open(&t); // an endpoint before the class descriptor
        cfg_interface(&t, 0, 0, 1, 0x03);
        cfg_endpoint(&t, 0x81, TUSB_XFER_INTERRUPT, 8, 1);
        cfg_hid(&t, rd);
        cfg_close(&t);
        assert(commit_raw(NULL, 0, &t) == 1);
        cfg_open(&t); // two class descriptors
        cfg_interface(&t, 0, 0, 1, 0x03);
        cfg_hid(&t, rd);
        cfg_hid(&t, rd);
        cfg_endpoint(&t, 0x81, TUSB_XFER_INTERRUPT, 8, 1);
        cfg_close(&t);
        assert(commit_raw(NULL, 0, &t) == 1);
        cfg_open(&t); // no endpoint
        cfg_interface(&t, 0, 0, 0, 0x03);
        cfg_hid(&t, rd);
        cfg_close(&t);
        assert(commit_raw(NULL, 0, &t) == 1);
        cfg_open(&t); // an OUT endpoint only
        cfg_interface(&t, 0, 0, 1, 0x03);
        cfg_hid(&t, rd);
        cfg_endpoint(&t, 0x01, TUSB_XFER_INTERRUPT, 8, 1);
        cfg_close(&t);
        assert(commit_raw(NULL, 0, &t) == 1);
        cfg_open(&t); // two IN endpoints
        cfg_interface(&t, 0, 0, 2, 0x03);
        cfg_hid(&t, rd);
        cfg_endpoint(&t, 0x81, TUSB_XFER_INTERRUPT, 8, 1);
        cfg_endpoint(&t, 0x82, TUSB_XFER_INTERRUPT, 8, 1);
        cfg_close(&t);
        assert(commit_raw(NULL, 0, &t) == 1);
        cfg_open(&t); // two OUT endpoints
        cfg_interface(&t, 0, 0, 3, 0x03);
        cfg_hid(&t, rd);
        cfg_endpoint(&t, 0x81, TUSB_XFER_INTERRUPT, 8, 1);
        cfg_endpoint(&t, 0x01, TUSB_XFER_INTERRUPT, 8, 1);
        cfg_endpoint(&t, 0x02, TUSB_XFER_INTERRUPT, 8, 1);
        cfg_close(&t);
        assert(commit_raw(NULL, 0, &t) == 1);
        cfg_open(&t); // bNumEndpoints says two, there is one
        cfg_interface(&t, 0, 0, 2, 0x03);
        cfg_hid(&t, rd);
        cfg_endpoint(&t, 0x81, TUSB_XFER_INTERRUPT, 8, 1);
        cfg_close(&t);
        expect_refused(commit_raw(NULL, 0, &t));
        cfg_open(&t); // bNumEndpoints says one, there are two
        cfg_interface(&t, 0, 0, 1, 0x03);
        cfg_hid(&t, rd);
        cfg_endpoint(&t, 0x81, TUSB_XFER_INTERRUPT, 8, 1);
        cfg_endpoint(&t, 0x01, TUSB_XFER_INTERRUPT, 8, 1);
        cfg_close(&t);
        expect_refused(commit_raw(NULL, 0, &t));
        // an IN and an OUT endpoint are fine, with the same number
        cfg_open(&t);
        cfg_interface(&t, 0, 0, 2, 0x03);
        cfg_hid(&t, rd);
        cfg_endpoint(&t, 0x81, TUSB_XFER_INTERRUPT, 8, 1);
        cfg_endpoint(&t, 0x01, TUSB_XFER_INTERRUPT, 8, 1);
        cfg_close(&t);
        assert(commit_raw(NULL, 0, &t) == 1);
    }

    // endpoint numbers and sizes (USB 2.0 9.6.6)
    {
        tcfg_t t;
        static const uint8_t bad_address[] = {0x80, 0x00, 0x90, 0xF1, 0x41, 0x71};
        for (size_t i = 0; i < sizeof(bad_address); i++) {
            cfg_open(&t);
            cfg_interface(&t, 0, 0, 1, 0x03);
            cfg_hid(&t, rd);
            cfg_endpoint(&t, bad_address[i], TUSB_XFER_INTERRUPT, 8, 1);
            cfg_close(&t);
            expect_refused(commit_raw(NULL, 0, &t));
        }
        // the highest number the controller has is fine, the next one is not (when there is one)
        cfg_open(&t);
        cfg_interface(&t, 0, 0, 1, 0x03);
        cfg_hid(&t, rd);
        cfg_endpoint(&t, 0x80 | MIMUSB_MAX_ENDPOINT_NUMBER, TUSB_XFER_INTERRUPT, 8, 1);
        cfg_close(&t);
        assert(commit_raw(NULL, 0, &t) == 1);
#if MIMUSB_MAX_ENDPOINT_NUMBER < 15
        cfg_open(&t);
        cfg_interface(&t, 0, 0, 1, 0x03);
        cfg_hid(&t, rd);
        cfg_endpoint(&t, 0x80 | (MIMUSB_MAX_ENDPOINT_NUMBER + 1), TUSB_XFER_INTERRUPT, 8, 1);
        cfg_close(&t);
        expect_refused(commit_raw(NULL, 0, &t));
        cfg_open(&t); // an OUT endpoint above it
        cfg_interface(&t, 0, 0, 2, 0x03);
        cfg_hid(&t, rd);
        cfg_endpoint(&t, 0x81, TUSB_XFER_INTERRUPT, 8, 1);
        cfg_endpoint(&t, MIMUSB_MAX_ENDPOINT_NUMBER + 1, TUSB_XFER_INTERRUPT, 8, 1);
        cfg_close(&t);
        expect_refused(commit_raw(NULL, 0, &t));
#endif
        // the same address twice: in one interface, and in two
        cfg_open(&t);
        cfg_interface(&t, 0, 0, 2, 0x03);
        cfg_hid(&t, rd);
        cfg_endpoint(&t, 0x81, TUSB_XFER_INTERRUPT, 8, 1);
        cfg_endpoint(&t, 0x81, TUSB_XFER_INTERRUPT, 8, 1);
        cfg_close(&t);
        expect_refused(commit_raw(NULL, 0, &t));
        cfg_open(&t);
        cfg_hid_interface(&t, 0, 1, rd);
        cfg_hid_interface(&t, 1, 1, rd);
        cfg_close(&t);
        expect_refused(commit_raw(NULL, 0, &t));
        // a bulk endpoint is the board's too (the sizes of USB 2.0 5.8.3: 8 to 64 at full speed);
        // control, isochronous and a type with other bits set are not
        cfg_open(&t);
        cfg_interface(&t, 0, 0, 1, 0x03);
        cfg_hid(&t, rd);
        cfg_endpoint(&t, 0x81, TUSB_XFER_BULK, 8, 0);
        cfg_close(&t);
        assert(commit_raw(NULL, 0, &t) == 1);
        static const uint8_t bad_type[] = {TUSB_XFER_CONTROL, TUSB_XFER_ISOCHRONOUS, 0x07};
        for (size_t i = 0; i < sizeof(bad_type); i++) {
            cfg_open(&t);
            cfg_interface(&t, 0, 0, 1, 0x03);
            cfg_hid(&t, rd);
            cfg_endpoint(&t, 0x81, bad_type[i], 8, 1);
            cfg_close(&t);
            expect_refused(commit_raw(NULL, 0, &t));
        }
        // packet sizes: 1 to what the controller takes
        static const uint16_t bad_size[] = {0, MIMUSB_MAX_ENDPOINT_BYTES + 1, 0x0800 | 8};
        for (size_t i = 0; i < sizeof(bad_size) / sizeof(bad_size[0]); i++) {
            cfg_open(&t);
            cfg_interface(&t, 0, 0, 1, 0x03);
            cfg_hid(&t, rd);
            cfg_endpoint(&t, 0x81, TUSB_XFER_INTERRUPT, bad_size[i], 1);
            cfg_close(&t);
            expect_refused(commit_raw(NULL, 0, &t));
        }
        cfg_open(&t);
        cfg_interface(&t, 0, 0, 1, 0x03);
        cfg_hid(&t, rd);
        cfg_endpoint(&t, 0x81, TUSB_XFER_INTERRUPT, 1, 1);
        cfg_close(&t);
        assert(commit_raw(NULL, 0, &t) == 1);
        cfg_open(&t);
        cfg_interface(&t, 0, 0, 1, 0x03);
        cfg_hid(&t, rd);
        cfg_endpoint(&t, 0x81, TUSB_XFER_INTERRUPT, MIMUSB_MAX_ENDPOINT_BYTES,
                     255 > 16 && !TUD_OPT_HIGH_SPEED ? 255 : 16);
        cfg_close(&t);
        assert(commit_raw(NULL, 0, &t) == 1);
        // bInterval 0 is no interval
        cfg_open(&t);
        cfg_interface(&t, 0, 0, 1, 0x03);
        cfg_hid(&t, rd);
        cfg_endpoint(&t, 0x81, TUSB_XFER_INTERRUPT, 8, 0);
        cfg_close(&t);
        expect_refused(commit_raw(NULL, 0, &t));
    }

    // a string is a complete descriptor of the length it announced, of type 3
    {
        tcfg_t t;
        cfg_open(&t);
        cfg_hid_interface(&t, 0, 1, rd);
        cfg_close(&t);
        uint8_t d[18];
        make_device(d);
        for (int mistake = 0; mistake < 3; mistake++) {
            uint8_t s[6] = {6, 3, 'a', 0, 'b', 0};
            assert(desc_reset() == 1);
            assert(send_blob(MIMUSB_DESC_DEVICE, 0, 0, d, 18) == 1);
            assert(send_blob(MIMUSB_DESC_CONFIGURATION, 0, 0, t.b, t.n) == 1);
            stage_endpoints(&t);
            if (mistake == 0)
                s[0] = 8; // bLength is not the length
            if (mistake == 1)
                s[1] = 2;       // not a string descriptor
            if (mistake == 2) { // only part of it
                assert(send_blob_partial(MIMUSB_DESC_STRING, 1, 0x0409, s, 6, 4, 0) == 1);
            } else {
                assert(send_blob(MIMUSB_DESC_STRING, 1, 0x0409, s, 6) == 1);
            }
            expect_refused(commit_staged());
        }
    }
}

// A COMMIT that attaches is refused (NAK STATE) by a board that is attached, whatever it carries:
// the target has enumerated the device, and new descriptors under it would never be seen. The
// refusal changes nothing, and a detach makes the same COMMIT work.
// The BOS descriptor (USB 3.2 9.6.2) of a device with a USB 2.0 Extension capability (USB 2.0 Link
// Power Management ECN) and the Microsoft OS 2.0 platform capability ("Microsoft OS 2.0 Descriptors
// Specification"): 5 + 7 + 28 bytes.
static size_t bos_sample(uint8_t* b) {
    static const uint8_t bos[] = {
        5, 0x0F, 40, 0, 2, // header: wTotalLength 40, two capabilities
        7, 0x10, 0x02, 0x02, 0, 0, 0, // USB 2.0 Extension: LPM supported
        28, 0x10, 0x05, 0, // platform capability
        0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C, 0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A,
        0x9F,              // MS OS 2.0 platform capability UUID
        0x00, 0x00, 0x03, 0x06, // dwWindowsVersion 0x06030000
        0x42, 0x00,        // wMSOSDescriptorSetTotalLength
        0x01, 0x00};       // bMS_VendorCode, bAltEnumCode
    memcpy(b, bos, sizeof(bos));
    return sizeof(bos);
}

// A device with the given bcdUSB and one interface, and a BOS of `bos_len` bytes of which `have`
// are sent (bos NULL: none), staged and committed. Returns the answer to the COMMIT.
static int commit_bos(uint16_t bcd_usb, const uint8_t* bos, size_t bos_len, size_t have) {
    tcfg_t c;
    cfg_open(&c);
    cfg_hid_interface(&c, 0, 1, sizeof(k_desc_a));
    cfg_close(&c);
    assert(desc_reset() == 1);
    uint8_t d[18];
    make_device(d);
    put_u16(&d[2], bcd_usb);
    assert(send_blob(MIMUSB_DESC_DEVICE, 0, 0, d, 18) == 1);
    assert(send_blob(MIMUSB_DESC_CONFIGURATION, 0, 0, c.b, c.n) == 1);
    if (bos != NULL) {
        const int r = send_blob_partial(MIMUSB_DESC_BOS, 0, 0, bos, bos_len, have, 0);
        if (r != 1)
            return r;
    }
    stage_endpoints(&c);
    return commit_staged();
}

// The BOS descriptor: served as it was given, only to a device whose bcdUSB asks for one; a device
// without one has none (TinyUSB then stalls the request); every malformed one is refused at COMMIT.
static void test_bos(void) {
    uint8_t good[64];
    const size_t n = bos_sample(good);
    assert(n == 40);

    // None given: none served, as before.
    assert(commit_bos(0x0200, NULL, 0, 0) == 1);
    assert(tud_descriptor_bos_cb() == NULL);

    // Given: served byte for byte, and replaced by the next configuration, which has none.
    assert(commit_bos(0x0210, good, n, n) == 1);
    assert(tud_descriptor_bos_cb() != NULL && memcmp(tud_descriptor_bos_cb(), good, n) == 0);
    assert(commit_bos(0x0201, good, n, n) == 1); // 0x0201 is the lowest that asks for it
    assert(tud_descriptor_bos_cb() != NULL);
    assert(commit_bos(0x0200, NULL, 0, 0) == 1);
    assert(tud_descriptor_bos_cb() == NULL);
    // A detach forgets it too.
    assert(commit_bos(0x0210, good, n, n) == 1);
    const uint8_t detach[] = {1};
    assert(command(MIMUSB_T_COMMIT, detach, sizeof(detach)) == 1);
    assert(tud_descriptor_bos_cb() == NULL);
    // A new DESC_RESET forgets a staged one.
    assert(desc_reset() == 1);
    assert(send_blob(MIMUSB_DESC_BOS, 0, 0, good, n) == 1);
    assert(commit_bos(0x0210, NULL, 0, 0) == 1);
    assert(tud_descriptor_bos_cb() == NULL);

    // A header with no capability is a BOS.
    static const uint8_t empty[5] = {5, 0x0F, 5, 0, 0};
    assert(commit_bos(0x0210, empty, sizeof(empty), sizeof(empty)) == 1);
    assert(tud_descriptor_bos_cb() != NULL && tud_descriptor_bos_cb()[2] == 5);

    // Refused at COMMIT: a bcdUSB below 0x0201, and each way the descriptor does not hold together.
    expect_nak(commit_bos(0x0200, good, n, n), MIMUSB_NAK_DESCRIPTOR);
    expect_nak(commit_bos(0x0110, good, n, n), MIMUSB_NAK_DESCRIPTOR);
    expect_nak(commit_bos(0x0210, good, n, n - 1), MIMUSB_NAK_DESCRIPTOR); // only in part
    uint8_t bad[64];
    const struct {
        size_t at;
        uint8_t value;
        const char* what;
    } mistakes[] = {
        {0, 4, "bLength of the header"},     {0, 6, "bLength of the header"},
        {1, 0x0E, "type of the header"},     {1, 0x10, "type of the header"},
        {2, 41, "wTotalLength longer"},      {2, 39, "wTotalLength shorter"},
        {4, 1, "fewer capabilities"},        {4, 3, "more capabilities"},
        {5, 2, "capability bLength below 3"}, {5, 8, "capability bLength skips the next"},
        {6, 0x0F, "capability type"},        {12, 0, "capability bLength 0"},
        {12, 29, "capability runs past the end"}, {13, 0x11, "capability type"},
    };
    for (size_t i = 0; i < sizeof(mistakes) / sizeof(mistakes[0]); i++) {
        memcpy(bad, good, n);
        bad[mistakes[i].at] = mistakes[i].value;
        expect_nak(commit_bos(0x0210, bad, n, n), MIMUSB_NAK_DESCRIPTOR);
    }

    // DESC_BEGIN of the kind BOS: index, language and flags are zero, the length is 1 to the
    // board's limit, and the same announcement again is acknowledged again.
    assert(desc_reset() == 1);
    expect_nak(desc_begin_raw(MIMUSB_DESC_BOS, 1, 0, 40, 0), MIMUSB_NAK_PARAM);
    expect_nak(desc_begin_raw(MIMUSB_DESC_BOS, 0, 0x0409, 40, 0), MIMUSB_NAK_PARAM);
    expect_nak(desc_begin_raw(MIMUSB_DESC_BOS, 0, 0, 40, 1), MIMUSB_NAK_PARAM);
    expect_nak(desc_begin_raw(MIMUSB_DESC_BOS, 0, 0, 0, 0), MIMUSB_NAK_PARAM);
    expect_nak(desc_begin_raw(MIMUSB_DESC_BOS, 0, 0, MIMUSB_MAX_BOS_BYTES + 1, 0),
               MIMUSB_NAK_OVERFLOW);
    assert(desc_begin_raw(MIMUSB_DESC_BOS, 0, 0, MIMUSB_MAX_BOS_BYTES, 0) == 1);
    assert(desc_begin_raw(MIMUSB_DESC_BOS, 0, 0, MIMUSB_MAX_BOS_BYTES, 0) == 1);
    expect_nak(desc_begin_raw(MIMUSB_DESC_BOS, 0, 0, 40, 0), MIMUSB_NAK_STATE);
    // The bytes of one that was not announced are refused, and so is more than was announced.
    assert(desc_reset() == 1);
    expect_nak(desc_data_raw(MIMUSB_DESC_BOS, 0, 0, 0, good, 5), MIMUSB_NAK_STATE);
    assert(desc_begin_raw(MIMUSB_DESC_BOS, 0, 0, 5, 0) == 1);
    expect_nak(desc_data_raw(MIMUSB_DESC_BOS, 0, 0, 0, good, 6), MIMUSB_NAK_PARAM);

    // The largest one the board holds is taken (a header and capabilities that fill it).
    {
        uint8_t big[MIMUSB_MAX_BOS_BYTES];
        memset(big, 0, sizeof(big));
        big[0] = 5;
        big[1] = 0x0F;
        put_u16(&big[2], MIMUSB_MAX_BOS_BYTES);
        big[4] = 1;
        big[5] = (uint8_t)(MIMUSB_MAX_BOS_BYTES - 5); // one capability that fills the rest
        big[6] = 0x10;
        big[7] = 0x05;
        assert(MIMUSB_MAX_BOS_BYTES - 5 <= 255);
        assert(commit_bos(0x0300, big, sizeof(big), sizeof(big)) == 1);
        assert(memcmp(tud_descriptor_bos_cb(), big, sizeof(big)) == 0);
    }
}

static void test_commit_attached_refused(void) {
    device_config(0, "First");
    base_node();
    assert(commit() == 1);
    assert(g_up && mimusb_active_interfaces() == 1);
    uint8_t first[64];
    const uint16_t first_len = (uint16_t)(tud_descriptor_configuration_cb(0)[2] |
                                          (tud_descriptor_configuration_cb(0)[3] << 8));
    assert(first_len <= sizeof(first));
    memcpy(first, tud_descriptor_configuration_cb(0), first_len);
    const uint8_t first_device = tud_descriptor_device_cb()[8];

    // Another configuration is staged in full and committed with no detach in between: refused,
    // and nothing the target can see has changed.
    device_config(0, "Second");
    node_begin(0, 0, sizeof(k_desc_a), 4, 16, 1, "Keys");
    node_data(0, 0, k_desc_a, sizeof(k_desc_a));
    node_begin(1, 1, sizeof(k_desc_b), 1, 64, 0, "");
    node_data(1, 0, k_desc_b, sizeof(k_desc_b));
    assert(stage() == 1);
    const int connects = g_connects, disconnects = g_disconnects;
    const uint8_t attach[] = {0};
    expect_nak(command(MIMUSB_T_COMMIT, attach, sizeof(attach)), MIMUSB_NAK_STATE);
    expect_nak(command(MIMUSB_T_COMMIT, NULL, 0), MIMUSB_NAK_STATE);
    assert(g_connects == connects && g_disconnects == disconnects);
    assert(mimusb_active_interfaces() == 1 && mimusb_core_attached());
    assert(memcmp(tud_descriptor_configuration_cb(0), first, first_len) == 0);
    assert(tud_descriptor_device_cb()[8] == first_device);

    // After the detach the same COMMIT is taken, and the second configuration is the active one.
    const uint8_t detach[] = {1};
    assert(command(MIMUSB_T_COMMIT, detach, sizeof(detach)) == 1);
    assert(!mimusb_core_attached() && mimusb_active_interfaces() == 0);
    assert(command(MIMUSB_T_COMMIT, NULL, 0) == 1);
    assert(mimusb_core_attached() && mimusb_active_interfaces() == 2);
    assert(g_connects == connects + 1);
    // A detach of a board that is not attached is no error: it leaves the board off the bus.
    assert(command(MIMUSB_T_COMMIT, detach, sizeof(detach)) == 1);
    assert(command(MIMUSB_T_COMMIT, detach, sizeof(detach)) == 1);
}

static void test_errors(void) {
    // A COMMIT needs everything staged: nothing at all, then each part missing in turn.
    g_disconnects = 0;
    assert(desc_reset() == 1);
    expect_nak(commit_staged(), MIMUSB_NAK_DESCRIPTOR);
    assert(g_disconnects == 0);

    // an ANSWER rule whose bytes are only partly sent is refused (NAK STATE) and does not
    // disconnect; with all its bytes the same table is taken
    device_config(0, "x");
    base_node();
    assert(stage() == 1);
    {
        const trule_t a = rule_answer(0xC0, 0x42, k_desc_a, sizeof(k_desc_a));
        uint8_t p[32];
        assert(command(MIMUSB_T_RULE, p, rule_frame(0, &a, p)) == 1);
        assert(send_blob_partial(MIMUSB_DESC_ANSWER, 0, 0, k_desc_a, sizeof(k_desc_a), 10, 0) == 1);
        expect_nak(commit_staged(), MIMUSB_NAK_STATE);
        assert(g_disconnects == 0);
        assert(send_blob(MIMUSB_DESC_ANSWER, 0, 0, k_desc_a, sizeof(k_desc_a)) == 1);
        assert(commit_staged() == 1);
    }

    // A gap in the interface numbers is refused (the host library refuses it before the board
    // sees it; the board refuses it too).
    {
        device_config(0, "");
        node_begin(0, 0, sizeof(k_desc_a), 1, 8, 0, "");
        node_data(0, 0, k_desc_a, sizeof(k_desc_a));
        node_begin(1, 2, sizeof(k_desc_b), 1, 8, 0, "");
        node_data(1, 0, k_desc_b, sizeof(k_desc_b));
        expect_nak(commit(), MIMUSB_NAK_DESCRIPTOR);
    }

    // Endpoint frames: type 0x40 (endpoint 0) is refused, not queued.
    device_config(0, "");
    base_node();
    assert(commit() == 1);
    reset_io();
    const uint8_t a[] = {1, 0x02};
    put_frame(MIMUSB_T_EP_IN_BASE, a, sizeof(a)); // endpoint 0
    pump();
    assert(g_nsent == 0);
}

static void test_packing(void) {
    // two nodes share interface 0; reports of both reach one queue in order
    device_config(0, "");
    node_begin(0, 0, sizeof(k_desc_a), 1, 8, 0, "");
    node_data(0, 0, k_desc_a, sizeof(k_desc_a));
    node_begin(1, 0, sizeof(k_desc_b), 1, 8, 0, "");
    node_data(1, 0, k_desc_b, sizeof(k_desc_b));
    assert(commit() == 1);
    assert(mimusb_active_interfaces() == 1);
    const uint8_t* c = tud_descriptor_configuration_cb(0);
    assert((c[2] | (c[3] << 8)) == 9 + 25 && c[9 + 16] == sizeof(k_desc_a) + sizeof(k_desc_b));
    reset_io();
    const uint8_t a[] = {1, 1}, b[] = {2, 2};
    ep_in(1, a, 2); // both nodes are on interface 0, whose IN endpoint is 1
    ep_in(1, b, 2);
    ep_in(1, a, 2);
    pump();
    assert(g_nsent == 3 && g_sent[0].data[0] == 1 && g_sent[1].data[0] == 2 &&
           g_sent[2].data[0] == 1);
    assert(g_sent[0].iface == 0 && g_sent[1].iface == 0);
}

// ---- control transfers ----
static dcd_event_t setup_event(uint8_t bm, uint8_t request, uint16_t value, uint16_t index,
                               uint16_t length) {
    const uint8_t raw[8] = {bm,
                            request,
                            (uint8_t)value,
                            (uint8_t)(value >> 8),
                            (uint8_t)index,
                            (uint8_t)(index >> 8),
                            (uint8_t)length,
                            (uint8_t)(length >> 8)};
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
        if (t == MIMUSB_T_EVENT && p[0] == MIMUSB_EV_CONTROL_SETUP) {
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
    if (n)
        memcpy(&p[2], data, n);
    return command(MIMUSB_T_CONTROL_REPLY, p, 2 + n);
}

// What the board answers by itself when no rule holds a request, where the specifications fix the
// answer: GetStatus() to an interface (USB 2.0 9.4.5).
static void test_specified_answers(void) {
    device_config(0, "");
    node_begin(0, 0, sizeof(k_desc_a), 4, 16, 0, "");
    node_data(0, 0, k_desc_a, sizeof(k_desc_a));
    assert(commit() == 1);
    g_mounted = true;
    reset_io();

    // GetStatus() to the interface that exists: two bytes of zero, sent through the vendor
    // callback.
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
    for (int i = 0; i < 5; i++)
        assert(g_forwarded[i].setup_received.bmRequestType != 0xC1);
    g_mounted = false; // address state: a Request Error
    reset_io();
    setup(0x81, TUSB_REQ_GET_STATUS, 0, 0, 2);
    assert(g_nforwarded == 1 && g_forwarded[0].setup_received.bmRequestType == 0x81);
    g_mounted = true;

    // A rule that matches it is stronger: the request is held for the host.
    trule_t held = rule_for(0x81, TUSB_REQ_GET_STATUS, 100, MIMUSB_REPLY_STALL);
    assert(control_config(0, &held, 1) == 1);
    reset_io();
    setup(0x81, TUSB_REQ_GET_STATUS, 0, 0, 2);
    assert(g_nforwarded == 0);
    pump();
    size_t pos = 0;
    uint8_t token, flags, raw[8];
    assert(next_control_event(&pos, &token, &flags, raw) && (flags & MIMUSB_CONTROL_F_HELD) &&
           raw[0] == 0x81);
    assert(control_config(0, NULL, 0) == 1);
}

static void test_control(void) {
    device_config(0, "");
    node_begin(0, 0, sizeof(k_desc_a), 4, 16, 0, "");
    node_data(0, 0, k_desc_a, sizeof(k_desc_a));
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
    assert(raw[0] == 0x80 && raw[1] == TUSB_REQ_GET_DESCRIPTOR && raw[2] == 0x02 &&
           raw[3] == 0x03 && raw[4] == 0x09 && raw[5] == 0x04 && raw[6] == 255 && raw[7] == 0);
    assert(next_control_event(&pos, &token2, &flags, raw) && token2 != token &&
           flags == MIMUSB_CONTROL_F_FIXED);
    assert(g_nforwarded == 2);

    // Too many for the queue: the next one carries the loss.
    reset_io();
    for (int i = 0; i < 12; i++)
        setup(0x80, TUSB_REQ_GET_STATUS, 0, 0, 2);
    assert(g_nforwarded == 12);
    pump();
    pos = 0;
    int seen = 0;
    while (next_control_event(&pos, &token, &flags, raw))
        seen++;
    assert(seen == 7); // one slot of eight is the full mark
    reset_io();
    setup(0x80, TUSB_REQ_GET_STATUS, 0, 0, 2);
    pump();
    pos = 0;
    assert(next_control_event(&pos, &token, &flags, raw) && (flags & MIMUSB_CONTROL_F_LOST));

    // A host-to-device data stage longer than the board takes is never held; with the report
    // flag it is reported as too long.
    trule_t any_class = rule_for(0x20, 0, 50, MIMUSB_REPLY_STALL);
    any_class.bm_mask = 0x60; // any class request
    any_class.request_mask = 0;
    assert(control_config(0, &any_class, 1) == 1);
    reset_io();
    setup(0x21, T_HID_SET_REPORT, 0x0201, 0, MIMUSB_CONTROL_DATA_MAX + 1);
    pump();
    assert(g_nforwarded == 1 && g_out_len == 0);
    assert(control_config(1, &any_class, 1) == 1);
    reset_io();
    setup(0x21, T_HID_SET_REPORT, 0x0201, 0, MIMUSB_CONTROL_DATA_MAX + 1);
    pump();
    pos = 0;
    assert(g_nforwarded == 1 && next_control_event(&pos, &token, &flags, raw) &&
           flags == MIMUSB_CONTROL_F_TOO_LONG);

    // An answer with data (device to host).
    trule_t vendor_in = rule_for(0xC0, 0x42, 100, MIMUSB_REPLY_STALL);
    assert(control_config(0, &vendor_in, 1) == 1);
    reset_io();
    setup(0xC0, 0x42, 5, 6, 64);
    assert(g_nforwarded == 0); // held
    pump();
    pos = 0;
    assert(next_control_event(&pos, &token, &flags, raw) && flags == MIMUSB_CONTROL_F_HELD);
    assert(raw[0] == 0xC0 && raw[1] == 0x42);
    expect_nak(reply((uint8_t)(token + 1), MIMUSB_REPLY_ANSWER, NULL, 0), MIMUSB_NAK_STATE);
    const uint8_t answer[4] = {1, 2, 3, 4};
    assert(reply(token, MIMUSB_REPLY_ANSWER, answer, sizeof(answer)) == 1);
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
    expect_nak(reply(token, MIMUSB_REPLY_ANSWER, answer, 1), MIMUSB_NAK_STATE); // too late

    // A longer answer than wLength is sent whole, as a device that overruns the host's length does.
    reset_io();
    setup(0xC0, 0x42, 0, 0, 2);
    pump();
    pos = 0;
    assert(next_control_event(&pos, &token, &flags, raw));
    assert(reply(token, MIMUSB_REPLY_ANSWER, answer, sizeof(answer)) == 1);
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
    assert(reply(token, MIMUSB_REPLY_ANSWER, answer, 2) == 1);
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
    assert(reply(token, MIMUSB_REPLY_ANSWER, NULL, 0) == 1);
    assert(g_nforwarded == 1 &&
           g_forwarded[0].setup_received.bmRequestType == 0x40); // direction dropped
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
    assert(reply(token, MIMUSB_REPLY_STALL, NULL, 0) == 1);
    {
        const tusb_control_request_t req = g_forwarded[0].setup_received;
        assert(!tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &req));
    }

    // No reply in time: the rule's action (here a stall).
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

    trule_t std_leave = rule_for(0x80, TUSB_REQ_GET_DESCRIPTOR, 30, MIMUSB_REPLY_STALL);
    assert(control_config(0, &std_leave, 1) == 1);
    reset_io();
    setup(0x80, TUSB_REQ_GET_DESCRIPTOR, 0x0300, 0, 4);
    pump();
    pos = 0;
    assert(next_control_event(&pos, &token, &flags, raw));
    assert(reply(token, MIMUSB_REPLY_LEAVE, NULL, 0) == 1);
    assert(g_nforwarded == 1 && g_forwarded[0].setup_received.bmRequestType == 0x80); // as it came
    assert(g_forwarded[0].setup_received.bRequest == TUSB_REQ_GET_DESCRIPTOR);

    // A request without a data stage: ANSWER is a plain acknowledgement.
    trule_t no_data = rule_for(0x00, TUSB_REQ_SET_FEATURE, 30, MIMUSB_REPLY_STALL);
    assert(control_config(0, &no_data, 1) == 1);
    reset_io();
    setup(0x00, TUSB_REQ_SET_FEATURE, 1, 0, 0);
    pump();
    pos = 0;
    assert(next_control_event(&pos, &token, &flags, raw));
    assert(reply(token, MIMUSB_REPLY_ANSWER, NULL, 0) == 1);
    {
        const tusb_control_request_t req = g_forwarded[0].setup_received;
        assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &req));
        assert(g_xfer.kind == 2);
        assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_ACK, &req));
    }

    // SET_ADDRESS and SET_CONFIGURATION are never held, whatever the rules say.
    trule_t everything = rule_for(0, 0, 100, MIMUSB_REPLY_STALL);
    everything.bm_mask = 0; // every request
    everything.request_mask = 0;
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
    expect_nak(reply(token, MIMUSB_REPLY_ANSWER, NULL, 0), MIMUSB_NAK_STATE);
    {
        dcd_event_t bus = {.rhport = 0, .event_id = DCD_EVENT_BUS_RESET};
        __wrap_dcd_event_handler(&bus, true);
    }
    g_now += 1000;
    pump();
    assert(g_nforwarded == 1 &&
           g_forwarded[0].event_id == DCD_EVENT_BUS_RESET); // nothing else went through

    reset_io();
    setup(0x80, TUSB_REQ_GET_DESCRIPTOR, 0x0100, 0, 18);
    assert(control_config(0, NULL, 0) == 1); // the transfer is given to the stack as it came
    assert(g_nforwarded == 1 && g_forwarded[0].setup_received.bmRequestType == 0x80);
    reset_io();
    setup(0x80, TUSB_REQ_GET_DESCRIPTOR, 0x0100, 0, 18);
    assert(g_nforwarded == 1); // no rules any more

    // Bad rules are refused and change nothing.
    trule_t bad = rule_for(0xC0, 0x42, 0, MIMUSB_REPLY_STALL);
    expect_nak(rule_stage(0, &bad), MIMUSB_NAK_PARAM); // no time
    // USB 2.0 9.2.6.1: 5 seconds is the most any command may take.
    assert(MIMUSB_CONTROL_TIMEOUT_MAX == 5000);
    bad = rule_for(0xC0, 0x42, MIMUSB_CONTROL_TIMEOUT_MAX, MIMUSB_REPLY_STALL);
    assert(control_config(0, &bad, 1) == 1);
    assert(control_config(0, NULL, 0) == 1);
    bad = rule_for(0xC0, 0x42, MIMUSB_CONTROL_TIMEOUT_MAX + 1, MIMUSB_REPLY_STALL);
    expect_nak(rule_stage(0, &bad), MIMUSB_NAK_PARAM);
    bad = rule_for(0xC0, 0x42, 10, MIMUSB_REPLY_ANSWER); // only STALL and DROP follow a timeout
    expect_nak(rule_stage(0, &bad), MIMUSB_NAK_PARAM);
    bad = rule_for(0xC0, 0x42, 10, MIMUSB_REPLY_LEAVE);
    expect_nak(rule_stage(0, &bad), MIMUSB_NAK_PARAM);
    bad = rule_for(0xC0, 0x42, 10, MIMUSB_REPLY_STALL);
    bad.value_value = 1; // outside its (zero) mask
    expect_nak(rule_stage(0, &bad), MIMUSB_NAK_PARAM);
    expect_nak(rules_apply(2, 0), MIMUSB_NAK_PARAM); // a reserved flag
    expect_nak(rules_apply(0, 1), MIMUSB_NAK_PARAM); // more rules than are staged
    {
        uint8_t p[1] = {0}; // a rule without its fields
        expect_nak(command(MIMUSB_T_RULE, p, 1), MIMUSB_NAK_PARAM);
        uint8_t q[2] = {0, MIMUSB_CONTROL_RULES + 1};
        expect_nak(command(MIMUSB_T_RULES_APPLY, q, 2), MIMUSB_NAK_PARAM);
    }
    reset_io();
    setup(0xC0, 0x42, 0, 0, 2);
    assert(g_nforwarded == 1); // still no rules

    // Host to device with a data stage: the data is shown and the host gives the verdict.
    mimusb_control_set_single_context(true);
    trule_t out_rule = rule_for(0x21, T_HID_SET_REPORT, 100, MIMUSB_REPLY_STALL);
    assert(control_config(0, &out_rule, 1) == 1);
    reset_io();
    setup(0x21, T_HID_SET_REPORT, 0x0302, 0, 3);
    pump();
    pos = 0;
    assert(next_control_event(&pos, &token, &flags, raw) && flags == MIMUSB_CONTROL_F_HELD);
    assert(reply(token, MIMUSB_REPLY_ANSWER, NULL, 0) == 1);
    {
        const tusb_control_request_t req = g_forwarded[0].setup_received;
        assert(req.bmRequestType == 0x41); // retyped as vendor, direction and recipient kept
        assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &req));
        assert(g_xfer.kind == 1 && g_xfer.length == 3);
        const uint8_t sent[3] = {9, 8, 7};
        memcpy(g_xfer.buffer, sent, 3);
        // The host refuses it.
        reset_io();
        uint8_t v[2] = {token, MIMUSB_REPLY_STALL};
        put_cmd(MIMUSB_T_CONTROL_REPLY, v, 2);
        assert(!tud_vendor_control_xfer_cb(0, CONTROL_STAGE_DATA, &req));
        size_t q = 0;
        uint8_t t, p[2048];
        uint16_t n;
        bool saw_data = false;
        while ((t = next_out(&q, p, &n)) != 0) {
            if (t == MIMUSB_T_EVENT && p[0] == MIMUSB_EV_CONTROL_DATA) {
                assert(n == 1 + 1 + 3 && p[1] == token && memcmp(&p[2], sent, 3) == 0);
                saw_data = true;
            }
        }
        assert(saw_data);
        assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_ACK, &req));
    }

    // No verdict in time: the rule's action again (a stall).
    reset_io();
    setup(0x21, T_HID_SET_REPORT, 0x0302, 0, 3);
    pump();
    pos = 0;
    assert(next_control_event(&pos, &token, &flags, raw));
    assert(reply(token, MIMUSB_REPLY_ANSWER, NULL, 0) == 1);
    {
        const tusb_control_request_t req = g_forwarded[0].setup_received;
        assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &req));
        g_now_step = 10; // each look at the clock is 10 ms later
        assert(!tud_vendor_control_xfer_cb(0, CONTROL_STAGE_DATA, &req));
        g_now_step = 0;
    }

    // Leaving the control routing: back to nothing.
    assert(control_config(0, NULL, 0) == 1);
    mimusb_control_set_single_context(false);
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
    for (int i = 0; i < 64; i++)
        buf[i] = (uint8_t)i;
    reset_io();

    // The size comes with the descriptor: 8, and the device descriptor reads 8.
    g_ep0_size = 8;
    device_config(0, "");
    node_begin(0, 0, sizeof(k_desc_a), 1, 8, 0, "");
    node_data(0, 0, k_desc_a, sizeof(k_desc_a));
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
    device_config(0, "");
    node_begin(0, 0, sizeof(k_desc_a), 1, 8, 0, "");
    node_data(0, 0, k_desc_a, sizeof(k_desc_a));
    assert(commit() == 1);
    ep0_begin(0x80, 255);
    assert(__wrap_dcd_edpt_xfer(0, 0x80, buf, 64));
    assert(g_ndcd == 1 && g_dcd[0].length == 64);
    ep0_done(64);
    assert(ep0_whole() == 64 && g_deferred == NULL);
}

// Every endpoint number the controller has is used: interfaces 0..n-1, each with its own IN
// endpoint, and each endpoint frame reaches the endpoint of its interface.
static void test_all_interfaces(void) {
    const uint8_t count = MIMUSB_MAX_ENDPOINT_NUMBER;
    g_depth = 2; // 15 queues of 3 slots fit the pool
    device_config(0, "");
    for (uint8_t i = 0; i < count; i++) {
        node_begin(i, i, sizeof(k_desc_a), 1, 8, 0, "");
        node_data(i, 0, k_desc_a, sizeof(k_desc_a));
    }
    assert(commit() == 1);
    assert(mimusb_active_interfaces() == count);
    reset_io();
    for (uint8_t i = 0; i < count; i++) {
        const mimusb_ep_info_t* e = mimusb_active_endpoint((uint8_t)(0x81 + i));
        assert(e != NULL && e->kind == MIMUSB_EP_GENERIC && e->interface == i);
        const uint8_t r[2] = {(uint8_t)(i + 1), (uint8_t)(0x10 + i)};
        ep_in((uint8_t)(1 + i), r, sizeof(r));
    }
    pump();
    assert(g_nsent == count);
    for (uint8_t i = 0; i < count; i++)
        assert(g_sent[i].iface == i && g_sent[i].data[0] == i + 1 && g_sent[i].data[1] == 0x10 + i);
    assert(mimusb_active_endpoint((uint8_t)(0x81 + count)) == NULL);
    // A detach forgets them all again.
    uint8_t detach[1] = {1};
    assert(command(MIMUSB_T_COMMIT, detach, 1) == 1);
    assert(mimusb_active_interfaces() == 0);
    for (uint8_t i = 0; i < count; i++)
        assert(mimusb_active_endpoint((uint8_t)(0x81 + i)) == NULL);
    g_depth = 15;
}

// EP_CONFIG: the depth of the queue of an endpoint of the staged configuration. Nothing is made
// up: zero, a bad address and a bad policy are refused; an endpoint the configuration lacks, or no
// configuration, is a state error.
static void test_ep_config(void) {
    device_config(0, "");
    base_node();
    assert(desc_reset() == 1);
    expect_nak(ep_config(0x81, 4, MIMUSB_EP_FULL_DROP_NEWEST), MIMUSB_NAK_STATE); // nothing staged
    assert(stage() == 1);
    assert(ep_config(0x81, 4, MIMUSB_EP_FULL_DROP_NEWEST) == 1);
    assert(ep_config(0x81, 4, MIMUSB_EP_FULL_DROP_NEWEST) == 1); // again: the same
    expect_nak(ep_config(0x81, 0, MIMUSB_EP_FULL_DROP_NEWEST), MIMUSB_NAK_PARAM);
    expect_nak(ep_config(0x81, 4, 0), MIMUSB_NAK_PARAM);
    expect_nak(ep_config(0x81, 4, 4), MIMUSB_NAK_PARAM);
    assert(ep_config(0x81, 4, MIMUSB_EP_FULL_DROP_OLDEST) == 1); // all three policies are taken
    assert(ep_config(0x81, 4, MIMUSB_EP_FULL_WAIT) == 1);
    expect_nak(ep_config(0x01, 4, MIMUSB_EP_FULL_DROP_NEWEST),
               MIMUSB_NAK_UNSUPPORTED); // an OUT address
    expect_nak(ep_config(0x00, 4, MIMUSB_EP_FULL_DROP_NEWEST), MIMUSB_NAK_PARAM);
    expect_nak(ep_config(0x91, 4, MIMUSB_EP_FULL_DROP_NEWEST), MIMUSB_NAK_PARAM);
    expect_nak(ep_config(0x8F + 1, 4, MIMUSB_EP_FULL_DROP_NEWEST), MIMUSB_NAK_PARAM);
    expect_nak(ep_config(0x83, 4, MIMUSB_EP_FULL_DROP_NEWEST), MIMUSB_NAK_STATE); // not in it
    expect_nak(ep_config(0x81, 255, MIMUSB_EP_FULL_DROP_NEWEST),
               MIMUSB_NAK_OVERFLOW); // deeper than the queue
    assert(commit_staged() == 1);
}

// The endpoint frames of this step: type 0x40 is refused and an unknown endpoint drops the frame.
static void test_ep_frames(void) {
    device_config(0, "");
    base_node();
    assert(commit() == 1);
    reset_io();
    const uint8_t a[] = {1, 0x02};
    put_frame(MIMUSB_T_EP_IN_BASE, a, sizeof(a)); // endpoint 0
    ep_in(1, a, sizeof(a));
    ep_in(2, a, sizeof(a)); // an endpoint that is none of the configuration's
    pump();
    assert(g_nsent == 1 && g_sent[0].iface == 0);
}

// Nothing is filled in for the host: a zero where a value is required is refused at COMMIT.
static void test_nothing_is_filled_in(void) {
    reset_io();
    g_bcd_usb = 0;
    device_config(0, "");
    base_node();
    expect_nak(commit(), MIMUSB_NAK_DESCRIPTOR);
    g_bcd_usb = 0x0200;
    g_bm_attributes = 0;
    expect_nak(commit(), MIMUSB_NAK_DESCRIPTOR);
    g_bm_attributes = 0x20; // bit 7 clear
    expect_nak(commit(), MIMUSB_NAK_DESCRIPTOR);
    g_bm_attributes = 0xA0;
    g_config_value = 0;
    expect_nak(commit(), MIMUSB_NAK_DESCRIPTOR);
    g_config_value = 1;
    node_begin(0, 0, sizeof(k_desc_a), 0, 8, 0, ""); // bInterval 0
    node_data(0, 0, k_desc_a, sizeof(k_desc_a));
    expect_nak(commit(), MIMUSB_NAK_DESCRIPTOR);
    node_begin(0, 0, sizeof(k_desc_a), 1, 0, 0, ""); // packet size 0
    node_data(0, 0, k_desc_a, sizeof(k_desc_a));
    expect_nak(commit(), MIMUSB_NAK_DESCRIPTOR);
    base_node();
    assert(commit() == 1);
}

#if TUD_OPT_HIGH_SPEED
// A high-speed build: the rules of USB 2.0 that only a high-speed device has. The tests above
// assume full-speed packet sizes, so this build runs these alone.
static void test_high_speed(void) {
    assert(MIMUSB_MAX_ENDPOINT_BYTES == 1024);
    // 9.6.1: the control endpoint of a high-speed device is 64 bytes, and only that.
    device_config(0, "");
    base_node();
    g_ep0_size = 8;
    expect_nak(commit(), MIMUSB_NAK_DESCRIPTOR);
    g_ep0_size = 32;
    expect_nak(commit(), MIMUSB_NAK_DESCRIPTOR);
    g_ep0_size = 64;
    assert(commit() == 1);
    // 9.6.6: bInterval of an interrupt endpoint is an exponent, 1 to 16.
    static const uint8_t bad[] = {0, 17, 255};
    for (size_t i = 0; i < sizeof(bad); i++) {
        node_begin(0, 0, sizeof(k_desc_a), bad[i], 8, 0, "");
        node_data(0, 0, k_desc_a, sizeof(k_desc_a));
        expect_nak(commit(), MIMUSB_NAK_DESCRIPTOR);
    }
    node_begin(0, 0, sizeof(k_desc_a), 16, 1024, 0, ""); // the largest interval and endpoint packet
    node_data(0, 0, k_desc_a, sizeof(k_desc_a));
    assert(commit() == 1);
    node_begin(0, 0, sizeof(k_desc_a), 16, 1025, 0, "");
    node_data(0, 0, k_desc_a, sizeof(k_desc_a));
    expect_nak(commit(), MIMUSB_NAK_DESCRIPTOR);
}
#endif

// ---- the generic driver: interfaces of any class, bulk and interrupt endpoints ----
static const usbd_class_driver_t* gen_driver(void) {
    uint8_t count = 0;
    const usbd_class_driver_t* d = usbd_app_driver_get_cb(&count);
    assert(count == 1 && d != NULL && d->open && d->xfer_cb && d->reset && d->control_xfer_cb);
    return d;
}

// What the stack's SET_CONFIGURATION does (process_set_config in src/device/usbd.c): the drivers'
// reset, then open for each interface in descriptor order, an association (skipped) first and a
// whole group with one call; the number of bytes open returns is where the walk goes on. Every
// interface is the generic driver's, whatever its class.
static void set_config(void) {
    const usbd_class_driver_t* d = gen_driver();
    memset(g_ep, 0, sizeof(g_ep));
    d->reset(0);
    const uint8_t* cfg = tud_descriptor_configuration_cb(0);
    const uint16_t total = (uint16_t)(cfg[2] | (cfg[3] << 8));
    uint16_t pos = 9;
    while (pos < total) {
        if (cfg[pos + 1] == TUSB_DESC_INTERFACE_ASSOCIATION) {
            pos = (uint16_t)(pos + cfg[pos]);
            continue;
        }
        assert(cfg[pos + 1] == TUSB_DESC_INTERFACE);
        const uint16_t used =
            d->open(0, (const tusb_desc_interface_t*)&cfg[pos], (uint16_t)(total - pos));
        assert(used >= 9 && used <= total - pos);
        pos = (uint16_t)(pos + used);
    }
}

// The controller finishes the transfer on an IN endpoint, or receives a packet on an OUT endpoint.
static void complete_in(uint8_t number) {
    const uint8_t address = (uint8_t)(0x80 | number);
    assert(EPSTATE(address).busy);
    EPSTATE(address).busy = false;
    gen_driver()->xfer_cb(0, address, XFER_RESULT_SUCCESS, EPSTATE(address).length);
}
static void out_packet(uint8_t number, const uint8_t* d, uint16_t n) {
    assert(EPSTATE(number).busy && n <= EPSTATE(number).length);
    memcpy(EPSTATE(number).buffer, d, n);
    EPSTATE(number).busy = false;
    gen_driver()->xfer_cb(0, number, XFER_RESULT_SUCCESS, n);
}
// The target sets or clears the Halt feature of an endpoint (USB 2.0 9.4.1, 9.4.5): the stack
// stalls or clears the endpoint, then tells the driver.
static void target_halt(uint8_t address, bool set) {
    if (set)
        usbd_edpt_stall(0, address);
    else
        usbd_edpt_clear_stall(0, address);
    tusb_control_request_t r = {0};
    r.bmRequestType = 0x02; // host to device, standard, endpoint
    r.bRequest = set ? TUSB_REQ_SET_FEATURE : TUSB_REQ_CLEAR_FEATURE;
    r.wValue = TUSB_REQ_FEATURE_EDPT_HALT;
    r.wIndex = address;
    assert(gen_driver()->control_xfer_cb(0, CONTROL_STAGE_SETUP, &r));
}
// The sum of what the events of a kind say for an endpoint (their count field), or the number of
// them when the kind has none (-1 for "last value").
static unsigned event_sum(uint8_t kind, uint8_t address) {
    size_t pos = 0;
    uint8_t p[2048];
    uint16_t n;
    uint8_t t;
    unsigned sum = 0;
    while ((t = next_out(&pos, p, &n)) != 0) {
        if (t != MIMUSB_T_EVENT || p[0] != kind || p[1] != address)
            continue;
        sum += kind == MIMUSB_EV_EP_HALT ? 1u : (unsigned)(p[2] | (p[3] << 8));
    }
    return sum;
}
static unsigned out_frames(uint8_t number, const uint8_t* want, uint16_t want_len) {
    size_t pos = 0;
    uint8_t p[2048];
    uint16_t n;
    uint8_t t;
    unsigned found = 0;
    while ((t = next_out(&pos, p, &n)) != 0) {
        if (t == (uint8_t)(MIMUSB_T_EP_OUT_BASE | number)) {
            assert(n == want_len && memcmp(p, want, n) == 0);
            found++;
        }
    }
    return found;
}

static void cfg_association(tcfg_t* c, uint8_t first, uint8_t count, uint8_t cls) {
    const uint8_t d[8] = {8, TUSB_DESC_INTERFACE_ASSOCIATION, first, count, cls, 0, 0, 0};
    memcpy(c->b + c->n, d, 8);
    c->n += 8;
}
static void cfg_class_descriptor(tcfg_t* c, uint8_t type, uint8_t length) {
    assert(length >= 2);
    c->b[c->n] = length;
    c->b[c->n + 1] = type;
    memset(&c->b[c->n + 2], 0x5A, (size_t)length - 2);
    c->n += length;
}

// Stages a device and the configuration, and the queues of its IN endpoints with a depth of 2 and
// the policy DROP_NEWEST (the tests that need others send EP_CONFIG again). Returns 1 or the first
// refusal.
static int stage_cfg(tcfg_t* c) {
    int r = desc_reset();
    if (r != 1)
        return r;
    uint8_t d[18];
    make_device(d);
    if ((r = send_blob(MIMUSB_DESC_DEVICE, 0, 0, d, 18)) != 1)
        return r;
    r = send_blob(MIMUSB_DESC_CONFIGURATION, 0, 0, c->b, c->n);
    return r;
}

// Interface 0: vendor class, bulk IN 1, bulk OUT 1, bulk IN 3, with a class descriptor of its own;
// interface 1: HID, interrupt IN 2 (the board runs it like the first one). Every build has the
// endpoint numbers 1 to 3.
static void vendor_config(tcfg_t* c) {
    cfg_open(c);
    cfg_interface(c, 0, 0, 3, 0xFF);
    cfg_class_descriptor(c, 0x24, 5);
    cfg_endpoint(c, 0x81, TUSB_XFER_BULK, 64, 0);
    cfg_endpoint(c, 0x01, TUSB_XFER_BULK, 64, 0);
    cfg_endpoint(c, 0x83, TUSB_XFER_BULK, 64, 0);
    cfg_hid_interface(c, 1, 2, sizeof(k_desc_a));
    cfg_close(c);
}

static void vendor_commit(uint8_t depth1, uint8_t policy1, uint8_t depth3, uint8_t policy3) {
    tcfg_t c;
    vendor_config(&c);
    assert(stage_cfg(&c) == 1);
    assert(ep_config(0x81, depth1, policy1) == 1);
    assert(ep_config(0x82, 2, MIMUSB_EP_FULL_DROP_NEWEST) == 1);
    assert(ep_config(0x83, depth3, policy3) == 1);
    assert(commit_staged() == 1);
    set_config();
    reset_io();
}

static void start_loss_counting(void) {
    const uint8_t on[3] = {1, 0, 0};
    assert(command(MIMUSB_T_LOSS_CONFIG, on, sizeof(on)) == 1);
}

// ---- control rules, one action at a time ----
// A request that a rule answered: the stack's vendor callback is given it, and the data stage it
// starts is checked against `want` (cut to wLength unless it is longer, as the stack does).
// Returns false when the callback refused the setup stage (a stall).
static bool serve(uint8_t bm, uint8_t request, uint16_t value, uint16_t index, uint16_t length,
                  const uint8_t* want, size_t n) {
    reset_io();
    setup(bm, request, value, index, length);
    assert(g_nforwarded == 1); // every rule answers on the spot: nothing is held for the host
    const tusb_control_request_t req = g_forwarded[0].setup_received;
    g_xfer.kind = 0;
    if (!tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &req))
        return false;
    assert(g_xfer.kind == 1 || g_xfer.kind == 2);
    assert(g_xfer.length == n && (n == 0 || memcmp(g_xfer.copy, want, n) == 0));
    assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_DATA, &req));
    assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_ACK, &req));
    return true;
}

static void test_rules(void) {
    g_auto = true;
    vendor_commit(2, MIMUSB_EP_FULL_DROP_NEWEST, 2, MIMUSB_EP_FULL_DROP_NEWEST);
    static const uint8_t A[3] = {0xA1, 0xA2, 0xA3}, B[2] = {0xB1, 0xB2};
    static const uint8_t R1[4] = {0x01, 1, 2, 3}, R2[4] = {0x02, 4, 5, 6};

    // ANSWER: its bytes, cut to wLength by the stack, and in the order of the table the first match
    // decides.
    {
        trule_t t[3];
        t[0] = rule_answer(0xC0, 0x50, A, sizeof(A));
        t[1] = rule_answer(0xC0, 0x50, B, sizeof(B)); // never reached: the rule above matches first
        t[2] = rule_answer(0xC0, 0x51, B, sizeof(B));
        assert(control_config(0, t, 3) == 1);
        assert(serve(0xC0, 0x50, 0, 0, 8, A, sizeof(A)));
        assert(serve(0xC0, 0x51, 0, 0, 8, B, sizeof(B)));
        // A request no rule matches is stalled by the stack: it goes to the stack as it came.
        reset_io();
        setup(0xC0, 0x52, 0, 0, 8);
        assert(g_nforwarded == 1 && g_forwarded[0].setup_received.bmRequestType == 0xC0);
        const tusb_control_request_t none = g_forwarded[0].setup_received;
        assert(!tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &none));

        // RULE_DATA: fewer bytes at once, not more than the rule was staged with, and only for an
        // ANSWER rule of the active table.
        assert(rule_data(2, A, 1) == 1);
        assert(serve(0xC0, 0x51, 0, 0, 8, A, 1));
        expect_nak(rule_data(2, A, 3), MIMUSB_NAK_OVERFLOW);
        expect_nak(rule_data(3, A, 1), MIMUSB_NAK_PARAM); // no such rule
        // It is not a rule of the Node: a STALL rule has no bytes.
        trule_t u[2] = {rule_of(MIMUSB_RULE_STALL, 0xC0, 0x53), rule_answer(0xC0, 0x54, A, 3)};
        assert(control_config(0, u, 2) == 1);
        expect_nak(rule_data(0, A, 1), MIMUSB_NAK_PARAM);
        // BUSY while the rule is being sent (RULE_DATA and RULES_APPLY both), fine after.
        reset_io();
        setup(0xC0, 0x54, 0, 0, 8);
        const tusb_control_request_t req = g_forwarded[0].setup_received;
        assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &req));
        expect_nak(rule_data(1, B, 2), MIMUSB_NAK_BUSY);
        expect_nak(rules_apply(0, 2), MIMUSB_NAK_BUSY);
        assert(g_xfer.length == 3 && memcmp(g_xfer.buffer, A, 3) == 0); // still the old bytes
        assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_DATA, &req));
        assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_ACK, &req));
        assert(rule_data(1, B, 2) == 1);
        assert(serve(0xC0, 0x54, 0, 0, 8, B, 2));
        // RULES_APPLY: the table is the first `count` rules; the rest is forgotten.
        assert(rules_apply(0, 1) == 1);
        reset_io();
        setup(0xC0, 0x54, 0, 0, 8);
        const tusb_control_request_t gone = g_forwarded[0].setup_received;
        assert(!tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &gone));
    }

    // The capacity of an ANSWER rule: the answer memory it reserves for as long as the table is
    // active, apart from the length it starts with. RULE_DATA sets any length up to the capacity, a
    // shortened answer grows again, and a table staged again carries its own capacity.
    {
        enum { CAP = 6 };
        static const uint8_t L[CAP + 1] = {1, 2, 3, 4, 5, 6, 7};
        trule_t t[2];
        t[0] = rule_answer(0xC0, 0x56, L, 2);
        t[0].capacity = CAP; // starts with 2 bytes, may hold 6
        t[1] = rule_answer(0xC0, 0x57, B, sizeof(B));
        assert(control_config(0, t, 2) == 1);
        assert(serve(0xC0, 0x56, 0, 0, 8, L, 2));
        assert(rule_data(0, L, CAP) == 1); // longer than it started, within the capacity
        assert(serve(0xC0, 0x56, 0, 0, 8, L, CAP));
        assert(rule_data(0, L, 1) == 1); // shorter ...
        assert(serve(0xC0, 0x56, 0, 0, 8, L, 1));
        assert(rule_data(0, L, 5) == 1); // ... and longer again
        assert(serve(0xC0, 0x56, 0, 0, 8, L, 5));
        assert(rule_data(0, NULL, 0) == 1); // none at all, and back
        assert(serve(0xC0, 0x56, 0, 0, 8, NULL, 0));
        assert(rule_data(0, L, CAP) == 1);
        expect_nak(rule_data(0, L, CAP + 1), MIMUSB_NAK_OVERFLOW); // past the capacity
        assert(serve(0xC0, 0x56, 0, 0, 8, L, CAP)); // the refused call changed nothing
        // The next rule's bytes were not touched by the growth of this one.
        assert(serve(0xC0, 0x57, 0, 0, 8, B, sizeof(B)));

        // A length above the capacity is not a rule.
        trule_t bad = t[0];
        bad.length = bad.capacity + 1;
        uint8_t p[32];
        expect_nak(command(MIMUSB_T_RULE, p, rule_frame(0, &bad, p)), MIMUSB_NAK_PARAM);

        // The capacity is what the answer memory is counted in, not the length.
        trule_t big = rule_answer(0xC0, 0x58, L, 1);
        big.capacity = MIMUSB_ANSWER_BYTES;
        assert(control_config(0, &big, 1) == 1);
        trule_t two[2] = {big, rule_answer(0xC0, 0x59, B, 1)};
        expect_nak(control_config(0, two, 2), MIMUSB_NAK_OVERFLOW);
        assert(control_config(0, t, 2) == 1);
    }

    // STALL: the stack stalls the data or status stage.
    {
        trule_t t = rule_of(MIMUSB_RULE_STALL, 0xC0, 0x55);
        assert(control_config(0, &t, 1) == 1);
        assert(!serve(0xC0, 0x55, 0, 0, 8, NULL, 0));
    }

    // ANSWER_LAST_SENT without a filter: the last transfer of the endpoint, none before the first
    // (a stall), the newer one after, copied when the request starts so that a transfer written
    // while the answer goes out cannot change it (the torn read).
    {
        trule_t t = rule_last(0xC0, 0x56, 0x81, 0, 0);
        assert(control_config(0, &t, 1) == 1);
        assert(!serve(0xC0, 0x56, 0, 0, 8, NULL, 0)); // nothing was sent yet
        ep_in(1, R1, sizeof(R1));
        pump();
        assert(serve(0xC0, 0x56, 0, 0, 8, R1, sizeof(R1)));
        // The writer is the generic driver's side of the endpoint; the reader is this request. A
        // new transfer between the setup stage and the end of the data stage must not show up.
        reset_io();
        setup(0xC0, 0x56, 0, 0, 8);
        const tusb_control_request_t req = g_forwarded[0].setup_received;
        assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &req));
        assert(g_xfer.length == sizeof(R1) && memcmp(g_xfer.buffer, R1, sizeof(R1)) == 0);
        ep_in(1, R2, sizeof(R2));
        pump();
        assert(memcmp(g_xfer.buffer, R1, sizeof(R1)) == 0); // the answer in flight is unchanged
        assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_DATA, &req));
        assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_ACK, &req));
        assert(serve(0xC0, 0x56, 0, 0, 8, R2, sizeof(R2))); // the next one has the newer
        // A table applied again with this rule unchanged keeps what it held; a new configuration
        // (COMMIT) starts it empty.
        assert(rules_apply(0, 1) == 1);
        assert(serve(0xC0, 0x56, 0, 0, 8, R2, sizeof(R2)));
        vendor_commit(2, MIMUSB_EP_FULL_DROP_NEWEST, 2, MIMUSB_EP_FULL_DROP_NEWEST);
        assert(control_config(0, &t, 1) == 1);
        assert(!serve(0xC0, 0x56, 0, 0, 8, NULL, 0));
    }

    // ANSWER_LAST_SENT with the filter: only a transfer that starts with the byte counts, before
    // and after the first one.
    {
        trule_t t[2] = {rule_last(0xC0, 0x57, 0x81, 1, 0x01), rule_last(0xC0, 0x58, 0x81, 1, 0x02)};
        assert(control_config(0, t, 2) == 1);
        assert(!serve(0xC0, 0x57, 0, 0, 8, NULL, 0));
        assert(!serve(0xC0, 0x58, 0, 0, 8, NULL, 0));
        ep_in(1, R2, sizeof(R2)); // starts with 0x02: the second rule's
        pump();
        assert(!serve(0xC0, 0x57, 0, 0, 8, NULL, 0)); // still nothing for 0x01
        assert(serve(0xC0, 0x58, 0, 0, 8, R2, sizeof(R2)));
        ep_in(1, R1, sizeof(R1));
        pump();
        assert(serve(0xC0, 0x57, 0, 0, 8, R1, sizeof(R1)));
        assert(serve(0xC0, 0x58, 0, 0, 8, R2, sizeof(R2))); // each keeps its own
        // The rule must name an IN endpoint of the configuration.
        trule_t bad = rule_last(0xC0, 0x59, 0x84, 0, 0);
        assert(rule_stage(0, &bad) == 1);
        expect_nak(rules_apply(0, 1), MIMUSB_NAK_STATE);
        assert(rules_apply(0, 0) == 1);
    }

    // ACCEPT without a data stage: acknowledged and reported with the ACCEPTED flag, nothing held.
    {
        trule_t t = rule_of(MIMUSB_RULE_ACCEPT, 0x40, 0x5A);
        assert(control_config(0, &t, 1) == 1);
        assert(serve(0x40, 0x5A, 7, 0, 0, NULL, 0));
        pump();
        size_t pos = 0;
        uint8_t token, flags, raw[8];
        assert(next_control_event(&pos, &token, &flags, raw) && flags == MIMUSB_CONTROL_F_ACCEPTED &&
               raw[1] == 0x5A && raw[2] == 7);
        // With a data stage the bytes come as CONTROL_DATA of the same token.
        reset_io();
        setup(0x40, 0x5A, 8, 0, 3);
        const tusb_control_request_t req = g_forwarded[0].setup_received;
        assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &req));
        assert(g_xfer.kind == 1 && g_xfer.length == 3);
        memcpy(g_xfer.buffer, A, 3);
        assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_DATA, &req));
        assert(tud_vendor_control_xfer_cb(0, CONTROL_STAGE_ACK, &req));
        pump();
        size_t q = 0;
        uint8_t type, p[2048];
        uint16_t n;
        bool setup_seen = false, data_seen = false;
        uint8_t setup_token = 0;
        while ((type = next_out(&q, p, &n)) != 0) {
            if (type != MIMUSB_T_EVENT)
                continue;
            if (p[0] == MIMUSB_EV_CONTROL_SETUP) {
                assert(p[2] == MIMUSB_CONTROL_F_ACCEPTED);
                setup_seen = true;
                setup_token = p[1];
            }
            if (p[0] == MIMUSB_EV_CONTROL_DATA) {
                assert(n == 1 + 1 + 3 && memcmp(&p[2], A, 3) == 0 && p[1] == setup_token);
                data_seen = true;
            }
        }
        assert(setup_seen && data_seen);
    }

    // ASK is the held request of test_control; with a rule in front of it the rule wins.
    {
        trule_t t[2] = {rule_answer(0xC0, 0x5B, A, sizeof(A)), rule_for(0xC0, 0x5B, 100, MIMUSB_REPLY_STALL)};
        assert(control_config(0, t, 2) == 1);
        assert(serve(0xC0, 0x5B, 0, 0, 8, A, sizeof(A)));
    }

    // The limits: the rule count, the answer memory and the order the table is written in.
    {
        trule_t t = rule_of(MIMUSB_RULE_STALL, 0xC0, 0x60);
        for (int i = 0; i < MIMUSB_CONTROL_RULES; i++)
            assert(rule_stage((uint8_t)i, &t) == 1);
        expect_nak(rule_stage(MIMUSB_CONTROL_RULES, &t), MIMUSB_NAK_OVERFLOW);
        assert(rules_apply(0, MIMUSB_CONTROL_RULES) == 1);
        assert(rule_stage(0, &t) == 1);
        expect_nak(rule_stage(2, &t), MIMUSB_NAK_PARAM); // not in order
        trule_t huge = rule_answer(0xC0, 0x61, NULL, MIMUSB_ANSWER_BYTES + 1);
        expect_nak(rule_stage(0, &huge), MIMUSB_NAK_OVERFLOW);
        assert(control_config(0, NULL, 0) == 1);
    }
}

static void test_generic(void) {
    g_auto = false; // the transfers below complete when the test says so
    static const uint8_t A[4] = {0xA0, 1, 2, 3}, B[4] = {0xB0, 1, 2, 3}, C[4] = {0xC0, 1, 2, 3},
                         D[4] = {0xD0, 1, 2, 3}, E[4] = {0xE0, 1, 2, 3}, F[4] = {0xF0, 1, 2, 3};

    // A configuration of any class is committed; the mapping of interfaces, HID instances and
    // endpoints is the walk's, and the generic driver takes the vendor interface in one open call
    // whose size is the interface and its endpoints (the HID interface is not its business).
    {
        tcfg_t c;
        vendor_config(&c);
        assert(stage_cfg(&c) == 1);
        // every IN endpoint needs its EP_CONFIG, the HID interface's too
        expect_nak(commit_staged(), MIMUSB_NAK_STATE);
        assert(ep_config(0x81, 2, MIMUSB_EP_FULL_DROP_NEWEST) == 1);
        expect_nak(commit_staged(), MIMUSB_NAK_STATE); // 0x82 and 0x83 still have none
        assert(ep_config(0x83, 2, MIMUSB_EP_FULL_DROP_NEWEST) == 1);
        expect_nak(commit_staged(), MIMUSB_NAK_STATE); // 0x82 still has none
        assert(ep_config(0x82, 2, MIMUSB_EP_FULL_DROP_NEWEST) == 1);
        assert(commit_staged() == 1);
        assert(mimusb_active_interfaces() == 2);
        const mimusb_ep_info_t* e = mimusb_active_endpoint(0x81);
        assert(e && e->kind == MIMUSB_EP_GENERIC && e->type == TUSB_XFER_BULK && e->packet_size == 64 &&
               e->interface == 0);
        e = mimusb_active_endpoint(0x01);
        assert(e && e->kind == MIMUSB_EP_GENERIC);
        e = mimusb_active_endpoint(0x82);
        assert(e && e->kind == MIMUSB_EP_GENERIC && e->type == TUSB_XFER_INTERRUPT &&
               e->interface == 1);
        assert(mimusb_active_endpoint(0x02) == NULL && mimusb_active_endpoint(0x84) == NULL);
        assert(mimusb_active_generic_span(0) == 9 + 5 + 3 * 7 &&
               mimusb_active_generic_span(1) == 9 + 9 + 7);
        // the open call
        const uint8_t* cfg = mimusb_active_config_bytes();
        memset(g_ep, 0, sizeof(g_ep));
        gen_driver()->reset(0); // the stack resets its drivers before it opens them
        assert(gen_driver()->open(0, (const tusb_desc_interface_t*)&cfg[9], 100) == 9 + 5 + 3 * 7);
        assert(EPSTATE(0x81).opened && EPSTATE(0x01).opened && EPSTATE(0x83).opened &&
               !EPSTATE(0x82).opened); // the interface's own endpoints only
        assert(EPSTATE(0x01).busy && EPSTATE(0x01).xfers == 1 && EPSTATE(0x01).length == 64);
        // the HID interface (class 0x03) is the driver's like any other
        assert(gen_driver()->open(0, (const tusb_desc_interface_t*)&cfg[9 + 35], 100) == 9 + 9 + 7);
        assert(EPSTATE(0x82).opened);
        // too little room for the interface and its endpoints
        memset(g_ep, 0, sizeof(g_ep));
        assert(gen_driver()->open(0, (const tusb_desc_interface_t*)&cfg[9], 20) == 0);
    }

    // DROP_NEWEST: depth 2 waiting, and one on the bus. Frames go to the controller in order, one
    // at a time.
    {
        vendor_commit(2, MIMUSB_EP_FULL_DROP_NEWEST, 2, MIMUSB_EP_FULL_DROP_NEWEST);
        start_loss_counting();
        reset_io();
        ep_in(1, A, 4);
        pump();
        assert(EPSTATE(0x81).busy && EPSTATE(0x81).xfers == 1 && EPSTATE(0x81).length == 4 &&
               memcmp(EPSTATE(0x81).buffer, A, 4) == 0);
        ep_in(1, B, 4);
        ep_in(1, C, 4);
        ep_in(1, D, 4); // the third that waits: dropped
        pump();
        assert(event_sum(MIMUSB_EV_TRANSFER_LOST, 0x81) == 1);
        assert(EPSTATE(0x81).xfers == 1); // one at a time
        complete_in(1);
        assert(EPSTATE(0x81).xfers == 2 && memcmp(EPSTATE(0x81).buffer, B, 4) == 0);
        complete_in(1);
        assert(EPSTATE(0x81).xfers == 3 && memcmp(EPSTATE(0x81).buffer, C, 4) == 0);
        complete_in(1);
        assert(!EPSTATE(0x81).busy && EPSTATE(0x81).xfers == 3);
        // the other endpoint is not affected
        ep_in(3, F, 4);
        pump();
        assert(EPSTATE(0x83).busy && memcmp(EPSTATE(0x83).buffer, F, 4) == 0);
    }

    // DROP_OLDEST: the oldest that is not on the bus goes.
    {
        vendor_commit(2, MIMUSB_EP_FULL_DROP_OLDEST, 2, MIMUSB_EP_FULL_DROP_NEWEST);
        start_loss_counting();
        reset_io();
        ep_in(1, A, 4);
        pump(); // on the bus
        ep_in(1, B, 4);
        ep_in(1, C, 4);
        ep_in(1, D, 4);
        ep_in(1, E, 4);
        ep_in(1, F, 4); // the ring is full: this one is dropped when it arrives
        pump();         // and the board drops B and C: two waiting are left
        assert(event_sum(MIMUSB_EV_TRANSFER_LOST, 0x81) == 3);
        assert(memcmp(EPSTATE(0x81).buffer, A, 4) == 0); // what is on the bus stays untouched
        complete_in(1);
        assert(memcmp(EPSTATE(0x81).buffer, D, 4) == 0);
        complete_in(1);
        assert(memcmp(EPSTATE(0x81).buffer, E, 4) == 0);
        complete_in(1);
        assert(!EPSTATE(0x81).busy && EPSTATE(0x81).xfers == 3);
    }

    // DROP_OLDEST with a target that does not take the transfer on the bus: however many
    // transfers arrive and are dropped behind it, the bytes on the bus stay what they were.
    {
        vendor_commit(2, MIMUSB_EP_FULL_DROP_OLDEST, 2, MIMUSB_EP_FULL_DROP_NEWEST);
        reset_io();
        ep_in(1, A, 4);
        pump(); // on the bus
        for (int round = 0; round < 8; round++) {
            uint8_t later[4] = {(uint8_t)(0xE0 + round), 1, 2, 3};
            ep_in(1, later, 4);
            pump();
            assert(EPSTATE(0x81).busy && EPSTATE(0x81).xfers == 1 && EPSTATE(0x81).length == 4 &&
                   memcmp(EPSTATE(0x81).buffer, A, 4) == 0);
        }
        // The newest two are what waits (rounds 6 and 7).
        complete_in(1);
        assert(EPSTATE(0x81).length == 4 && EPSTATE(0x81).buffer[0] == 0xE6);
        complete_in(1);
        assert(EPSTATE(0x81).length == 4 && EPSTATE(0x81).buffer[0] == 0xE7);
        complete_in(1);
        assert(!EPSTATE(0x81).busy);
    }

    // WAIT: nothing is dropped that the host may send (it counts), and the board reports every
    // transfer that completed.
    {
        vendor_commit(2, MIMUSB_EP_FULL_WAIT, 2, MIMUSB_EP_FULL_DROP_NEWEST);
        reset_io();
        ep_in(1, A, 4);
        pump(); // A is on the bus
        ep_in(1, B, 4);
        ep_in(1, C, 4);
        pump();
        complete_in(1);
        complete_in(1);
        pump();
        assert(event_sum(MIMUSB_EV_EP_DONE, 0x81) == 2);
        complete_in(1);
        reset_io();
        pump();
        assert(event_sum(MIMUSB_EV_EP_DONE, 0x81) == 1);
        // an endpoint that does not wait reports no completions
        reset_io();
        ep_in(3, A, 4);
        pump();
        complete_in(3);
        pump();
        assert(event_sum(MIMUSB_EV_EP_DONE, 0x83) == 0);
    }

    // A zero-length transfer is a zero-length packet (USB 2.0 5.8.3); a full-size one is one
    // transfer; a frame longer than the largest transfer is not accepted.
    {
        vendor_commit(2, MIMUSB_EP_FULL_DROP_NEWEST, 2, MIMUSB_EP_FULL_DROP_NEWEST);
        reset_io();
        ep_in(1, NULL, 0);
        pump();
        assert(EPSTATE(0x81).busy && EPSTATE(0x81).length == 0 && EPSTATE(0x81).xfers == 1);
        complete_in(1);
        uint8_t big[MIMUSB_MAX_TRANSFER_BYTES];
        for (size_t i = 0; i < sizeof(big); i++)
            big[i] = (uint8_t)(i * 3);
        ep_in(1, big, sizeof(big));
        pump();
        assert(EPSTATE(0x81).length == sizeof(big) && memcmp(EPSTATE(0x81).buffer, big, sizeof(big)) == 0);
        complete_in(1);
        uint8_t over[MIMUSB_MAX_TRANSFER_BYTES + 1] = {0};
        ep_in(1, over, sizeof(over));
        pump();
        assert(!EPSTATE(0x81).busy && EPSTATE(0x81).xfers == 2);
        // the HID interface's endpoint takes an empty frame as well
        ep_in(2, NULL, 0);
        pump();
        assert(EPSTATE(0x82).busy && EPSTATE(0x82).length == 0);
        // an IN endpoint that is not the configuration's drops its frames
        const int before = g_nsent;
        ep_in(5, A, 4);
        pump();
        assert(EPSTATE(0x85).xfers == 0 && g_nsent == before);
    }

    // OUT: a packet goes to the host as it came and the endpoint is armed again after it.
    {
        vendor_commit(2, MIMUSB_EP_FULL_DROP_NEWEST, 2, MIMUSB_EP_FULL_DROP_NEWEST);
        assert(EPSTATE(0x01).busy && EPSTATE(0x01).xfers == 1);
        const uint8_t pkt[5] = {9, 8, 7, 6, 5};
        out_packet(1, pkt, sizeof(pkt));
        assert(out_frames(1, pkt, sizeof(pkt)) == 1);
        assert(EPSTATE(0x01).busy && EPSTATE(0x01).xfers == 2);
        out_packet(1, pkt, 0); // a zero-length packet comes through too
        assert(EPSTATE(0x01).xfers == 3);
    }

    // The Halt feature: set by the board (EP_STALL) or by the target (USB 2.0 9.4.1, 9.4.5).
    {
        vendor_commit(2, MIMUSB_EP_FULL_WAIT, 2, MIMUSB_EP_FULL_DROP_NEWEST);
        start_loss_counting();
        reset_io();
        ep_in(1, A, 4);
        pump();
        ep_in(1, B, 4);
        pump(); // A on the bus, B waits
        const uint8_t st[2] = {0x81, 1}, cl[2] = {0x81, 0};
        assert(command(MIMUSB_T_EP_STALL, st, 2) == 1);
        assert(EPSTATE(0x81).stalled);
        ep_in(1, C, 4);
        pump(); // queued while stalled, not started
        assert(EPSTATE(0x81).xfers == 1);
        assert(command(MIMUSB_T_EP_STALL, cl, 2) == 1);
        assert(!EPSTATE(0x81).stalled && EPSTATE(0x81).xfers == 2 &&
               memcmp(EPSTATE(0x81).buffer, C, 4) == 0); // what waited at the stall is gone
        complete_in(1);
        // the target halts the endpoint while a transfer is on the bus
        ep_in(1, D, 4);
        pump();
        assert(EPSTATE(0x81).busy && memcmp(EPSTATE(0x81).buffer, D, 4) == 0);
        ep_in(1, E, 4);
        pump();
        reset_io();
        target_halt(0x81, true);
        pump();
        assert(event_sum(MIMUSB_EV_EP_HALT, 0x81) == 1);
        assert(event_sum(MIMUSB_EV_TRANSFER_LOST, 0x81) == 1); // D was on the bus
        assert(EPSTATE(0x81).stalled && EPSTATE(0x81).xfers == 3);
        reset_io();
        target_halt(0x81, false);
        pump();
        assert(event_sum(MIMUSB_EV_EP_HALT, 0x81) == 1);
        assert(!EPSTATE(0x81).stalled && EPSTATE(0x81).xfers == 4 &&
               memcmp(EPSTATE(0x81).buffer, E, 4) == 0); // what waited goes on
        // the same on an OUT endpoint: not armed while halted, armed again when cleared
        const int armed = EPSTATE(0x01).xfers;
        target_halt(0x01, true);
        assert(EPSTATE(0x01).stalled);
        target_halt(0x01, false);
        assert(EPSTATE(0x01).busy && EPSTATE(0x01).xfers == armed + 1);
        // the stack's other requests to an endpoint are not the driver's
        tusb_control_request_t r = {0};
        r.bmRequestType = 0x82;
        r.bRequest = TUSB_REQ_GET_STATUS;
        r.wIndex = 0x81;
        assert(!gen_driver()->control_xfer_cb(0, CONTROL_STAGE_SETUP, &r));
        // and an endpoint that is not its own
        r.bmRequestType = 0x02;
        r.bRequest = TUSB_REQ_SET_FEATURE;
        r.wValue = TUSB_REQ_FEATURE_EDPT_HALT;
        r.wIndex = 0x84; // not in the configuration
        assert(!gen_driver()->control_xfer_cb(0, CONTROL_STAGE_SETUP, &r));
    }

    // EP_FLUSH and the refusals of EP_STALL / EP_FLUSH.
    {
        vendor_commit(2, MIMUSB_EP_FULL_WAIT, 2, MIMUSB_EP_FULL_DROP_NEWEST);
        reset_io();
        ep_in(1, A, 4);
        pump();
        ep_in(1, B, 4);
        ep_in(1, C, 4);
        pump();
        const uint8_t fl[1] = {0x81};
        assert(command(MIMUSB_T_EP_FLUSH, fl, 1) == 1);
        complete_in(1); // the one on the bus goes on; nothing follows
        assert(!EPSTATE(0x81).busy && EPSTATE(0x81).xfers == 1);
        reset_io();
        pump();
        assert(event_sum(MIMUSB_EV_EP_DONE, 0x81) == 0); // a flushed transfer is not counted
        const uint8_t out1[1] = {0x01}, hid[2] = {0x82, 1}, none[2] = {0x84, 1}, bad[2] = {0x81, 2};
        expect_nak(command(MIMUSB_T_EP_FLUSH, out1, 1), MIMUSB_NAK_PARAM);
        assert(command(MIMUSB_T_EP_STALL, hid, 2) == 1); // the HID interface's endpoint: the same
        assert(EPSTATE(0x82).stalled);
        const uint8_t hid_clear[2] = {0x82, 0};
        assert(command(MIMUSB_T_EP_STALL, hid_clear, 2) == 1 && !EPSTATE(0x82).stalled);
        expect_nak(command(MIMUSB_T_EP_STALL, none, 2), MIMUSB_NAK_STATE);
        expect_nak(command(MIMUSB_T_EP_STALL, bad, 2), MIMUSB_NAK_PARAM);
        const uint8_t zero[1] = {0x00}, dirbit[1] = {0x91};
        expect_nak(command(MIMUSB_T_EP_FLUSH, zero, 1), MIMUSB_NAK_PARAM);
        expect_nak(command(MIMUSB_T_EP_FLUSH, dirbit, 1), MIMUSB_NAK_PARAM);
        expect_nak(command(MIMUSB_T_EP_FLUSH, hid, 2), MIMUSB_NAK_PARAM); // the length
        // RESET drops what waits on every endpoint
        ep_in(1, A, 4);
        pump();
        complete_in(1);
        ep_in(1, A, 4);
        ep_in(1, B, 4);
        pump();
        assert(command(MIMUSB_T_RESET, NULL, 0) == 1);
        const int xfers = EPSTATE(0x81).xfers;
        complete_in(1);
        assert(EPSTATE(0x81).xfers == xfers && !EPSTATE(0x81).busy);
    }

    // EP_CONFIG of a generic endpoint: any of the three policies; deeper than the pool is
    // OVERFLOW; after a COMMIT a change takes effect at once if the queue cut at COMMIT holds it,
    // before the COMMIT it only stages.
    {
        tcfg_t c;
        vendor_config(&c);
        assert(stage_cfg(&c) == 1);
        assert(ep_config(0x81, 4, MIMUSB_EP_FULL_DROP_OLDEST) == 1);
        assert(ep_config(0x81, 4, MIMUSB_EP_FULL_WAIT) == 1);
        expect_nak(ep_config(0x81, MIMUSB_EP_POOL_SLOTS, MIMUSB_EP_FULL_DROP_NEWEST),
                   MIMUSB_NAK_OVERFLOW);
        expect_nak(ep_config(0x81, MIMUSB_EP_POOL_SLOTS / 2, MIMUSB_EP_FULL_DROP_OLDEST),
                   MIMUSB_NAK_OVERFLOW);
        expect_nak(ep_config(0x01, 4, MIMUSB_EP_FULL_DROP_NEWEST), MIMUSB_NAK_UNSUPPORTED); // OUT
        assert(ep_config(0x82, 4, MIMUSB_EP_FULL_WAIT) == 1); // the HID interface's: any policy
        assert(ep_config(0x82, 1, MIMUSB_EP_FULL_DROP_NEWEST) == 1); // 2 slots
        // together the queues must fit the pool: 0x81 takes depth + 1 slots, 0x83 takes 2 * depth + 1
        const int d1 = MIMUSB_EP_POOL_SLOTS - 11;
        assert(ep_config(0x81, (uint8_t)d1, MIMUSB_EP_FULL_DROP_NEWEST) == 1); // d1 + 1 slots
        assert(ep_config(0x83, 5, MIMUSB_EP_FULL_DROP_OLDEST) == 1);          // 11 slots
        expect_nak(commit_staged(), MIMUSB_NAK_OVERFLOW);
        assert(ep_config(0x83, 4, MIMUSB_EP_FULL_DROP_OLDEST) == 1); // 9 slots
        assert(ep_config(0x81, (uint8_t)d1, MIMUSB_EP_FULL_DROP_NEWEST) == 1);
        // 0x81: d1 + 1 = pool - 10 slots, 0x82: 2, 0x83: 9: together pool + 1: still too many
        expect_nak(commit_staged(), MIMUSB_NAK_OVERFLOW);
        assert(ep_config(0x81, (uint8_t)(d1 - 1), MIMUSB_EP_FULL_DROP_NEWEST) == 1); // exactly the pool
        assert(commit_staged() == 1);
        set_config();
        // live: 0x83 holds 9 slots, so DROP_NEWEST of depth 8 fits and depth 9 does not
        assert(ep_config(0x83, 8, MIMUSB_EP_FULL_DROP_NEWEST) == 1);
        expect_nak(ep_config(0x83, 9, MIMUSB_EP_FULL_DROP_NEWEST), MIMUSB_NAK_OVERFLOW);
        // staging a new configuration does not change a running queue
        assert(desc_reset() == 1);
        expect_nak(ep_config(0x83, 4, MIMUSB_EP_FULL_DROP_NEWEST), MIMUSB_NAK_STATE);
    }

    // A COMMIT the board refuses leaves the queues and endpoints of the active configuration as
    // they were. A board that is attached refuses every COMMIT that attaches, so this one is sent
    // as it is, with no detach before it, and is refused for that (the missing EP_CONFIG would
    // refuse it too).
    {
        vendor_commit(2, MIMUSB_EP_FULL_DROP_NEWEST, 2, MIMUSB_EP_FULL_DROP_NEWEST);
        reset_io();
        ep_in(1, A, 4);
        pump();
        tcfg_t c;
        vendor_config(&c);
        assert(stage_cfg(&c) == 1);
        expect_nak(command(MIMUSB_T_COMMIT, NULL, 0), MIMUSB_NAK_STATE);
        assert(EPSTATE(0x81).busy && memcmp(EPSTATE(0x81).buffer, A, 4) == 0);
        ep_in(1, B, 4);
        pump();
        complete_in(1);
        assert(memcmp(EPSTATE(0x81).buffer, B, 4) == 0);
    }

    // A bus reset or a new configuration closes the endpoints: what waited is dropped and the
    // endpoints are opened again by set_config.
    {
        vendor_commit(2, MIMUSB_EP_FULL_DROP_NEWEST, 2, MIMUSB_EP_FULL_DROP_NEWEST);
        ep_in(1, A, 4);
        ep_in(1, B, 4);
        pump();
        gen_driver()->reset(0);
        memset(g_ep, 0, sizeof(g_ep));
        ep_in(1, C, 4);
        pump();
        assert(EPSTATE(0x81).xfers == 0); // not open: nothing starts
        set_config(); // the configuration starts again: C, queued while closed, is dropped as well
        pump();
        assert(EPSTATE(0x81).xfers == 0 && !EPSTATE(0x81).busy);
        ep_in(1, D, 4);
        pump();
        assert(EPSTATE(0x81).xfers == 1 && memcmp(EPSTATE(0x81).buffer, D, 4) == 0);
    }

    // The walk: what the generic driver does not take is refused.
    {
        tcfg_t c;
        const uint16_t rd[1] = {sizeof(k_desc_a)};
        (void)rd;
        // a bulk endpoint of a packet size no controller has
        cfg_open(&c);
        cfg_interface(&c, 0, 0, 1, 0xFF);
        cfg_endpoint(&c, 0x81, TUSB_XFER_BULK, 48, 0);
        cfg_close(&c);
        assert(stage_cfg(&c) == 1);
        expect_nak(ep_config(0x81, 2, MIMUSB_EP_FULL_DROP_NEWEST), MIMUSB_NAK_STATE); // no walk
        expect_refused(commit_staged());
        // isochronous and control endpoints
        for (uint8_t type = TUSB_XFER_CONTROL; type <= TUSB_XFER_ISOCHRONOUS; type = (uint8_t)(type + 1)) {
            if (type == TUSB_XFER_BULK)
                continue;
            cfg_open(&c);
            cfg_interface(&c, 0, 0, 1, 0xFF);
            cfg_endpoint(&c, 0x81, type, 64, 1);
            cfg_close(&c);
            if (type == TUSB_XFER_INTERRUPT)
                continue;
            assert(stage_cfg(&c) == 1);
            expect_refused(commit_staged());
        }
        // an interrupt endpoint of a generic interface needs its bInterval
        cfg_open(&c);
        cfg_interface(&c, 0, 0, 1, 0xFF);
        cfg_endpoint(&c, 0x81, TUSB_XFER_INTERRUPT, 8, 0);
        cfg_close(&c);
        assert(stage_cfg(&c) == 1);
        expect_refused(commit_staged());
        // the same address twice, in one interface or two
        cfg_open(&c);
        cfg_interface(&c, 0, 0, 2, 0xFF);
        cfg_endpoint(&c, 0x81, TUSB_XFER_BULK, 64, 0);
        cfg_endpoint(&c, 0x81, TUSB_XFER_BULK, 64, 0);
        cfg_close(&c);
        assert(stage_cfg(&c) == 1);
        expect_refused(commit_staged());
        // an endpoint number the controller does not have
        cfg_open(&c);
        cfg_interface(&c, 0, 0, 1, 0xFF);
        cfg_endpoint(&c, (uint8_t)(0x80 | (MIMUSB_MAX_ENDPOINT_NUMBER + 1)), TUSB_XFER_BULK, 64, 0);
        cfg_close(&c);
        assert(stage_cfg(&c) == 1);
        expect_refused(commit_staged());
        // fewer endpoints than the interface announced
        cfg_open(&c);
        cfg_interface(&c, 0, 0, 2, 0xFF);
        cfg_endpoint(&c, 0x81, TUSB_XFER_BULK, 64, 0);
        cfg_close(&c);
        assert(stage_cfg(&c) == 1);
        expect_refused(commit_staged());
        // an interface with no endpoint at all is a valid interface (USB 2.0 9.6.5)
        cfg_open(&c);
        cfg_interface(&c, 0, 0, 0, 0xFF);
        cfg_close(&c);
        assert(stage_cfg(&c) == 1);
        assert(commit_staged() == 1);
        // a HID interface is no different: the board reads no HID descriptor, and a bulk endpoint
        // or none at all is as good as an interrupt one
        cfg_open(&c);
        cfg_interface(&c, 0, 0, 1, 0x03);
        cfg_endpoint(&c, 0x81, TUSB_XFER_INTERRUPT, 8, 1);
        cfg_close(&c);
        assert(commit_raw(NULL, 0, &c) == 1);
        cfg_open(&c);
        cfg_interface(&c, 0, 0, 1, 0x03);
        cfg_hid(&c, sizeof(k_desc_a));
        cfg_endpoint(&c, 0x81, TUSB_XFER_BULK, 64, 0);
        cfg_close(&c);
        assert(commit_raw(NULL, 0, &c) == 1);
        // an association with nothing after it, with the wrong first interface, and around HID
        cfg_open(&c);
        cfg_association(&c, 0, 2, 0xFF);
        cfg_interface(&c, 0, 0, 0, 0xFF);
        cfg_close(&c);
        assert(stage_cfg(&c) == 1);
        expect_refused(commit_staged());
        cfg_open(&c);
        cfg_association(&c, 1, 1, 0xFF);
        cfg_interface(&c, 0, 0, 0, 0xFF);
        cfg_close(&c);
        assert(stage_cfg(&c) == 1);
        expect_refused(commit_staged());
        cfg_open(&c);
        cfg_association(&c, 0, 2, 0xFF);
        cfg_interface(&c, 0, 0, 0, 0xFF);
        cfg_hid_interface(&c, 1, 1, sizeof(k_desc_a));
        cfg_close(&c);
        assert(commit_raw(NULL, 0, &c) == 1); // a HID interface is a member like any other
        // a class descriptor shorter than 2 bytes, or running past the blob
        cfg_open(&c);
        cfg_interface(&c, 0, 0, 0, 0xFF);
        c.b[c.n++] = 1;
        c.b[c.n++] = 0x24;
        cfg_close(&c);
        assert(stage_cfg(&c) == 1);
        expect_refused(commit_staged());
        cfg_open(&c);
        cfg_interface(&c, 0, 0, 0, 0xFF);
        c.b[c.n++] = 9;
        c.b[c.n++] = 0x24;
        cfg_close(&c);
        assert(stage_cfg(&c) == 1);
        expect_refused(commit_staged());
        // too many interfaces of any class
        cfg_open(&c);
        for (uint8_t i = 0; i <= MIMUSB_MAX_INTERFACES; i++)
            cfg_interface(&c, i, 0, 0, 0xFF);
        cfg_close(&c);
        assert(stage_cfg(&c) == 1);
        expect_refused(commit_staged());
        // MIMUSB_MAX_INTERFACES interfaces of any class are a valid configuration
        cfg_open(&c);
        for (uint8_t i = 0; i < MIMUSB_MAX_INTERFACES; i++)
            cfg_interface(&c, i, 0, 0, 0xFF);
        cfg_close(&c);
        assert(stage_cfg(&c) == 1);
        assert(commit_staged() == 1);
        assert(mimusb_active_interfaces() == MIMUSB_MAX_INTERFACES);
    }

#if MIMUSB_MAX_ENDPOINT_NUMBER >= 5
    // A function of two interfaces behind an association (a CDC-ACM shape: a control interface
    // with an interrupt IN endpoint and class descriptors, a data interface with two bulk
    // endpoints), then a HID interface: the stack calls open once for the group, which takes all
    // of it.
    {
        tcfg_t c;
        cfg_open(&c);
        cfg_association(&c, 0, 2, 0x02);
        cfg_interface(&c, 0, 0, 1, 0x02);
        cfg_class_descriptor(&c, 0x24, 5);
        cfg_class_descriptor(&c, 0x24, 4);
        cfg_endpoint(&c, 0x83, TUSB_XFER_INTERRUPT, 8, 16);
        cfg_interface(&c, 1, 0, 2, 0x0A);
        cfg_endpoint(&c, 0x84, TUSB_XFER_BULK, 64, 0);
        cfg_endpoint(&c, 0x02, TUSB_XFER_BULK, 64, 0);
        cfg_hid_interface(&c, 2, 5, sizeof(k_desc_a));
        cfg_close(&c);
        assert(stage_cfg(&c) == 1);
        assert(ep_config(0x83, 2, MIMUSB_EP_FULL_DROP_NEWEST) == 1);
        assert(ep_config(0x84, 2, MIMUSB_EP_FULL_DROP_NEWEST) == 1);
        assert(ep_config(0x85, 2, MIMUSB_EP_FULL_DROP_NEWEST) == 1);
        assert(commit_staged() == 1);
        // from the first interface descriptor (the association descriptor is stepped over by the stack)
        const uint16_t group = 9 + 5 + 4 + 7 + 9 + 7 + 7;
        assert(mimusb_active_generic_span(0) == group);
        assert(mimusb_active_generic_span(1) == 9 + 7 + 7); // the second one of the group on its own
        set_config();
        assert(EPSTATE(0x83).opened && EPSTATE(0x84).opened && EPSTATE(0x02).opened &&
               EPSTATE(0x85).opened); // the HID interface is opened by the same driver
        assert(EPSTATE(0x02).busy); // the OUT endpoint of the group is armed
        reset_io();
        ep_in(3, A, 4);
        ep_in(4, B, 4);
        pump();
        assert(EPSTATE(0x83).busy && EPSTATE(0x84).busy);
        ep_in(5, B, 4);
        pump();
        assert(EPSTATE(0x85).busy && mimusb_active_generic_span(2) == 9 + 9 + 7);
    }
#endif
}

#if TUD_OPT_HIGH_SPEED
// The packet sizes of USB 2.0 5.8.3 for a generic bulk endpoint at high speed: 512 only; and the
// interrupt interval of 9.6.6 is an exponent, 1 to 16.
static void test_generic_hs(void) {
    tcfg_t c;
    static const uint16_t sizes[] = {8, 64, 256, 513, 1024};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        cfg_open(&c);
        cfg_interface(&c, 0, 0, 1, 0xFF);
        cfg_endpoint(&c, 0x81, TUSB_XFER_BULK, sizes[i], 0);
        cfg_close(&c);
        assert(stage_cfg(&c) == 1);
        expect_refused(commit_staged());
    }
    cfg_open(&c);
    cfg_interface(&c, 0, 0, 2, 0xFF);
    cfg_endpoint(&c, 0x81, TUSB_XFER_BULK, 512, 0);
    cfg_endpoint(&c, 0x02, TUSB_XFER_INTERRUPT, 1024, 16);
    cfg_close(&c);
    assert(stage_cfg(&c) == 1);
    assert(ep_config(0x81, 2, MIMUSB_EP_FULL_DROP_NEWEST) == 1);
    assert(commit_staged() == 1);
    static const uint8_t bad[] = {0, 17};
    for (size_t i = 0; i < sizeof(bad); i++) {
        cfg_open(&c);
        cfg_interface(&c, 0, 0, 1, 0xFF);
        cfg_endpoint(&c, 0x81, TUSB_XFER_INTERRUPT, 8, bad[i]);
        cfg_close(&c);
        assert(stage_cfg(&c) == 1);
        expect_refused(commit_staged());
    }
    // The refusals left the board off the bus (the harness detaches before each COMMIT): the
    // configuration with the 512-byte endpoint goes on again.
    cfg_open(&c);
    cfg_interface(&c, 0, 0, 2, 0xFF);
    cfg_endpoint(&c, 0x81, TUSB_XFER_BULK, 512, 0);
    cfg_endpoint(&c, 0x02, TUSB_XFER_INTERRUPT, 1024, 16);
    cfg_close(&c);
    assert(stage_cfg(&c) == 1);
    assert(ep_config(0x81, 2, MIMUSB_EP_FULL_DROP_NEWEST) == 1);
    assert(commit_staged() == 1);
    // a slot holds the largest transfer, which is the largest packet of the endpoint: 512 here
    set_config();
    uint8_t big[512];
    memset(big, 0x77, sizeof(big));
    reset_io();
    g_auto = 0; // the target does not poll endpoint 1: the transfer stays started
    ep_in(1, big, sizeof(big));
    pump();
    assert(EPSTATE(0x81).busy && EPSTATE(0x81).length == 512);
    g_auto = 1;
}
#endif

int main(void) {
    printf("firmware core, endpoint numbers 1 to %d\n", MIMUSB_MAX_ENDPOINT_NUMBER);
    mimusb_core_init();
#if TUD_OPT_HIGH_SPEED
    test_high_speed();
    test_generic_hs();
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
    test_detach();
    test_serial_from_chip_id();
    test_desc_commands();
    test_commit_walk();
    test_commit_attached_refused();
    test_bos();
    test_errors();
    test_all_interfaces();
    test_ep_config();
    test_ep_frames();
    test_packing();
    test_specified_answers();
    test_control();
    test_ep0();
    test_nothing_is_filled_in();
    test_rules();
    test_generic();
    printf("firmware core OK\n");
    return 0;
}
