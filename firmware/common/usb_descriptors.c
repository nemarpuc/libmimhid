// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
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
// mimusb_core_init replaces with the fallback (mimusb_clear_config); an initializer here would put
// a copy of the whole structure, 18 KB, in the flash image.
static mimusb_config_t g_active;
// What the walk of the active configuration found. Zero (no interfaces) is a valid starting state.
static mimusb_layout_t g_layout;

// The unique id as a serial number string descriptor: the hexadecimal digits of up to 15 bytes of
// the id, which fit the string limit (USB 2.0 9.6.7). Returns the descriptor length in bytes, or 0
// when the chip has no id.
uint16_t mimusb_unique_id_string(uint8_t* dst) {
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
    bool inside;      // after an interface descriptor and before the next one
    uint8_t declared; // bNumEndpoints
    uint8_t seen;
    uint8_t number; // bInterfaceNumber
    uint8_t alt;    // bAlternateSetting
    uint16_t start; // offset of the interface descriptor
} walk_itf_t;

// An interface association announced and not finished (the interfaces it names still follow).
typedef struct {
    uint8_t first; // bFirstInterface
    uint8_t left;  // interfaces of the group whose descriptors have not been seen yet
    bool pending;  // announced, its first interface descriptor not yet seen
} walk_group_t;

// The interface descriptor that has just ended, which began at w->start and ends at `end`, must be
// complete: every endpoint it announced. What lies between its descriptors is the caller's: the
// board reads no class. A descriptor of alternate setting 0 is an interface of its own; one of
// another setting extends the span of the interface it belongs to, so that TinyUSB's open (which
// is called once per interface, or once per association) takes the settings with it.
static bool finish_interface(const walk_itf_t* w, uint16_t end, mimusb_layout_t* out) {
    if (w->seen != w->declared)
        return false;
    const uint8_t n = w->number;
    if (w->alt == 0) {
        out->itf_start[n] = w->start;
        out->itf_alts[n] = 1;
        out->count++;
    } else {
        out->itf_alts[n]++;
    }
    out->itf_span[n] = (uint16_t)(end - out->itf_start[n]);
    // Another interface of an association group extends the span of the group's first one: TinyUSB
    // calls the driver's open once for the group (process_set_config in usbd.c) and goes on from
    // the end of what open returns.
    const uint8_t first = out->itf_group[n];
    if (first != n)
        out->itf_span[first] = (uint16_t)(end - out->itf_start[first]);
    return true;
}

bool mimusb_walk_config(const mimusb_config_t* c, mimusb_layout_t* out) {
    memset(out, 0, sizeof(*out));

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
    uint32_t addresses = 0;     // bit n: OUT endpoint n, bit 16 + n: IN endpoint n, of any setting
    uint32_t alt_addresses = 0; // the same, of the interface descriptor being walked
    uint16_t pos = 9;
    while (pos < len) {
        const uint8_t l = cfg[pos];
        if (l < 2 || (uint32_t)pos + l > len)
            return false; // every bLength at least 2 and inside the blob
        const uint8_t type = cfg[pos + 1];
        // An interface or an association ends the interface before it.
        if (type == TUSB_DESC_INTERFACE || type == TUSB_DESC_INTERFACE_ASSOCIATION) {
            if (w.inside) {
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
            // in order (the host composes them so, and TinyUSB binds an association by number); the
            // alternate settings of an interface follow its setting 0 as 1, 2, ... in order, and
            // nothing comes between them (an association, in particular, does not).
            if (l != 9 || out->count > MIMUSB_MAX_INTERFACES)
                return false;
            const uint8_t number = cfg[pos + 2];
            const uint8_t alt = cfg[pos + 3];
            if (alt == 0) {
                if (out->count >= MIMUSB_MAX_INTERFACES || number != out->count)
                    return false;
                // The group an interface belongs to is named by its first interface; the first one
                // also spans the whole group, which finish_interface completes.
                out->itf_group[number] = g.left != 0 ? g.first : number;
                if (g.left != 0)
                    g.left--;
                g.pending = false;
            } else {
                if (out->count == 0 || number != out->count - 1 || g.pending ||
                    alt != out->itf_alts[number] || out->itf_alts[number] == 255)
                    return false;
            }
            memset(&w, 0, sizeof(w));
            w.inside = true;
            w.number = number;
            w.alt = alt;
            w.declared = cfg[pos + 4];
            w.start = pos;
            alt_addresses = 0;
            break;
        }
        case TUSB_DESC_ENDPOINT: {
            // 7 bytes, or 9 with the bRefresh and bSynchAddress of an audio-class endpoint (USB
            // Device Class Definition for Audio Devices 1.0, 4.6.1.1 and 4.6.2.1; the board reads no
            // class, and the two bytes are served as given).
            if (!w.inside || (l != 7 && l != 9) || ++w.seen > w.declared)
                return false;
            const uint8_t address = cfg[pos + 2];
            const uint8_t number = address & 0x0F;
            const uint8_t attributes = cfg[pos + 3];
            const uint16_t size = (uint16_t)(cfg[pos + 4] | (cfg[pos + 5] << 8));
            const uint8_t interval = cfg[pos + 6];
            const bool in = (address & 0x80) != 0;
            // 9.6.6: bits 6..4 of the address are zero and the number is not 0; the board's
            // controller has endpoints up to MIMUSB_MAX_ENDPOINT_NUMBER.
            if ((address & 0x70) != 0 || number == 0 || number > MIMUSB_MAX_ENDPOINT_NUMBER)
                return false;
            // The transfer type is bits 1..0 of bmAttributes. Interrupt and bulk carry nothing else
            // there (9.6.6: bits 5..2 are for isochronous endpoints, 7..6 are reserved); an
            // isochronous one carries its synchronization and usage type in bits 5..2.
            const uint8_t xfer = attributes & 0x03;
            const bool iso = xfer == TUSB_XFER_ISOCHRONOUS;
            if (iso ? (MIMUSB_ISO_BYTES == 0 || (attributes & 0xC0) != 0)
                    : (attributes != TUSB_XFER_INTERRUPT && attributes != TUSB_XFER_BULK))
                return false;
            if (iso) {
                // 5.6.3, 9.6.6: 1 to the largest the controller takes, and bInterval 1 at full
                // speed (an exponent of 1 to 16 at high speed). Packet sizes that name more than one
                // transaction per microframe (bits 12..11) are above MIMUSB_ISO_BYTES.
                if (size == 0 || size > MIMUSB_ISO_BYTES)
                    return false;
#if TUD_OPT_HIGH_SPEED
                if (interval == 0 || interval > 16)
                    return false;
#else
                if (interval != 1)
                    return false;
#endif
            } else {
                // A packet of 1 to the largest the controller takes. Bulk packet sizes are 8, 16, 32
                // or 64 at full speed and 512 at high speed (USB 2.0 5.8.3).
                if (size == 0 || size > MIMUSB_MAX_ENDPOINT_BYTES)
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
            }
            const uint32_t bit = in ? (1ul << (16 + number)) : (1ul << number);
            if (alt_addresses & bit)
                return false; // an address once in a setting
            alt_addresses |= bit;
            mimusb_ep_info_t* info = &out->ep[in ? 1 : 0][number];
            if (addresses & bit) {
                // Seen before: in another setting of the same interface, or in another interface,
                // which is not allowed. The controller opens a bulk or interrupt endpoint once for
                // the whole configuration (generic.c), so its descriptor repeats as it was; an
                // isochronous endpoint is activated with the descriptor of the setting selected, and
                // its buffer is the largest of them.
                if (info->interface != w.number || info->type != xfer)
                    return false;
                if (iso) {
                    if (size > info->packet_size)
                        info->packet_size = size;
                } else if (info->packet_size != size || info->interval != interval ||
                           info->attributes != attributes) {
                    return false;
                }
                break;
            }
            addresses |= bit;
            info->kind = MIMUSB_EP_GENERIC;
            info->type = xfer;
            info->attributes = attributes;
            info->interval = interval;
            info->packet_size = size;
            info->interface = w.number;
            break;
        }
        default:
            // Anything else is a class descriptor of the interface it follows, placed as given (a
            // HID descriptor is one; the board does not read it).
            if (!w.inside)
                return false;
            break;
        }
        pos = (uint16_t)(pos + l);
    }
    if (w.inside && !finish_interface(&w, len, out))
        return false;
    if (g.pending || g.left != 0)
        return false; // an association whose interfaces are not all there
    if (out->count == 0 || cfg[4] != out->count)
        return false; // bNumInterfaces says what the walk found

    // The BOS descriptor, if there is one (USB 3.2 9.6.2): a 5-byte header (bLength 5, type 0x0F,
    // wTotalLength its own length, bNumDeviceCaps) and that many device capability descriptors, each
    // at least 3 bytes (bLength, bDescriptorType 0x10, bDevCapabilityType) and inside the bytes. No
    // capability is read beyond that. A host asks for a BOS only when bcdUSB is 0x0201 or more (USB
    // 2.0 Link Power Management ECN; USB 3.2 9.6.2), so a BOS with a lower bcdUSB is a mistake.
    if (c->bos.used) {
        if (c->bos.recv != c->bos.total || c->bos.total < 5)
            return false;
        const uint8_t* b = c->bos_bytes;
        const uint16_t blen = c->bos.total;
        if (b[0] != 5 || b[1] != 0x0F || (uint16_t)(b[2] | (b[3] << 8)) != blen)
            return false;
        if ((uint16_t)(d[2] | (d[3] << 8)) < 0x0201)
            return false;
        uint16_t at = 5;
        uint8_t caps = 0;
        while (at < blen) {
            const uint8_t cl = b[at];
            if (cl < 3 || (uint32_t)at + cl > blen || b[at + 1] != 0x10)
                return false;
            at = (uint16_t)(at + cl);
            if (++caps == 0)
                return false; // 256 capabilities cannot be counted in a byte
        }
        if (caps != b[4])
            return false;
    }

    // A string is a complete descriptor of its own length (9.6.7), unless the board makes it.
    for (uint8_t i = 0; i < MIMUSB_MAX_STRINGS; i++) {
        const mimusb_string_t* s = &c->string[i];
        if (!s->d.used || s->from_id)
            continue;
        const uint8_t* b = &c->string_pool[s->off];
        if (s->d.recv != s->d.total || b[0] != s->d.total || b[1] != TUSB_DESC_STRING)
            return false;
    }
    return true;
}

// ---- the active configuration ----

void mimusb_clear_config(void) {
    memset(&g_layout, 0, sizeof(g_layout));
    memset(&g_active, 0, sizeof(g_active));
    static const uint8_t device[18] = {FALLBACK_DEVICE};
    static const uint8_t config[TUD_CONFIG_DESC_LEN] = {FALLBACK_CONFIG};
    memcpy(g_active.device_bytes, device, sizeof(device));
    g_active.device = (mimusb_desc_t){.used = true, .total = 18, .recv = 18};
    memcpy(g_active.config_bytes, config, sizeof(config));
    g_active.config =
        (mimusb_desc_t){.used = true, .total = TUD_CONFIG_DESC_LEN, .recv = TUD_CONFIG_DESC_LEN};
    mimusb_ep0_set_size(device[7]);
}

bool mimusb_apply_config(const mimusb_config_t* cfg) {
    mimusb_layout_t layout;
    if (!mimusb_walk_config(cfg, &layout))
        return false; // the previous configuration stays
    memcpy(&g_active, cfg, sizeof(g_active));
    g_layout = layout;
    mimusb_ep0_set_size(g_active.device_bytes[7]);
    return true;
}

const mimusb_layout_t* mimusb_active_layout(void) { return &g_layout; }

uint8_t mimusb_active_interfaces(void) { return g_layout.count; }

const mimusb_ep_info_t* mimusb_active_endpoint(uint8_t address) {
    const mimusb_ep_info_t* e = &g_layout.ep[(address >> 7) & 1][address & 0x0F];
    return (address & 0x70) == 0 && (address & 0x0F) != 0 && e->kind != MIMUSB_EP_NONE ? e : NULL;
}

uint16_t mimusb_active_generic_span(uint8_t iface) {
    if (iface >= g_layout.count)
        return 0;
    return g_layout.itf_span[iface];
}

const uint8_t* mimusb_active_config_bytes(void) { return g_active.config_bytes; }

// ---- TinyUSB callbacks ----

uint8_t const* tud_descriptor_device_cb(void) { return g_active.device_bytes; }

// The BOS descriptor of the active configuration, or NULL: TinyUSB stalls the request then (usbd.c,
// process_control_request for TUSB_DESC_BOS), as it does with the default callback it replaces.
// g_active is written by the USB side at COMMIT (mimusb_apply_config) and read here, by the USB
// side's tud_task: one context, so a reader never sees it half written. A COMMIT that changes it
// comes after a detach, which ends any transfer of it.
uint8_t const* tud_descriptor_bos_cb(void) { return g_active.bos.used ? g_active.bos_bytes : NULL; }

uint8_t const* tud_descriptor_configuration_cb(uint8_t index) {
    if (index >= g_active.device_bytes[17])
        return NULL;
    return g_active.config_bytes;
}

static uint16_t g_desc_string[MIMUSB_STRING_DESC_MAX / 2 + 1];

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    if (index == 0) {
        // The language list (USB 2.0 9.6.7): the languages of the strings the board holds, once
        // each, in the order they were staged. Nothing is made up for a board with no strings.
        uint16_t langs[MIMUSB_MAX_STRINGS];
        uint8_t count = 0;
        for (uint8_t i = 0; i < MIMUSB_MAX_STRINGS; i++) {
            const mimusb_string_t* s = &g_active.string[i];
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
    for (uint8_t i = 0; i < MIMUSB_MAX_STRINGS; i++) {
        const mimusb_string_t* s = &g_active.string[i];
        if (!s->d.used || s->index != index || s->language != langid)
            continue;
        memcpy(g_desc_string, &g_active.string_pool[s->off], s->d.total);
        return g_desc_string;
    }
    return NULL;
}
