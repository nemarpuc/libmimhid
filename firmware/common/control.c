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

typedef struct {
    uint8_t bm_mask;
    uint8_t bm_value;
    uint8_t request_mask;
    uint8_t request_value;
    uint16_t value_mask;
    uint16_t value_value;
    uint16_t index_mask;
    uint16_t index_value;
    uint16_t timeout_ms;
    uint8_t action;
} rule_t;

typedef struct {
    uint8_t token;
    uint8_t flags;
    uint8_t setup[8];
} note_t;

// The rules are read in the interrupt and replaced with it masked.
static rule_t g_rule[MIMHID_CONTROL_RULES];
static volatile uint8_t g_rule_count;
static volatile bool g_report_all;

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
    uint32_t deadline_ms;
} g_hold;

// What the host's answer needs from the vendor callback: `armed` from the moment the retyped
// request is queued until its setup stage arrives, `active` from there to the end of the transfer.
// buf[0] is the token that CONTROL_DATA starts with; buf[1..] is the data of the answer, or of
// the host-to-device data stage.
static struct {
    volatile bool armed;
    volatile bool active;
    tusb_control_request_t request;
    uint8_t action;
    uint8_t token;
    uint16_t length;
    uint16_t timeout_ms;
    uint8_t timeout_action;
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

// ---- rules ----
CONTROL_FAST static bool is_fixed(const tusb_control_request_t* r) {
    return r->bmRequestType == 0x00 && r->wLength == 0 && r->wIndex == 0 &&
           (r->bRequest == TUSB_REQ_SET_ADDRESS ||
            (r->bRequest == TUSB_REQ_SET_CONFIGURATION && r->wValue == 1));
}

CONTROL_FAST static int match_rule(const tusb_control_request_t* r) {
    const uint8_t count = g_rule_count;
    for (uint8_t i = 0; i < count; i++) {
        const rule_t* q = &g_rule[i];
        if ((r->bmRequestType & q->bm_mask) == q->bm_value &&
            (r->bRequest & q->request_mask) == q->request_value &&
            (r->wValue & q->value_mask) == q->value_value &&
            (r->wIndex & q->index_mask) == q->index_value)
            return i;
    }
    return -1;
}

uint8_t mimhid_control_config(const uint8_t* p, uint16_t len) {
    if (len < 2 || (p[0] & ~1u) != 0 || p[1] > MIMHID_CONTROL_RULES ||
        len != 2u + (uint16_t)p[1] * MIMHID_CONTROL_RULE_LEN)
        return MIMHID_NAK_PARAM;
    const uint8_t count = p[1];
    rule_t staged[MIMHID_CONTROL_RULES];
    for (uint8_t i = 0; i < count; i++) {
        const uint8_t* q = &p[2 + i * MIMHID_CONTROL_RULE_LEN];
        rule_t* r = &staged[i];
        r->bm_mask = q[0];
        r->bm_value = q[1];
        r->request_mask = q[2];
        r->request_value = q[3];
        r->value_mask = (uint16_t)(q[4] | (q[5] << 8));
        r->value_value = (uint16_t)(q[6] | (q[7] << 8));
        r->index_mask = (uint16_t)(q[8] | (q[9] << 8));
        r->index_value = (uint16_t)(q[10] | (q[11] << 8));
        r->timeout_ms = (uint16_t)(q[12] | (q[13] << 8));
        r->action = q[14];
        if (r->timeout_ms == 0 || r->timeout_ms > MIMHID_CONTROL_TIMEOUT_MAX ||
            r->action > MIMHID_CONTROL_DROP)
            return MIMHID_NAK_PARAM;
        // A value outside its mask never matches, which would only hide a mistake.
        if ((r->bm_value & ~r->bm_mask) || (r->request_value & ~r->request_mask) ||
            (r->value_value & ~r->value_mask) || (r->index_value & ~r->index_mask))
            return MIMHID_NAK_PARAM;
    }

    // A transfer held under the old rules goes to TinyUSB as if it had not matched.
    dcd_event_t released;
    dcd_int_disable(RHPORT);
    const bool had_hold = g_hold.state == HOLD_HELD;
    if (had_hold) {
        released = g_hold.event;
        g_hold.state = HOLD_IDLE;
        g_expect = 0;
    }
    memcpy(g_rule, staged, sizeof(rule_t) * count);
    g_rule_count = count;
    g_report_all = (p[0] & 1u) != 0;
    dcd_int_enable(RHPORT);
    if (had_hold) __real_dcd_event_handler(&released, false);
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
    g_suppress_stall = false;
}

CONTROL_FAST void __wrap_dcd_event_handler(dcd_event_t const* event, bool in_isr) {
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

    const bool fixed = is_fixed(r);
    const bool too_long = r->bmRequestType_bit.direction == TUSB_DIR_OUT &&
                          r->wLength > MIMHID_CONTROL_DATA_MAX;
    const int rule = (fixed || too_long) ? -1 : match_rule(r);
    if (rule < 0 && !g_report_all) {
        __real_dcd_event_handler(event, in_isr);
        return;
    }
    if (!note_room()) {
        g_lost = true;
        __real_dcd_event_handler(event, in_isr);
        return;
    }

    if (++g_token == 0) g_token = 1;
    uint8_t flags = 0;
    if (rule >= 0) flags |= MIMHID_CONTROL_F_HELD;
    if (too_long) flags |= MIMHID_CONTROL_F_TOO_LONG;
    if (fixed) flags |= MIMHID_CONTROL_F_FIXED;
    if (g_lost) {
        flags |= MIMHID_CONTROL_F_LOST;
        g_lost = false;
    }
    note_push(g_token, flags, r);

    if (rule < 0) {
        __real_dcd_event_handler(event, in_isr);
        return;
    }
    g_hold.event = *event;
    g_hold.token = g_token;
    g_hold.rule = (uint8_t)rule;
    g_hold.deadline_ms = hal_millis() + g_rule[rule].timeout_ms;
    g_reply.full = 0;
    g_expect = g_token;
    __atomic_store_n(&g_hold.state, HOLD_HELD, __ATOMIC_RELEASE);
}

// ---- the receive side ----
uint8_t mimhid_control_reply(const uint8_t* p, uint16_t len) {
    if (len < 2 || p[1] > MIMHID_CONTROL_DROP) return MIMHID_NAK_PARAM;
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
    const rule_t* rule = &g_rule[g_hold.rule];
    if (action == BY_RULE) action = rule->action;
    g_hold.state = HOLD_IDLE;
    g_expect = 0;
    if (action == MIMHID_CONTROL_DROP) {
        g_suppress_stall = true;
        hal_control_silent_drop();
        dcd_int_enable(RHPORT);
        return;
    }
    if (action != MIMHID_CONTROL_LEAVE) {
        g_answer.action = action;
        g_answer.token = token;
        g_answer.length = length;
        g_answer.timeout_ms = rule->timeout_ms;
        g_answer.timeout_action = rule->action;
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

void mimhid_control_clear(void) {
    dcd_int_disable(RHPORT);
    drop_held();
    g_answer.active = false;
    dcd_int_enable(RHPORT);
}

void mimhid_control_poll(void) {
    while (g_note_tail != __atomic_load_n(&g_note_head, __ATOMIC_ACQUIRE)) {
        const note_t* n = &g_note[g_note_tail];
        uint8_t ev[2 + 8];
        ev[0] = n->token;
        ev[1] = n->flags;
        memcpy(&ev[2], n->setup, 8);
        mimhid_send_event(MIMHID_EV_CONTROL_SETUP, ev, sizeof(ev));
        __atomic_store_n(&g_note_tail, (uint8_t)((g_note_tail + 1) & NOTE_MASK), __ATOMIC_RELEASE);
    }

    if (__atomic_load_n(&g_hold.state, __ATOMIC_ACQUIRE) != HOLD_HELD) return;
    const uint8_t token = g_hold.token;
    if (__atomic_load_n(&g_reply.full, __ATOMIC_ACQUIRE)) {
        if (g_reply.token == token) settle(token, g_reply.action, g_reply.data, g_reply.length);
        g_reply.full = 0;
    } else if ((int32_t)(hal_millis() - g_hold.deadline_ms) >= 0) {
        settle(token, BY_RULE, NULL, 0);
    }
}

// Waits for the host to say yes or no to a host-to-device transfer whose data it has now seen.
// Returns true to acknowledge the status stage.
static bool wait_for_verdict(void) {
    g_reply.full = 0;
    g_expect = g_answer.token;
    g_answer.buf[0] = g_answer.token;
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
    if (action == MIMHID_CONTROL_DROP) {
        g_suppress_stall = true;
        hal_control_silent_drop();
        return false;
    }
    return action != MIMHID_CONTROL_STALL;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const* request) {
    switch (stage) {
    case CONTROL_STAGE_SETUP:
        // A vendor request the host did not answer is refused, as it was before.
        if (!g_answer.armed || memcmp(request, &g_answer.request, sizeof(*request)) != 0) return false;
        g_answer.armed = false;
        if (g_answer.action == MIMHID_CONTROL_STALL) return false;
        g_answer.active = true;
        if (request->wLength == 0 && g_answer.length == 0) return tud_control_status(rhport, request);
        if (request->bmRequestType_bit.direction == TUSB_DIR_IN) {
            tusb_control_request_t req = *request;
            req.wLength = g_answer.length;
            return tud_control_xfer(rhport, &req, &g_answer.buf[1], g_answer.length);
        }
        // Host to device: take the data first; the verdict comes once it has been seen.
        memset(&g_answer.buf[1], 0, request->wLength);
        g_answer.length = request->wLength;
        return tud_control_xfer(rhport, request, &g_answer.buf[1], request->wLength);

    case CONTROL_STAGE_DATA:
        if (!g_answer.active) return false;
        if (request->bmRequestType_bit.direction == TUSB_DIR_IN) return true;
        return wait_for_verdict();

    default: // CONTROL_STAGE_ACK
        g_answer.active = false;
        return true;
    }
}
