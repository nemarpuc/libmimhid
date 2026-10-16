// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#ifndef MIMHID_HAL_H
#define MIMHID_HAL_H

#include <stddef.h>
#include <stdint.h>

// Per-MCU services the portable core needs. Each port implements these in its own hal_*.c.

// Brings up clocks, the board, USB PHY if needed, and the UART receiver (921600 baud, 8N1).
void hal_init(void);

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

#endif // MIMHID_HAL_H
