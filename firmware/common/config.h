// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#ifndef MIMHID_CONFIG_H
#define MIMHID_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include "mimhid_proto.h"

#ifndef MIMHID_MAX_DESC_BYTES
#define MIMHID_MAX_DESC_BYTES 1024 // all report descriptors together
#endif
#ifndef MIMHID_MAX_FEATURES
#define MIMHID_MAX_FEATURES 8
#endif
#define MIMHID_MAX_REPORT_BYTES 64 // largest report accepted from the host (the HID EP buffer)

typedef struct {
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t bcd_device;
    uint8_t  device_class;
    uint8_t  device_subclass;
    uint8_t  device_protocol;
    uint16_t language_id;
    uint8_t  flags;
    uint8_t  max_power; // 2 mA units
    char     manufacturer[MIMHID_STRING_MAX + 1];
    char     product[MIMHID_STRING_MAX + 1];
    char     serial[MIMHID_STRING_MAX + 1];
    char     configuration[MIMHID_STRING_MAX + 1];
} mimhid_dev_cfg_t;

typedef struct {
    bool     used;
    uint8_t  iface;
    uint8_t  poll_interval;
    uint8_t  ep_size;
    uint8_t  ep_out_size;
    uint8_t  boot_protocol;
    uint8_t  country;
    uint16_t bcd_hid;
    uint16_t desc_len;
    uint16_t desc_recv;
    uint16_t desc_off; // into the pool
    char     interface_string[MIMHID_STRING_MAX + 1];
} mimhid_node_cfg_t;

typedef struct {
    bool    used;
    uint8_t node;
    uint8_t report_id;
    uint8_t len;
    uint8_t data[MIMHID_FEATURE_MAX];
} mimhid_feature_t;

typedef struct {
    bool               valid; // DEVICE_CONFIG seen
    mimhid_dev_cfg_t  dev;
    mimhid_node_cfg_t node[MIMHID_MAX_NODES];
    mimhid_feature_t  feature[MIMHID_MAX_FEATURES];
    uint16_t           pool_used;
    uint8_t            pool[MIMHID_MAX_DESC_BYTES];
} mimhid_config_t;

// Builds the active descriptors from a configuration and makes it the one TinyUSB serves.
// Returns false (leaving the previous descriptors) if the nodes do not form a valid layout.
bool mimhid_apply_config(const mimhid_config_t* cfg);

// Feature report data for GET_REPORT of the active configuration, or NULL.
const mimhid_feature_t* mimhid_find_feature(uint8_t iface, uint8_t report_id);

// Changes the data returned for a Feature report of the active configuration; a no-op when the
// node or report is not part of it.
void mimhid_active_set_feature(uint8_t node, uint8_t report_id, const uint8_t* data, uint8_t len);

// Forgets the active configuration (no interfaces, no nodes).
void mimhid_clear_config(void);

// Interface (HID instance) count of the active configuration.
uint8_t mimhid_active_interfaces(void);

// Interface that carries a node of the active configuration, or 0xFF.
uint8_t mimhid_active_node_interface(uint8_t node);

#endif // MIMHID_CONFIG_H
