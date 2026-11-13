// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
#ifndef MIMUSB_CORE_H
#define MIMUSB_CORE_H

#include <stdbool.h>
#include <stdint.h>

// Firmware version reported in the Pong frame; keep in step with the library version.
#define FW_VERSION_MAJOR 0
#define FW_VERSION_MINOR 6
#define FW_VERSION_PATCH 1

// What the chip's USB controller has. Every port states these in its build, on the compiler
// command line, and the build stops when one is missing: nothing is guessed from the chip family
// or from the headers read so far (tusb_config.h, config.h and every source read these, and they
// must come out the same in all of them whatever they include first).
//
//   MIMUSB_MAX_ENDPOINT_NUMBER  the highest endpoint number it has (an address is refused above it)
//   MIMUSB_MAX_ENDPOINT_BYTES   the largest endpoint packet: 64 at full speed, 1024 at high speed
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
#ifndef MIMUSB_MAX_ENDPOINT_NUMBER
#error MIMUSB_MAX_ENDPOINT_NUMBER must be defined by the build of the port (see core.h)
#endif
#ifndef MIMUSB_MAX_ENDPOINT_BYTES
#error MIMUSB_MAX_ENDPOINT_BYTES must be defined by the build of the port (see core.h)
#endif
// Two more that a port may state, and that default to nothing:
//
//   MIMUSB_ISO_BYTES            the largest isochronous packet the port takes: 1023 at full speed,
//                               1024 at high speed (USB 2.0 5.6.3, 9.6.6). 0 or not stated: the
//                               port has no isochronous endpoints (the PONG says so).
//                               A port states it only when the TinyUSB device driver it runs has
//                               dcd_edpt_iso_alloc and dcd_edpt_iso_activate (TUP_DCD_EDPT_ISO_ALLOC);
//                               the RP2040 and RP2350 do (src/portable/raspberrypi/rp2040/dcd_rp2040.c).
//   MIMUSB_EP_BUFFER_BYTES      the packet memory the controller shares between its endpoints, when it
//                               must be told how much of it a configuration needs (the RP2040's
//                               DPRAM: 3712 bytes, of which its driver asserts that one is left, and
//                               it takes 64-byte units: a bulk endpoint double buffered, an
//                               interrupt or isochronous one single). 0 or not stated: not checked.
#ifndef MIMUSB_ISO_BYTES
#define MIMUSB_ISO_BYTES 0
#endif
#ifndef MIMUSB_EP_BUFFER_BYTES
#define MIMUSB_EP_BUFFER_BYTES 0
#endif
#if MIMUSB_ISO_BYTES != 0 && (MIMUSB_ISO_BYTES < 64 || MIMUSB_ISO_BYTES > 1024)
#error MIMUSB_ISO_BYTES is 0 (none) or 64 to 1024
#endif
#if MIMUSB_MAX_ENDPOINT_NUMBER < 1 || MIMUSB_MAX_ENDPOINT_NUMBER > 15
#error MIMUSB_MAX_ENDPOINT_NUMBER is 1 to 15
#endif
#if MIMUSB_MAX_ENDPOINT_BYTES != 64 && MIMUSB_MAX_ENDPOINT_BYTES != 1024
#error MIMUSB_MAX_ENDPOINT_BYTES is 64 (full speed) or 1024 (high speed)
#endif

// True from a COMMIT that applied a configuration until a detach: the board is meant to be on the
// target's bus. Before the first COMMIT, and after a detach, D+ stays disconnected.
bool mimusb_core_attached(void);

// Initializes TinyUSB and holds D+ disconnected until the host commits a configuration.
// Call after hal_init().
void mimusb_core_init(void);

// Receive side: pulls bytes from the HAL, validates frames, answers pings, switches the line
// rate, queues reports, and hands configuration commands to the USB side.
void mimusb_rx_poll(void);

// USB side: runs configuration commands, the reconnect timer, and queue-to-endpoint transfer.
// Call after tud_task().
void mimusb_usb_poll(void);

// Sends an EVENT frame to the host. USB side only: it uses one shared buffer.
void mimusb_send_event(uint8_t kind, const uint8_t* data, uint16_t len);

// Single-core ports: runs tud_task(), mimusb_rx_poll() and mimusb_usb_poll() forever.
void mimusb_run(void);

#endif // MIMUSB_CORE_H
