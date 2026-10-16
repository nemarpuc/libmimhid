// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
// Descriptors served to the target, built from the configuration the host committed.
#include "config.h"
#include "core.h"
#include "hal.h"
#include "tusb.h"
#include <string.h>

// Strings: 0 language, 1 manufacturer, 2 product, 3 serial, 4 configuration, 5.. interfaces.
#define STR_FIRST_INTERFACE 5

#define IFACE_DESC_LEN 9
#define HID_DESC_LEN 9
#define EP_DESC_LEN 7
#define PER_ITF_LEN (IFACE_DESC_LEN + HID_DESC_LEN + EP_DESC_LEN)
#define MAX_PER_ITF_LEN (IFACE_DESC_LEN + HID_DESC_LEN + 2 * EP_DESC_LEN)

static const tusb_desc_device_t k_fallback_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0xCAFE,
    .idProduct = 0x4000,
    .bcdDevice = 0x0100,
    .iManufacturer = 0,
    .iProduct = 0,
    .iSerialNumber = 0,
    .bNumConfigurations = 1,
};

static const uint8_t k_fallback_cfg_desc[TUD_CONFIG_DESC_LEN] = {
    TUD_CONFIG_DESCRIPTOR(1, 0, 0, TUD_CONFIG_DESC_LEN, 0x80, 100)};

static tusb_desc_device_t g_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0xCAFE,
    .idProduct = 0x4000,
    .bcdDevice = 0x0100,
    .iManufacturer = 0,
    .iProduct = 0,
    .iSerialNumber = 0,
    .bNumConfigurations = 1,
};
static uint8_t g_cfg_desc[TUD_CONFIG_DESC_LEN + MIMHID_HID_ITFS * MAX_PER_ITF_LEN] = {
    TUD_CONFIG_DESCRIPTOR(1, 0, 0, TUD_CONFIG_DESC_LEN, 0x80, 100)};
static uint8_t g_rd_pool[MIMHID_MAX_DESC_BYTES];
static uint16_t g_rd_off[MIMHID_HID_ITFS];
static uint16_t g_rd_len[MIMHID_HID_ITFS];
static uint8_t g_iface_count = 0;
static uint8_t g_node_iface[MIMHID_MAX_NODES] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static uint16_t g_language = 0x0409;

static char g_str_mfg[MIMHID_STRING_MAX + 1];
static char g_str_product[MIMHID_STRING_MAX + 1];
static char g_str_serial[MIMHID_STRING_MAX + 1];
static char g_str_config[MIMHID_STRING_MAX + 1];
static char g_str_iface[MIMHID_HID_ITFS][MIMHID_STRING_MAX + 1];

static mimhid_feature_t g_feature[MIMHID_MAX_FEATURES];

_Static_assert(MIMHID_MAX_NODES <= 16, "node id is a 4-bit field in the report frame type");

void mimhid_clear_config(void) {
    g_iface_count = 0;
    for (uint8_t n = 0; n < MIMHID_MAX_NODES; n++)
        g_node_iface[n] = 0xFF;
    memset(g_feature, 0, sizeof(g_feature));
    g_device = k_fallback_device;
    memcpy(g_cfg_desc, k_fallback_cfg_desc, sizeof(k_fallback_cfg_desc));
}

uint8_t mimhid_active_interfaces(void) { return g_iface_count; }

uint8_t mimhid_active_node_interface(uint8_t node) {
    return node < MIMHID_MAX_NODES ? g_node_iface[node] : 0xFF;
}

static void copy_str(char* dst, const char* src) {
    size_t i = 0;
    while (i < MIMHID_STRING_MAX && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void hex_serial(char* dst) {
    uint8_t id[16];
    size_t n = hal_unique_id(id, sizeof(id));
    if (n > 15)
        n = 15; // 30 hex characters fit the string limit
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        dst[2 * i] = hex[id[i] >> 4];
        dst[2 * i + 1] = hex[id[i] & 15];
    }
    dst[2 * n] = '\0';
}

bool mimhid_apply_config(const mimhid_config_t* cfg) {
    if (!cfg->valid)
        return false;

    // Interface indices must be dense: every index below the highest one carries a node.
    uint8_t count = 0;
    bool seen[MIMHID_HID_ITFS] = {false};
    for (uint8_t n = 0; n < MIMHID_MAX_NODES; n++) {
        const mimhid_node_cfg_t* node = &cfg->node[n];
        if (!node->used)
            continue;
        if (node->iface >= MIMHID_HID_ITFS || node->desc_recv != node->desc_len)
            return false;
        seen[node->iface] = true;
        if (node->iface + 1u > count)
            count = (uint8_t)(node->iface + 1u);
    }
    if (count == 0)
        return false;
    for (uint8_t i = 0; i < count; i++) {
        if (!seen[i])
            return false;
    }

    // Report descriptors: the nodes of an interface, in node order, back to back.
    uint16_t pos = 0;
    for (uint8_t i = 0; i < count; i++) {
        g_rd_off[i] = pos;
        for (uint8_t n = 0; n < MIMHID_MAX_NODES; n++) {
            const mimhid_node_cfg_t* node = &cfg->node[n];
            if (!node->used || node->iface != i)
                continue;
            if ((uint32_t)pos + node->desc_len > sizeof(g_rd_pool))
                return false;
            memcpy(&g_rd_pool[pos], &cfg->pool[node->desc_off], node->desc_len);
            pos = (uint16_t)(pos + node->desc_len);
        }
        g_rd_len[i] = (uint16_t)(pos - g_rd_off[i]);
    }

    for (uint8_t n = 0; n < MIMHID_MAX_NODES; n++) {
        g_node_iface[n] = cfg->node[n].used ? cfg->node[n].iface : 0xFF;
    }

    // Device descriptor
    const mimhid_dev_cfg_t* dev = &cfg->dev;
    g_device = (tusb_desc_device_t){
        .bLength = sizeof(tusb_desc_device_t),
        .bDescriptorType = TUSB_DESC_DEVICE,
        .bcdUSB = 0x0200,
        .bDeviceClass = dev->device_class,
        .bDeviceSubClass = dev->device_subclass,
        .bDeviceProtocol = dev->device_protocol,
        .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
        .idVendor = dev->vendor_id,
        .idProduct = dev->product_id,
        .bcdDevice = dev->bcd_device,
        .iManufacturer = dev->manufacturer[0] ? 1 : 0,
        .iProduct = dev->product[0] ? 2 : 0,
        .iSerialNumber = 0,
        .bNumConfigurations = 1,
    };

    g_language = dev->language_id ? dev->language_id : 0x0409;
    copy_str(g_str_mfg, dev->manufacturer);
    copy_str(g_str_product, dev->product);
    copy_str(g_str_config, dev->configuration);
    copy_str(g_str_serial, dev->serial);
    if (g_str_serial[0] == '\0' && (dev->flags & MIMHID_DEVFLAG_SERIAL_FROM_ID)) {
        hex_serial(g_str_serial);
    }
    g_device.iSerialNumber = g_str_serial[0] ? 3 : 0;

    // Configuration descriptor built in a single pass
    uint16_t total = TUD_CONFIG_DESC_LEN;
    uint8_t* p = g_cfg_desc + TUD_CONFIG_DESC_LEN;
    for (uint8_t i = 0; i < count; i++) {
        const mimhid_node_cfg_t* first = NULL;
        uint8_t ep_out = 0;
        uint8_t ep_in = 0;
        uint8_t min_interval = 255;
        for (uint8_t n = 0; n < MIMHID_MAX_NODES; n++) {
            if (cfg->node[n].used && cfg->node[n].iface == i) {
                if (first == NULL)
                    first = &cfg->node[n];
                if (cfg->node[n].ep_size > ep_in) {
                    ep_in = cfg->node[n].ep_size;
                }
                if (cfg->node[n].poll_interval > 0 && cfg->node[n].poll_interval < min_interval) {
                    min_interval = cfg->node[n].poll_interval;
                }
                if (cfg->node[n].ep_out_size > ep_out) {
                    ep_out = cfg->node[n].ep_out_size;
                }
            }
        }
        if (first == NULL)
            return false;
        const uint16_t itf_len = (uint16_t)(IFACE_DESC_LEN + HID_DESC_LEN + EP_DESC_LEN +
                                            (ep_out > 0 ? EP_DESC_LEN : 0));
        if (total + itf_len > sizeof(g_cfg_desc))
            return false;
        total = (uint16_t)(total + itf_len);
        copy_str(g_str_iface[i], first->interface_string);
        const uint8_t boot = first->boot_protocol;
        const uint8_t ep_size = ep_in > 0 ? ep_in : first->ep_size;
        const uint8_t interval = min_interval != 255 ? min_interval : first->poll_interval;
        const uint8_t itf_head[] = {
            IFACE_DESC_LEN,
            TUSB_DESC_INTERFACE,
            i,
            0,
            (uint8_t)(ep_out > 0 ? 2 : 1),
            TUSB_CLASS_HID,
            boot ? HID_SUBCLASS_BOOT : HID_SUBCLASS_NONE,
            boot ? (uint8_t)boot : HID_ITF_PROTOCOL_NONE,
            (uint8_t)(g_str_iface[i][0] ? STR_FIRST_INTERFACE + i : 0),
            HID_DESC_LEN,
            HID_DESC_TYPE_HID,
            (uint8_t)(first->bcd_hid & 0xFF),
            (uint8_t)(first->bcd_hid >> 8),
            first->country,
            1,
            HID_DESC_TYPE_REPORT,
            (uint8_t)(g_rd_len[i] & 0xFF),
            (uint8_t)(g_rd_len[i] >> 8),
            EP_DESC_LEN,
            TUSB_DESC_ENDPOINT,
            (uint8_t)(0x81 + i),
            TUSB_XFER_INTERRUPT,
            ep_size,
            0,
            interval,
        };
        memcpy(p, itf_head, sizeof(itf_head));
        p += sizeof(itf_head);

        if (ep_out > 0) {
            const uint8_t ep_out_desc[] = {
                EP_DESC_LEN, TUSB_DESC_ENDPOINT, (uint8_t)(0x01 + i), TUSB_XFER_INTERRUPT, ep_out,
                0,           interval,
            };
            memcpy(p, ep_out_desc, sizeof(ep_out_desc));
            p += sizeof(ep_out_desc);
        }
    }

    uint8_t attr = 0x80;
    if (dev->flags & MIMHID_DEVFLAG_SELF_POWERED)
        attr |= 0x40;
    if (dev->flags & MIMHID_DEVFLAG_REMOTE_WAKEUP)
        attr |= 0x20;
    const uint8_t head[] = {
        TUD_CONFIG_DESCRIPTOR(1, count, g_str_config[0] ? 4 : 0, total, attr, 0)};
    memcpy(g_cfg_desc, head, sizeof(head));
    g_cfg_desc[8] = dev->max_power;

    memcpy(g_feature, cfg->feature, sizeof(g_feature));
    g_iface_count = count;
    return true;
}

// Feature report data for GET_REPORT; the id byte is added by TinyUSB.
const mimhid_feature_t* mimhid_find_feature(uint8_t iface, uint8_t report_id) {
    for (uint8_t f = 0; f < MIMHID_MAX_FEATURES; f++) {
        if (g_feature[f].used && g_feature[f].report_id == report_id &&
            mimhid_active_node_interface(g_feature[f].node) == iface) {
            return &g_feature[f];
        }
    }
    return NULL;
}

void mimhid_active_set_feature(uint8_t node, uint8_t report_id, const uint8_t* data, uint8_t len) {
    if (mimhid_active_node_interface(node) == 0xFF || len > MIMHID_FEATURE_MAX)
        return;
    mimhid_feature_t* free_slot = NULL;
    for (uint8_t f = 0; f < MIMHID_MAX_FEATURES; f++) {
        mimhid_feature_t* e = &g_feature[f];
        if (e->used && e->node == node && e->report_id == report_id) {
            free_slot = e;
            break;
        }
        if (!e->used && free_slot == NULL)
            free_slot = e;
    }
    if (free_slot == NULL)
        return;
    free_slot->used = true;
    free_slot->node = node;
    free_slot->report_id = report_id;
    free_slot->len = len;
    memcpy(free_slot->data, data, len);
}

uint8_t const* tud_descriptor_device_cb(void) { return (uint8_t const*)&g_device; }

uint8_t const* tud_hid_descriptor_report_cb(uint8_t instance) {
    if (instance >= g_iface_count)
        return NULL;
    return &g_rd_pool[g_rd_off[instance]];
}

uint8_t const* tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return g_cfg_desc;
}

static uint16_t g_desc_string[MIMHID_STRING_MAX + 2];

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    uint8_t chr_count = 0;

    if (index == 0) {
        g_desc_string[1] = g_language;
        chr_count = 1;
    } else {
        const char* str = NULL;
        if (index == 1)
            str = g_str_mfg;
        else if (index == 2)
            str = g_str_product;
        else if (index == 3)
            str = g_str_serial;
        else if (index == 4)
            str = g_str_config;
        else if (index >= STR_FIRST_INTERFACE && index < STR_FIRST_INTERFACE + g_iface_count)
            str = g_str_iface[index - STR_FIRST_INTERFACE];
        else
            return NULL;

        chr_count = (uint8_t)strlen(str);
        for (uint8_t i = 0; i < chr_count; i++) {
            g_desc_string[1 + i] = (uint16_t)(uint8_t)str[i];
        }
    }

    g_desc_string[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return g_desc_string;
}
