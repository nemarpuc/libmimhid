// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#ifndef MIMHID_GENERIC_H
#define MIMHID_GENERIC_H

// The board's own USB class driver, for every interface of the configuration: interrupt and bulk
// endpoints in both directions, whatever the class (HID too: the board runs no class driver of
// TinyUSB's, so the stack's HID driver is not built). TinyUSB tries application drivers before its
// own (get_driver and process_set_config in src/device/usbd.c); the driver is returned from
// usbd_app_driver_get_cb (generic.c).
//
// IN endpoints: one queue each, cut at COMMIT from a pool of MIMHID_EP_POOL_SLOTS slots (config.h).
// The receive side writes a frame's payload into the slot at the head of the queue while it checks
// the frame, and publishes it afterwards; the USB side hands the slot at the tail to the controller
// with usbd_edpt_xfer and frees it when the transfer completes. A queue of depth D holds D transfers
// that wait, and one more that is on the bus: the slot of a transfer on the bus is taken out of the
// queue when it starts and stays reserved until it completes, so nothing can overwrite it.
//
// OUT endpoints: one packet buffer each. The packet goes to the host as an endpoint data frame, and
// the endpoint is armed again only after that frame has been written.
//
// Contexts: the receive side (mimhid_rx_poll) calls mimhid_generic_in_reserve, _commit and _discard;
// everything else runs on the USB side (tud_task, mimhid_usb_poll), which is one context.

#include "config.h"
#include <stdbool.h>
#include <stdint.h>

// Policies of an IN endpoint are MIMHID_EP_FULL_* (mimhid_proto.h).

// Slots of the pool a queue of this depth and policy takes (see generic.c: depth + 1 for
// DROP_NEWEST and WAIT, 2 * depth + 1 for DROP_OLDEST), or 0 for a depth or policy that is none.
uint16_t mimhid_generic_slots(uint8_t depth, uint8_t policy);

// COMMIT, first half: checks that the staged EP_CONFIG settings of the IN endpoints of the generic
// driver fit the pool. False with the NAK code in *code: MIMHID_NAK_STATE for an endpoint that has
// no EP_CONFIG, MIMHID_NAK_OVERFLOW for a pool that is too small.
bool mimhid_generic_plan(const mimhid_config_t* cfg, const mimhid_layout_t* layout, uint8_t* code);

// COMMIT, second half, after mimhid_apply_config: cuts the pool into the queues the plan counted
// and forgets everything queued before. USB side.
void mimhid_generic_apply(const mimhid_config_t* cfg, const mimhid_layout_t* layout);

// EP_CONFIG of an IN endpoint of the generic driver of the active configuration, with no new
// configuration staged: changes depth and policy live. 0, MIMHID_NAK_OVERFLOW when the queue
// allocated at COMMIT is too small for them, or MIMHID_NAK_STATE when the address is not such an
// endpoint. USB side.
uint8_t mimhid_generic_set_policy(uint8_t address, uint8_t depth, uint8_t policy);

// EP_STALL and EP_FLUSH of the active configuration; 0 or a NAK code (MIMHID_NAK_STATE for an
// address that is not an endpoint of the generic driver of the active configuration). A stall drops
// what waits on an IN endpoint and the transfer on the bus, and stops the endpoint until it is
// cleared; a flush drops what waits and keeps the transfer already handed to the controller. USB
// side.
uint8_t mimhid_generic_stall(uint8_t address, bool stall);
uint8_t mimhid_generic_flush(uint8_t address);

// Drops what waits on every IN endpoint and clears the wait counts (RESET). USB side.
void mimhid_generic_drop_all(void);

// Receive side, for a frame to IN endpoint number `number`: where to write its payload (at most
// MIMHID_MAX_TRANSFER_BYTES), or NULL when the endpoint is not one of the generic driver's or its
// queue is full (the transfer is dropped, and counted when loss counting is on).
uint8_t* mimhid_generic_in_reserve(uint8_t number);
// Receive side: the frame checked out, so `length` bytes were written to the reserved slot; queues
// them. A reconfiguration since the reservation drops the frame.
void mimhid_generic_in_commit(uint16_t length);
// Receive side: the frame failed its check, so the reservation is forgotten.
void mimhid_generic_in_discard(void);

// USB side, once per pass after tud_task: starts transfers, removes what DROP_OLDEST overflows, and
// reports completions of endpoints with the wait policy (MIMHID_EV_EP_DONE).
void mimhid_generic_poll(void);

// Loss counting (MIMHID_T_LOSS_CONFIG): counts are kept only while on, and are cleared when it is
// switched. USB side takes the count of one endpoint (0 for none) and clears it.
void mimhid_generic_loss_set(bool on);
uint32_t mimhid_generic_loss_take(uint8_t number);

// The packet of an OUT endpoint, to the host: an endpoint data frame (core.c). USB side.
void mimhid_send_ep_out(uint8_t number, const uint8_t* data, uint16_t length);

#endif // MIMHID_GENERIC_H
