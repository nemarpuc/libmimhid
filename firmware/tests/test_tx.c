// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
// Host test of the frame queue (common/tx.c) against a fake UART that stays busy for a number of
// polls after every start, as a DMA channel does.
#include "config.h"
#include "hal.h"
#include "tx.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

// ---- fake UART ----
static uint8_t g_wire[1 << 20];
static size_t g_wire_len;
static const uint8_t* g_cur_data;
static size_t g_cur_len;
static unsigned g_busy_polls;     // polls a started transfer stays busy
static unsigned g_busy_left;
static unsigned g_starts;
static unsigned g_rate_switches;
static size_t g_queued_at_switch;

void hal_uart_tx_start(const uint8_t* d, size_t n) {
    assert(n >= 1);
    assert(g_cur_len == 0 && "started while busy");
    g_cur_data = d;
    g_cur_len = n;
    g_busy_left = g_busy_polls;
    g_starts++;
}
static void finish_if_due(void) {
    if (g_cur_len != 0 && g_busy_left == 0) {
        memcpy(g_wire + g_wire_len, g_cur_data, g_cur_len);
        g_wire_len += g_cur_len;
        g_cur_len = 0;
    }
}
bool hal_uart_tx_busy(void) {
    if (g_cur_len == 0)
        return false;
    if (g_busy_left != 0)
        g_busy_left--;
    finish_if_due();
    return g_cur_len != 0;
}
void hal_uart_tx_wait_done(void) {
    while (g_cur_len != 0) {
        g_busy_left = 0;
        finish_if_due();
    }
}
void hal_uart_set_baud(uint32_t baud) {
    (void)baud;
    assert(g_cur_len == 0 && "the rate changed with a transfer running");
    g_rate_switches++;
    g_queued_at_switch = g_wire_len;
}
// the rest of the HAL, never called here
void hal_init(void) {}
uint32_t hal_uart_baud_actual(uint32_t b) { return b; }
uint32_t hal_uart_max_baud(void) { return 1; }
bool hal_uart_has_break(void) { return false; }
uint8_t hal_uart_errors(void) { return 0; }
size_t hal_uart_read(uint8_t* d, size_t m) { (void)d; (void)m; return 0; }
uint32_t hal_millis(void) { return 0; }
size_t hal_unique_id(uint8_t* o, size_t m) { (void)o; (void)m; return 0; }
uint8_t hal_chip(void) { return 0; }
bool hal_has_bootloader(void) { return false; }
void hal_enter_bootloader(void) {}
void hal_control_silent_drop(void) {}
void hal_usb_clear_endpoint(uint8_t a) { (void)a; }

// ---- the receiver of the frames ----
typedef struct {
    uint8_t type;
    uint16_t len;
    uint8_t data[MIMUSB_MAX_FRAME_PAYLOAD];
} got_t;
static got_t g_got[4096];
static size_t g_got_n;

static void parse_wire(void) {
    size_t i = 0;
    g_got_n = 0;
    while (i < g_wire_len) {
        assert(g_wire[i] == MIMUSB_SYNC);
        const uint8_t type = g_wire[i + 1];
        uint16_t len;
        size_t hdr;
        uint16_t crc = mimusb_crc16_update(MIMUSB_CRC16_INIT, type);
        if (g_wire[i + 2] < 0x80) {
            len = g_wire[i + 2];
            hdr = 3;
            crc = mimusb_crc16_update(crc, g_wire[i + 2]);
        } else {
            len = (uint16_t)((g_wire[i + 2] & 0x7F) | (g_wire[i + 3] << 7));
            hdr = 4;
            crc = mimusb_crc16_update(crc, g_wire[i + 2]);
            crc = mimusb_crc16_update(crc, g_wire[i + 3]);
        }
        for (uint16_t k = 0; k < len; k++)
            crc = mimusb_crc16_update(crc, g_wire[i + hdr + k]);
        assert(g_wire[i + hdr + len] == (uint8_t)(crc & 0xFF));
        assert(g_wire[i + hdr + len + 1] == (uint8_t)(crc >> 8));
        assert(g_got_n < sizeof(g_got) / sizeof(g_got[0]));
        g_got[g_got_n].type = type;
        g_got[g_got_n].len = len;
        memcpy(g_got[g_got_n].data, &g_wire[i + hdr], len);
        g_got_n++;
        i += hdr + len + 2;
    }
    assert(i == g_wire_len);
}

static void reset(unsigned busy) {
    mimusb_tx_flush();
    g_wire_len = 0;
    g_busy_polls = busy;
    g_starts = 0;
}

static void settle(void) {
    mimusb_tx_flush();
    parse_wire();
}

static void test_single_frame(void) {
    reset(0);
    const uint8_t p[3] = {1, 2, 3};
    mimusb_tx_frame(MIMUSB_TX_USB_SIDE, 0x21, p, 3);
    settle();
    assert(g_got_n == 1 && g_got[0].type == 0x21 && g_got[0].len == 3 && !memcmp(g_got[0].data, p, 3));
}

// A frame of every length from 0 to the limit, so the one- and two-byte length forms and the ring's
// wrap are all met; each is checked whole.
static void test_every_length_and_wrap(void) {
    reset(3);
    static uint8_t pay[MIMUSB_MAX_FRAME_PAYLOAD];
    for (size_t i = 0; i < sizeof(pay); i++)
        pay[i] = (uint8_t)(i * 7 + 1);
    size_t sent = 0;
    for (uint16_t len = 0; len <= MIMUSB_MAX_FRAME_PAYLOAD; len += (len < 300 ? 1 : 37)) {
        mimusb_tx_frame(MIMUSB_TX_USB_SIDE, 0x22, pay, len);
        sent++;
        if (sent % 5 == 0) {
            settle();
            for (size_t k = 0; k < g_got_n; k++) {
                assert(!memcmp(g_got[k].data, pay, g_got[k].len));
            }
            g_wire_len = 0;
        }
    }
    settle();
}

// Two rings, one frame stream each, queued alternately while the UART is busy: every frame arrives
// whole and each ring's frames arrive in the order they were queued.
static void test_two_rings_in_order(void) {
    reset(2);
    uint8_t p[40];
    for (unsigned n = 0; n < 600; n++) {
        memset(p, (int)n, sizeof(p));
        p[0] = (uint8_t)n;
        p[1] = (uint8_t)(n >> 8);
        mimusb_tx_frame(n & 1 ? MIMUSB_TX_USB_SIDE : MIMUSB_TX_RX_SIDE, n & 1 ? 0x31 : 0x30, p,
                        (uint16_t)(2 + n % 38));
        if (n % 50 == 49)
            mimusb_tx_pump();
    }
    settle();
    assert(g_got_n == 600);
    unsigned next[2] = {0, 1};
    for (size_t k = 0; k < g_got_n; k++) {
        const unsigned side = g_got[k].type & 1u;
        const unsigned n = (unsigned)(g_got[k].data[1] << 8) | g_got[k].data[0];
        assert(n == next[side]);
        next[side] += 2;
    }
}

// A frame waits for room instead of overwriting: far more than the ring holds is queued with the
// UART slow.
static void test_ring_full_waits(void) {
    reset(40);
    static uint8_t pay[500];
    for (unsigned n = 0; n < 200; n++) {
        memset(pay, (int)n, sizeof(pay));
        mimusb_tx_frame(MIMUSB_TX_USB_SIDE, 0x40, pay, sizeof(pay));
    }
    settle();
    assert(g_got_n == 200);
    for (unsigned n = 0; n < 200; n++) {
        assert(g_got[n].len == 500 && g_got[n].data[0] == (uint8_t)n && g_got[n].data[499] == (uint8_t)n);
    }
}

// The rate changes only after what was queued has left, and nothing starts while it changes.
static void test_set_baud_after_queue(void) {
    reset(5);
    const uint8_t p[4] = {9, 9, 9, 9};
    for (int i = 0; i < 10; i++)
        mimusb_tx_frame(MIMUSB_TX_RX_SIDE, 0x50, p, 4);
    const size_t expect = 10 * (5 + 4);
    g_rate_switches = 0;
    mimusb_tx_set_baud(3000000u);
    assert(g_rate_switches == 1);
    assert(g_queued_at_switch == expect);
    mimusb_tx_frame(MIMUSB_TX_RX_SIDE, 0x51, p, 4);
    settle();
    assert(g_got_n == 11 && g_got[10].type == 0x51);
}

// The producer returns without waiting for the line: with the UART busy, queuing one frame starts a
// transfer and does not wait for it.
static void test_producer_does_not_wait(void) {
    reset(1000000);
    const uint8_t p[8] = {0};
    mimusb_tx_frame(MIMUSB_TX_USB_SIDE, 0x60, p, 8);
    assert(g_starts == 1 && g_wire_len == 0);
    g_busy_polls = 0;
    settle();
    assert(g_got_n == 1);
}

int main(void) {
    test_single_frame();
    test_every_length_and_wrap();
    test_two_rings_in_order();
    test_ring_full_waits();
    test_set_baud_after_queue();
    test_producer_does_not_wait();
    puts("test_tx: ok");
    return 0;
}
