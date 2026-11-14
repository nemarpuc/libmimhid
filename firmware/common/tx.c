// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
#include "tx.h"
#include "config.h"
#include "hal.h"
#include <stdbool.h>
#include <string.h>

// What a frame adds to its payload: sync, type, one length byte (two from 128 bytes) and the CRC.
#define FRAME_OVERHEAD_SHORT 5u
#define FRAME_OVERHEAD_LONG  6u

typedef struct {
    uint8_t* buf;
    uint16_t size;
    // `head` is the producer's and `tail` the consumer's; one byte is left unused so that
    // head == tail means empty.
    uint16_t head;
    uint16_t tail;
} ring_t;

static uint8_t g_buf_rx[MIMUSB_TX_RX_RING_BYTES];
static uint8_t g_buf_usb[MIMUSB_TX_RING_BYTES];
static ring_t g_ring[2] = {
    {g_buf_rx, MIMUSB_TX_RX_RING_BYTES, 0, 0},
    {g_buf_usb, MIMUSB_TX_RING_BYTES, 0, 0},
};

// The USB side's ring holds the longest frame it sends (an event or an EP_OUT frame at the frame
// limit); the receive side's holds the longest answer it sends (a PONG with the longest unique id).
_Static_assert(MIMUSB_TX_RING_BYTES >= FRAME_OVERHEAD_LONG + MIMUSB_MAX_FRAME_PAYLOAD + 1,
               "the USB side's ring holds the longest frame");
_Static_assert(MIMUSB_TX_RX_RING_BYTES >=
                   FRAME_OVERHEAD_SHORT + MIMUSB_PONG_MIN_LEN + MIMUSB_UNIQUE_ID_MAX + 1,
               "the receive side's ring holds a PONG");
_Static_assert(MIMUSB_TX_RING_BYTES <= 0xFFFF && MIMUSB_TX_RX_RING_BYTES <= 0xFFFF,
               "ring indices are 16 bits");

// ---- the consumer ----
// Taken by whoever pumps; `g_cur`, `g_left` and `g_chunk` belong to the holder.
static uint8_t g_lock;
static uint8_t g_next;   // the ring looked at first when the previous batch is done
static uint8_t g_cur;    // the ring being sent
static uint16_t g_left;  // bytes of the batch not yet handed to the UART
static uint16_t g_chunk; // bytes handed to the UART and not yet taken off the ring

static bool try_lock(void) { return __atomic_exchange_n(&g_lock, 1u, __ATOMIC_ACQUIRE) == 0u; }
static void lock(void) {
    while (!try_lock()) {
    }
}
// The release is an exchange too: on a core without atomic instructions (Cortex-M0+) the exchange
// runs under a hardware spinlock and a plain store does not, so a store landing between the other
// core's read and write of the exchange would be overwritten and the lock would stay taken.
static void unlock(void) { (void)__atomic_exchange_n(&g_lock, 0u, __ATOMIC_RELEASE); }

static uint16_t used(const ring_t* r) {
    const int32_t d = (int32_t)__atomic_load_n(&r->head, __ATOMIC_ACQUIRE) - (int32_t)r->tail;
    return (uint16_t)(d < 0 ? d + r->size : d);
}

// One step of the consumer, with the lock held. Returns true when it did something that may let
// the next step do more.
static bool step(void) {
    if (g_chunk != 0) {
        if (hal_uart_tx_busy())
            return false;
        ring_t* r = &g_ring[g_cur];
        uint32_t tail = (uint32_t)r->tail + g_chunk;
        if (tail >= r->size)
            tail -= r->size;
        __atomic_store_n(&r->tail, (uint16_t)tail, __ATOMIC_RELEASE);
        g_chunk = 0;
    }
    if (g_left == 0) {
        // A batch is what a ring held when it was chosen: whole frames only, because a producer
        // publishes its head after the last byte of a frame.
        const uint8_t first = g_next;
        for (uint8_t k = 0; k < 2; k++) {
            const uint8_t i = (uint8_t)((first + k) & 1u);
            const uint16_t n = used(&g_ring[i]);
            if (n != 0) {
                g_cur = i;
                g_left = n;
                g_next = (uint8_t)(i ^ 1u);
                break;
            }
        }
        if (g_left == 0)
            return false;
    }
    ring_t* r = &g_ring[g_cur];
    uint16_t n = (uint16_t)(r->size - r->tail); // up to the end of the ring
    if (n > g_left)
        n = g_left;
    g_chunk = n;
    g_left = (uint16_t)(g_left - n);
    hal_uart_tx_start(&r->buf[r->tail], n);
    return true;
}

static bool idle(void) {
    return g_chunk == 0 && g_left == 0 && used(&g_ring[0]) == 0 && used(&g_ring[1]) == 0;
}

void mimusb_tx_pump(void) {
    if (!try_lock())
        return;
    while (step()) {
    }
    unlock();
}

// With the lock held: until everything queued has left the wire.
static void drain(void) {
    while (!idle())
        (void)step();
    hal_uart_tx_wait_done();
}

void mimusb_tx_flush(void) {
    lock();
    drain();
    unlock();
}

void mimusb_tx_set_baud(uint32_t baud) {
    lock();
    drain();
    hal_uart_set_baud(baud);
    unlock();
}

// ---- the producers ----
static void put(ring_t* r, uint16_t* pos, uint8_t b) {
    r->buf[*pos] = b;
    if (++*pos == r->size)
        *pos = 0;
}

void mimusb_tx_frame(mimusb_tx_side side, uint8_t type, const uint8_t* payload, uint16_t len) {
    ring_t* r = &g_ring[side];
    const uint16_t need = (uint16_t)((len < 0x80 ? FRAME_OVERHEAD_SHORT : FRAME_OVERHEAD_LONG) + len);
    for (;;) {
        int32_t room = (int32_t)__atomic_load_n(&r->tail, __ATOMIC_ACQUIRE) - (int32_t)r->head;
        if (room <= 0)
            room += r->size;
        if (room - 1 >= (int32_t)need)
            break;
        mimusb_tx_pump(); // the room comes from the UART taking bytes
    }
    uint16_t pos = r->head;
    uint16_t crc = mimusb_crc16_update(MIMUSB_CRC16_INIT, type);
    put(r, &pos, MIMUSB_SYNC);
    put(r, &pos, type);
    if (len < 0x80) {
        put(r, &pos, (uint8_t)len);
        crc = mimusb_crc16_update(crc, (uint8_t)len);
    } else {
        const uint8_t low = (uint8_t)(0x80 | (len & 0x7F));
        const uint8_t high = (uint8_t)(len >> 7);
        put(r, &pos, low);
        put(r, &pos, high);
        crc = mimusb_crc16_update(crc, low);
        crc = mimusb_crc16_update(crc, high);
    }
    for (uint16_t i = 0; i < len; i++) {
        put(r, &pos, payload[i]);
        crc = mimusb_crc16_update(crc, payload[i]);
    }
    put(r, &pos, (uint8_t)(crc & 0xFF));
    put(r, &pos, (uint8_t)(crc >> 8));
    __atomic_store_n(&r->head, pos, __ATOMIC_RELEASE);
    mimusb_tx_pump();
}
