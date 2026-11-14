// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
#ifndef MIMUSB_TX_H
#define MIMUSB_TX_H

#include <stdint.h>

// Frames to the host. A frame is encoded straight into a ring and the UART is fed from the ring,
// so the context that produces a frame (the USB side, which must keep serving the target, or the
// receive side) never waits for the line: it waits only when its ring has no room.
//
// There are two rings, one for each producing context, each with one producer. The USB side's
// frames (events, EP_OUT data, the answers to configuration commands) go to one, the receive
// side's (PONG, the answers to SET_BAUD, COUNTERS and CONTROL_REPLY) to the other, so no lock is
// taken to queue a frame. One consumer at a time moves the bytes to the UART (mimusb_tx_pump); it
// takes whichever ring has bytes, and sends all that one held when it was chosen before it looks at
// the other, so two frames are never mixed.
typedef enum {
    MIMUSB_TX_RX_SIDE = 0,  // the receive side (mimusb_rx_poll)
    MIMUSB_TX_USB_SIDE = 1, // the USB side (tud_task, mimusb_usb_poll and their callbacks)
} mimusb_tx_side;

// Encodes one frame (mimusb_proto.h: sync, type, length, payload, CRC-16) into the ring of `side`
// and starts the UART if it is idle. Waits only while the ring has no room for the whole frame.
// Called from the context `side` names (in a port that runs everything in one loop, from it).
void mimusb_tx_frame(mimusb_tx_side side, uint8_t type, const uint8_t* payload, uint16_t len);

// Moves what the rings hold to the UART as far as the UART takes it now. Does nothing when another
// context is doing it. Cheap enough to call on every pass of a loop.
void mimusb_tx_pump(void);

// Returns when every queued frame has left the wire. For a context that is about to take the
// board off the line (the bootloader).
void mimusb_tx_flush(void);

// Sends what is queued, waits until it has left, and switches the UART to `baud`; no frame starts
// in between. `baud` is one hal_uart_baud_actual accepted.
void mimusb_tx_set_baud(uint32_t baud);

#endif // MIMUSB_TX_H
