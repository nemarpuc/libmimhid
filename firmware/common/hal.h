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

// True when the UART can run within 2 % of baud.
bool hal_uart_baud_ok(uint32_t baud);

// Waits until queued output has left, then switches the UART to baud (one hal_uart_baud_ok
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

#endif // MIMHID_HAL_H
