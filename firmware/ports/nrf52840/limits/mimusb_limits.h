// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
#ifndef MIMUSB_LIMITS_H
#define MIMUSB_LIMITS_H

// The limits of the nRF52840 board.
//
// RAM: 256 kB (nRF52840 Product Specification, Nordic Semiconductor, "Memory": 1 MB flash and
// 256 kB RAM); the BSP's linker script (nrf52840_xxaa.ld) gives the image all of it, 262144 bytes.
//
// The image takes data + bss (tools/check_ram.py prints the sum at every build); the build stops
// when the linker has no room, or when fewer than the reserve its CMake file states are left (a
// project choice: room for the stack, the interrupt handlers and the growth of the next TinyUSB).
// Each limit is also bound by the protocol or by a field of the board's PONG, said beside it.
// config.h says what each one is.
#define MIMUSB_MAX_INTERFACES 15      // interfaces of any class, numbered 0, 1, ... (USB 2.0 9.6.5); an interface may have no endpoint
#define MIMUSB_MAX_CONFIG_BYTES 8192                 // two copies (staged and active); wTotalLength is 16 bits
#define MIMUSB_MAX_BOS_BYTES 1024                    // two copies; wTotalLength of a BOS is 16 bits (USB 3.2 9.6.2)
#define MIMUSB_MAX_STRINGS 128                       // PONG: one byte (at most 255)
#define MIMUSB_MAX_STRING_BYTES 16384                // two copies; an offset into the pool is 16 bits
#define MIMUSB_MAX_REPORT_BYTES MIMUSB_MAX_ENDPOINT_BYTES // one endpoint packet
#define MIMUSB_CONTROL_DATA_MAX 4088                 // the data stage; a frame takes at most 4096 (MIMUSB_PROTO_MAX_PAYLOAD)
#define MIMUSB_EP_POOL_SLOTS 576                     // each slot holds MIMUSB_MAX_TRANSFER_BYTES; PONG: 16 bits
#define MIMUSB_CONTROL_RULES 254                     // PONG: one byte, and 0xFF means none, so 254 at most
#define MIMUSB_ANSWER_BYTES 32768                    // two copies; a length and an offset are 16 bits
#define MIMUSB_LAST_SENT_SLOTS 16                    // one per Input report of the interfaces (2 each) and two to spare; at most 254

#endif
