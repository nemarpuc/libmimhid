// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
#ifndef MIMUSB_HAL_H
#define MIMUSB_HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "mimusb_proto.h"

// Per-MCU services the portable core needs. Each port implements these in its own hal_*.c.

// Brings up clocks, the board, USB PHY if needed, and the UART at MIMUSB_BOOT_BAUD, 8N1.
void hal_init(void);

// The rate the UART's divider really gives for a requested baud, or 0 when it cannot be made at
// all. The core refuses a request whose error is over MIMUSB_BAUD_TOLERANCE_PPM.
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

// The chip, as MIMUSB_CHIP_* names it.
uint8_t hal_chip(void);

// True when hal_enter_bootloader does something on this chip.
bool hal_has_bootloader(void);

// Waits until queued output has left, then restarts the chip into its own bootloader (on the
// RP2040 and RP2350 the USB mass-storage bootloader in ROM). Does not return when
// hal_has_bootloader is true; does nothing otherwise.
void hal_enter_bootloader(void);

// Disarms EP0 IN and OUT without stalling, leaving EP0 to NAK incoming tokens indefinitely.
void hal_control_silent_drop(void);

// Takes the transfer that is armed on one endpoint of the controller off it: its buffer is no longer
// offered to the target and its completion is forgotten. Called when a SET_INTERFACE stops or
// starts an isochronous endpoint. The target stops polling an endpoint of the setting it left, so
// the transfer that was armed is never taken; the driver (the RP2040's) stops with "ep was already
// available" when the next transfer is armed on top of it. Does nothing on a chip whose driver has
// no such state.
void hal_usb_clear_endpoint(uint8_t address);

#endif // MIMUSB_HAL_H
