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
// The interfaces of any class one configuration holds: bInterfaceNumber is 0, 1, 2, ... in order
// (USB 2.0 9.6.5), and TinyUSB keeps a driver per interface number (CFG_TUD_INTERFACE_MAX, 16 unless
// a port says otherwise). HID interfaces among them are limited by the controller's endpoints
// (MIMHID_HID_ITFS in core.h).
#ifndef MIMHID_MAX_INTERFACES
#define MIMHID_MAX_INTERFACES 15
#endif

// The configuration descriptor the board holds, as the host composes it (USB 2.0 9.6.3). A
// configuration that does not fit is refused with NAK OVERFLOW. The size is that of the header, 9
// bytes, and 15 interfaces of 64 bytes each: an interface descriptor (9, USB 2.0 9.6.5), an
// interface association descriptor (8, USB-IF Interface Association Descriptor ECN), two endpoint
// descriptors (2 x 7, 9.6.6) and 33 bytes of class descriptors (a CDC-ACM function's header, call
// management, ACM and union descriptors are 19 of them, by the USB CDC and PSTN specifications; a
// HID descriptor is 9, HID 1.11 6.2.1). That
// is 969 bytes, rounded up to 1024. A port with the RAM for a configuration of larger functions
// states a larger value.
#ifndef MIMHID_MAX_CONFIG_BYTES
#define MIMHID_MAX_CONFIG_BYTES 1024
#endif
// The string descriptors the board holds, each with its index and language: manufacturer, product,
// serial and configuration, one for each interface that has a name, and the ones the caller added
// (mimhid_device_add_string). The store holds them all at the longest length USB allows (USB 2.0
// 9.6.7, 254 bytes); a string that does not fit is refused with NAK OVERFLOW.
#ifndef MIMHID_MAX_STRINGS
#define MIMHID_MAX_STRINGS 24
#endif
#ifndef MIMHID_MAX_STRING_BYTES
#define MIMHID_MAX_STRING_BYTES (MIMHID_MAX_STRINGS * MIMHID_STRING_DESC_MAX)
#endif

// What the board takes, announced in the PONG. MIMHID_MAX_ENDPOINT_BYTES is the port's own
// (core.h); the others may be set by a port before this header. A Feature report travels on the
// control pipe, so it may be longer than an endpoint packet.
#include "core.h"
_Static_assert(MIMHID_MAX_INTERFACES >= MIMHID_HID_ITFS && MIMHID_MAX_INTERFACES <= 15,
               "MIMHID_MAX_INTERFACES is MIMHID_HID_ITFS to 15");
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

// One endpoint data frame carries one transfer of the generic driver, so a transfer queued on the
// board is at most one endpoint packet (the slot of the queue pool below); the host sends a longer
// one as several frames, each a multiple of the endpoint's packet size except the last (USB 2.0
// 5.8.3: a transfer ends with a short packet).
#define MIMHID_MAX_TRANSFER_BYTES MIMHID_MAX_ENDPOINT_BYTES
_Static_assert(MIMHID_MAX_REPORT_BYTES <= MIMHID_MAX_TRANSFER_BYTES,
               "a report is one endpoint data frame");
// The queues of the generic driver's IN endpoints are cut from one pool of slots, each holding a
// transfer of up to MIMHID_MAX_TRANSFER_BYTES (a port constant; the PONG reports it). 64 slots of 64
// bytes is 4 KiB at full speed, 32 slots of 1024 bytes is 32 KiB at high speed: a few endpoints with
// a depth of eight, or one with a long queue. HID interfaces keep their own queues (core.c).
#ifndef MIMHID_EP_POOL_SLOTS
#if MIMHID_MAX_ENDPOINT_BYTES <= 64
#define MIMHID_EP_POOL_SLOTS 64
#else
#define MIMHID_EP_POOL_SLOTS 32
#endif
#endif
_Static_assert(MIMHID_EP_POOL_SLOTS >= 1 && MIMHID_EP_POOL_SLOTS <= 0xFFFF, "slots fit the PONG");

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
    mimhid_report_desc_t report[MIMHID_MAX_INTERFACES]; // by interface number, HID interfaces only
    uint16_t report_used;
    uint8_t report_pool[MIMHID_MAX_DESC_BYTES];
    mimhid_feature_t feature[MIMHID_MAX_FEATURES];
    mimhid_ep_cfg_t ep[16]; // by IN endpoint number
} mimhid_config_t;

// Who serves an endpoint of the active configuration.
enum {
    MIMHID_EP_NONE = 0,
    MIMHID_EP_HID,    // TinyUSB's HID driver
    MIMHID_EP_GENERIC // generic.c
};

typedef struct {
    uint8_t kind;         // MIMHID_EP_*
    uint8_t type;         // TUSB_XFER_BULK or TUSB_XFER_INTERRUPT
    uint16_t packet_size; // wMaxPacketSize
    uint8_t interface;    // bInterfaceNumber it belongs to
} mimhid_ep_info_t;

// What a walk of a staged configuration finds. HID interfaces are numbered 0, 1, 2, ... in the
// order of the descriptor among the HID interfaces, which is the order TinyUSB's HID driver numbers
// its instances in (hidd_open takes the first free one), so a HID instance is the position of its
// interface among the HID interfaces.
typedef struct {
    uint8_t count;     // interfaces of any class
    uint8_t hid_count; // HID interfaces
    uint8_t itf_inst[MIMHID_MAX_INTERFACES];  // HID instance of an interface, 0xFF when it is not HID
    uint8_t itf_group[MIMHID_MAX_INTERFACES]; // number of the first interface of its association
                                              // group, its own number when it has none
    uint16_t itf_start[MIMHID_MAX_INTERFACES]; // offset of its descriptor in the configuration
    uint16_t itf_span[MIMHID_MAX_INTERFACES];  // bytes from there to the next interface or
                                               // association descriptor (a group's first
                                               // interface: to the end of the group)
    uint8_t inst_itf[MIMHID_HID_ITFS];         // the interface of a HID instance
    uint8_t inst_ep_in[MIMHID_HID_ITFS];       // the IN endpoint of a HID instance
    uint16_t report_len[MIMHID_MAX_INTERFACES]; // wReportLength of a HID interface
    uint8_t ep_inst[16];                       // IN endpoint number to HID instance, 0xFF when none
    mimhid_ep_info_t ep[2][16];                // [0] OUT, [1] IN, by endpoint number
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

// Interfaces of the active configuration, of every class.
uint8_t mimhid_active_interfaces(void);

// HID instances of the active configuration.
uint8_t mimhid_active_hid_instances(void);

// The HID instance of an interface number of the active configuration, or 0xFF when the interface
// does not exist or is not HID.
uint8_t mimhid_active_interface_instance(uint8_t iface);

// The interface number of a HID instance of the active configuration, or 0xFF.
uint8_t mimhid_active_instance_interface(uint8_t instance);

// The HID instance whose IN endpoint has this number in the active configuration, or 0xFF.
uint8_t mimhid_active_endpoint_instance(uint8_t endpoint_number);

// The address of the IN endpoint of a HID instance of the active configuration, or 0.
uint8_t mimhid_active_instance_endpoint(uint8_t instance);

// The active configuration's description of an endpoint (direction from bit 7 of the address), or
// NULL when the address is not one of an endpoint of the active configuration.
const mimhid_ep_info_t* mimhid_active_endpoint(uint8_t address);

// How the active configuration spans an interface, for the generic driver's open: the bytes the
// interface (or, for the first interface of an association group, the whole group) covers, from its
// interface descriptor. 0 when the interface is HID or not part of the configuration.
uint16_t mimhid_active_generic_span(uint8_t iface);

// The active configuration's interface and endpoint descriptors, for the generic driver.
const uint8_t* mimhid_active_config_bytes(void);

#endif // MIMHID_CONFIG_H
