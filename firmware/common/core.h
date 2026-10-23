// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#ifndef MIMHID_CORE_H
#define MIMHID_CORE_H

#include <stdint.h>

// Firmware version reported in the Pong frame; keep in step with the library version.
#define FW_VERSION_MAJOR 1
#define FW_VERSION_MINOR 3
#define FW_VERSION_PATCH 3

// Number of USB HID interfaces (one IN endpoint each) the MCU can provide. A port with fewer
// than seven IN endpoints sets this in its build; the host packs nodes to fit.
#ifndef MIMHID_HID_ITFS
#define MIMHID_HID_ITFS 7
#endif

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
