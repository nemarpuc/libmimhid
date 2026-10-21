// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#ifndef MIMHID_CONFIG_H
#define MIMHID_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include "mimhid_proto.h"

#ifndef MIMHID_MAX_DESC_BYTES
#define MIMHID_MAX_DESC_BYTES 2048 // all report descriptors together
#endif
#ifndef MIMHID_MAX_FEATURES
#define MIMHID_MAX_FEATURES 8
#endif

// What the board takes, set here to the largest the chip can hold and announced in the PONG.
// A port may set any of them before this header. A full-speed endpoint holds 64 bytes and a
// high-speed one 1024; a Feature report travels on the control pipe, so it may be longer than
// an endpoint packet.
#ifndef MIMHID_MAX_ENDPOINT_BYTES
#if defined(CFG_TUSB_RHPORT0_MODE) && defined(OPT_MODE_HIGH_SPEED) && \
    (CFG_TUSB_RHPORT0_MODE & OPT_MODE_HIGH_SPEED)
#define MIMHID_MAX_ENDPOINT_BYTES 1024
#else
#define MIMHID_MAX_ENDPOINT_BYTES 64
#endif
#endif
#ifndef MIMHID_MAX_REPORT_BYTES
#define MIMHID_MAX_REPORT_BYTES MIMHID_MAX_ENDPOINT_BYTES // largest report accepted from the host
#endif
#ifndef MIMHID_FEATURE_MAX
#define MIMHID_FEATURE_MAX 1024 // largest Feature report the board answers
#endif
// The buffer TinyUSB keeps per HID interface: it holds a report and a Feature report.
#define MIMHID_HID_BUFSIZE \
    (MIMHID_MAX_REPORT_BYTES > MIMHID_FEATURE_MAX ? MIMHID_MAX_REPORT_BYTES : MIMHID_FEATURE_MAX)
// A frame carries a report, a Feature report or a configuration command, and a few bytes more.
#define MIMHID_MAX_FRAME_PAYLOAD_RAW (MIMHID_HID_BUFSIZE + 8)
#define MIMHID_MAX_FRAME_PAYLOAD \
    (MIMHID_MAX_FRAME_PAYLOAD_RAW > MIMHID_MIN_FRAME_PAYLOAD ? MIMHID_MAX_FRAME_PAYLOAD_RAW \
                                                             : MIMHID_MIN_FRAME_PAYLOAD)
_Static_assert(MIMHID_MAX_FRAME_PAYLOAD <= MIMHID_PROTO_MAX_PAYLOAD, "frame limit of the protocol");
_Static_assert(MIMHID_MAX_ENDPOINT_BYTES <= 1024, "an endpoint packet is at most 1024 bytes");

// The data stage of a control transfer the host may answer or read: a Feature report fits, and a
// descriptor of the sizes this board can hold.
#ifndef MIMHID_CONTROL_DATA_MAX
#define MIMHID_CONTROL_DATA_MAX MIMHID_HID_BUFSIZE
#endif
_Static_assert(MIMHID_CONTROL_DATA_MAX + 3 <= MIMHID_MAX_FRAME_PAYLOAD, "a reply fits a frame");

typedef struct {
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t bcd_device;
    uint8_t  device_class;
    uint8_t  device_subclass;
    uint8_t  device_protocol;
    uint16_t language_id;
    uint16_t bcd_usb;
    uint8_t  ep0_max_packet_size;
    uint8_t  configuration_value;
    uint8_t  bm_attributes;
    uint8_t  bmax_power; // 2 mA units
    uint8_t  flags;      // bit 0: serial from chip id
    char     manufacturer[MIMHID_STRING_MAX + 1];
    char     product[MIMHID_STRING_MAX + 1];
    char     serial[MIMHID_STRING_MAX + 1];
    char     configuration[MIMHID_STRING_MAX + 1];
} mimhid_dev_cfg_t;

typedef struct {
    bool     used;
    uint8_t  iface;
    uint8_t  interface_class;
    uint8_t  interface_subclass;
    uint8_t  interface_protocol;
    uint16_t bcd_hid;
    uint8_t  country;
    uint8_t  ep_in_addr;
    uint16_t ep_size;
    uint8_t  poll_interval;
    uint8_t  ep_out_addr;
    uint16_t ep_out_size;
    uint8_t  poll_interval_out;
    uint16_t desc_len;
    uint16_t desc_recv;
    uint16_t desc_off; // into the pool
    char     interface_string[MIMHID_STRING_MAX + 1];
} mimhid_node_cfg_t;

typedef struct {
    bool    used;
    uint8_t node;
    uint8_t report_id;
    uint16_t len;
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
void mimhid_active_set_feature(uint8_t node, uint8_t report_id, const uint8_t* data, uint16_t len);

// Forgets the active configuration (no interfaces, no nodes).
void mimhid_clear_config(void);

// Interface (HID instance) count of the active configuration.
uint8_t mimhid_active_interfaces(void);

// Interface that carries a node of the active configuration, or 0xFF.
uint8_t mimhid_active_node_interface(uint8_t node);

#endif // MIMHID_CONFIG_H
