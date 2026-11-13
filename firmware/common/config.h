// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
#ifndef MIMUSB_CONFIG_H
#define MIMUSB_CONFIG_H

#include "mimusb_proto.h"
#include <stdbool.h>
#include <stdint.h>

// The limits of the board's RAM: how many interfaces, how long a configuration, how many strings,
// control rules and answer bytes, how deep the queues. They are the port's, from the RAM its chip
// has, and each port states every one in its own `mimusb_limits.h` (on the include path of its
// build; see firmware/PORTING in docs/PORTING.md). None has a default here: a build whose port
// leaves one out stops, with the message that names it, and a port that sizes its RAM wrongly stops
// in the linker or in the RAM check of its build (tools/check_ram.py).
#if !__has_include("mimusb_limits.h")
#error mimusb_limits.h must be provided by the port (on its include path); see docs/PORTING.md
#endif
#include "mimusb_limits.h"
#ifndef MIMUSB_MAX_INTERFACES
#error MIMUSB_MAX_INTERFACES must be defined by the port in mimusb_limits.h (see docs/PORTING.md)
#endif
#ifndef MIMUSB_MAX_CONFIG_BYTES
#error MIMUSB_MAX_CONFIG_BYTES must be defined by the port in mimusb_limits.h (see docs/PORTING.md)
#endif
#ifndef MIMUSB_MAX_BOS_BYTES
#error MIMUSB_MAX_BOS_BYTES must be defined by the port in mimusb_limits.h (see docs/PORTING.md)
#endif
#ifndef MIMUSB_MAX_STRINGS
#error MIMUSB_MAX_STRINGS must be defined by the port in mimusb_limits.h (see docs/PORTING.md)
#endif
#ifndef MIMUSB_MAX_STRING_BYTES
#error MIMUSB_MAX_STRING_BYTES must be defined by the port in mimusb_limits.h (see docs/PORTING.md)
#endif
#ifndef MIMUSB_MAX_REPORT_BYTES
#error MIMUSB_MAX_REPORT_BYTES must be defined by the port in mimusb_limits.h (see docs/PORTING.md)
#endif
#ifndef MIMUSB_CONTROL_DATA_MAX
#error MIMUSB_CONTROL_DATA_MAX must be defined by the port in mimusb_limits.h (see docs/PORTING.md)
#endif
#ifndef MIMUSB_EP_POOL_SLOTS
#error MIMUSB_EP_POOL_SLOTS must be defined by the port in mimusb_limits.h (see docs/PORTING.md)
#endif
#ifndef MIMUSB_CONTROL_RULES
#error MIMUSB_CONTROL_RULES must be defined by the port in mimusb_limits.h (see docs/PORTING.md)
#endif
#ifndef MIMUSB_ANSWER_BYTES
#error MIMUSB_ANSWER_BYTES must be defined by the port in mimusb_limits.h (see docs/PORTING.md)
#endif
#ifndef MIMUSB_LAST_SENT_SLOTS
#error MIMUSB_LAST_SENT_SLOTS must be defined by the port in mimusb_limits.h (see docs/PORTING.md)
#endif

// The interfaces one configuration holds, of any class: bInterfaceNumber is 0, 1, 2, ... in order
// (USB 2.0 9.6.5), and TinyUSB keeps a driver per interface number (CFG_TUD_INTERFACE_MAX, 16 unless
// a port says otherwise). The endpoints they use are limited by the controller (core.h).

// The configuration descriptor the board holds, as the host composes it (USB 2.0 9.6.3). A
// configuration that does not fit is refused with NAK OVERFLOW. The size is that of the header, 9
// bytes, and 15 interfaces of 64 bytes each: an interface descriptor (9, USB 2.0 9.6.5), an
// interface association descriptor (8, USB-IF Interface Association Descriptor ECN), two endpoint
// descriptors (2 x 7, 9.6.6) and 33 bytes of class descriptors (a CDC-ACM function's header, call
// management, ACM and union descriptors are 19 of them, by the USB CDC and PSTN specifications; a
// HID descriptor is 9, HID 1.11 6.2.1). That
// is 969 bytes, rounded up to 1024. A port with the RAM for a configuration of larger functions
// states a larger value.
// The Binary Object Store descriptor the board holds (USB 3.2 9.6.2), as the host composes or gives
// it. The 5-byte header, a USB 2.0 Extension capability (7 bytes, USB 2.0 LPM ECN) and the Microsoft
// OS 2.0 platform capability (28 bytes, "Microsoft OS 2.0 Descriptors Specification") are 40 bytes;
// a Container ID capability (20 bytes, USB 3.2 9.6.2.3) and room for a few capabilities of the
// caller's make 256. One copy of it is staged and one is active, each in the configuration
// structure below.
_Static_assert(MIMUSB_MAX_BOS_BYTES >= 5 && MIMUSB_MAX_BOS_BYTES <= 0xFFFF,
               "a BOS descriptor has a 5-byte header, and its length fits the wire");
// The string descriptors the board holds, each with its index and language: manufacturer, product,
// serial and configuration, one for each interface that has a name, and the ones the caller added
// (mimusb_device_add_string). The store holds them all at the longest length USB allows (USB 2.0
// 9.6.7, 254 bytes); a string that does not fit is refused with NAK OVERFLOW.

// What the board takes, announced in the PONG. MIMUSB_MAX_ENDPOINT_BYTES is the port's own
// (core.h); the others may be set by a port before this header.
#include "core.h"
_Static_assert(MIMUSB_MAX_INTERFACES >= 1 && MIMUSB_MAX_INTERFACES <= 15,
               "MIMUSB_MAX_INTERFACES is 1 to 15");
// The data stage of a control transfer the host may read or answer, and of one an ACCEPT rule takes:
// a Feature report fits, and so does the data of a RULE_DATA frame.
// A frame carries a transfer, the data stage of a control transfer or a configuration command, and
// a few bytes more.
#define MIMUSB_MAX_FRAME_PAYLOAD_RAW                                                               \
    ((MIMUSB_MAX_REPORT_BYTES > MIMUSB_CONTROL_DATA_MAX ? MIMUSB_MAX_REPORT_BYTES                   \
                                                        : MIMUSB_CONTROL_DATA_MAX) +                \
     8)
#define MIMUSB_MAX_FRAME_PAYLOAD                                                                   \
    (MIMUSB_MAX_FRAME_PAYLOAD_RAW > MIMUSB_MIN_FRAME_PAYLOAD ? MIMUSB_MAX_FRAME_PAYLOAD_RAW        \
                                                             : MIMUSB_MIN_FRAME_PAYLOAD)
_Static_assert(MIMUSB_MAX_FRAME_PAYLOAD <= MIMUSB_PROTO_MAX_PAYLOAD, "frame limit of the protocol");
// DESC_DATA: its sequence number, the six bytes that name the descriptor and the chunk.
_Static_assert(1 + 6 + MIMUSB_DESC_DATA_CHUNK <= MIMUSB_MAX_FRAME_PAYLOAD,
               "a DESC_DATA chunk fits a frame");
_Static_assert(MIMUSB_MAX_ENDPOINT_BYTES <= 1024, "an endpoint packet is at most 1024 bytes");

// One endpoint data frame carries one transfer of the generic driver, so a transfer queued on the
// board is at most one endpoint packet (the slot of the queue pool below); the host sends a longer
// one as several frames, each a multiple of the endpoint's packet size except the last (USB 2.0
// 5.8.3: a transfer ends with a short packet).
//
// A port with isochronous endpoints (MIMUSB_ISO_BYTES, core.h) holds a whole isochronous packet in
// one slot, so a transfer is at most the larger of the two; a bulk or interrupt transfer may then
// be longer than its endpoint packet, and the controller sends it as several (USB 2.0 5.8.3).
#define MIMUSB_MAX_TRANSFER_BYTES                                                                  \
    (MIMUSB_ISO_BYTES > MIMUSB_MAX_ENDPOINT_BYTES ? MIMUSB_ISO_BYTES : MIMUSB_MAX_ENDPOINT_BYTES)
_Static_assert(MIMUSB_MAX_TRANSFER_BYTES <= MIMUSB_MAX_FRAME_PAYLOAD,
               "an endpoint data frame fits the frame limit");
_Static_assert(MIMUSB_MAX_REPORT_BYTES <= MIMUSB_MAX_TRANSFER_BYTES,
               "a report is one endpoint data frame");
// The queues of the generic driver's IN endpoints are cut from one pool of slots, each holding a
// transfer of up to MIMUSB_MAX_TRANSFER_BYTES (a port constant; the PONG reports it). 64 slots of 64
// bytes is 4 KiB at full speed, 32 slots of 1024 bytes is 32 KiB at high speed: a few endpoints with
// a depth of eight, or one with a long queue.
_Static_assert(MIMUSB_EP_POOL_SLOTS >= 1 && MIMUSB_EP_POOL_SLOTS <= 0xFFFF, "slots fit the PONG");

// Control rules (control.c). The table the host stages holds the rules of every Node and the
// device's own; two copies of it are kept (the active one and the one being staged), each with its
// own answer memory.
//
// MIMUSB_CONTROL_RULES is sized for fifteen HID interfaces with Report IDs and some to spare. The
// profile layer writes, per interface, these rules: the report descriptor, the HID descriptor,
// SET_REPORT, SET_IDLE, SET_PROTOCOL, GET_IDLE and GET_PROTOCOL (7), one GET_REPORT for each
// Input report (a Spec with Report IDs has one for each ID: say 2) and one answer for each Feature
// report (say 1): 10 per interface, 150 for fifteen, which leaves 10 for the rules of the caller.
// A rule is under 32 bytes, so the two tables are under 10 KB.
_Static_assert(MIMUSB_CONTROL_RULES >= 1 && MIMUSB_CONTROL_RULES <= 254,
               "a rule index is a byte, and 0xFF means none");
// MIMUSB_ANSWER_BYTES is the answer memory of one table: the bytes of every ANSWER rule together.
// Fifteen HID interfaces take at most 2048 bytes of report descriptors together (what 0.2 held), 135
// of HID descriptors (9 each, HID 1.11 6.2.1), 30 for GET_IDLE and GET_PROTOCOL, and the Feature
// answers; the rest of 4096 is for those and the caller's rules.
_Static_assert(MIMUSB_ANSWER_BYTES >= 1 && MIMUSB_ANSWER_BYTES <= 0xFFFF, "a length fits the wire");
// ANSWER_LAST_SENT rules each keep the last transfer on their endpoint in a buffer of
// MIMUSB_MAX_TRANSFER_BYTES, apart from the tables (they are written as transfers are sent): one for
// each Input report of fifteen interfaces (2 each, as above) and two to spare.
_Static_assert(MIMUSB_LAST_SENT_SLOTS >= 1 && MIMUSB_LAST_SENT_SLOTS <= 254, "a slot is a byte");
_Static_assert(MIMUSB_CONTROL_DATA_MAX + 3 <= MIMUSB_MAX_FRAME_PAYLOAD, "a reply fits a frame");

// A descriptor the host announced with DESC_BEGIN and is still sending (or has sent) with
// DESC_DATA.
typedef struct {
    bool used;
    uint16_t total; // length announced
    uint16_t recv;  // bytes received so far (no gaps)
} mimusb_desc_t;

typedef struct {
    mimusb_desc_t d;
    uint8_t index;
    uint16_t language;
    bool from_id; // made by the board from the chip id (MIMUSB_DESC_F_SERIAL_FROM_ID)
    uint16_t off; // into the string pool
} mimusb_string_t;

typedef struct {
    uint8_t depth;  // transfers that may wait, 0: not set
    uint8_t policy; // MIMUSB_EP_FULL_*
} mimusb_ep_cfg_t;

// What the host staged with DESC_* and EP_CONFIG, and what a COMMIT makes active. (The staged
// control rules are control.c's.)
typedef struct {
    mimusb_desc_t device;
    _Alignas(4) uint8_t device_bytes[18];
    mimusb_desc_t config;
    _Alignas(4) uint8_t config_bytes[MIMUSB_MAX_CONFIG_BYTES];
    mimusb_desc_t bos; // not used: no BOS descriptor
    _Alignas(4) uint8_t bos_bytes[MIMUSB_MAX_BOS_BYTES];
    mimusb_string_t string[MIMUSB_MAX_STRINGS];
    uint16_t string_used; // bytes of the pool
    uint8_t string_pool[MIMUSB_MAX_STRING_BYTES];
    mimusb_ep_cfg_t ep[16]; // by IN endpoint number
} mimusb_config_t;

// Who serves an endpoint of the active configuration: the board's one driver for every interface
// (generic.c), whatever the class.
enum {
    MIMUSB_EP_NONE = 0,
    MIMUSB_EP_GENERIC
};

typedef struct {
    uint8_t kind;         // MIMUSB_EP_*
    uint8_t type;         // TUSB_XFER_BULK, TUSB_XFER_INTERRUPT or TUSB_XFER_ISOCHRONOUS
    uint16_t packet_size; // wMaxPacketSize; of an isochronous endpoint in several alternate
                          // settings, the largest of them
    uint8_t interface;    // bInterfaceNumber it belongs to
    uint8_t attributes;   // bmAttributes
    uint8_t interval;     // bInterval
} mimusb_ep_info_t;

// What a walk of a staged configuration finds.
typedef struct {
    uint8_t count; // interfaces
    uint8_t itf_group[MIMUSB_MAX_INTERFACES]; // number of the first interface of its association
                                              // group, its own number when it has none
    uint16_t itf_start[MIMUSB_MAX_INTERFACES]; // offset of its descriptor in the configuration
    uint16_t itf_span[MIMUSB_MAX_INTERFACES];  // bytes from there to the next interface or
                                               // association descriptor of another interface (a
                                               // group's first interface: to the end of the group);
                                               // its alternate settings are inside it
    uint8_t itf_alts[MIMUSB_MAX_INTERFACES];   // alternate settings it has (1: only setting 0)
    mimusb_ep_info_t ep[2][16];                // [0] OUT, [1] IN, by endpoint number
} mimusb_layout_t;

// Writes the chip's unique id as a string descriptor (the hexadecimal digits of up to 15 bytes of
// it, USB 2.0 9.6.7) to dst, which holds MIMUSB_STRING_DESC_MAX bytes, and returns its length; 0
// when the chip has no id.
uint16_t mimusb_unique_id_string(uint8_t* dst);

// Walks the staged configuration and checks that it holds together (see MIMUSB_T_COMMIT in
// mimusb_proto.h and docs/PROTOCOL.md). Returns false when it does not; *layout is then
// unspecified.
bool mimusb_walk_config(const mimusb_config_t* cfg, mimusb_layout_t* layout);

// Makes a staged configuration the one TinyUSB serves, after checking it with mimusb_walk_config.
// Returns false (leaving the previous descriptors) when it does not hold together.
bool mimusb_apply_config(const mimusb_config_t* cfg);

// Forgets the active configuration (no interfaces, no endpoints).
void mimusb_clear_config(void);

// What the walk of the active configuration found.
const mimusb_layout_t* mimusb_active_layout(void);

// Interfaces of the active configuration.
uint8_t mimusb_active_interfaces(void);

// The active configuration's description of an endpoint (direction from bit 7 of the address), or
// NULL when the address is not one of an endpoint of the active configuration.
const mimusb_ep_info_t* mimusb_active_endpoint(uint8_t address);

// How the active configuration spans an interface, for the generic driver's open: the bytes the
// interface (or, for the first interface of an association group, the whole group) covers, from its
// interface descriptor. 0 when the interface is not part of the configuration.
uint16_t mimusb_active_generic_span(uint8_t iface);

// The active configuration's interface and endpoint descriptors, for the generic driver.
const uint8_t* mimusb_active_config_bytes(void);

#endif // MIMUSB_CONFIG_H
