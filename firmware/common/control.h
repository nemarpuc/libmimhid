// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
#ifndef MIMUSB_CONTROL_H
#define MIMUSB_CONTROL_H

#include "config.h"
#include <stdbool.h>
#include <stdint.h>

// Control transfers of the target (the setup packets on endpoint 0): a table of rules the host
// writes says what the board does with each (see "Control transfers" in mimusb_proto.h). Nothing
// happens until the host has committed rules: with no rule and no report flag every request goes to
// TinyUSB untouched, and the stack answers the standard requests it can and stalls the rest.
//
// The setup packet is taken in the USB interrupt by wrapping dcd_event_handler (the linker
// option -Wl,--wrap=dcd_event_handler in every port): a request that matches a rule is either held
// back from TinyUSB while the host decides (ASK), or handed to it as it came (the board's own
// answer) or retyped as a vendor request, which tud_vendor_control_xfer_cb answers from the rule
// (ANSWER, ANSWER_LAST_SENT, ACCEPT) or from the host's data. TinyUSB's control state machine is
// never bypassed.
//
// Contexts. The USB interrupt matches the setup packet against the ACTIVE table and starts the
// answer; the USB side (tud_task, mimusb_usb_poll: one context) stages rules, applies them, writes
// the data an ANSWER_LAST_SENT rule keeps and sends the answer's packets; the receive side (the
// other core on the RP ports) only hands over CONTROL_REPLY frames. Two tables exist, the active
// one (read by the interrupt, and by the USB side to send an ANSWER in place) and the staged one
// (written by the USB side only); applying them swaps which is which with the interrupt masked.

// ---- the staged table (USB side, commands) ----
// Each returns 0 or a MIMUSB_NAK_* code.

// DESC_RESET: forgets the staged rules.
void mimusb_control_reset(void);

// The count that means every staged rule.
#define MIMUSB_CONTROL_ALL 0xFFFF

// RULE: payload after the sequence number.
uint8_t mimusb_control_stage(const uint8_t* payload, uint16_t len);

// DESC_BEGIN, DESC_DATA of the kind ANSWER: the bookkeeping and the bytes of the staged ANSWER rule
// `index`, or false when there is no such rule (or it has no bytes to send).
bool mimusb_control_staged_answer(uint8_t index, mimusb_desc_t** desc, uint8_t** bytes);

// Checks the staged rules against the layout the board will run (every ANSWER rule complete, every
// ANSWER_LAST_SENT rule on an IN endpoint of the layout): 0 or MIMUSB_NAK_STATE. `count` is how many
// staged rules are meant (all of them at COMMIT, the host's number at RULES_APPLY), MIMUSB_NAK_PARAM
// when it is more than are staged.
uint8_t mimusb_control_check(const mimusb_layout_t* layout, uint16_t count);

// Makes the first `count` staged rules the active ones (mimusb_control_check said yes) and keeps
// what they hold. With `committed` the configuration changes too: nothing is kept (every
// ANSWER_LAST_SENT rule starts empty) and no transfer is under way. Otherwise: MIMUSB_NAK_BUSY when
// the board is answering from the active answer memory. A transfer held under the old table goes to
// TinyUSB as if it had not matched.
uint8_t mimusb_control_apply(bool report_all, uint16_t count, bool committed);

// RULE_DATA: payload after the sequence number.
uint8_t mimusb_control_rule_data(const uint8_t* payload, uint16_t len);

// COMMIT with a detach: no rules, nothing reported.
void mimusb_control_detach(void);

// RESET: forgets what ANSWER_LAST_SENT rules hold. USB side.
void mimusb_control_forget_sent(void);

// ---- ANSWER_LAST_SENT (USB side) ----
// The IN endpoints that have an ANSWER_LAST_SENT rule in the active table, as the first rule of the
// list of that endpoint (0xFF: none), so that a transfer on any other endpoint costs one test. The
// USB side only: the table is changed on the USB side, with no transfer being started.
extern uint8_t mimusb_control_sent_head[16];
void mimusb_control_sent_rules(uint8_t number, const uint8_t* data, uint16_t len);

// Called by the generic driver when it starts a transfer of `len` bytes on IN endpoint `number`.
static inline void mimusb_control_sent(uint8_t number, const uint8_t* data, uint16_t len) {
    if (mimusb_control_sent_head[number & 0x0F] != 0xFF)
        mimusb_control_sent_rules(number, data, len);
}

// ---- the rest ----

// Receive side, CONTROL_REPLY: payload after the sequence number. Returns 0 when the board waits
// for that token and has taken the answer, otherwise a MIMUSB_NAK_* code.
uint8_t mimusb_control_reply(const uint8_t* payload, uint16_t len);

// The stack asks the controller for a transfer on endpoint ep_addr. When it is the zero-length status
// stage of a host-to-device transfer whose data the host is still judging, the board keeps the
// request (returns true: do not start it); mimusb_control_poll gives it to the controller, or stalls
// the transfer, when the verdict is in.
bool mimusb_control_hold_status(uint8_t ep_addr, uint16_t total_bytes);

// USB side: reports the transfers seen, and settles a held one when the host answered or its
// time ran out.
void mimusb_control_poll(void);

#endif // MIMUSB_CONTROL_H
