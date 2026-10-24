// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#include "ep0.h"
#include "device/usbd_pvt.h"

// Code that runs in the USB interrupt sits in RAM where the port asks for it (see control.c).
#ifdef __APPLE__
#define EP0_FAST
#else
#define EP0_FAST TU_ATTR_FAST_FUNC
#endif

// The linker maps these to the stack's own functions (-Wl,--wrap=...).
void __real_dcd_event_handler(dcd_event_t const* event, bool in_isr);
bool __real_dcd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t* buffer, uint16_t total_bytes);
bool __wrap_dcd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t* buffer, uint16_t total_bytes);

static volatile uint8_t g_size = CFG_TUD_ENDPOINT0_SIZE;

// The transfer the stack asked for, sent or taken one packet at a time. `round` changes with every
// setup packet and bus reset, so a packet started from the task for an older transfer is not sent.
static struct {
    volatile uint32_t round;
    uint8_t size;      // the packet size of this control transfer, taken at its setup packet
    uint16_t w_length; // wLength of the setup packet
    uint16_t in_sent;  // IN data sent in this control transfer
    bool busy;
    bool zlp;          // the packet under way is the zero-length one
    uint8_t rhport;
    uint8_t ep_addr;
    uint8_t* buffer;
    uint16_t length;   // what the stack asked for
    uint16_t done;
    uint16_t packet;   // the packet under way
} g_x;

void mimhid_ep0_set_size(uint8_t size) {
    g_size = size;
}

static void next_packet(void* param) {
    dcd_int_disable(g_x.rhport);
    if (!g_x.busy || g_x.round != (uint32_t)(uintptr_t)param) {
        dcd_int_enable(g_x.rhport);
        return;
    }
    const uint16_t left = (uint16_t)(g_x.length - g_x.done);
    g_x.packet = g_x.zlp ? 0 : (left < g_x.size ? left : g_x.size);
    const uint8_t rhport = g_x.rhport;
    const uint8_t ep_addr = g_x.ep_addr;
    uint8_t* const at = g_x.packet ? g_x.buffer + g_x.done : NULL;
    const uint16_t packet = g_x.packet;
    dcd_int_enable(rhport);
    __real_dcd_edpt_xfer(rhport, ep_addr, at, packet);
}

bool __wrap_dcd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t* buffer, uint16_t total_bytes) {
    if ((ep_addr & 0x7F) != 0 || g_x.size == 0 || g_x.size >= CFG_TUD_ENDPOINT0_SIZE)
        return __real_dcd_edpt_xfer(rhport, ep_addr, buffer, total_bytes);
    dcd_int_disable(rhport);
    g_x.busy = true;
    g_x.zlp = false;
    g_x.rhport = rhport;
    g_x.ep_addr = ep_addr;
    g_x.buffer = buffer;
    g_x.length = total_bytes;
    g_x.done = 0;
    g_x.packet = total_bytes < g_x.size ? total_bytes : g_x.size;
    const uint16_t packet = g_x.packet;
    dcd_int_enable(rhport);
    return __real_dcd_edpt_xfer(rhport, ep_addr, buffer, packet);
}

EP0_FAST static void finish(dcd_event_t const* event, bool in_isr) {
    dcd_event_t whole = *event;
    whole.xfer_complete.len = g_x.done;
    g_x.busy = false;
    __real_dcd_event_handler(&whole, in_isr);
}

EP0_FAST bool mimhid_ep0_event(dcd_event_t const* event, bool in_isr) {
    switch (event->event_id) {
    case DCD_EVENT_SETUP_RECEIVED:
        g_x.round++;
        g_x.busy = false;
        g_x.size = g_size;
        g_x.w_length = event->setup_received.wLength;
        g_x.in_sent = 0;
        return false;
    case DCD_EVENT_BUS_RESET:
    case DCD_EVENT_UNPLUGGED:
        g_x.round++;
        g_x.busy = false;
        g_x.size = 0;
        return false;
    case DCD_EVENT_XFER_COMPLETE:
        break;
    default:
        return false;
    }
    if (!g_x.busy || event->xfer_complete.ep_addr != g_x.ep_addr) return false;

    const uint16_t got = (uint16_t)event->xfer_complete.len;
    g_x.done = (uint16_t)(g_x.done + got);
    const bool in = (g_x.ep_addr & 0x80) != 0;
    if (in) g_x.in_sent = (uint16_t)(g_x.in_sent + got);
    if (event->xfer_complete.result != XFER_RESULT_SUCCESS || g_x.zlp) {
        finish(event, in_isr);
        return true;
    }

    // The controller driver may still touch the endpoint after this event (the RP2040 driver
    // clears its transfer state), so the next packet starts from the task.
    if (got == g_x.packet && got != 0 && g_x.done < g_x.length) {
        usbd_defer_func(next_packet, (void*)(uintptr_t)g_x.round, in_isr);
        return true;
    }
    // A full last packet and a shorter one than CFG_TUD_ENDPOINT0_SIZE: the stack takes the data
    // stage as ended, the target waits for more until a short or zero-length packet.
    if (in && got == g_x.size && g_x.done == g_x.length && g_x.length < CFG_TUD_ENDPOINT0_SIZE &&
        g_x.in_sent < g_x.w_length) {
        g_x.zlp = true;
        usbd_defer_func(next_packet, (void*)(uintptr_t)g_x.round, in_isr);
        return true;
    }
    finish(event, in_isr);
    return true;
}
