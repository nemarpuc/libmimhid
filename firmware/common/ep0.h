// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#ifndef MIMHID_EP0_H
#define MIMHID_EP0_H

#include <stdbool.h>
#include <stdint.h>
#include "tusb.h"
// usbd.h declares dcd_int_handler too, which a family build's -Wredundant-decls turns into an error.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wredundant-decls"
#endif
#include "device/dcd.h"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

// Endpoint 0 in the packet size the device descriptor announces (bMaxPacketSize0: 8, 16, 32 or
// 64). TinyUSB gives the controller up to CFG_TUD_ENDPOINT0_SIZE (64) bytes per transfer and the
// RP2040 driver opens endpoint 0 at 64, so with a smaller size the target would see packets
// longer than it was told. dcd_edpt_xfer is wrapped (-Wl,--wrap=dcd_edpt_xfer in every port): a
// transfer on endpoint 0 goes to the controller one packet at a time, the next one started from
// the stack's task (usbd_defer_func), and the stack sees one completion for the whole. An IN data
// stage shorter than wLength that ends on a full packet gets its zero-length packet, which the
// stack, counting in 64-byte steps, would not send. At 64 nothing changes.

// The size the device descriptor announces; usb_descriptors.c sets it with the descriptor.
void mimhid_ep0_set_size(uint8_t size);

// Interrupt side, from __wrap_dcd_event_handler before anything else: returns true when the
// event was taken (a packet of a transfer that goes on), false when it goes on to the stack.
bool mimhid_ep0_event(dcd_event_t const* event, bool in_isr);

#endif // MIMHID_EP0_H
