// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
#ifndef MIMUSB_LIMITS_H
#define MIMUSB_LIMITS_H

// The limits of the RP2350 board (config.h says what each one is).
//
// RAM: 520 kB of on-chip SRAM in ten banks (RP2350 datasheet, Raspberry Pi Ltd: "520 kB of
// on-chip SRAM"; the RP2350 product brief says the same): 520 kB = 532480 bytes.
//
// These are the figures that fill it: the image takes data + bss (tools/check_ram.py prints the
// sum at every build) and the build stops when fewer than 32 KiB are left (a project choice: room for
// the stack, the interrupt handlers and the growth of the next TinyUSB). Raising one needs the RAM
// to be there: `cmake --build` fails in the linker, or in check_ram.py, when it is not.
// Each is also limited by the protocol (proto/mimusb_proto.h) or by a field of the board's
// PONG, which is said beside it.
#define MIMUSB_MAX_INTERFACES 15     // USB 2.0: interfaces number 0, 1, ...; 15 is the endpoint numbers
#define MIMUSB_MAX_CONFIG_BYTES 8192 // two copies of it (staged and active); wTotalLength is 16 bits
#define MIMUSB_MAX_BOS_BYTES 1024    // two copies; wTotalLength of a BOS is 16 bits (USB 3.2 9.6.2)
#define MIMUSB_MAX_STRINGS 128       // PONG: one byte (at most 255)
#define MIMUSB_MAX_STRING_BYTES 16384 // two copies; an offset into the pool is 16 bits
#define MIMUSB_MAX_REPORT_BYTES MIMUSB_MAX_ENDPOINT_BYTES // one endpoint packet
#define MIMUSB_CONTROL_DATA_MAX 4088 // the data stage; a frame takes at most 4096 (MIMUSB_PROTO_MAX_PAYLOAD)
#define MIMUSB_EP_POOL_SLOTS 192     // each slot holds MIMUSB_MAX_TRANSFER_BYTES (1023: isochronous)
#define MIMUSB_CONTROL_RULES 254     // PONG: one byte, and 0xFF means none, so 254 at most
#define MIMUSB_ANSWER_BYTES 32768    // two copies; a length and an offset are 16 bits
#define MIMUSB_LAST_SENT_SLOTS 32    // one slot per Input report of 15 interfaces (2 each) and two to spare

#endif
