// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
#include "generic.h"
#include "config.h"
#include "control.h"
#include "core.h"
#include "hal.h"
#include "tusb.h"
#include "device/usbd_pvt.h"
#include <string.h>

// The controller driver's stall (src/device/dcd.h; not included here, as control.c does not: some
// TinyUSB versions declare dcd_int_handler twice in it). The stack stalls endpoint 0 with it when a
// control request fails, and set_interface does the same for a setting that is not there.
void dcd_edpt_stall(uint8_t rhport, uint8_t ep_addr);

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
// bytes on the bus must not live in one. The copy is the length of the transfer.
//
// A ring of n slots holds n - 1 queued transfers at most (head == tail means empty, so the ring is
// never filled). `depth` is how many transfers wait in the ring, so a queue costs depth + 1 slots.
// DROP_OLDEST needs room to run ahead of the USB side, which removes the oldest transfers when it
// next looks; it costs 2 * depth + 1.
// ---------------------------------------------------------------------------------------------

typedef struct {
    _Alignas(4) uint8_t data[MIMUSB_MAX_TRANSFER_BYTES];
    uint16_t len;
} slot_t;

static slot_t g_pool[MIMUSB_EP_POOL_SLOTS];

// The transfer each IN endpoint has on the bus. The USB side only.
static struct {
    _Alignas(4) uint8_t data[MIMUSB_MAX_TRANSFER_BYTES];
} g_bus[MIMUSB_MAX_ENDPOINT_NUMBER + 1];

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
    uint16_t bus_len; // its length, to start it again (resume_after_clear)
    bool orphan; // a flush forgot the transfer on the bus; its completion is not counted
    bool open;   // the target configured the device: the endpoint is open
    uint16_t done; // completions not yet reported (wait policy only)
} in_t;

static in_t g_in[16];

typedef struct {
    _Alignas(4) uint8_t buf[MIMUSB_MAX_TRANSFER_BYTES];
    uint16_t packet;
    bool generic; // an OUT endpoint of the generic driver in the active configuration
    bool open;
    bool armed;
} out_t;

static out_t g_out[MIMUSB_MAX_ENDPOINT_NUMBER + 1];

// What the target selected in the active configuration. Both are the USB side's, and both start
// again at COMMIT, at a bus reset and at SET_CONFIGURATION.
// The alternate setting of each interface (GET_INTERFACE answers it).
static uint8_t g_alt[MIMUSB_MAX_INTERFACES];
// The endpoints the controller has been given in this configuration, bit n of [0] OUT and [1] IN:
// a bulk or interrupt endpoint is opened once, an isochronous one gets its packet buffer once (the
// largest of its settings), and every setting that names the address uses what was given.
static uint16_t g_has[2];

// The generation of the queues: odd while COMMIT cuts the pool again, even otherwise. The receive
// side reads a queue's geometry and head without a lock, so a frame it began under one generation
// is dropped when it ends under another. Only the USB side writes it.
static volatile uint32_t g_gen;

static volatile bool g_loss_on;

// Ring arithmetic without a division: the Cortex-M0+ of the RP2040 has no divide instruction, and
// these run for every frame. Both arguments are below n (a drop count may add up to n - 1).
static inline uint16_t ring_distance(uint16_t head, uint16_t tail, uint16_t n) {
    return (uint16_t)(head >= tail ? head - tail : head + n - tail);
}
static inline uint16_t ring_add(uint16_t index, uint16_t count, uint16_t n) {
    const uint32_t sum = (uint32_t)index + count;
    return (uint16_t)(sum >= n ? sum - n : sum);
}

static uint16_t slots_of(uint8_t depth, uint8_t policy) {
    if (depth == 0)
        return 0;
    switch (policy) {
    case MIMUSB_EP_FULL_DROP_NEWEST:
    case MIMUSB_EP_FULL_WAIT:
        return (uint16_t)(depth + 1u);
    case MIMUSB_EP_FULL_DROP_OLDEST:
        return (uint16_t)(2u * depth + 1u);
    default:
        return 0;
    }
}

uint16_t mimusb_generic_slots(uint8_t depth, uint8_t policy) { return slots_of(depth, policy); }

// ---- COMMIT ----

bool mimusb_generic_plan(const mimusb_config_t* cfg, const mimusb_layout_t* layout, uint8_t* code) {
    uint32_t total = 0;
    for (uint8_t number = 1; number < 16; number++) {
        if (layout->ep[1][number].kind != MIMUSB_EP_GENERIC)
            continue;
        const uint16_t cost = slots_of(cfg->ep[number].depth, cfg->ep[number].policy);
        if (cost == 0) {
            *code = MIMUSB_NAK_STATE; // no EP_CONFIG for this endpoint
            return false;
        }
        total += cost;
    }
    if (total > MIMUSB_EP_POOL_SLOTS) {
        *code = MIMUSB_NAK_OVERFLOW;
        return false;
    }
#if MIMUSB_EP_BUFFER_BYTES != 0
    // The controller's packet memory (core.h): a driver that finds none left stops the board.
    uint32_t memory = 0;
    for (uint8_t dir = 0; dir < 2; dir++) {
        for (uint8_t number = 1; number < 16; number++) {
            const mimusb_ep_info_t* e = &layout->ep[dir][number];
            if (e->kind != MIMUSB_EP_GENERIC)
                continue;
            const uint32_t unit = ((uint32_t)e->packet_size + 63u) & ~63u;
            memory += e->type == TUSB_XFER_BULK ? 2u * unit : unit;
        }
    }
    if (memory > MIMUSB_EP_BUFFER_BYTES) {
        *code = MIMUSB_NAK_OVERFLOW;
        return false;
    }
#endif
    return true;
}

void mimusb_generic_apply(const mimusb_config_t* cfg, const mimusb_layout_t* layout) {
    // Odd: being cut. The store and the fence are sequentially consistent, so the receive side on
    // the other core cannot see a queue change before it can see the odd generation.
    __atomic_store_n(&g_gen, g_gen + 1u, __ATOMIC_SEQ_CST);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    memset(g_alt, 0, sizeof(g_alt));
    g_has[0] = g_has[1] = 0;
    uint16_t next = 0;
    for (uint8_t number = 1; number < 16; number++) {
        in_t* q = &g_in[number];
        const mimusb_ep_info_t* in = &layout->ep[1][number];
        q->open = false;
        q->inflight = false;
        q->orphan = false;
        q->done = 0;
        q->lost = 0;
        q->tail = 0;
        q->n = 0;
        __atomic_store_n(&q->geom, 0u, __ATOMIC_RELEASE);
        if (in->kind == MIMUSB_EP_GENERIC) {
            const uint16_t n = slots_of(cfg->ep[number].depth, cfg->ep[number].policy);
            q->base = next;
            q->n = n;
            q->depth = cfg->ep[number].depth;
            q->policy = cfg->ep[number].policy;
            next = (uint16_t)(next + n);
            __atomic_store_n(&q->geom, ((uint32_t)n << 16) | q->base, __ATOMIC_RELEASE);
        }
        if (number <= MIMUSB_MAX_ENDPOINT_NUMBER) {
            out_t* o = &g_out[number];
            o->open = false;
            o->armed = false;
            o->generic = layout->ep[0][number].kind == MIMUSB_EP_GENERIC;
            o->packet = layout->ep[0][number].packet_size;
        }
    }
    __atomic_store_n(&g_gen, g_gen + 1u, __ATOMIC_SEQ_CST); // even: cut
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
    if ((address & 0xF0) != 0 || number == 0 || number > MIMUSB_MAX_ENDPOINT_NUMBER ||
        !g_out[number].generic)
        return NULL;
    return &g_out[number];
}

uint8_t mimusb_generic_set_policy(uint8_t address, uint8_t depth, uint8_t policy) {
    in_t* q = active_in(address);
    if (q == NULL)
        return MIMUSB_NAK_STATE;
    // The ring was cut at COMMIT; the new settings must fit what it holds.
    const uint16_t cost = slots_of(depth, policy);
    if (cost == 0 || cost > q->n)
        return MIMUSB_NAK_OVERFLOW;
    if (policy != MIMUSB_EP_FULL_WAIT)
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
static void lost_add(in_t* q, uint32_t count);

static void out_arm(uint8_t number) {
    out_t* o = &g_out[number];
    if (!o->open || o->armed || !usbd_edpt_ready(0, number))
        return; // stalled endpoints are busy in the stack (usbd_edpt_stall sets both flags)
    // One packet at a time: the transfer ends with the packet (USB 2.0 5.8.3).
    if (usbd_edpt_xfer(0, number, o->buf, o->packet))
        o->armed = true;
}

// ClearFeature(ENDPOINT_HALT) sets the data toggle to DATA0 whatever the state of the endpoint (USB
// 2.0 9.4.5): a host does it on an endpoint that is not halted, as the recovery of a Bulk-Only
// reset does. The stack does it for the controller (usbd_edpt_clear_stall) and clears the busy flag
// of the endpoint, but on the RP2040 and RP2350 a transfer that was armed before keeps the PID it was
// armed with (dcd_rp2040.c: dcd_edpt_clear_stall sets next_pid to DATA0, and the buffer control of
// an armed transfer is not touched). An OUT endpoint then takes the target's next packet, DATA0,
// for the repeat of the one before and drops it; an IN transfer on the bus is taken the same way by
// the target. So the transfer that was armed is taken off the controller (hal_usb_clear_endpoint:
// the driver stops with "ep was already available" when a transfer is armed on top of another) and
// started again with the toggle as now set. A packet that arrived in the moment between the clear and
// this is lost; a host does not send on an endpoint it is clearing.
static void resume_after_clear(uint8_t number, in_t* q, out_t* o) {
#if CFG_TUSB_MCU == OPT_MCU_RP2040 || CFG_TUSB_MCU == OPT_MCU_RP2350
    if (q != NULL && q->inflight) {
        hal_usb_clear_endpoint((uint8_t)(0x80u | number));
        if (!usbd_edpt_xfer(0, (uint8_t)(0x80u | number), g_bus[number].data, q->bus_len)) {
            q->inflight = false; // the transfer is lost
            lost_add(q, 1u);
        }
        return; // it is still the transfer on the bus: nothing else is started
    }
    if (o != NULL && o->armed) {
        hal_usb_clear_endpoint(number);
        o->armed = false; // what was armed is void: the stack cleared the busy flag
    }
#else
    (void)o;
#endif
    if (q != NULL)
        in_service(number);
    else
        out_arm(number);
}

uint8_t mimusb_generic_stall(uint8_t address, bool stall) {
    const uint8_t number = address & 0x0F;
    in_t* q = active_in(address);
    out_t* o = q == NULL ? active_out(address) : NULL;
    if ((q == NULL && o == NULL) || (q != NULL && !q->open) || (o != NULL && !o->open))
        return MIMUSB_NAK_STATE;
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
        resume_after_clear(number, q, o);
    }
    return 0;
}

uint8_t mimusb_generic_flush(uint8_t address) {
    in_t* q = active_in(address);
    if (q == NULL)
        return active_out(address) != NULL ? MIMUSB_NAK_PARAM : MIMUSB_NAK_STATE;
    if (!q->open)
        return MIMUSB_NAK_STATE;
    // The transfer already handed to the controller cannot be recalled: it completes, and is not
    // counted. What waits is dropped.
    if (producing(q))
        __atomic_store_n(&q->tail, __atomic_load_n(&q->head, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
    if (q->inflight)
        q->orphan = true;
    q->done = 0;
    return 0;
}

void mimusb_generic_drop_all(void) {
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

uint8_t* mimusb_generic_in_reserve(uint8_t number) {
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
    // ring's, which can point outside the new ring. The generation is read again: when it has not
    // changed, the geometry, the head and the tail all belong to the same cut (COMMIT makes the
    // generation odd before it touches them). A frame that straddles a cut is dropped.
    //
    // That nothing is written outside the pool does not rest on this ordering argument alone: the
    // geometry is one word, so `n` and the base always belong together and base + n is inside the
    // pool, and a head or a tail that is not below `n` is refused.
    if (__atomic_load_n(&g_gen, __ATOMIC_SEQ_CST) != gen || head >= n || tail >= n)
        return NULL;
    const uint16_t waiting = ring_distance(head, tail, n);
    const uint8_t policy = __atomic_load_n(&q->policy, __ATOMIC_RELAXED);
    // Most that may be queued: `depth`, or for DROP_OLDEST everything the ring holds, which the USB
    // side cuts back to `depth` when it next looks (see the top of this file).
    const uint16_t max_queued =
        policy == MIMUSB_EP_FULL_DROP_OLDEST ? (uint16_t)(n - 1u)
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

void mimusb_generic_in_commit(uint16_t length) {
    in_t* q = g_rx.q;
    g_rx.q = NULL;
    if (q == NULL)
        return;
    if (__atomic_load_n(&g_gen, __ATOMIC_ACQUIRE) != g_rx.gen)
        return; // the pool was cut again while the frame was read: it belongs to no queue now
    g_pool[g_rx.slot].len = length;
    __atomic_store_n(&q->head, ring_add(g_rx.head, 1u, g_rx.n), __ATOMIC_RELEASE);
}

void mimusb_generic_in_discard(void) { g_rx.q = NULL; }

// ---- the USB side ----

// Starts the transfer at the tail when the endpoint is free, and cuts back what DROP_OLDEST holds
// beyond its depth. The USB side is one context (tud_task and mimusb_usb_poll), so this does not
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
    uint16_t waiting = ring_distance(__atomic_load_n(&q->head, __ATOMIC_ACQUIRE), tail, n);
    const uint8_t depth = __atomic_load_n(&q->depth, __ATOMIC_RELAXED);
    if (__atomic_load_n(&q->policy, __ATOMIC_RELAXED) == MIMUSB_EP_FULL_DROP_OLDEST &&
        waiting > depth) {
        const uint16_t drop = (uint16_t)(waiting - depth);
        tail = ring_add(tail, drop, n);
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
    q->bus_len = len;
    __atomic_store_n(&q->tail, ring_add(tail, 1u, n), __ATOMIC_RELEASE);
    if (!usbd_edpt_xfer(0, address, g_bus[number].data, len)) {
        q->inflight = false; // the transfer is lost
        lost_add(q, 1u);
        return;
    }
    // What an ANSWER_LAST_SENT rule of this endpoint answers with from now on. Here, on the USB
    // side, which is also where the vendor callback that answers the request runs (control.c), so
    // the one that reads it never meets a half-written copy. Hot path: one test when no rule names
    // this endpoint.
    mimusb_control_sent(number, g_bus[number].data, len);
}

void mimusb_generic_poll(void) {
    for (uint8_t number = 1; number < 16; number++) {
        in_t* q = &g_in[number];
        if (q->n == 0)
            continue;
        in_service(number);
        if (q->done != 0) {
            if (__atomic_load_n(&q->policy, __ATOMIC_RELAXED) == MIMUSB_EP_FULL_WAIT) {
                const uint8_t ev[3] = {(uint8_t)(0x80u | number), (uint8_t)(q->done & 0xFF),
                                       (uint8_t)(q->done >> 8)};
                mimusb_send_event(MIMUSB_EV_EP_DONE, ev, sizeof(ev));
            }
            q->done = 0;
        }
    }
}

void mimusb_generic_loss_set(bool on) {
    __atomic_store_n(&g_loss_on, false, __ATOMIC_RELEASE);
    for (uint8_t number = 1; number < 16; number++)
        __atomic_store_n(&g_in[number].lost, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_loss_on, on, __ATOMIC_RELEASE);
}

uint32_t mimusb_generic_loss_take(uint8_t number) {
    if (number == 0 || number >= 16)
        return 0;
    return __atomic_exchange_n(&g_in[number].lost, 0u, __ATOMIC_RELAXED);
}

// ---- the TinyUSB class driver ----

static void gen_init(void) {}

static void gen_reset(uint8_t rhport) {
    (void)rhport;
    memset(g_alt, 0, sizeof(g_alt));
    g_has[0] = g_has[1] = 0;
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
        if (number <= MIMUSB_MAX_ENDPOINT_NUMBER) {
            g_out[number].open = false;
            g_out[number].armed = false;
        }
    }
}

// ---- the endpoints of a configuration, and its alternate settings ----
//
// A setting is the interface descriptor with its bAlternateSetting and the endpoint descriptors that
// follow it. The walk at COMMIT (usb_descriptors.c) made sure a bulk or interrupt endpoint is the
// same in every setting that names it, so the controller opens it once and it stays open; an
// isochronous endpoint gets one packet buffer (the largest of its settings) and is activated, with
// the descriptor of the setting, when that setting is selected and stopped when it is left. What
// the target does with a setting is all the board does: it adds no data to a stream and removes
// none that the host program sent.

typedef bool (*ep_visit_t)(const tusb_desc_endpoint_t* ep, uint8_t itf, uint8_t alt, void* ctx);

// Calls `visit` for every endpoint descriptor in [p, end), with the number and setting of the
// interface descriptor it follows; false as soon as one call says so.
static bool scan_endpoints(const uint8_t* p, const uint8_t* end, ep_visit_t visit, void* ctx) {
    uint8_t itf = 0xFF;
    uint8_t alt = 0;
    for (; p + 2 <= end && p[0] >= 2; p += p[0]) {
        if (p[1] == TUSB_DESC_INTERFACE) {
            itf = p[2];
            alt = p[3];
        } else if (p[1] == TUSB_DESC_ENDPOINT && itf != 0xFF) {
            if (!visit((const tusb_desc_endpoint_t*)p, itf, alt, ctx))
                return false;
        }
    }
    return true;
}

static bool is_iso(const tusb_desc_endpoint_t* ep) {
    return ep->bmAttributes.xfer == TUSB_XFER_ISOCHRONOUS;
}

// An endpoint of the setting that is now selected starts: the controller is given the descriptor
// of an isochronous one, and the queue or the buffer of the endpoint is ready for the target.
static bool ep_start(uint8_t rhport, const tusb_desc_endpoint_t* ep) {
    const uint8_t address = ep->bEndpointAddress;
    const uint8_t number = address & 0x0F;
    if (is_iso(ep)) {
        // What was armed when the target left this endpoint's setting is still on the controller
        // and would meet the transfer armed next.
        hal_usb_clear_endpoint(address);
        if (!usbd_edpt_iso_activate(rhport, ep))
            return false;
    }
    if (address & 0x80) {
        in_t* q = &g_in[number];
        q->open = true;
        q->inflight = false;
        q->orphan = false;
    } else if (number <= MIMUSB_MAX_ENDPOINT_NUMBER) {
        out_t* o = &g_out[number];
        o->packet = tu_edpt_packet_size(ep);
        o->open = true;
        o->armed = false;
        out_arm(number);
    }
    return true;
}

// An isochronous endpoint of the setting that is left stops: what waited for it is dropped, and a
// transfer on the bus is forgotten (activating the endpoint again starts over, so no completion
// of it is awaited).
static void ep_stop(uint8_t address) {
    const uint8_t number = address & 0x0F;
    hal_usb_clear_endpoint(address);
    if (address & 0x80) {
        in_t* q = &g_in[number];
        q->open = false;
        q->inflight = false;
        q->orphan = false;
        q->done = 0;
        if (q->n != 0 && producing(q))
            __atomic_store_n(&q->tail, __atomic_load_n(&q->head, __ATOMIC_ACQUIRE),
                             __ATOMIC_RELEASE);
    } else if (number <= MIMUSB_MAX_ENDPOINT_NUMBER) {
        g_out[number].open = false;
        g_out[number].armed = false;
    }
}

static bool open_visit(const tusb_desc_endpoint_t* ep, uint8_t itf, uint8_t alt, void* ctx) {
    (void)itf;
    const uint8_t rhport = *(const uint8_t*)ctx;
    const uint8_t address = ep->bEndpointAddress;
    const uint16_t bit = (uint16_t)(1u << (address & 0x0F));
    uint16_t* has = &g_has[(address >> 7) & 1];
    const bool first = (*has & bit) == 0;
    if (first) {
        if (is_iso(ep)) {
            // The buffer holds the largest packet of any setting the address is in (the walk kept it).
            const mimusb_ep_info_t* info = mimusb_active_endpoint(address);
            if (info == NULL || !usbd_edpt_iso_alloc(rhport, address, info->packet_size))
                return false;
        } else if (!usbd_edpt_open(rhport, ep)) {
            return false;
        }
        *has |= bit;
    }
    // A bulk or interrupt endpoint is in use from the start, whichever setting names it first;
    // an isochronous one starts with setting 0, the one the target starts in.
    if (is_iso(ep))
        return alt != 0 || ep_start(rhport, ep);
    return !first || ep_start(rhport, ep);
}

// process_set_config in src/device/usbd.c calls open for the first interface of every association
// (after the association descriptor) and for every other interface; whatever it returns is the
// number of bytes the driver took, and the stack goes on from there, binding all the endpoints in
// them to this driver (tu_edpt_bind_driver). An association takes all its interfaces in one call,
// and an interface takes its alternate settings (the span of the walk at COMMIT).
static uint16_t gen_open(uint8_t rhport, tusb_desc_interface_t const* desc_itf, uint16_t max_len) {
    // 0 for an interface the walk at COMMIT did not find: the stack then looks no further.
    const uint16_t span = mimusb_active_generic_span(desc_itf->bInterfaceNumber);
    if (span == 0 || span > max_len)
        return 0;
    const uint8_t* p = (const uint8_t*)desc_itf;
    uint8_t port = rhport;
    return scan_endpoints(p, p + span, open_visit, &port) ? span : 0;
}

typedef struct {
    uint8_t rhport;
    uint8_t itf;
    uint8_t from; // the setting that is left
    uint8_t to;   // the setting that is selected
    bool ok;
} switch_t;

static bool stop_visit(const tusb_desc_endpoint_t* ep, uint8_t itf, uint8_t alt, void* ctx) {
    const switch_t* sw = (const switch_t*)ctx;
    if (itf == sw->itf && alt == sw->from && is_iso(ep))
        ep_stop(ep->bEndpointAddress);
    return true;
}

static bool start_visit(const tusb_desc_endpoint_t* ep, uint8_t itf, uint8_t alt, void* ctx) {
    const switch_t* sw = (const switch_t*)ctx;
    return itf != sw->itf || alt != sw->to || !is_iso(ep) || ep_start(sw->rhport, ep);
}

// SET_INTERFACE (USB 2.0 9.4.10): the settings the interface has are those of the walk. A setting
// it does not have is a request error, answered with a stall (9.2.7).
static bool set_interface(uint8_t rhport, tusb_control_request_t const* request) {
    const uint8_t itf = (uint8_t)(request->wIndex & 0xFF);
    const uint8_t alt = (uint8_t)(request->wValue & 0xFF);
    const mimusb_layout_t* layout = mimusb_active_layout();
    if (request->wIndex > 0xFF || request->wValue > 0xFF || itf >= layout->count ||
        alt >= layout->itf_alts[itf]) {
        dcd_edpt_stall(rhport, 0x00);
        dcd_edpt_stall(rhport, 0x80);
        return true;
    }
    const uint8_t* cfg = mimusb_active_config_bytes();
    const uint8_t* end = cfg + (uint16_t)(cfg[2] | (cfg[3] << 8));
    switch_t sw = {.rhport = rhport, .itf = itf, .from = g_alt[itf], .to = alt, .ok = true};
    scan_endpoints(cfg + cfg[0], end, stop_visit, &sw);
    sw.ok = scan_endpoints(cfg + cfg[0], end, start_visit, &sw);
    g_alt[itf] = alt;
    if (!sw.ok) {
        dcd_edpt_stall(rhport, 0x00);
        dcd_edpt_stall(rhport, 0x80);
        return true;
    }
    tud_control_status(rhport, request);
    const uint8_t ev[2] = {itf, alt};
    mimusb_send_event(MIMUSB_EV_SET_INTERFACE, ev, sizeof(ev));
    return true;
}

static bool gen_control_xfer(uint8_t rhport, uint8_t stage, tusb_control_request_t const* request) {
    // The stack stalls or clears the endpoint itself and forwards the request to the endpoint's
    // driver (the TUSB_REQ_RCPT_ENDPOINT case of process_control_request in src/device/usbd.c, which
    // acknowledges the request whatever is returned). Only the halt feature (USB 2.0 9.4.1, 9.4.5)
    // of an endpoint of ours is looked at; class and vendor requests reach the program only through
    // control rules, which hold a request before the stack sees it, so there is nothing else to
    // answer.
    if (stage == CONTROL_STAGE_SETUP && request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_INTERFACE &&
        request->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD) {
        if (request->bRequest == TUSB_REQ_SET_INTERFACE)
            return set_interface(rhport, request);
        if (request->bRequest == TUSB_REQ_GET_INTERFACE) {
            if (request->wIndex >= MIMUSB_MAX_INTERFACES)
                return false;
            return tud_control_xfer(rhport, request, &g_alt[request->wIndex], 1);
        }
    }
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
            if (__atomic_load_n(&q->policy, __ATOMIC_RELAXED) == MIMUSB_EP_FULL_WAIT &&
                q->done != 0xFFFF)
                q->done++;
            lost_add(q, 1u);
        }
        if (o != NULL)
            o->armed = false;
    } else {
        resume_after_clear(number, q, o);
    }
    const uint8_t ev[2] = {address, halted ? 1u : 0u};
    mimusb_send_event(MIMUSB_EV_EP_HALT, ev, sizeof(ev));
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
            if (__atomic_load_n(&q->policy, __ATOMIC_RELAXED) == MIMUSB_EP_FULL_WAIT &&
                q->done != 0xFFFF)
                q->done++;
        }
        q->inflight = false;
        in_service(number);
        return true;
    }
    if (number > MIMUSB_MAX_ENDPOINT_NUMBER || !g_out[number].generic)
        return true;
    out_t* o = &g_out[number];
    o->armed = false;
    if (result == XFER_RESULT_SUCCESS && o->open) {
        // The packet goes to the host first (a blocking write: the target is held by NAKs while it
        // is written), and only then is the endpoint armed again.
        mimusb_send_ep_out(number, o->buf, (uint16_t)xferred_bytes);
    }
    out_arm(number);
    return true;
}

static const usbd_class_driver_t g_driver = {
    .name = "MIMUSB_GENERIC",
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
