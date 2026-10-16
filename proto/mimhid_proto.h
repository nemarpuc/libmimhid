// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#ifndef MIMHID_PROTO_H
#define MIMHID_PROTO_H

/*
 * Serial wire protocol v2, shared by the host library and the firmware.
 *
 *   frame  = SYNC TYPE LEN payload[LEN] XOR
 *   XOR    = TYPE ^ LEN ^ every payload byte
 *
 * Command frames (0x01..0x3F) start their payload with a one-byte sequence number that the
 * board echoes in its ACK or NAK. Report frames (0x40..0x4F) carry the report exactly as the
 * target receives it, Report ID included, and are not acknowledged.
 */

#define MIMHID_PROTO_VERSION 2
#define MIMHID_SYNC          0xA5

/* Host to board. */
#define MIMHID_T_PING          0x01 /* payload: host protocol version (no sequence number) */
#define MIMHID_T_DEVICE_CONFIG 0x10 /* starts a new staged configuration, clearing all nodes */
#define MIMHID_T_NODE_BEGIN    0x11
#define MIMHID_T_NODE_DATA     0x12
#define MIMHID_T_COMMIT        0x13 /* apply: disconnect, rebuild descriptors, reconnect, then ACK;
                                        payload after sequence number: [detach: 1 byte] [delay_ms: 2 bytes little-endian] */
#define MIMHID_T_FEATURE       0x14 /* data returned for GET_REPORT of a feature report */
#define MIMHID_T_RESET         0x15 /* drop every queued report */
#define MIMHID_T_REMOTE_WAKEUP 0x16
#define MIMHID_T_REPORT_BASE   0x40 /* 0x40 | node id, node id 0..15 */
#define MIMHID_T_REPORT_LAST   0x4F

/* Board to host. */
#define MIMHID_T_PONG  0x81
#define MIMHID_T_ACK   0x82 /* payload: seq, type */
#define MIMHID_T_NAK   0x83 /* payload: seq, type, code */
#define MIMHID_T_EVENT 0x84 /* payload: kind, ... */

/* Events. */
#define MIMHID_EV_MOUNTED       1
#define MIMHID_EV_UNMOUNTED     2
#define MIMHID_EV_SUSPEND       3 /* data: remote wakeup enabled by the target (0/1) */
#define MIMHID_EV_RESUME        4
#define MIMHID_EV_SET_REPORT    5 /* data: interface, report type, report id, bytes... */
#define MIMHID_EV_SET_PROTOCOL  6 /* data: interface, protocol (0 boot, 1 report) */
#define MIMHID_EV_SET_IDLE      7 /* data: interface, idle rate */
#define MIMHID_EV_OUTPUT_REPORT 8 /* data: interface, length, bytes... */

/* NAK codes. */
#define MIMHID_NAK_PARAM      1
#define MIMHID_NAK_OVERFLOW   2
#define MIMHID_NAK_STATE      3
#define MIMHID_NAK_DESCRIPTOR 4
#define MIMHID_NAK_UNSUPPORTED 5
#define MIMHID_NAK_BUSY       6

/* Limits that are part of the wire format. */
#define MIMHID_MAX_NODES        8
#define MIMHID_MAX_FRAME_PAYLOAD 255
#define MIMHID_NODE_DATA_CHUNK  240
#define MIMHID_STRING_MAX       31
#define MIMHID_FEATURE_MAX      32

/*
 * PONG payload (all single bytes unless noted):
 *   0 fw_major  1 fw_minor  2 fw_patch  3 ready  4 proto_version
 *   5 max_nodes  6 max_interfaces  7 max_report_bytes
 *   8..9 max_descriptor_bytes (little endian)  10 flags (bit 0: has chip unique id)
 */
#define MIMHID_PONG_LEN 11

/*
 * DEVICE_CONFIG payload after the sequence number:
 *   0..1 vendor_id  2..3 product_id  4..5 bcd_device  6 device_class  7 device_subclass
 *   8 device_protocol  9..10 language_id  11 flags  12 max_power (2 mA units)
 *   13.. four strings, each: length byte, then that many ASCII bytes
 *        (manufacturer, product, serial, configuration)
 * flags: bit 0 self powered, bit 1 remote wakeup, bit 2 serial from chip id
 */
#define MIMHID_DEVFLAG_SELF_POWERED   0x01
#define MIMHID_DEVFLAG_REMOTE_WAKEUP  0x02
#define MIMHID_DEVFLAG_SERIAL_FROM_ID 0x04

/*
 * NODE_BEGIN payload after the sequence number:
 *   0 node id  1 interface index  2..3 report descriptor length  4 poll interval (ms)
 *   5 endpoint size  6..7 bcdHID  8 country code  9 boot protocol (0 none, 1 keyboard, 2 mouse)
 *   10 idle rate  11 flags (reserved, zero)  12 ep_out_size  13 interface string length, then its bytes
 * NODE_DATA payload after the sequence number:
 *   0 node id  1..2 offset  3.. descriptor bytes
 * FEATURE payload after the sequence number:
 *   0 node id  1 report id  2.. bytes (without the report id)
 */

#endif /* MIMHID_PROTO_H */
