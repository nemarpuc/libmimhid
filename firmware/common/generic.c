// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#include "generic.h"
#include "config.h"
#include "core.h"
#include "tusb.h"
#include "device/usbd_pvt.h"
#include <string.h>

// ---------------------------------------------------------------------------------------------
// The pool of IN slots, and the queue of each IN endpoint.
//
// A slot holds one transfer. The queue of an endpoint is a ring of `n` slots of the pool, with a
// head that only the receive side writes and a tail that only the USB side writes, as the report
// queues in core.c are.
//
// The transfer on the bus is not in the ring. When the USB side starts a transfer it copies the
// slot at the tail into the endpoint's own buffer (g_bus) and frees the slot at once. The
// controller may read that buffer until the transfer completes, which with a target that does not
// poll is as long as the target likes; a slot of the ring is free to be written again as soon as
// the tail has passed it (a flush, a stall and DROP_OLDEST all move the tail past slots), so the
// bytes on the bus must not live in one. The copy is the length of the transfer, as the one
// TinyUSB's HID driver makes for a report (hidd.c, tud_hid_n_report).
//
// A ring of n slots holds n - 1 queued transfers at most (head == tail means empty, so the ring is
// never filled). `depth` is how many transfers wait in the ring, so a queue costs depth + 1 slots.
// DROP_OLDEST needs room to run ahead of the USB side, which removes the oldest transfers when it
// next looks; it costs 2 * depth + 1.
// ---------------------------------------------------------------------------------------------

typedef struct {
    _Alignas(4) uint8_t data[MIMHID_MAX_TRANSFER_BYTES];
    uint16_t len;
} slot_t;

static slot_t g_pool[MIMHID_EP_POOL_SLOTS];

// The transfer each IN endpoint has on the bus. The USB side only.
static struct {
    _Alignas(4) uint8_t data[MIMHID_MAX_TRANSFER_BYTES];
} g_bus[MIMHID_MAX_ENDPOINT_NUMBER + 1];

typedef struct {
    // Written by the USB side at COMMIT, read by both.
    uint32_t geom; // (slots << 16) | first slot of the pool; 0 slots: not an endpoint of ours
    // The producer.
    volatile uint16_t head;
    volatile uint32_t prod_gen; // the generation (g_gen) the producer has reset `head` for
    // The consumer.
    volatile uint16_t tail;
    // Settings, either side reads them: `depth` and `policy` change live.
    volatile uint8_t depth;
    volatile uint8_t policy;
    volatile uint32_t lost;
    // The USB side only.
    uint16_t base;
    uint16_t n;
    bool inflight; // a transfer is on the bus, from g_bus
    bool orphan; // a flush forgot the transfer on the bus; its completion is not counted
    bool open;   // the target configured the device: the endpoint is open
    uint16_t done; // completions not yet reported (wait policy only)
} in_t;

static in_t g_in[16];

typedef struct {
    _Alignas(4) uint8_t buf[MIMHID_MAX_ENDPOINT_BYTES];
    uint16_t packet;
    bool generic; // an OUT endpoint of the generic driver in the active configuration
    bool open;
    bool armed;
} out_t;

static out_t g_out[MIMHID_MAX_ENDPOINT_NUMBER + 1];

// The generation of the queues: odd while COMMIT cuts the pool again, even otherwise. The receive
// side reads a queue's geometry and head without a lock, so a frame it began under one generation
// is dropped when it ends under another. Only the USB side writes it.
static volatile uint32_t g_gen;

static volatile bool g_loss_on;

static uint16_t slots_of(uint8_t depth, uint8_t policy) {
    if (depth == 0)
        return 0;
    switch (policy) {
    case MIMHID_EP_FULL_DROP_NEWEST:
    case MIMHID_EP_FULL_WAIT:
        return (uint16_t)(depth + 1u);
    case MIMHID_EP_FULL_DROP_OLDEST:
        return (uint16_t)(2u * depth + 1u);
    default:
        return 0;
    }
}

uint16_t mimhid_generic_slots(uint8_t depth, uint8_t policy) { return slots_of(depth, policy); }

// ---- COMMIT ----

bool mimhid_generic_plan(const mimhid_config_t* cfg, const mimhid_layout_t* layout, uint8_t* code) {
    uint32_t total = 0;
    for (uint8_t number = 1; number < 16; number++) {
        if (layout->ep[1][number].kind != MIMHID_EP_GENERIC)
            continue;
        const uint16_t cost = slots_of(cfg->ep[number].depth, cfg->ep[number].policy);
        if (cost == 0) {
            *code = MIMHID_NAK_STATE; // no EP_CONFIG for this endpoint
            return false;
        }
        total += cost;
    }
    if (total > MIMHID_EP_POOL_SLOTS) {
        *code = MIMHID_NAK_OVERFLOW;
        return false;
    }
    return true;
}

void mimhid_generic_apply(const mimhid_config_t* cfg, const mimhid_layout_t* layout) {
    __atomic_store_n(&g_gen, g_gen + 1u, __ATOMIC_RELEASE); // odd: being cut
    uint16_t next = 0;
    for (uint8_t number = 1; number < 16; number++) {
        in_t* q = &g_in[number];
        const mimhid_ep_info_t* in = &layout->ep[1][number];
        q->open = false;
        q->inflight = false;
        q->orphan = false;
        q->done = 0;
        q->lost = 0;
        q->tail = 0;
        q->n = 0;
        q->geom = 0;
        if (in->kind == MIMHID_EP_GENERIC) {
            const uint16_t n = slots_of(cfg->ep[number].depth, cfg->ep[number].policy);
            q->base = next;
            q->n = n;
            q->depth = cfg->ep[number].depth;
            q->policy = cfg->ep[number].policy;
            next = (uint16_t)(next + n);
            q->geom = ((uint32_t)n << 16) | q->base;
        }
        if (number <= MIMHID_MAX_ENDPOINT_NUMBER) {
            out_t* o = &g_out[number];
            o->open = false;
            o->armed = false;
            o->generic = layout->ep[0][number].kind == MIMHID_EP_GENERIC;
            o->packet = layout->ep[0][number].packet_size;
        }
    }
    __atomic_store_n(&g_gen, g_gen + 1u, __ATOMIC_RELEASE); // even: cut
}

// ---- live changes ----

static in_t* active_in(uint8_t address) {
    if ((address & 0x80) == 0 || (address & 0x70) != 0)
        return NULL;
    const uint8_t number = address & 0x0F;
    if (number == 0 || g_in[number].n == 0)
        return NULL;
    return &g_in[number];
}

static out_t* active_out(uint8_t address) {
    const uint8_t number = address & 0x0F;
    if ((address & 0xF0) != 0 || number == 0 || number > MIMHID_MAX_ENDPOINT_NUMBER ||
        !g_out[number].generic)
        return NULL;
    return &g_out[number];
}

uint8_t mimhid_generic_set_policy(uint8_t address, uint8_t depth, uint8_t policy) {
    in_t* q = active_in(address);
    if (q == NULL)
        return MIMHID_NAK_STATE;
    // The ring was cut at COMMIT; the new settings must fit what it holds.
    const uint16_t cost = slots_of(depth, policy);
    if (cost == 0 || cost > q->n)
        return MIMHID_NAK_OVERFLOW;
    if (policy != MIMHID_EP_FULL_WAIT)
        q->done = 0;
    __atomic_store_n(&q->depth, depth, __ATOMIC_RELAXED);
    __atomic_store_n(&q->policy, policy, __ATOMIC_RELAXED);
    return 0;
}

// True when the producer has begun the current generation, so `head` means something.
static bool producing(const in_t* q) {
    return __atomic_load_n(&q->prod_gen, __ATOMIC_ACQUIRE) == g_gen;
}

static void in_service(uint8_t number);

static void out_arm(uint8_t number) {
    out_t* o = &g_out[number];
    if (!o->open || o->armed || !usbd_edpt_ready(0, number))
        return; // stalled endpoints are busy in the stack (usbd_edpt_stall sets both flags)
    // One packet at a time: the transfer ends with the packet (USB 2.0 5.8.3).
    if (usbd_edpt_xfer(0, number, o->buf, o->packet))
        o->armed = true;
}

uint8_t mimhid_generic_stall(uint8_t address, bool stall) {
    const uint8_t number = address & 0x0F;
    in_t* q = active_in(address);
    out_t* o = q == NULL ? active_out(address) : NULL;
    if ((q == NULL && o == NULL) || (q != NULL && !q->open) || (o != NULL && !o->open))
        return MIMHID_NAK_STATE;
    if (stall) {
        usbd_edpt_stall(0, address);
        if (q != NULL) {
            // What waits is dropped (the writer counted it done), and the transfer that was on the
            // bus is not completed by a stalled endpoint (USB 2.0 8.5.3.4: a halted endpoint answers
            // STALL), so it is not waited for.
            if (producing(q))
                __atomic_store_n(&q->tail, __atomic_load_n(&q->head, __ATOMIC_ACQUIRE),
                                 __ATOMIC_RELEASE);
            q->inflight = false;
            q->orphan = false;
            q->done = 0;
        } else {
            o->armed = false;
        }
    } else {
        usbd_edpt_clear_stall(0, address);
        if (q != NULL)
            in_service(number);
        else
            out_arm(number);
    }
    return 0;
}

uint8_t mimhid_generic_flush(uint8_t address) {
    in_t* q = active_in(address);
    if (q == NULL)
        return active_out(address) != NULL ? MIMHID_NAK_PARAM : MIMHID_NAK_STATE;
    if (!q->open)
        return MIMHID_NAK_STATE;
    // The transfer already handed to the controller cannot be recalled: it completes, and is not
    // counted. What waits is dropped.
    if (producing(q))
        __atomic_store_n(&q->tail, __atomic_load_n(&q->head, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
    if (q->inflight)
        q->orphan = true;
    q->done = 0;
    return 0;
}

void mimhid_generic_drop_all(void) {
    for (uint8_t number = 1; number < 16; number++) {
        in_t* q = &g_in[number];
        if (q->n == 0)
            continue;
        if (producing(q))
            __atomic_store_n(&q->tail, __atomic_load_n(&q->head, __ATOMIC_ACQUIRE),
                             __ATOMIC_RELEASE);
        if (q->inflight)
            q->orphan = true; // it completes on the bus, and nobody waits for it
        q->done = 0;
    }
}

// ---- the receive side ----

// The reservation the receive side has in hand (one frame at a time).
static struct {
    in_t* q;
    uint32_t gen;
    uint16_t head;
    uint16_t n;
    uint16_t slot; // index in the pool
} g_rx;

static void lost_add(in_t* q, uint32_t count) {
    if (__atomic_load_n(&g_loss_on, __ATOMIC_ACQUIRE))
        __atomic_fetch_add(&q->lost, count, __ATOMIC_RELAXED);
}

uint8_t* mimhid_generic_in_reserve(uint8_t number) {
    g_rx.q = NULL;
    if (number == 0 || number >= 16)
        return NULL;
    in_t* q = &g_in[number];
    const uint32_t gen = __atomic_load_n(&g_gen, __ATOMIC_ACQUIRE);
    if (gen & 1u)
        return NULL; // the pool is being cut again
    const uint32_t geom = __atomic_load_n(&q->geom, __ATOMIC_RELAXED);
    const uint16_t n = (uint16_t)(geom >> 16);
    if (n == 0)
        return NULL; // not an IN endpoint of the generic driver
    if (q->prod_gen != gen) {
        // The pool was cut again since this endpoint's last frame: its ring starts empty (COMMIT
        // zeroed the tail), and only this side writes the head.
        __atomic_store_n(&q->head, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&q->prod_gen, gen, __ATOMIC_RELEASE);
    }
    const uint16_t head = q->head;
    const uint16_t tail = __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
    // A COMMIT on the USB side may have cut the pool again between the read of the generation and
    // the reads of this queue: its geometry would then be the new one and `head` still the old
    // ring's, which can point outside the new ring. The generation is read again, and a frame that
    // straddles a cut is dropped; the indices are checked as well, so nothing is ever written
    // outside the ring whatever the two sides did.
    if (__atomic_load_n(&g_gen, __ATOMIC_ACQUIRE) != gen || head >= n || tail >= n ||
        (geom & 0xFFFFu) + n > MIMHID_EP_POOL_SLOTS)
        return NULL;
    const uint16_t waiting = (uint16_t)((head + n - tail) % n);
    const uint8_t policy = __atomic_load_n(&q->policy, __ATOMIC_RELAXED);
    // Most that may be queued: `depth`, or for DROP_OLDEST everything the ring holds, which the USB
    // side cuts back to `depth` when it next looks (see the top of this file).
    const uint16_t max_queued =
        policy == MIMHID_EP_FULL_DROP_OLDEST ? (uint16_t)(n - 1u)
                                             : (uint16_t)__atomic_load_n(&q->depth, __ATOMIC_RELAXED);
    if (waiting >= max_queued) {
        lost_add(q, 1u);
        return NULL;
    }
    g_rx.q = q;
    g_rx.gen = gen;
    g_rx.head = head;
    g_rx.n = n;
    g_rx.slot = (uint16_t)((geom & 0xFFFFu) + head);
    return g_pool[g_rx.slot].data;
}

void mimhid_generic_in_commit(uint16_t length) {
    in_t* q = g_rx.q;
    g_rx.q = NULL;
    if (q == NULL)
        return;
    if (__atomic_load_n(&g_gen, __ATOMIC_ACQUIRE) != g_rx.gen)
        return; // the pool was cut again while the frame was read: it belongs to no queue now
    g_pool[g_rx.slot].len = length;
    __atomic_store_n(&q->head, (uint16_t)((g_rx.head + 1u) % g_rx.n), __ATOMIC_RELEASE);
}

void mimhid_generic_in_discard(void) { g_rx.q = NULL; }

// ---- the USB side ----

// Starts the transfer at the tail when the endpoint is free, and cuts back what DROP_OLDEST holds
// beyond its depth. The USB side is one context (tud_task and mimhid_usb_poll), so this does not
// race with xfer_cb, reset or the commands.
//
// DROP_OLDEST: only the USB side moves the tail, so only it may remove a queued transfer; the
// producer, which may only move the head, keeps queueing into the room beyond `depth` and this
// function removes the oldest of the queue (the one at the tail: the transfer on the bus is not in
// the ring, and its bytes are in g_bus, out of the producer's reach) before it starts the next.
// The oldest transfer that is not being sent is thus always the one dropped, and nothing is
// removed from the middle of the ring. If the producer outruns the
// USB side by more than the ring holds, it drops the arriving transfer, as the other policies do.
static void in_service(uint8_t number) {
    in_t* q = &g_in[number];
    if (q->n == 0 || !producing(q))
        return;
    const uint16_t n = q->n;
    uint16_t tail = q->tail;
    uint16_t waiting = (uint16_t)((__atomic_load_n(&q->head, __ATOMIC_ACQUIRE) + n - tail) % n);
    const uint8_t depth = __atomic_load_n(&q->depth, __ATOMIC_RELAXED);
    if (__atomic_load_n(&q->policy, __ATOMIC_RELAXED) == MIMHID_EP_FULL_DROP_OLDEST &&
        waiting > depth) {
        const uint16_t drop = (uint16_t)(waiting - depth);
        tail = (uint16_t)((tail + drop) % n);
        __atomic_store_n(&q->tail, tail, __ATOMIC_RELEASE);
        lost_add(q, drop);
        waiting = depth;
    }
    if (!q->open || q->inflight || waiting == 0)
        return;
    const uint8_t address = (uint8_t)(0x80u | number);
    if (!usbd_edpt_ready(0, address))
        return; // the controller is busy with the endpoint, or the target halted it
    // The slot is copied out and freed before the transfer starts: the controller keeps the
    // pointer it is given until the transfer completes (usbd_edpt_xfer in src/device/usbd.c passes
    // the buffer to dcd_edpt_xfer), and the slot may be written again long before that. A length of
    // 0 is a zero-length packet.
    const slot_t* slot = &g_pool[q->base + tail];
    const uint16_t len = slot->len;
    memcpy(g_bus[number].data, slot->data, len);
    q->inflight = true;
    __atomic_store_n(&q->tail, (uint16_t)((tail + 1u) % n), __ATOMIC_RELEASE);
    if (!usbd_edpt_xfer(0, address, g_bus[number].data, len)) {
        q->inflight = false; // the transfer is lost
        lost_add(q, 1u);
    }
}

void mimhid_generic_poll(void) {
    for (uint8_t number = 1; number < 16; number++) {
        in_t* q = &g_in[number];
        if (q->n == 0)
            continue;
        in_service(number);
        if (q->done != 0) {
            if (__atomic_load_n(&q->policy, __ATOMIC_RELAXED) == MIMHID_EP_FULL_WAIT) {
                const uint8_t ev[3] = {(uint8_t)(0x80u | number), (uint8_t)(q->done & 0xFF),
                                       (uint8_t)(q->done >> 8)};
                mimhid_send_event(MIMHID_EV_EP_DONE, ev, sizeof(ev));
            }
            q->done = 0;
        }
    }
}

void mimhid_generic_loss_set(bool on) {
    __atomic_store_n(&g_loss_on, false, __ATOMIC_RELEASE);
    for (uint8_t number = 1; number < 16; number++)
        __atomic_store_n(&g_in[number].lost, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_loss_on, on, __ATOMIC_RELEASE);
}

uint32_t mimhid_generic_loss_take(uint8_t number) {
    if (number == 0 || number >= 16)
        return 0;
    return __atomic_exchange_n(&g_in[number].lost, 0u, __ATOMIC_RELAXED);
}

// ---- the TinyUSB class driver ----

static void gen_init(void) {}

static void gen_reset(uint8_t rhport) {
    (void)rhport;
    // A bus reset or a new configuration: the controller closes every endpoint and forgets every
    // transfer, so what waited for them is dropped, and the wait counts start again.
    for (uint8_t number = 1; number < 16; number++) {
        in_t* q = &g_in[number];
        q->open = false;
        q->inflight = false;
        q->orphan = false;
        q->done = 0;
        if (q->n != 0 && producing(q))
            __atomic_store_n(&q->tail, __atomic_load_n(&q->head, __ATOMIC_ACQUIRE),
                             __ATOMIC_RELEASE);
        if (number <= MIMHID_MAX_ENDPOINT_NUMBER) {
            g_out[number].open = false;
            g_out[number].armed = false;
        }
    }
}

// process_set_config in src/device/usbd.c calls open for the first interface of every association
// (after the association descriptor) and for every other interface; whatever it returns is the
// number of bytes the driver took, and the stack goes on from there, binding all the endpoints in
// them to this driver (tu_edpt_bind_driver). An association takes all its interfaces in one call.
static uint16_t gen_open(uint8_t rhport, tusb_desc_interface_t const* desc_itf, uint16_t max_len) {
    (void)rhport;
    // An HID interface: 0, so the stack's HID driver takes it. The walk at COMMIT has already
    // decided which interfaces those are.
    const uint16_t span = mimhid_active_generic_span(desc_itf->bInterfaceNumber);
    if (span == 0 || span > max_len)
        return 0;
    const uint8_t* p = (const uint8_t*)desc_itf;
    const uint8_t* end = p + span;
    for (; p < end; p += p[0]) {
        if (p[1] != TUSB_DESC_ENDPOINT)
            continue;
        const tusb_desc_endpoint_t* ep = (const tusb_desc_endpoint_t*)p;
        if (!usbd_edpt_open(rhport, ep))
            return 0;
        const uint8_t address = ep->bEndpointAddress;
        const uint8_t number = address & 0x0F;
        if (address & 0x80) {
            g_in[number].open = true;
            g_in[number].inflight = false;
            g_in[number].orphan = false;
        } else if (number <= MIMHID_MAX_ENDPOINT_NUMBER) {
            g_out[number].open = true;
            g_out[number].armed = false;
            out_arm(number);
        }
    }
    return span;
}

static bool gen_control_xfer(uint8_t rhport, uint8_t stage, tusb_control_request_t const* request) {
    (void)rhport;
    // The stack stalls or clears the endpoint itself and forwards the request to the endpoint's
    // driver (the TUSB_REQ_RCPT_ENDPOINT case of process_control_request in src/device/usbd.c, which
    // acknowledges the request whatever is returned). Only the halt feature (USB 2.0 9.4.1, 9.4.5)
    // of an endpoint of ours is looked at; class and vendor requests reach the program only through
    // control rules, which hold a request before the stack sees it, so there is nothing else to
    // answer.
    if (stage != CONTROL_STAGE_SETUP || request->bmRequestType_bit.recipient != TUSB_REQ_RCPT_ENDPOINT ||
        request->bmRequestType_bit.type != TUSB_REQ_TYPE_STANDARD ||
        (request->bRequest != TUSB_REQ_SET_FEATURE && request->bRequest != TUSB_REQ_CLEAR_FEATURE) ||
        request->wValue != TUSB_REQ_FEATURE_EDPT_HALT)
        return false;
    const uint8_t address = (uint8_t)(request->wIndex & 0xFF);
    const uint8_t number = address & 0x0F;
    in_t* q = active_in(address);
    out_t* o = q == NULL ? active_out(address) : NULL;
    if (q == NULL && o == NULL)
        return false;
    const bool halted = request->bRequest == TUSB_REQ_SET_FEATURE;
    if (halted) {
        if (q != NULL && q->inflight) {
            // The stalled endpoint does not complete the transfer on the bus. The writer is told it
            // is done (so its count stays right), and it is lost.
            q->inflight = false;
            q->orphan = false;
            if (__atomic_load_n(&q->policy, __ATOMIC_RELAXED) == MIMHID_EP_FULL_WAIT &&
                q->done != 0xFFFF)
                q->done++;
            lost_add(q, 1u);
        }
        if (o != NULL)
            o->armed = false;
    } else if (q != NULL) {
        in_service(number);
    } else {
        out_arm(number);
    }
    const uint8_t ev[2] = {address, halted ? 1u : 0u};
    mimhid_send_event(MIMHID_EV_EP_HALT, ev, sizeof(ev));
    return true;
}

static bool gen_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result,
                        uint32_t xferred_bytes) {
    (void)rhport;
    const uint8_t number = ep_addr & 0x0F;
    if (ep_addr & 0x80) {
        in_t* q = &g_in[number];
        if (q->n == 0 || (!q->inflight && !q->orphan))
            return true;
        if (q->orphan) {
            q->orphan = false; // a flush forgot it
        } else {
            if (__atomic_load_n(&q->policy, __ATOMIC_RELAXED) == MIMHID_EP_FULL_WAIT &&
                q->done != 0xFFFF)
                q->done++;
        }
        q->inflight = false;
        in_service(number);
        return true;
    }
    if (number > MIMHID_MAX_ENDPOINT_NUMBER || !g_out[number].generic)
        return true;
    out_t* o = &g_out[number];
    o->armed = false;
    if (result == XFER_RESULT_SUCCESS && o->open) {
        // The packet goes to the host first (a blocking write: the target is held by NAKs while it
        // is written), and only then is the endpoint armed again.
        mimhid_send_ep_out(number, o->buf, (uint16_t)xferred_bytes);
    }
    out_arm(number);
    return true;
}

static const usbd_class_driver_t g_driver = {
    .name = "MIMHID_GENERIC",
    .init = gen_init,
    .deinit = NULL,
    .reset = gen_reset,
    .open = gen_open,
    .control_xfer_cb = gen_control_xfer,
    .xfer_cb = gen_xfer_cb,
    .sof = NULL,
};

usbd_class_driver_t const* usbd_app_driver_get_cb(uint8_t* driver_count) {
    *driver_count = 1;
    return &g_driver;
}
