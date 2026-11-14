// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
#ifndef MIMUSB_LIMITS_H
#define MIMUSB_LIMITS_H

// The limits of the ESP32-S3 board.
//
// RAM: 512 KB of SRAM (ESP32-S3 Series Datasheet, Espressif Systems, "512 KB SRAM"), of which the
// image's static data may use what ESP-IDF's linker script gives it (about 340 kB of DRAM in the
// build summary of a small application; the figure is the framework's, not the chip's). These
// numbers are half of what the nRF52840 takes and have NOT been built here (this workspace has no
// ESP-IDF): build the port with ESP-IDF and read the memory summary before raising them.
//
// The image takes data + bss (tools/check_ram.py prints the sum at every build); the build stops
// when the linker has no room, or when fewer than the reserve its CMake file states are left (a
// project choice: room for the stack, the interrupt handlers and the growth of the next TinyUSB).
// Each limit is also bound by the protocol or by a field of the board's PONG, said beside it.
// config.h says what each one is.
#define MIMUSB_MAX_INTERFACES 15      // interfaces of any class, numbered 0, 1, ... (USB 2.0 9.6.5); an interface may have no endpoint
#define MIMUSB_MAX_CONFIG_BYTES 4096                 // two copies (staged and active); wTotalLength is 16 bits
#define MIMUSB_MAX_BOS_BYTES 512                     // two copies; wTotalLength of a BOS is 16 bits (USB 3.2 9.6.2)
#define MIMUSB_MAX_STRINGS 64                        // PONG: one byte (at most 255)
#define MIMUSB_MAX_STRING_BYTES 8192                 // two copies; an offset into the pool is 16 bits
#define MIMUSB_MAX_REPORT_BYTES MIMUSB_MAX_ENDPOINT_BYTES // one endpoint packet
#define MIMUSB_CONTROL_DATA_MAX 2048                 // the data stage; a frame takes at most 4096 (MIMUSB_PROTO_MAX_PAYLOAD)
#define MIMUSB_EP_POOL_SLOTS 256                     // each slot holds MIMUSB_MAX_TRANSFER_BYTES; PONG: 16 bits
#define MIMUSB_CONTROL_RULES 254                     // PONG: one byte, and 0xFF means none, so 254 at most
#define MIMUSB_ANSWER_BYTES 16384                    // two copies; a length and an offset are 16 bits
#define MIMUSB_LAST_SENT_SLOTS 16                    // one per Input report of the interfaces (2 each) and two to spare; at most 254
#define MIMUSB_TX_RING_BYTES 2112      // frames the USB side queues for the host (tx.c): the longest one (6 + the frame limit) and room to spare
#define MIMUSB_TX_RX_RING_BYTES 128    // frames the receive side queues (a PONG is at most 71 bytes)

#endif
