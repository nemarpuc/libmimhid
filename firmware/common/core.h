// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#ifndef MIMHID_CORE_H
#define MIMHID_CORE_H

#include <stdbool.h>
#include <stdint.h>

// Firmware version reported in the Pong frame; keep in step with the library version.
#define FW_VERSION_MAJOR 0
#define FW_VERSION_MINOR 3
#define FW_VERSION_PATCH 0

// What the chip's USB controller has. Every port states these in its build, on the compiler
// command line, and the build stops when one is missing: nothing is guessed from the chip family
// or from the headers read so far (tusb_config.h, config.h and every source read these, and they
// must come out the same in all of them whatever they include first).
//
//   MIMHID_MAX_ENDPOINT_NUMBER  the highest endpoint number it has (an address is refused above it)
//   MIMHID_MAX_ENDPOINT_BYTES   the largest endpoint packet: 64 at full speed, 1024 at high speed
//                               (USB 2.0 5.7.3, 5.7.4 and 9.6.6)
//
// The values of the ports, from the TinyUSB device drivers that open the endpoints:
//   RP2040, RP2350   15, 64  (endpoint numbers 1 to 15 in both directions; RP2040 datasheet
//                             4.1.1.1.1, RP2350 datasheet "USB", Device Mode)
//   nRF52840          7, 64  (EPIN1..7 and EPOUT1..7; number 8 is the isochronous endpoint)
//   STM32F4 (FS)      3, 64  (USB_OTG_FS_MAX_IN_ENDPOINTS 4 with endpoint 0)
//   ESP32-S3          6, 64  (TinyUSB ep_count 7 with endpoint 0; it has five IN endpoints at once,
//                             ep_in_count 5 with endpoint 0, so a configuration with more is not
//                             one this chip can run)
#ifndef MIMHID_MAX_ENDPOINT_NUMBER
#error MIMHID_MAX_ENDPOINT_NUMBER must be defined by the build of the port (see core.h)
#endif
#ifndef MIMHID_MAX_ENDPOINT_BYTES
#error MIMHID_MAX_ENDPOINT_BYTES must be defined by the build of the port (see core.h)
#endif
#if MIMHID_MAX_ENDPOINT_NUMBER < 1 || MIMHID_MAX_ENDPOINT_NUMBER > 15
#error MIMHID_MAX_ENDPOINT_NUMBER is 1 to 15
#endif
#if MIMHID_MAX_ENDPOINT_BYTES != 64 && MIMHID_MAX_ENDPOINT_BYTES != 1024
#error MIMHID_MAX_ENDPOINT_BYTES is 64 (full speed) or 1024 (high speed)
#endif

// True from a COMMIT that applied a configuration until a detach: the board is meant to be on the
// target's bus. Before the first COMMIT, and after a detach, D+ stays disconnected.
bool mimhid_core_attached(void);

// Initializes TinyUSB and holds D+ disconnected until the host commits a configuration.
// Call after hal_init().
void mimhid_core_init(void);

// Receive side: pulls bytes from the HAL, validates frames, answers pings, switches the line
// rate, queues reports, and hands configuration commands to the USB side.
void mimhid_rx_poll(void);

// USB side: runs configuration commands, the reconnect timer, and queue-to-endpoint transfer.
// Call after tud_task().
void mimhid_usb_poll(void);

// Sends an EVENT frame to the host. USB side only: it uses one shared buffer.
void mimhid_send_event(uint8_t kind, const uint8_t* data, uint16_t len);

// Single-core ports: runs tud_task(), mimhid_rx_poll() and mimhid_usb_poll() forever.
void mimhid_run(void);

#endif // MIMHID_CORE_H
