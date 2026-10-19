// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#ifndef MIMHID_CONTROL_H
#define MIMHID_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

// Control transfers of the target (the setup packets on endpoint 0), reported to the host and
// answered by it when a rule says so. Nothing happens until the host sends CONTROL_CONFIG: with
// no rule and no report flag every request goes to TinyUSB untouched.
//
// The setup packet is taken in the USB interrupt by wrapping dcd_event_handler (the linker
// option -Wl,--wrap=dcd_event_handler in every port): a request that matches a rule is held back
// from TinyUSB while the host decides, and then handed to it either as it came (the board's own
// answer) or retyped as a vendor request, which tud_vendor_control_xfer_cb answers from the
// host's data. TinyUSB's control state machine is never bypassed.

// Single-context ports (mimhid_run) read the UART from the USB side, so a wait inside a TinyUSB
// callback has to read it too; the two-core RP port reads it on the other core.
void mimhid_control_set_single_context(bool single);

// USB side, CONTROL_CONFIG: payload after the sequence number. Returns 0 or a MIMHID_NAK_* code.
uint8_t mimhid_control_config(const uint8_t* payload, uint16_t len);

// Receive side, CONTROL_REPLY: payload after the sequence number. Returns 0 when the board waits
// for that token and has taken the answer, otherwise a MIMHID_NAK_* code.
uint8_t mimhid_control_reply(const uint8_t* payload, uint16_t len);

// USB side: reports the transfers seen, and settles a held one when the host answered or its
// time ran out.
void mimhid_control_poll(void);

// USB side: forgets a held transfer and an answer under way (the descriptors are about to change).
void mimhid_control_clear(void);

#endif // MIMHID_CONTROL_H
