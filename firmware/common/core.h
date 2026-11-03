// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#ifndef MIMHID_CORE_H
#define MIMHID_CORE_H

#include <stdint.h>

// Firmware version reported in the Pong frame; keep in step with the library version.
#define FW_VERSION_MAJOR 2
#define FW_VERSION_MINOR 0
#define FW_VERSION_PATCH 5

// Number of USB HID interfaces (one IN endpoint each) the MCU can provide; the host packs nodes to
// fit. The RP2040 and the RP2350 have the endpoint numbers 1 to 15 in both directions (RP2040
// datasheet 4.1.1.1.1, RP2350 datasheet "USB", Device Mode: "Endpoints 0 -> 15 in both in and out
// directions"), which is also the most USB allows (USB 2.0 2.1: an endpoint number is four bits)
// and fits the 16 interfaces the stack takes (CFG_TUD_INTERFACE_MAX). Every other port sets the
// number its chip has in its build (4 on the ESP32-S3, 3 on the STM32F4 at full speed); the
// default of 7 is the nRF52840's. tusb_config.h and every source read this, and it must come out
// the same in all of them whatever they include first, so it is chosen from the macros the Pico
// SDK puts on the command line (PICO_RP2040, PICO_RP2350), not from TinyUSB's OPT_MCU_* ones,
// which exist only once tusb_option.h has been read.
#ifndef MIMHID_HID_ITFS
#if defined(PICO_RP2040) || defined(PICO_RP2350)
#define MIMHID_HID_ITFS 15
#else
#define MIMHID_HID_ITFS 7
#endif
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
