// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#ifndef MIMHID_HAL_H
#define MIMHID_HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "mimhid_proto.h"

// Per-MCU services the portable core needs. Each port implements these in its own hal_*.c.

// Brings up clocks, the board, USB PHY if needed, and the UART at MIMHID_BOOT_BAUD, 8N1.
void hal_init(void);

// The rate the UART's divider really gives for a requested baud, or 0 when it cannot be made at
// all. The core refuses a request whose error is over MIMHID_BAUD_TOLERANCE_PPM.
uint32_t hal_uart_baud_actual(uint32_t baud);

// The fastest rate the UART can make, from its clock and divider limits.
uint32_t hal_uart_max_baud(void);

// True when the port sees a UART BREAK (see hal_uart_errors); false when it cannot.
bool hal_uart_has_break(void);

// What went wrong on the line since the last call, as HAL_UART_ERR_* bits, and clears it. A
// hardware flag that only latches is reported once per call.
#define HAL_UART_ERR_BREAK   0x01 // the line was held low for more than a character
#define HAL_UART_ERR_FRAMING 0x02 // a framing error that is not a BREAK
#define HAL_UART_ERR_OVERRUN 0x04 // the UART lost a byte
#define HAL_UART_ERR_RING    0x08 // the receive ring was lapped (software or DMA)
uint8_t hal_uart_errors(void);

// Waits until queued output has left, then switches the UART to baud (one hal_uart_baud_actual
// accepts). Called from the receive context; it must not interleave with hal_uart_write.
void hal_uart_set_baud(uint32_t baud);

// Copies up to max received bytes into dst without blocking and returns the count.
// Called from one context only.
size_t hal_uart_read(uint8_t* dst, size_t max);

// Sends len bytes, blocking until queued. Safe to call from both the receive and USB contexts,
// and one call is never interleaved with another.
void hal_uart_write(const uint8_t* data, size_t len);

// Milliseconds since boot; wraps at 2^32.
uint32_t hal_millis(void);

// Copies the chip's unique identifier into out and returns its length, or 0 if there is none.
size_t hal_unique_id(uint8_t* out, size_t max);

// The chip, as MIMHID_CHIP_* names it.
uint8_t hal_chip(void);

// Disarms EP0 IN and OUT without stalling, leaving EP0 to NAK incoming tokens indefinitely.
void hal_control_silent_drop(void);

#endif // MIMHID_HAL_H
