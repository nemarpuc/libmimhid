// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#ifndef MIMHID_CONFIG_H
#define MIMHID_CONFIG_H

#include "mimhid_proto.h"
#include <stdbool.h>
#include <stdint.h>

#ifndef MIMHID_MAX_DESC_BYTES
#define MIMHID_MAX_DESC_BYTES 2048 // all report descriptors together (DESC_HID_REPORT)
#endif
#ifndef MIMHID_MAX_FEATURES
#define MIMHID_MAX_FEATURES 8
#endif
// The configuration descriptor the board holds, as the host composes it: the 9-byte header and, per
// interface, its interface descriptor, HID descriptor and two endpoint descriptors (USB 2.0 9.6.3,
// 9.6.5, 9.6.6; HID 1.11 6.2.1). A configuration that does not fit is refused with NAK OVERFLOW.
#ifndef MIMHID_MAX_CONFIG_BYTES
#define MIMHID_MAX_CONFIG_BYTES (9 + MIMHID_HID_ITFS * (9 + 9 + 7 + 7))
#endif
// The string descriptors the board holds, each with its index and language: manufacturer, product,
// serial, configuration and one per interface. The store is large enough for all of them at the
// longest length USB allows (USB 2.0 9.6.7, 254 bytes); a string that does not fit is refused with
// NAK OVERFLOW.
#ifndef MIMHID_MAX_STRINGS
#define MIMHID_MAX_STRINGS (4 + MIMHID_HID_ITFS)
#endif
#ifndef MIMHID_MAX_STRING_BYTES
#define MIMHID_MAX_STRING_BYTES (MIMHID_MAX_STRINGS * MIMHID_STRING_DESC_MAX)
#endif

// What the board takes, announced in the PONG. MIMHID_MAX_ENDPOINT_BYTES is the port's own
// (core.h); the others may be set by a port before this header. A Feature report travels on the
// control pipe, so it may be longer than an endpoint packet.
#include "core.h"
#ifndef MIMHID_MAX_REPORT_BYTES
#define MIMHID_MAX_REPORT_BYTES MIMHID_MAX_ENDPOINT_BYTES // largest report accepted from the host
#endif
#ifndef MIMHID_FEATURE_MAX
#define MIMHID_FEATURE_MAX 1024 // largest Feature report the board answers
#endif
// The buffer TinyUSB keeps per HID interface: it holds a report and a Feature report.
#define MIMHID_HID_BUFSIZE                                                                         \
    (MIMHID_MAX_REPORT_BYTES > MIMHID_FEATURE_MAX ? MIMHID_MAX_REPORT_BYTES : MIMHID_FEATURE_MAX)
// A frame carries a report, a Feature report or a configuration command, and a few bytes more.
#define MIMHID_MAX_FRAME_PAYLOAD_RAW (MIMHID_HID_BUFSIZE + 8)
#define MIMHID_MAX_FRAME_PAYLOAD                                                                   \
    (MIMHID_MAX_FRAME_PAYLOAD_RAW > MIMHID_MIN_FRAME_PAYLOAD ? MIMHID_MAX_FRAME_PAYLOAD_RAW        \
                                                             : MIMHID_MIN_FRAME_PAYLOAD)
_Static_assert(MIMHID_MAX_FRAME_PAYLOAD <= MIMHID_PROTO_MAX_PAYLOAD, "frame limit of the protocol");
// DESC_DATA: its sequence number, the six bytes that name the descriptor and the chunk.
_Static_assert(1 + 6 + MIMHID_DESC_DATA_CHUNK <= MIMHID_MAX_FRAME_PAYLOAD,
               "a DESC_DATA chunk fits a frame");
_Static_assert(MIMHID_MAX_ENDPOINT_BYTES <= 1024, "an endpoint packet is at most 1024 bytes");

// The data stage of a control transfer the host may answer or read: a Feature report fits, and a
// descriptor of the sizes this board can hold.
#ifndef MIMHID_CONTROL_DATA_MAX
#define MIMHID_CONTROL_DATA_MAX MIMHID_HID_BUFSIZE
#endif
_Static_assert(MIMHID_CONTROL_DATA_MAX + 3 <= MIMHID_MAX_FRAME_PAYLOAD, "a reply fits a frame");

// A descriptor the host announced with DESC_BEGIN and is still sending (or has sent) with
// DESC_DATA.
typedef struct {
    bool used;
    uint16_t total; // length announced
    uint16_t recv;  // bytes received so far (no gaps)
} mimhid_desc_t;

typedef struct {
    mimhid_desc_t d;
    uint8_t index;
    uint16_t language;
    bool from_id; // made by the board from the chip id (MIMHID_DESC_F_SERIAL_FROM_ID)
    uint16_t off; // into the string pool
} mimhid_string_t;

typedef struct {
    mimhid_desc_t d;
    uint16_t off; // into the report descriptor pool
} mimhid_report_desc_t;

typedef struct {
    bool used;
    uint8_t iface;
    uint8_t report_id;
    uint16_t len;
    uint8_t data[MIMHID_FEATURE_MAX];
} mimhid_feature_t;

typedef struct {
    uint8_t depth;  // transfers that may wait, 0: not set
    uint8_t policy; // MIMHID_EP_FULL_*
} mimhid_ep_cfg_t;

// What the host staged with DESC_*, FEATURE and EP_CONFIG, and what a COMMIT makes active.
typedef struct {
    mimhid_desc_t device;
    _Alignas(4) uint8_t device_bytes[18];
    mimhid_desc_t config;
    _Alignas(4) uint8_t config_bytes[MIMHID_MAX_CONFIG_BYTES];
    mimhid_string_t string[MIMHID_MAX_STRINGS];
    uint16_t string_used; // bytes of the pool
    uint8_t string_pool[MIMHID_MAX_STRING_BYTES];
    mimhid_report_desc_t report[MIMHID_HID_ITFS]; // by interface number
    uint16_t report_used;
    uint8_t report_pool[MIMHID_MAX_DESC_BYTES];
    mimhid_feature_t feature[MIMHID_MAX_FEATURES];
    mimhid_ep_cfg_t ep[16]; // by IN endpoint number
} mimhid_config_t;

// What a walk of a staged configuration finds. The HID interfaces are numbered 0, 1, 2, ... in the
// order of the descriptor, which is also the order TinyUSB's HID driver numbers its instances in,
// so an interface number is its instance while every interface is HID.
typedef struct {
    uint8_t count;                       // interfaces
    uint8_t iface_inst[MIMHID_HID_ITFS]; // interface number to HID instance
    uint8_t ep_in_addr[MIMHID_HID_ITFS]; // the IN endpoint of each interface
    uint8_t ep_inst[16];                 // IN endpoint number to HID instance, 0xFF when it is none
    uint16_t report_len[MIMHID_HID_ITFS]; // wReportLength of each interface's HID descriptor
} mimhid_layout_t;

// Writes the chip's unique id as a string descriptor (the hexadecimal digits of up to 15 bytes of
// it, USB 2.0 9.6.7) to dst, which holds MIMHID_STRING_DESC_MAX bytes, and returns its length; 0
// when the chip has no id.
uint16_t mimhid_unique_id_string(uint8_t* dst);

// Walks the staged configuration and checks that it holds together (see MIMHID_T_COMMIT in
// mimhid_proto.h and docs/PROTOCOL.md). Returns false when it does not; *layout is then
// unspecified.
bool mimhid_walk_config(const mimhid_config_t* cfg, mimhid_layout_t* layout);

// Makes a staged configuration the one TinyUSB serves, after checking it with mimhid_walk_config.
// Returns false (leaving the previous descriptors) when it does not hold together.
bool mimhid_apply_config(const mimhid_config_t* cfg);

// Feature report data for GET_REPORT of the active configuration, or NULL.
const mimhid_feature_t* mimhid_find_feature(uint8_t instance, uint8_t report_id);

// Changes the data returned for a Feature report of the active configuration; a no-op when the
// interface is not part of it.
void mimhid_active_set_feature(uint8_t iface, uint8_t report_id, const uint8_t* data, uint16_t len);

// Forgets the active configuration (no interfaces, no endpoints).
void mimhid_clear_config(void);

// Interface (HID instance) count of the active configuration.
uint8_t mimhid_active_interfaces(void);

// The HID instance of an interface number of the active configuration, or 0xFF.
uint8_t mimhid_active_interface_instance(uint8_t iface);

// The HID instance whose IN endpoint has this number in the active configuration, or 0xFF.
uint8_t mimhid_active_endpoint_instance(uint8_t endpoint_number);

// The address of the IN endpoint of a HID instance of the active configuration, or 0.
uint8_t mimhid_active_instance_endpoint(uint8_t instance);

#endif // MIMHID_CONFIG_H
