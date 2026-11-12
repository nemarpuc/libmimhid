// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#ifndef MIMHID_CONTROL_H
#define MIMHID_CONTROL_H

#include "config.h"
#include <stdbool.h>
#include <stdint.h>

// Control transfers of the target (the setup packets on endpoint 0): a table of rules the host
// writes says what the board does with each (see "Control transfers" in mimhid_proto.h). Nothing
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
// answer; the USB side (tud_task, mimhid_usb_poll: one context) stages rules, applies them, writes
// the data an ANSWER_LAST_SENT rule keeps and sends the answer's packets; the receive side (the
// other core on the RP ports) only hands over CONTROL_REPLY frames. Two tables exist, the active
// one (read by the interrupt, and by the USB side to send an ANSWER in place) and the staged one
// (written by the USB side only); applying them swaps which is which with the interrupt masked.

// Single-context ports (mimhid_run) read the UART from the USB side, so a wait inside a TinyUSB
// callback has to read it too; the two-core RP port reads it on the other core.
void mimhid_control_set_single_context(bool single);

// ---- the staged table (USB side, commands) ----
// Each returns 0 or a MIMHID_NAK_* code.

// DESC_RESET: forgets the staged rules.
void mimhid_control_reset(void);

// The count that means every staged rule.
#define MIMHID_CONTROL_ALL 0xFFFF

// RULE: payload after the sequence number.
uint8_t mimhid_control_stage(const uint8_t* payload, uint16_t len);

// DESC_BEGIN, DESC_DATA of the kind ANSWER: the bookkeeping and the bytes of the staged ANSWER rule
// `index`, or false when there is no such rule (or it has no bytes to send).
bool mimhid_control_staged_answer(uint8_t index, mimhid_desc_t** desc, uint8_t** bytes);

// Checks the staged rules against the layout the board will run (every ANSWER rule complete, every
// ANSWER_LAST_SENT rule on an IN endpoint of the layout): 0 or MIMHID_NAK_STATE. `count` is how many
// staged rules are meant (all of them at COMMIT, the host's number at RULES_APPLY), MIMHID_NAK_PARAM
// when it is more than are staged.
uint8_t mimhid_control_check(const mimhid_layout_t* layout, uint16_t count);

// Makes the first `count` staged rules the active ones (mimhid_control_check said yes) and keeps
// what they hold. With `committed` the configuration changes too: nothing is kept (every
// ANSWER_LAST_SENT rule starts empty) and no transfer is under way. Otherwise: MIMHID_NAK_BUSY when
// the board is answering from the active answer memory. A transfer held under the old table goes to
// TinyUSB as if it had not matched.
uint8_t mimhid_control_apply(bool report_all, uint16_t count, bool committed);

// RULE_DATA: payload after the sequence number.
uint8_t mimhid_control_rule_data(const uint8_t* payload, uint16_t len);

// COMMIT with a detach: no rules, nothing reported.
void mimhid_control_detach(void);

// RESET: forgets what ANSWER_LAST_SENT rules hold. USB side.
void mimhid_control_forget_sent(void);

// ---- ANSWER_LAST_SENT (USB side) ----
// The IN endpoints that have an ANSWER_LAST_SENT rule in the active table, as the first rule of the
// list of that endpoint (0xFF: none), so that a transfer on any other endpoint costs one test. The
// USB side only: the table is changed on the USB side, with no transfer being started.
extern uint8_t mimhid_control_sent_head[16];
void mimhid_control_sent_rules(uint8_t number, const uint8_t* data, uint16_t len);

// Called by the generic driver when it starts a transfer of `len` bytes on IN endpoint `number`.
static inline void mimhid_control_sent(uint8_t number, const uint8_t* data, uint16_t len) {
    if (mimhid_control_sent_head[number & 0x0F] != 0xFF)
        mimhid_control_sent_rules(number, data, len);
}

// ---- the rest ----

// Receive side, CONTROL_REPLY: payload after the sequence number. Returns 0 when the board waits
// for that token and has taken the answer, otherwise a MIMHID_NAK_* code.
uint8_t mimhid_control_reply(const uint8_t* payload, uint16_t len);

// USB side: reports the transfers seen, and settles a held one when the host answered or its
// time ran out.
void mimhid_control_poll(void);

#endif // MIMHID_CONTROL_H
