// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#include <string.h>
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
#include "config.h"
#include "control.h"
#include "core.h"
#include "ep0.h"
#include "hal.h"

// Code that runs in the USB interrupt sits in RAM where the port asks for it. The host-side tests
// set an RP2040 target, whose section name a Mach-O compiler refuses.
#ifdef __APPLE__
#define CONTROL_FAST
#else
#define CONTROL_FAST TU_ATTR_FAST_FUNC
#endif

// The root hub port the stack runs on (tusb_init() with no argument is port 0).
#define RHPORT 0

// Transfers waiting to be reported; a power of two. The USB interrupt fills it, the USB side
// empties it.
#define NOTE_SLOTS 8
#define NOTE_MASK  (NOTE_SLOTS - 1)

// settle(): do what the rule of the held transfer says.
#define BY_RULE 0xFF

#define NONE 0xFF // no rule, no slot

// What the vendor callback does with a request it was handed (g_answer.action).
enum {
    ANS_HOST = 1, // the host answered a held transfer (or the board answers GET_STATUS): data in buf
    ANS_RULE,     // an ANSWER rule: its bytes, in place in the answer memory
    ANS_LAST,     // an ANSWER_LAST_SENT rule: the last transfer of its endpoint
    ANS_ACCEPT,   // an ACCEPT rule: take the data stage and report it
    ANS_STALL     // stall
};

typedef struct {
    uint16_t value_mask;
    uint16_t value_value;
    uint16_t index_mask;
    uint16_t index_value;
    uint16_t timeout_ms;  // ASK
    uint16_t offset;      // ANSWER: where its bytes are in the answer memory
    uint16_t length;      // ANSWER: the bytes it answers with now (at most d.total)
    uint16_t answer_end;  // answer memory used by this rule and the ones before it
    mimhid_desc_t d;      // ANSWER: the bytes the host announced and has sent
    uint8_t bm_mask;
    uint8_t bm_value;
    uint8_t request_mask;
    uint8_t request_value;
    uint8_t action;       // MIMHID_RULE_*
    uint8_t after;        // ASK: MIMHID_REPLY_STALL or MIMHID_REPLY_DROP
    uint8_t endpoint;     // ANSWER_LAST_SENT: IN endpoint address
    uint8_t filter;       // ANSWER_LAST_SENT: only transfers whose first byte is `first`
    uint8_t first;
    uint8_t slot;         // ANSWER_LAST_SENT: its buffer in g_sent
    uint8_t slots_end;    // slots used by this rule and the ones before it
    uint8_t next;         // ANSWER_LAST_SENT: next rule of the same endpoint (USB side only)
} rule_t;

typedef struct {
    rule_t rule[MIMHID_CONTROL_RULES];
    uint8_t count;
    uint8_t dirty_slot;   // staged table: lowest slot of a rule changed since it equalled the active one
    _Alignas(4) uint8_t answer[MIMHID_ANSWER_BYTES];
} rule_set_t;

typedef struct {
    uint8_t token;
    uint8_t flags;
    uint8_t setup[8];
} note_t;

// The two tables. g_set[g_act] is the active one: the interrupt reads its rules to match a setup
// packet, and the USB side reads its answer memory to send an ANSWER in place. The other is the
// staged one, which only the USB side reads and writes. They change roles in apply(), with the
// interrupt masked, so the interrupt never reads a table that is being written; the table that
// has just become the staged one is overwritten with a copy of the active one, which is done only
// when nothing is being answered from the old active one (see g_answer.serving).
static rule_set_t g_set[2];
static volatile uint8_t g_act;
static volatile bool g_report_all;

// What ANSWER_LAST_SENT rules keep: the last transfer started on their endpoint. Written by the USB
// side when the generic driver starts a transfer (mimhid_control_sent_rules, from in_service in
// generic.c, which runs in the USB side's context: from tud_task through xfer callbacks and from
// mimhid_usb_poll, never at once) and read by the USB side when the vendor callback starts the
// answer to GET_REPORT (also tud_task). One context writes and reads, so a reader never sees a
// report half written. The interrupt does not read it: the answer is copied into g_answer.buf when
// its setup stage is handled, and the controller is given the copy, so a transfer started while the
// answer goes out cannot change it. The tables only say which slots exist.
static struct {
    bool valid;
    uint16_t len;
    _Alignas(4) uint8_t data[MIMHID_MAX_TRANSFER_BYTES];
} g_sent[MIMHID_LAST_SENT_SLOTS];
uint8_t mimhid_control_sent_head[16];

static note_t g_note[NOTE_SLOTS];
static volatile uint8_t g_note_head; // the interrupt writes
static volatile uint8_t g_note_tail; // the USB side writes
static volatile bool g_lost;         // a transfer could not be reported
static uint8_t g_token;              // the interrupt only

// The transfer held back from TinyUSB while the host decides. The interrupt makes it HELD last,
// and the USB side takes it back to IDLE with the interrupt masked.
enum { HOLD_IDLE = 0, HOLD_HELD = 1 };
static struct {
    volatile uint8_t state;
    dcd_event_t event;
    uint8_t token;
    uint8_t rule;
    uint16_t timeout_ms;
    uint8_t after;
    uint32_t deadline_ms;
} g_hold;

// What the answer to a request needs from the vendor callback: `armed` from the moment the retyped
// request is queued until its setup stage arrives, `active` from there to the end of the transfer.
// buf[0] is the token that CONTROL_DATA starts with; buf[1..] is the data of the answer, or of
// the host-to-device data stage.
//
// `src` is where an ANSWER rule's bytes are (in the active table's answer memory, which TinyUSB
// reads one packet at a time from its task until the data stage is over), and `serving` is the rule
// being answered from it: the interrupt sets it with the setup packet, and the callback clears it
// when the data stage ends, a new setup packet or a bus event voids the transfer, or the rules are
// cleared. While it is set, the USB side does not change that rule's bytes (RULE_DATA) or swap the
// tables (RULES_APPLY): both answer NAK_BUSY and the host asks again.
static struct {
    volatile bool armed;
    volatile bool active;
    volatile uint8_t serving;
    tusb_control_request_t request;
    uint8_t action;
    uint8_t token;
    uint16_t length;
    uint16_t timeout_ms;
    uint8_t timeout_action;
    uint8_t slot;
    const uint8_t* src;
    uint8_t buf[1 + MIMHID_CONTROL_DATA_MAX];
} g_answer;

// The host's reply, written by the receive side. The token of the transfer being waited for is
// in g_expect (0: none); a reply for any other token is refused.
static struct {
    volatile uint8_t full;
    uint8_t token;
    uint8_t action;
    uint16_t length;
    uint8_t data[MIMHID_CONTROL_DATA_MAX];
} g_reply;
static volatile uint8_t g_expect;

static bool g_single_context;

// The linker maps these to the stack's own dcd functions (-Wl,--wrap=...).
void __real_dcd_event_handler(dcd_event_t const* event, bool in_isr);
void __wrap_dcd_event_handler(dcd_event_t const* event, bool in_isr);
void __real_dcd_edpt_stall(uint8_t rhport, uint8_t ep_addr);
void __wrap_dcd_edpt_stall(uint8_t rhport, uint8_t ep_addr);

static volatile bool g_suppress_stall;

void __wrap_dcd_edpt_stall(uint8_t rhport, uint8_t ep_addr) {
    if (g_suppress_stall && (ep_addr == 0x00 || ep_addr == 0x80)) {
        hal_control_silent_drop();
        return;
    }
    __real_dcd_edpt_stall(rhport, ep_addr);
}

void mimhid_control_set_single_context(bool single) {
    g_single_context = single;
}

static rule_set_t* staged(void) { return &g_set[1u - g_act]; }

// ---- rules ----
CONTROL_FAST static bool is_fixed(const tusb_control_request_t* r) {
    return r->bmRequestType == 0x00 && r->wLength == 0 && r->wIndex == 0 &&
           (r->bRequest == TUSB_REQ_SET_ADDRESS ||
            (r->bRequest == TUSB_REQ_SET_CONFIGURATION && r->wValue == 1));
}

CONTROL_FAST static int match_rule(const rule_set_t* set, const tusb_control_request_t* r) {
    const uint8_t count = set->count;
    for (uint8_t i = 0; i < count; i++) {
        const rule_t* q = &set->rule[i];
        if ((r->bmRequestType & q->bm_mask) == q->bm_value &&
            (r->bRequest & q->request_mask) == q->request_value &&
            (r->wValue & q->value_mask) == q->value_value &&
            (r->wIndex & q->index_mask) == q->index_value)
            return i;
    }
    return -1;
}

// ---- staging (USB side) ----
void mimhid_control_reset(void) {
    rule_set_t* s = staged();
    s->count = 0;
    s->dirty_slot = 0;
}

static uint16_t get_u16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

uint8_t mimhid_control_stage(const uint8_t* p, uint16_t len) {
    if (len < 14)
        return MIMHID_NAK_PARAM;
    rule_set_t* s = staged();
    const uint8_t index = p[0];
    if (index > s->count)
        return MIMHID_NAK_PARAM; // the table is written in order
    if (index >= MIMHID_CONTROL_RULES)
        return MIMHID_NAK_OVERFLOW;
    rule_t r;
    memset(&r, 0, sizeof(r));
    r.bm_mask = p[1];
    r.bm_value = p[2];
    r.request_mask = p[3];
    r.request_value = p[4];
    r.value_mask = get_u16(&p[5]);
    r.value_value = get_u16(&p[7]);
    r.index_mask = get_u16(&p[9]);
    r.index_value = get_u16(&p[11]);
    r.action = p[13];
    // A value outside its mask never matches, which would only hide a mistake.
    if ((r.bm_value & ~r.bm_mask) || (r.request_value & ~r.request_mask) ||
        (r.value_value & ~r.value_mask) || (r.index_value & ~r.index_mask))
        return MIMHID_NAK_PARAM;
    const uint16_t answer_before = index ? s->rule[index - 1].answer_end : 0;
    const uint8_t slots_before = index ? s->rule[index - 1].slots_end : 0;
    r.answer_end = answer_before;
    r.slots_end = slots_before;
    r.next = NONE;
    switch (r.action) {
    case MIMHID_RULE_ANSWER: {
        if (len != 16)
            return MIMHID_NAK_PARAM;
        // The data stage of a device-to-host request only: USB 2.0 9.3.1 (bit 7 of bmRequestType).
        if ((r.bm_mask & 0x80) == 0 || (r.bm_value & 0x80) == 0)
            return MIMHID_NAK_PARAM;
        const uint16_t length = get_u16(&p[14]);
        if ((uint32_t)answer_before + length > MIMHID_ANSWER_BYTES)
            return MIMHID_NAK_OVERFLOW;
        r.offset = answer_before;
        r.length = length;
        r.d.total = length;
        r.d.used = length == 0; // nothing to send
        r.answer_end = (uint16_t)(answer_before + length);
        break;
    }
    case MIMHID_RULE_ANSWER_LAST_SENT:
        if (len != 17)
            return MIMHID_NAK_PARAM;
        if ((r.bm_mask & 0x80) == 0 || (r.bm_value & 0x80) == 0)
            return MIMHID_NAK_PARAM; // device to host only, as ANSWER
        // An IN endpoint address: bit 7 set, bits 6..4 zero, a number (USB 2.0 9.6.6).
        if ((p[14] & 0x80) == 0 || (p[14] & 0x70) != 0 || (p[14] & 0x0F) == 0 || p[15] > 1)
            return MIMHID_NAK_PARAM;
        if (slots_before >= MIMHID_LAST_SENT_SLOTS)
            return MIMHID_NAK_OVERFLOW;
        r.endpoint = p[14];
        r.filter = p[15];
        r.first = p[16];
        r.slot = slots_before;
        r.slots_end = (uint8_t)(slots_before + 1);
        break;
    case MIMHID_RULE_ASK:
        if (len != 17)
            return MIMHID_NAK_PARAM;
        r.timeout_ms = get_u16(&p[14]);
        r.after = p[16];
        if (r.timeout_ms == 0 || r.timeout_ms > MIMHID_CONTROL_TIMEOUT_MAX ||
            (r.after != MIMHID_REPLY_STALL && r.after != MIMHID_REPLY_DROP))
            return MIMHID_NAK_PARAM;
        break;
    case MIMHID_RULE_ACCEPT:
    case MIMHID_RULE_STALL:
        if (len != 14)
            return MIMHID_NAK_PARAM;
        break;
    default:
        return MIMHID_NAK_PARAM;
    }
    // The rules after this one are forgotten, and with them their last-sent slots from this rule's
    // first slot on.
    if (slots_before < s->dirty_slot)
        s->dirty_slot = slots_before;
    s->rule[index] = r;
    s->count = (uint8_t)(index + 1);
    return 0;
}

bool mimhid_control_staged_answer(uint8_t index, mimhid_desc_t** desc, uint8_t** bytes) {
    rule_set_t* s = staged();
    if (index >= s->count || s->rule[index].action != MIMHID_RULE_ANSWER)
        return false;
    *desc = &s->rule[index].d;
    *bytes = &s->answer[s->rule[index].offset];
    return true;
}

uint8_t mimhid_control_check(const mimhid_layout_t* layout, uint16_t count) {
    const rule_set_t* s = staged();
    if (count == MIMHID_CONTROL_ALL)
        count = s->count;
    if (count > s->count)
        return MIMHID_NAK_PARAM;
    for (uint16_t i = 0; i < count; i++) {
        const rule_t* r = &s->rule[i];
        if (r->action == MIMHID_RULE_ANSWER && !(r->d.used && r->d.recv == r->d.total))
            return MIMHID_NAK_STATE; // the bytes it answers with are not all here
        if (r->action == MIMHID_RULE_ANSWER_LAST_SENT &&
            layout->ep[1][r->endpoint & 0x0F].kind == MIMHID_EP_NONE)
            return MIMHID_NAK_STATE; // not an IN endpoint of the configuration
    }
    return 0;
}

// ---- the interrupt side ----
CONTROL_FAST static void note_push(uint8_t token, uint8_t flags, const tusb_control_request_t* r) {
    note_t* n = &g_note[g_note_head];
    n->token = token;
    n->flags = flags;
    memcpy(n->setup, r, 8);
    __atomic_store_n(&g_note_head, (uint8_t)((g_note_head + 1) & NOTE_MASK), __ATOMIC_RELEASE);
}

CONTROL_FAST static bool note_room(void) {
    return ((g_note_head + 1) & NOTE_MASK) != __atomic_load_n(&g_note_tail, __ATOMIC_ACQUIRE);
}

CONTROL_FAST static void drop_held(void) {
    g_hold.state = HOLD_IDLE;
    g_expect = 0;
    g_answer.armed = false;
    g_answer.serving = NONE;
    g_suppress_stall = false;
}

// The one request whose answer the specification fixes whatever the product is: GetStatus() to an
// interface that exists returns two bytes of zero (USB 2.0 9.4.5, figure 9-5). TinyUSB's generic
// class driver (generic.c) does not answer it, so without a rule the board would STALL a request
// every device must serve. An address-state device, a missing interface or a wLength other than 2
// is left to TinyUSB.
CONTROL_FAST static bool is_interface_status(const tusb_control_request_t* r) {
    return r->bmRequestType == 0x81 && r->bRequest == TUSB_REQ_GET_STATUS && r->wValue == 0 &&
           r->wLength == 2 && r->wIndex < mimhid_active_interfaces() && tud_mounted();
}

// Gives a request nothing holds to TinyUSB. An interface status is retyped as a vendor request, as
// an answer from the host is, and tud_vendor_control_xfer_cb sends its two zero bytes.
CONTROL_FAST static void pass_on(const dcd_event_t* event, bool in_isr) {
    if (!is_interface_status(&event->setup_received)) {
        __real_dcd_event_handler(event, in_isr);
        return;
    }
    dcd_event_t retyped = *event;
    retyped.setup_received.bmRequestType = 0xC1; // vendor, device to host, interface
    g_answer.action = ANS_HOST;
    g_answer.token = 0;
    g_answer.length = 2;
    g_answer.timeout_ms = 0;
    g_answer.timeout_action = MIMHID_REPLY_STALL;
    g_answer.buf[1] = 0;
    g_answer.buf[2] = 0;
    g_answer.request = retyped.setup_received;
    g_answer.active = false;
    g_answer.armed = true;
    __real_dcd_event_handler(&retyped, in_isr);
}

// Hands a request that matched a rule of the board's own (ANSWER, ANSWER_LAST_SENT, ACCEPT, STALL)
// to TinyUSB retyped as a vendor request, which tud_vendor_control_xfer_cb answers. A request
// without a data stage (wLength 0) has an IN zero-length packet as its status stage whatever its
// direction, so it is retyped as host to device.
CONTROL_FAST static void start_rule_answer(const dcd_event_t* event, bool in_isr, uint8_t action,
                                           uint8_t token, uint8_t rule, uint8_t slot,
                                           const uint8_t* src, uint16_t length) {
    dcd_event_t retyped = *event;
    uint8_t bm = (uint8_t)((event->setup_received.bmRequestType & 0x9F) | 0x40); // type: vendor
    if (event->setup_received.wLength == 0)
        bm &= 0x7F;
    retyped.setup_received.bmRequestType = bm;
    g_answer.action = action;
    g_answer.token = token;
    g_answer.length = length;
    g_answer.timeout_ms = 0;
    g_answer.timeout_action = MIMHID_REPLY_STALL;
    g_answer.slot = slot;
    g_answer.src = src;
    g_answer.request = retyped.setup_received;
    g_answer.active = false;
    g_answer.serving = rule;
    g_answer.armed = true;
    __real_dcd_event_handler(&retyped, in_isr);
}

CONTROL_FAST void __wrap_dcd_event_handler(dcd_event_t const* event, bool in_isr) {
    if (mimhid_ep0_event(event, in_isr)) return;
    if (event->event_id != DCD_EVENT_SETUP_RECEIVED) {
        // The host has left or reset the bus: whatever it asked is void.
        if (event->event_id == DCD_EVENT_BUS_RESET || event->event_id == DCD_EVENT_UNPLUGGED ||
            event->event_id == DCD_EVENT_SUSPEND)
            drop_held();
        __real_dcd_event_handler(event, in_isr);
        return;
    }

    g_suppress_stall = false;
    const tusb_control_request_t* r = &event->setup_received;
    // A new request means the host gave up on the one it was waiting for.
    drop_held();

    const rule_set_t* set = &g_set[g_act];
    const bool fixed = is_fixed(r);
    const bool too_long = r->bmRequestType_bit.direction == TUSB_DIR_OUT &&
                          r->wLength > MIMHID_CONTROL_DATA_MAX;
    int rule = fixed ? -1 : match_rule(set, r);
    const rule_t* q = rule >= 0 ? &set->rule[rule] : NULL;
    uint8_t action = q != NULL ? q->action : 0;
    // A data stage the board cannot take goes to TinyUSB, which stalls what nobody answers. An
    // ACCEPT rule says so to the host; any other rule leaves it silent, as before.
    if (too_long && action != MIMHID_RULE_ACCEPT) {
        rule = -1;
        q = NULL;
        action = 0;
    }
    const bool taken = !too_long && (action == MIMHID_RULE_ASK || action == MIMHID_RULE_ACCEPT);
    const bool report = g_report_all || action == MIMHID_RULE_ASK || action == MIMHID_RULE_ACCEPT;
    uint8_t token = 0;
    if (report) {
        if (note_room()) {
            if (++g_token == 0) g_token = 1;
            token = g_token;
            uint8_t flags = 0;
            if (taken && action == MIMHID_RULE_ASK) flags |= MIMHID_CONTROL_F_HELD;
            if (taken && action == MIMHID_RULE_ACCEPT) flags |= MIMHID_CONTROL_F_ACCEPTED;
            if (too_long) flags |= MIMHID_CONTROL_F_TOO_LONG;
            if (fixed) flags |= MIMHID_CONTROL_F_FIXED;
            if (g_lost) {
                flags |= MIMHID_CONTROL_F_LOST;
                g_lost = false;
            }
            note_push(token, flags, r);
        } else {
            g_lost = true;
        }
    }
    // What cannot be reported is not held or taken: the host would not know to answer.
    if (q == NULL || (taken && token == 0) || too_long) {
        pass_on(event, in_isr);
        return;
    }

    switch (action) {
    case MIMHID_RULE_ASK:
        g_hold.event = *event;
        g_hold.token = token;
        g_hold.rule = (uint8_t)rule;
        g_hold.timeout_ms = q->timeout_ms;
        g_hold.after = q->after;
        g_hold.deadline_ms = hal_millis() + q->timeout_ms;
        g_reply.full = 0;
        g_expect = token;
        __atomic_store_n(&g_hold.state, HOLD_HELD, __ATOMIC_RELEASE);
        return;
    case MIMHID_RULE_ANSWER:
        start_rule_answer(event, in_isr, ANS_RULE, token, (uint8_t)rule, NONE, &set->answer[q->offset],
                          q->length);
        return;
    case MIMHID_RULE_ANSWER_LAST_SENT:
        start_rule_answer(event, in_isr, ANS_LAST, token, NONE, q->slot, NULL, 0);
        return;
    case MIMHID_RULE_ACCEPT:
        start_rule_answer(event, in_isr, ANS_ACCEPT, token, NONE, NONE, NULL, 0);
        return;
    default: // STALL
        start_rule_answer(event, in_isr, ANS_STALL, token, NONE, NONE, NULL, 0);
        return;
    }
}

// ---- the receive side ----
uint8_t mimhid_control_reply(const uint8_t* p, uint16_t len) {
    if (len < 2 || p[1] > MIMHID_REPLY_DROP) return MIMHID_NAK_PARAM;
    if (len - 2u > MIMHID_CONTROL_DATA_MAX) return MIMHID_NAK_OVERFLOW;
    if (p[0] == 0 || p[0] != g_expect) return MIMHID_NAK_STATE;
    if (g_reply.full) return MIMHID_NAK_BUSY;
    g_reply.token = p[0];
    g_reply.action = p[1];
    g_reply.length = (uint16_t)(len - 2);
    memcpy(g_reply.data, &p[2], g_reply.length);
    __atomic_store_n(&g_reply.full, 1, __ATOMIC_RELEASE);
    return 0;
}

// ---- the USB side ----
// Gives the held transfer, if it is still the one with this token, to TinyUSB: as it came when
// the action is LEAVE, and otherwise retyped as a vendor request that tud_vendor_control_xfer_cb
// answers.
static void settle(uint8_t token, uint8_t action, const uint8_t* data, uint16_t length) {
    dcd_event_t event;
    dcd_int_disable(RHPORT);
    if (g_hold.state != HOLD_HELD || g_hold.token != token) {
        dcd_int_enable(RHPORT);
        return;
    }
    event = g_hold.event;
    if (action == BY_RULE) action = g_hold.after;
    g_hold.state = HOLD_IDLE;
    g_expect = 0;
    if (action == MIMHID_REPLY_DROP) {
        g_suppress_stall = true;
        hal_control_silent_drop();
        dcd_int_enable(RHPORT);
        return;
    }
    if (action != MIMHID_REPLY_LEAVE) {
        g_answer.action = action == MIMHID_REPLY_STALL ? ANS_STALL : ANS_HOST;
        g_answer.token = token;
        g_answer.length = length;
        g_answer.timeout_ms = g_hold.timeout_ms;
        g_answer.timeout_action = g_hold.after;
        if (length) memcpy(&g_answer.buf[1], data, length);
        uint8_t bm = (uint8_t)((event.setup_received.bmRequestType & 0x9F) | 0x40); // type: vendor
        // No data stage and no answer data: the status stage is an IN ZLP. An answer with data goes out
        // whatever wLength says, as the real device does.
        if (event.setup_received.wLength == 0 && length == 0) bm &= 0x7F;
        event.setup_received.bmRequestType = bm;
        g_answer.request = event.setup_received;
        g_answer.active = false;
        g_answer.armed = true;
    }
    dcd_int_enable(RHPORT);
    __real_dcd_event_handler(&event, false);
}

// Reports the transfers the interrupt has seen.
static void flush_notes(void) {
    while (g_note_tail != __atomic_load_n(&g_note_head, __ATOMIC_ACQUIRE)) {
        const note_t* n = &g_note[g_note_tail];
        uint8_t ev[2 + 8];
        ev[0] = n->token;
        ev[1] = n->flags;
        memcpy(&ev[2], n->setup, 8);
        mimhid_send_event(MIMHID_EV_CONTROL_SETUP, ev, sizeof(ev));
        __atomic_store_n(&g_note_tail, (uint8_t)((g_note_tail + 1) & NOTE_MASK), __ATOMIC_RELEASE);
    }
}

void mimhid_control_poll(void) {
    flush_notes();

    if (__atomic_load_n(&g_hold.state, __ATOMIC_ACQUIRE) != HOLD_HELD) return;
    const uint8_t token = g_hold.token;
    if (__atomic_load_n(&g_reply.full, __ATOMIC_ACQUIRE)) {
        if (g_reply.token == token) settle(token, g_reply.action, g_reply.data, g_reply.length);
        g_reply.full = 0;
    } else if ((int32_t)(hal_millis() - g_hold.deadline_ms) >= 0) {
        settle(token, BY_RULE, NULL, 0);
    }
}

// ---- applying the staged table (USB side) ----

// The lists of ANSWER_LAST_SENT rules by endpoint, for the table that is about to become active.
// `next` is read by the USB side only.
static void build_sent_lists(rule_set_t* s) {
    memset(mimhid_control_sent_head, NONE, sizeof(mimhid_control_sent_head));
    for (uint8_t i = 0; i < s->count; i++) {
        rule_t* r = &s->rule[i];
        if (r->action != MIMHID_RULE_ANSWER_LAST_SENT)
            continue;
        r->next = mimhid_control_sent_head[r->endpoint & 0x0F];
        mimhid_control_sent_head[r->endpoint & 0x0F] = i;
    }
}

uint8_t mimhid_control_apply(bool report_all, uint16_t count, bool committed) {
    rule_set_t* s = staged();
    if (count == MIMHID_CONTROL_ALL)
        count = s->count;
    if (count > s->count)
        return MIMHID_NAK_PARAM;
    dcd_event_t released;
    dcd_int_disable(RHPORT);
    // A rule being answered from the answer memory keeps the table it is in: the new staged copy
    // would be written over it.
    if (!committed && g_answer.serving != NONE) {
        dcd_int_enable(RHPORT);
        return MIMHID_NAK_BUSY;
    }
    dcd_int_enable(RHPORT);

    // The rules after `count` are forgotten, and the lists of the last-sent rules made, before the
    // table is visible to the interrupt.
    s->count = (uint8_t)count;
    build_sent_lists(s);

    dcd_int_disable(RHPORT);
    // A transfer held under the old rules goes to TinyUSB as if it had not matched. A COMMIT also
    // drops what is being answered: the target is about to see another device.
    const bool had_hold = g_hold.state == HOLD_HELD;
    if (had_hold) released = g_hold.event;
    drop_held();
    if (committed)
        g_answer.active = false;
    g_act = (uint8_t)(1u - g_act);
    g_report_all = report_all;
    dcd_int_enable(RHPORT);
    if (had_hold && !committed) __real_dcd_event_handler(&released, false);

    // What an ANSWER_LAST_SENT rule held belongs to the rule: a rule that was not changed keeps it
    // (RULES_APPLY), and a new configuration starts all of them empty.
    const uint8_t from = committed ? 0 : g_set[g_act].dirty_slot;
    for (uint8_t i = from; i < MIMHID_LAST_SENT_SLOTS; i++)
        g_sent[i].valid = false;
    // The table that was active is the staged one now, with the previous rules in it: it takes the
    // new ones, so that the next RULE writes into a copy of what runs.
    memcpy(&g_set[1u - g_act], &g_set[g_act], sizeof(rule_set_t));
    g_set[1u - g_act].dirty_slot = NONE;
    g_set[g_act].dirty_slot = NONE;
    return 0;
}

uint8_t mimhid_control_rule_data(const uint8_t* p, uint16_t len) {
    if (len < 1)
        return MIMHID_NAK_PARAM;
    const uint8_t index = p[0];
    const uint16_t n = (uint16_t)(len - 1);
    dcd_int_disable(RHPORT);
    rule_set_t* a = &g_set[g_act];
    rule_set_t* s = staged();
    if (index >= a->count || a->rule[index].action != MIMHID_RULE_ANSWER) {
        dcd_int_enable(RHPORT);
        return MIMHID_NAK_PARAM;
    }
    rule_t* r = &a->rule[index];
    if (n > r->d.total) {
        dcd_int_enable(RHPORT);
        return MIMHID_NAK_OVERFLOW;
    }
    if (g_answer.serving == index) {
        dcd_int_enable(RHPORT);
        return MIMHID_NAK_BUSY; // being sent: it would go out half old, half new
    }
    // The interrupt is masked for the copy, so it sees the old bytes with the old length or the new
    // ones with the new length. (A rule is answered from here from its setup packet until the end
    // of its data stage, which `serving` covers.)
    memcpy(&a->answer[r->offset], &p[1], n);
    r->length = n;
    // The staged table is a copy of the active one until the host stages something else; it keeps
    // the change if its rule is still this one.
    if (index < s->count && s->rule[index].action == MIMHID_RULE_ANSWER &&
        s->rule[index].offset == r->offset && s->rule[index].d.total == r->d.total) {
        memcpy(&s->answer[r->offset], &p[1], n);
        s->rule[index].length = n;
    }
    dcd_int_enable(RHPORT);
    return 0;
}

void mimhid_control_detach(void) {
    dcd_int_disable(RHPORT);
    drop_held();
    g_answer.active = false;
    g_set[g_act].count = 0;
    dcd_int_enable(RHPORT);
    memset(mimhid_control_sent_head, NONE, sizeof(mimhid_control_sent_head));
    mimhid_control_forget_sent();
}

void mimhid_control_forget_sent(void) {
    for (uint8_t i = 0; i < MIMHID_LAST_SENT_SLOTS; i++)
        g_sent[i].valid = false;
}

void mimhid_control_sent_rules(uint8_t number, const uint8_t* data, uint16_t len) {
    const rule_set_t* a = &g_set[g_act];
    for (uint8_t i = mimhid_control_sent_head[number & 0x0F]; i != NONE; i = a->rule[i].next) {
        const rule_t* r = &a->rule[i];
        if (r->filter && (len == 0 || data[0] != r->first))
            continue;
        if (len > sizeof(g_sent[0].data))
            continue; // cannot happen: a transfer is at most one endpoint packet
        memcpy(g_sent[r->slot].data, data, len);
        g_sent[r->slot].len = len;
        g_sent[r->slot].valid = true;
    }
}

// Waits for the host to say yes or no to a host-to-device transfer whose data it has now seen.
// Returns true to acknowledge the status stage.
static bool wait_for_verdict(void) {
    g_reply.full = 0;
    g_expect = g_answer.token;
    g_answer.buf[0] = g_answer.token;
    flush_notes(); // the setup first, so the host knows what the data belongs to
    mimhid_send_event(MIMHID_EV_CONTROL_DATA, g_answer.buf, (uint16_t)(1 + g_answer.length));

    uint8_t action = g_answer.timeout_action;
    const uint32_t start = hal_millis();
    while ((uint32_t)(hal_millis() - start) < g_answer.timeout_ms) {
        if (g_single_context) mimhid_rx_poll();
        if (__atomic_load_n(&g_reply.full, __ATOMIC_ACQUIRE)) {
            if (g_reply.token == g_answer.token) action = g_reply.action;
            g_reply.full = 0;
            break;
        }
    }
    g_expect = 0;
    if (action == MIMHID_REPLY_DROP) {
        g_suppress_stall = true;
        hal_control_silent_drop();
        return false;
    }
    return action != MIMHID_REPLY_STALL;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const* request) {
    switch (stage) {
    case CONTROL_STAGE_SETUP:
        // A vendor request nobody answered is refused, as it was before.
        if (!g_answer.armed || memcmp(request, &g_answer.request, sizeof(*request)) != 0) return false;
        g_answer.armed = false;
        if (g_answer.action == ANS_STALL) {
            g_answer.serving = NONE;
            return false;
        }
        g_answer.active = true;
        if (request->wLength == 0 && (g_answer.length == 0 || g_answer.action != ANS_HOST))
            return tud_control_status(rhport, request);
        if (g_answer.action == ANS_RULE) {
            // The bytes are read from the answer memory by the stack until the data stage is done;
            // `serving` keeps RULE_DATA and RULES_APPLY away from them meanwhile. The stack cuts
            // what it sends to wLength (usbd_control.c, tud_control_xfer).
            return tud_control_xfer(rhport, request, (void*)(uintptr_t)g_answer.src, g_answer.length);
        }
        if (g_answer.action == ANS_LAST) {
            // The last transfer on the endpoint, copied now, in the context that writes it: the
            // answer is this copy, and a transfer started while it goes out cannot change it.
            if (!g_sent[g_answer.slot].valid) return false; // none was sent yet
            g_answer.length = g_sent[g_answer.slot].len;
            memcpy(&g_answer.buf[1], g_sent[g_answer.slot].data, g_answer.length);
            return tud_control_xfer(rhport, request, &g_answer.buf[1], g_answer.length);
        }
        if (request->bmRequestType_bit.direction == TUSB_DIR_IN) {
            if (g_answer.action == ANS_ACCEPT) {
                // An empty data stage: nothing is there to take or give.
                g_answer.length = 0;
                return tud_control_xfer(rhport, request, &g_answer.buf[1], 0);
            }
            // An answer longer than wLength goes out whole; a shorter one keeps wLength, so the
            // stack ends it with a zero-length packet when it fills its last packet.
            tusb_control_request_t req = *request;
            if (req.wLength < g_answer.length) req.wLength = g_answer.length;
            return tud_control_xfer(rhport, &req, &g_answer.buf[1], g_answer.length);
        }
        // Host to device: take the data first; the verdict (or the event, for ACCEPT) comes once it
        // has been seen.
        memset(&g_answer.buf[1], 0, request->wLength);
        g_answer.length = request->wLength;
        return tud_control_xfer(rhport, request, &g_answer.buf[1], request->wLength);

    case CONTROL_STAGE_DATA:
        if (!g_answer.active) return false;
        if (request->bmRequestType_bit.direction == TUSB_DIR_IN) {
            g_answer.serving = NONE; // the last byte has been given to the controller
            return true;
        }
        if (g_answer.action == ANS_ACCEPT) {
            g_answer.buf[0] = g_answer.token;
            flush_notes(); // the setup first, so the host knows what the data belongs to
            mimhid_send_event(MIMHID_EV_CONTROL_DATA, g_answer.buf, (uint16_t)(1 + g_answer.length));
            return true;
        }
        return wait_for_verdict();

    default: // CONTROL_STAGE_ACK
        g_answer.active = false;
        g_answer.serving = NONE;
        return true;
    }
}
