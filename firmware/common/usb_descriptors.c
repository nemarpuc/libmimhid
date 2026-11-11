// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
// Descriptors served to the target. The host composes them and sends them as bytes; the board
// checks that they hold together when it commits them, and serves them as they came.
#include "config.h"
#include "core.h"
#include "ep0.h"
#include "hal.h"
#include "tusb.h"
#include <string.h>

// What the board presents while nothing is committed: a device with no interface (USB 2.0 9.6.1,
// 9.6.3). It carries no identity of the caller's.
#define FALLBACK_DEVICE                                                                            \
    18, TUSB_DESC_DEVICE, 0x00, 0x02, 0x00, 0x00, 0x00, CFG_TUD_ENDPOINT0_SIZE, 0xFE, 0xCA, 0x00,  \
        0x40, 0x00, 0x01, 0, 0, 0, 1
#define FALLBACK_CONFIG TUD_CONFIG_DESCRIPTOR(1, 0, 0, TUD_CONFIG_DESC_LEN, 0x80, 100)

// The active configuration: what a COMMIT copied from the staged one. It starts as zeros, which
// mimhid_core_init replaces with the fallback (mimhid_clear_config); an initializer here would put
// a copy of the whole structure, 18 KB, in the flash image.
static mimhid_config_t g_active;
// What the walk of the active configuration found. Zero (no interfaces) is a valid starting state:
// an endpoint number maps to an instance only when the instance is below `hid_count`.
static mimhid_layout_t g_layout;

// The unique id as a serial number string descriptor: the hexadecimal digits of up to 15 bytes of
// the id, which fit the string limit (USB 2.0 9.6.7). Returns the descriptor length in bytes, or 0
// when the chip has no id.
uint16_t mimhid_unique_id_string(uint8_t* dst) {
    uint8_t id[16];
    size_t n = hal_unique_id(id, sizeof(id));
    if (n == 0)
        return 0;
    if (n > 15)
        n = 15; // 30 hex characters fit the string limit
    static const char hex[] = "0123456789ABCDEF";
    dst[0] = (uint8_t)(2 + 4 * n);
    dst[1] = TUSB_DESC_STRING;
    for (size_t i = 0; i < n; i++) {
        dst[2 + 4 * i] = (uint8_t)hex[id[i] >> 4];
        dst[3 + 4 * i] = 0;
        dst[4 + 4 * i] = (uint8_t)hex[id[i] & 15];
        dst[5 + 4 * i] = 0;
    }
    return (uint16_t)(2 + 4 * n);
}

// ---- the walk ----

// What the walk keeps of the interface it is in.
typedef struct {
    int state;        // 0 outside an interface, 1 after its descriptor, 2 after its HID descriptor
                      // (a HID interface) or its descriptor (any other class)
    bool hid;
    uint8_t declared; // bNumEndpoints
    uint8_t seen;
    uint8_t in_count;
    uint8_t out_count;
    uint8_t in_addr;
    uint16_t start; // offset of the interface descriptor
} walk_itf_t;

// An interface association announced and not finished (the interfaces it names still follow).
typedef struct {
    uint8_t first; // bFirstInterface
    uint8_t left;  // interfaces of the group whose descriptors have not been seen yet
    bool pending;  // announced, its first interface descriptor not yet seen
} walk_group_t;

// The interface that has just ended, which began at w->start and ends at `end`, must be complete:
// every endpoint it announced, and for HID its HID descriptor, one interrupt IN endpoint and at
// most one interrupt OUT endpoint.
static bool finish_interface(const walk_itf_t* w, uint16_t end, mimhid_layout_t* out) {
    if (w->seen != w->declared)
        return false;
    const uint8_t n = out->count;
    out->itf_start[n] = w->start;
    out->itf_span[n] = (uint16_t)(end - w->start);
    out->itf_inst[n] = 0xFF;
    if (w->hid) {
        if (w->state != 2 || w->in_count != 1 || w->out_count > 1)
            return false;
        out->itf_inst[n] = out->hid_count;
        out->inst_itf[out->hid_count] = n;
        out->inst_ep_in[out->hid_count] = w->in_addr;
        out->hid_count++;
    }
    // Another interface of an association group extends the span of the group's first one: TinyUSB
    // calls the driver's open once for the group (process_set_config in usbd.c) and goes on from
    // the end of what open returns.
    const uint8_t first = out->itf_group[n];
    if (first != n)
        out->itf_span[first] = (uint16_t)(end - out->itf_start[first]);
    out->count++;
    return true;
}

bool mimhid_walk_config(const mimhid_config_t* c, mimhid_layout_t* out) {
    memset(out, 0, sizeof(*out));
    memset(out->ep_inst, 0xFF, sizeof(out->ep_inst));
    memset(out->inst_itf, 0xFF, sizeof(out->inst_itf));

    // The device descriptor (USB 2.0 9.6.1): 18 bytes, one configuration, and a control endpoint
    // size the board's controller driver can work with (8, 16, 32 or 64; 64 only at high speed).
    if (!c->device.used || c->device.total != 18 || c->device.recv != 18)
        return false;
    const uint8_t* d = c->device_bytes;
    const uint8_t ep0 = d[7];
    if (d[0] != 18 || d[1] != TUSB_DESC_DEVICE || d[17] != 1)
        return false;
    if (d[2] == 0 && d[3] == 0)
        return false; // bcdUSB is given, not filled in
    if (ep0 != 8 && ep0 != 16 && ep0 != 32 && ep0 != 64)
        return false;
#if TUD_OPT_HIGH_SPEED
    if (ep0 != 64)
        return false; // 9.6.1: a high-speed device has a 64-byte control endpoint, and only that
#endif

    // The configuration descriptor (9.6.3): its own length and total length, the one
    // configuration the stack maps SET_CONFIGURATION(1) to, and the attribute bits (D7 set, D4..0
    // reserved and zero).
    if (!c->config.used || c->config.recv != c->config.total)
        return false;
    const uint8_t* cfg = c->config_bytes;
    const uint16_t len = c->config.total;
    if (len < 9 || cfg[0] != 9 || cfg[1] != TUSB_DESC_CONFIGURATION)
        return false;
    if ((uint16_t)(cfg[2] | (cfg[3] << 8)) != len || cfg[5] != 1)
        return false;
    if ((cfg[7] & 0x80) == 0 || (cfg[7] & 0x1F) != 0)
        return false;

    walk_itf_t w;
    walk_group_t g;
    memset(&w, 0, sizeof(w));
    memset(&g, 0, sizeof(g));
    uint32_t addresses = 0; // bit n: OUT endpoint n, bit 16 + n: IN endpoint n
    uint16_t pos = 9;
    while (pos < len) {
        const uint8_t l = cfg[pos];
        if (l < 2 || (uint32_t)pos + l > len)
            return false; // every bLength at least 2 and inside the blob
        const uint8_t type = cfg[pos + 1];
        // An interface or an association ends the interface before it.
        if (type == TUSB_DESC_INTERFACE || type == TUSB_DESC_INTERFACE_ASSOCIATION) {
            if (w.state != 0) {
                if (!finish_interface(&w, pos, out))
                    return false;
                memset(&w, 0, sizeof(w));
            }
        }
        switch (type) {
        case TUSB_DESC_INTERFACE_ASSOCIATION: {
            // The association precedes the first of its interfaces and names the interfaces that
            // follow it (USB-IF Interface Association Descriptor ECN): 8 bytes, bFirstInterface
            // then bInterfaceCount. One at a time, not inside another.
            if (l != 8 || g.pending || g.left != 0 || cfg[pos + 3] == 0)
                return false;
            g.first = cfg[pos + 2];
            g.left = cfg[pos + 3];
            g.pending = true;
            if (g.first != out->count)
                return false; // it names the interface that comes next
            break;
        }
        case TUSB_DESC_INTERFACE: {
            // 9.6.5: the interface number, then the alternate setting. The numbers are 0, 1, 2, ...
            // in order (the host composes them so, and TinyUSB binds an association by number);
            // alternate settings are not supported.
            if (l != 9 || out->count >= MIMHID_MAX_INTERFACES || cfg[pos + 2] != out->count ||
                cfg[pos + 3] != 0)
                return false;
            const bool hid = cfg[pos + 5] == TUSB_CLASS_HID;
            // HID interfaces run on TinyUSB's HID driver (hidd_open takes one interface and the
            // stack binds an association's other interfaces to the same driver), so they cannot be
            // part of an association, and there are as many as the controller's endpoints allow.
            if (hid && (g.pending || g.left != 0 || out->hid_count >= MIMHID_HID_ITFS))
                return false;
            // The group an interface belongs to is named by its first interface; the first one
            // also spans the whole group, which finish_interface completes.
            out->itf_group[out->count] = g.left != 0 ? g.first : out->count;
            if (g.left != 0)
                g.left--;
            g.pending = false;
            memset(&w, 0, sizeof(w));
            w.hid = hid;
            w.state = hid ? 1 : 2; // a HID interface still needs its HID descriptor
            w.declared = cfg[pos + 4];
            w.start = pos;
            break;
        }
        case HID_DESC_TYPE_HID:
            if (w.state == 0)
                return false; // before any interface
            if (w.hid) {
                // HID 1.11 6.2.1: right after the interface descriptor, announcing the report
                // descriptor first.
                if (w.state != 1 || l < 9 || cfg[pos + 6] != HID_DESC_TYPE_REPORT)
                    return false;
                out->report_len[out->count] = (uint16_t)(cfg[pos + 7] | (cfg[pos + 8] << 8));
                w.state = 2;
            }
            break;
        case TUSB_DESC_ENDPOINT: {
            if (w.state != 2 || l != 7 || ++w.seen > w.declared)
                return false;
            const uint8_t address = cfg[pos + 2];
            const uint8_t number = address & 0x0F;
            const uint8_t attributes = cfg[pos + 3];
            const uint16_t size = (uint16_t)(cfg[pos + 4] | (cfg[pos + 5] << 8));
            const uint8_t interval = cfg[pos + 6];
            const bool in = (address & 0x80) != 0;
            // 9.6.6: bits 6..4 of the address are zero and the number is not 0; the board's
            // controller has endpoints up to MIMHID_MAX_ENDPOINT_NUMBER.
            if ((address & 0x70) != 0 || number == 0 || number > MIMHID_MAX_ENDPOINT_NUMBER)
                return false;
            // Interrupt or bulk, and nothing else in bmAttributes (9.6.6: bits 5..2 are for
            // isochronous endpoints, 7..6 are reserved). HID endpoints are interrupt.
            if (attributes != TUSB_XFER_INTERRUPT && (w.hid || attributes != TUSB_XFER_BULK))
                return false;
            // A packet of 1 to the largest the controller takes. Bulk packet sizes are 8, 16, 32
            // or 64 at full speed and 512 at high speed (USB 2.0 5.8.3).
            if (size == 0 || size > MIMHID_MAX_ENDPOINT_BYTES)
                return false;
            if (attributes == TUSB_XFER_BULK) {
#if TUD_OPT_HIGH_SPEED
                if (size != 512)
                    return false;
#else
                if (size != 8 && size != 16 && size != 32 && size != 64)
                    return false;
#endif
            }
            // bInterval of an interrupt endpoint: 1 to 255 frames at full speed, an exponent 1 to
            // 16 at high speed (9.6.6). A bulk endpoint's is not a polling interval.
            if (attributes == TUSB_XFER_INTERRUPT) {
                if (interval == 0)
                    return false;
#if TUD_OPT_HIGH_SPEED
                if (interval > 16)
                    return false;
#endif
            }
            const uint32_t bit = in ? (1ul << (16 + number)) : (1ul << number);
            if (addresses & bit)
                return false; // an address once
            addresses |= bit;
            mimhid_ep_info_t* info = &out->ep[in ? 1 : 0][number];
            info->kind = w.hid ? MIMHID_EP_HID : MIMHID_EP_GENERIC;
            info->type = attributes;
            info->packet_size = size;
            info->interface = out->count;
            if (in) {
                w.in_count++;
                w.in_addr = address;
                if (w.hid)
                    out->ep_inst[number] = out->hid_count;
            } else {
                w.out_count++;
            }
            break;
        }
        default:
            // Anything else is a class descriptor of a generic interface, placed as given; a HID
            // interface has nothing but its HID descriptor and its endpoints.
            if (w.state == 0 || w.hid)
                return false;
            break;
        }
        pos = (uint16_t)(pos + l);
    }
    if (w.state != 0 && !finish_interface(&w, len, out))
        return false;
    if (g.pending || g.left != 0)
        return false; // an association whose interfaces are not all there
    if (out->count == 0 || cfg[4] != out->count)
        return false; // bNumInterfaces says what the walk found

    // A report descriptor for every HID interface, of the length its HID descriptor announced, and
    // none for any other interface.
    for (uint8_t i = 0; i < MIMHID_MAX_INTERFACES; i++) {
        const mimhid_report_desc_t* r = &c->report[i];
        if (i >= out->count || out->itf_inst[i] == 0xFF) {
            if (r->d.used)
                return false;
            continue;
        }
        if (!r->d.used || r->d.recv != r->d.total || r->d.total != out->report_len[i])
            return false;
    }

    // A string is a complete descriptor of its own length (9.6.7), unless the board makes it.
    for (uint8_t i = 0; i < MIMHID_MAX_STRINGS; i++) {
        const mimhid_string_t* s = &c->string[i];
        if (!s->d.used || s->from_id)
            continue;
        const uint8_t* b = &c->string_pool[s->off];
        if (s->d.recv != s->d.total || b[0] != s->d.total || b[1] != TUSB_DESC_STRING)
            return false;
    }
    return true;
}

// ---- the active configuration ----

void mimhid_clear_config(void) {
    memset(&g_layout, 0, sizeof(g_layout));
    memset(&g_active, 0, sizeof(g_active));
    static const uint8_t device[18] = {FALLBACK_DEVICE};
    static const uint8_t config[TUD_CONFIG_DESC_LEN] = {FALLBACK_CONFIG};
    memcpy(g_active.device_bytes, device, sizeof(device));
    g_active.device = (mimhid_desc_t){.used = true, .total = 18, .recv = 18};
    memcpy(g_active.config_bytes, config, sizeof(config));
    g_active.config =
        (mimhid_desc_t){.used = true, .total = TUD_CONFIG_DESC_LEN, .recv = TUD_CONFIG_DESC_LEN};
    mimhid_ep0_set_size(device[7]);
}

bool mimhid_apply_config(const mimhid_config_t* cfg) {
    mimhid_layout_t layout;
    if (!mimhid_walk_config(cfg, &layout))
        return false; // the previous configuration stays
    memcpy(&g_active, cfg, sizeof(g_active));
    g_layout = layout;
    mimhid_ep0_set_size(g_active.device_bytes[7]);
    return true;
}

uint8_t mimhid_active_interfaces(void) { return g_layout.count; }

uint8_t mimhid_active_hid_instances(void) { return g_layout.hid_count; }

uint8_t mimhid_active_interface_instance(uint8_t iface) {
    return iface < g_layout.count ? g_layout.itf_inst[iface] : 0xFF;
}

uint8_t mimhid_active_instance_interface(uint8_t instance) {
    return instance < g_layout.hid_count ? g_layout.inst_itf[instance] : 0xFF;
}

uint8_t mimhid_active_endpoint_instance(uint8_t endpoint_number) {
    if (endpoint_number >= 16)
        return 0xFF;
    const uint8_t inst = g_layout.ep_inst[endpoint_number];
    return inst < g_layout.hid_count ? inst : 0xFF;
}

uint8_t mimhid_active_instance_endpoint(uint8_t instance) {
    return instance < g_layout.hid_count ? g_layout.inst_ep_in[instance] : 0;
}

const mimhid_ep_info_t* mimhid_active_endpoint(uint8_t address) {
    const mimhid_ep_info_t* e = &g_layout.ep[(address >> 7) & 1][address & 0x0F];
    return (address & 0x70) == 0 && (address & 0x0F) != 0 && e->kind != MIMHID_EP_NONE ? e : NULL;
}

uint16_t mimhid_active_generic_span(uint8_t iface) {
    if (iface >= g_layout.count || g_layout.itf_inst[iface] != 0xFF)
        return 0;
    return g_layout.itf_span[iface];
}

const uint8_t* mimhid_active_config_bytes(void) { return g_active.config_bytes; }

// Feature report data for GET_REPORT; the id byte is added by TinyUSB.
const mimhid_feature_t* mimhid_find_feature(uint8_t instance, uint8_t report_id) {
    for (uint8_t f = 0; f < MIMHID_MAX_FEATURES; f++) {
        if (g_active.feature[f].used && g_active.feature[f].report_id == report_id &&
            mimhid_active_interface_instance(g_active.feature[f].iface) == instance) {
            return &g_active.feature[f];
        }
    }
    return NULL;
}

void mimhid_active_set_feature(uint8_t iface, uint8_t report_id, const uint8_t* data,
                               uint16_t len) {
    if (mimhid_active_interface_instance(iface) == 0xFF || len > MIMHID_FEATURE_MAX)
        return;
    mimhid_feature_t* free_slot = NULL;
    for (uint8_t f = 0; f < MIMHID_MAX_FEATURES; f++) {
        mimhid_feature_t* e = &g_active.feature[f];
        if (e->used && e->iface == iface && e->report_id == report_id) {
            free_slot = e;
            break;
        }
        if (!e->used && free_slot == NULL)
            free_slot = e;
    }
    if (free_slot == NULL)
        return;
    free_slot->used = true;
    free_slot->iface = iface;
    free_slot->report_id = report_id;
    free_slot->len = len;
    memcpy(free_slot->data, data, len);
}

// ---- TinyUSB callbacks ----

uint8_t const* tud_descriptor_device_cb(void) { return g_active.device_bytes; }

uint8_t const* tud_hid_descriptor_report_cb(uint8_t instance) {
    const uint8_t iface = mimhid_active_instance_interface(instance);
    return iface == 0xFF ? NULL : &g_active.report_pool[g_active.report[iface].off];
}

uint8_t const* tud_descriptor_configuration_cb(uint8_t index) {
    if (index >= g_active.device_bytes[17])
        return NULL;
    return g_active.config_bytes;
}

static uint16_t g_desc_string[MIMHID_STRING_DESC_MAX / 2 + 1];

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    if (index == 0) {
        // The language list (USB 2.0 9.6.7): the languages of the strings the board holds, once
        // each, in the order they were staged. Nothing is made up for a board with no strings.
        uint16_t langs[MIMHID_MAX_STRINGS];
        uint8_t count = 0;
        for (uint8_t i = 0; i < MIMHID_MAX_STRINGS; i++) {
            const mimhid_string_t* s = &g_active.string[i];
            if (!s->d.used)
                continue;
            bool known = false;
            for (uint8_t k = 0; k < count; k++)
                known = known || langs[k] == s->language;
            if (!known)
                langs[count++] = s->language;
        }
        if (count == 0)
            return NULL;
        for (uint8_t k = 0; k < count; k++)
            g_desc_string[1 + k] = langs[k];
        g_desc_string[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * count + 2));
        return g_desc_string;
    }
    for (uint8_t i = 0; i < MIMHID_MAX_STRINGS; i++) {
        const mimhid_string_t* s = &g_active.string[i];
        if (!s->d.used || s->index != index || s->language != langid)
            continue;
        memcpy(g_desc_string, &g_active.string_pool[s->off], s->d.total);
        return g_desc_string;
    }
    return NULL;
}
