// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#ifndef MIMHID_PROTO_H
#define MIMHID_PROTO_H

/*
 * Serial wire protocol v5, shared by the host library and the firmware.
 *
 *   frame  = SYNC TYPE LEN payload[LEN] XOR
 *   LEN    = one byte when the payload is shorter than 128 bytes, else two: (0x80 | LEN & 0x7F)
 *            then LEN >> 7. A payload of up to 16383 bytes can be announced; the protocol
 *            allows MIMHID_PROTO_MAX_PAYLOAD, and a board says how much it takes in its PONG.
 *   XOR    = TYPE ^ every LEN byte ^ every payload byte
 *
 * A short frame is the same bytes as in protocol 3, so a board on protocol 3 still answers a
 * PING, and the host can say which protocol it speaks.
 *
 * Command frames (0x01..0x3F) start their payload with a one-byte sequence number that the
 * board echoes in its ACK or NAK. Report frames (0x40..0x4F) carry the report exactly as the
 * target receives it, Report ID included, and are not acknowledged.
 */

#define MIMHID_PROTO_VERSION 5
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
#define MIMHID_T_SET_BAUD      0x17 /* payload after sequence number: baud index (see below) */
#define MIMHID_T_CONTROL_CONFIG 0x18 /* which control transfers of the target are reported or held */
#define MIMHID_T_CONTROL_REPLY  0x19 /* the answer to a held control transfer */
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
#define MIMHID_EV_CONTROL_SETUP 9 /* data: token, flags, the 8 setup bytes */
#define MIMHID_EV_CONTROL_DATA  10 /* data: token, the data stage of a host-to-device transfer */

/* NAK codes. */
#define MIMHID_NAK_PARAM      1
#define MIMHID_NAK_OVERFLOW   2
#define MIMHID_NAK_STATE      3
#define MIMHID_NAK_DESCRIPTOR 4
#define MIMHID_NAK_UNSUPPORTED 5
#define MIMHID_NAK_BUSY       6

/* Limits that are part of the wire format. */
#define MIMHID_MAX_NODES        8
/* The largest payload a frame may carry on any board. A board's own limit is in its PONG, and
 * is never below MIMHID_MIN_FRAME_PAYLOAD, so that every configuration command fits. */
#define MIMHID_PROTO_MAX_PAYLOAD 4096
#define MIMHID_MIN_FRAME_PAYLOAD 255
#define MIMHID_NODE_DATA_CHUNK  240
#define MIMHID_STRING_MAX       31
#define MIMHID_CONTROL_RULES    8 /* rules a board holds at once */
#define MIMHID_CONTROL_RULE_LEN 15

/*
 * Control transfers. Every request the target sends on endpoint 0 (the setup packet of a
 * control transfer) can be reported, and the answer to chosen ones can come from the host.
 *
 * CONTROL_CONFIG payload after the sequence number:
 *   0 flags (bit 0: report every control transfer)  1 rule count n (0..MIMHID_CONTROL_RULES)
 *   2.. n rules of MIMHID_CONTROL_RULE_LEN bytes:
 *     0 bmRequestType mask  1 bmRequestType value  2 bRequest mask  3 bRequest value
 *     4..5 wValue mask  6..7 wValue value  8..9 wIndex mask  10..11 wIndex value
 *     12..13 timeout in ms (1..MIMHID_CONTROL_TIMEOUT_MAX)  14 action after the timeout
 *   A request matches a rule when each field ANDed with its mask equals the value; the first
 *   matching rule holds the transfer. The set replaces the previous one; zero rules and no flag
 *   leave every request to the board's own handling, which is how a board starts.
 *
 * EVENT CONTROL_SETUP payload: kind, token, flags, the 8 setup bytes as they came (little
 *   endian wValue, wIndex, wLength). flags: bit 0 held (a CONTROL_REPLY with this token is
 *   awaited), bit 1 too long (a host-to-device data stage longer than the board takes, not held),
 *   bit 2 fixed (SET_ADDRESS and SET_CONFIGURATION change the board's own USB state, so they are
 *   reported and never held), bit 3 lost (reports before this one were dropped: the board's queue
 *   was full).
 * EVENT CONTROL_DATA payload: kind, token, the bytes of the data stage. Sent after the board
 *   accepted a held host-to-device transfer with a data stage; the status stage waits for the
 *   second CONTROL_REPLY.
 * CONTROL_REPLY payload after the sequence number: 0 token  1 action  2.. the data stage of a
 *   device-to-host transfer (action ANSWER only; cut to wLength). Acknowledged when the board
 *   still waits for that token; a NAK with MIMHID_NAK_STATE says it no longer does.
 *
 * A held transfer is answered by the host within its rule's timeout; after it the board does the
 * rule's action. USB 2.0 (9.2.6.4) asks a device to return the first data packet of a standard
 * request within 500 ms, and a control transfer is allowed 5 s in all (9.2.6.1); hosts may give
 * up earlier, and a request without a data stage may be held to a shorter time still.
 */
#define MIMHID_CONTROL_LEAVE  0 /* the board's own handling, as if the host had not been asked */
#define MIMHID_CONTROL_ANSWER 1 /* the data (device to host) or an empty status (otherwise) */
#define MIMHID_CONTROL_STALL  2
#define MIMHID_CONTROL_TIMEOUT_MAX 4000

#define MIMHID_CONTROL_F_HELD     0x01
#define MIMHID_CONTROL_F_TOO_LONG 0x02
#define MIMHID_CONTROL_F_FIXED    0x04
#define MIMHID_CONTROL_F_LOST     0x08

/*
 * Line rates. A board starts at the boot rate, index 0, and the host asks for another with
 * SET_BAUD. The board ACKs at the rate it is on, switches once the ACK has left, and keeps the
 * new rate only when a valid frame arrives at it within MIMHID_BAUD_CONFIRM_MS; otherwise it
 * goes back to the boot rate. A switch to the boot rate needs no confirmation.
 */
#define MIMHID_BAUD_RATES      {115200u, 230400u, 460800u, 921600u, 1000000u, 1500000u, 2000000u, 3000000u, 4000000u}
#define MIMHID_BAUD_COUNT      9
#define MIMHID_BOOT_BAUD       115200u
#define MIMHID_BAUD_CONFIRM_MS 100

/* Chips, as the PONG names them. */
#define MIMHID_CHIP_UNKNOWN  0
#define MIMHID_CHIP_RP2040   1
#define MIMHID_CHIP_RP2350   2
#define MIMHID_CHIP_ESP32S3  3
#define MIMHID_CHIP_NRF52840 4
#define MIMHID_CHIP_STM32F4  5

/*
 * PONG payload (all single bytes unless noted; every two-byte value is little endian):
 *   0 fw_major  1 fw_minor  2 fw_patch  3 ready  4 proto_version
 *   5 max_nodes  6 max_interfaces  7 flags (bit 0: has chip unique id)
 *   8 chip  9 usb speed (0 full, 1 high)  10 baud index the board is on
 *   11..12 baud mask (bit i: the board can run at rate i)
 *   13..14 max_descriptor_bytes  15..16 max_report_bytes  17..18 max_feature_bytes
 *   19..20 max_endpoint_bytes (the largest endpoint packet)  21..22 max_frame_payload
 *   23..24 max_control_data_bytes (the data stage of a control transfer the host may read or
 *   answer)  25 max_control_rules
 *   26 unique id length n (0..16)  27.. the chip's unique id, n bytes
 * Bytes 0..4 are the same in every protocol version, and a PONG shorter than 128 bytes is framed
 * the same, so a host reads the version of a board on another protocol. A reader ignores bytes
 * past the ones it knows.
 */
#define MIMHID_PONG_MIN_LEN   27
#define MIMHID_UNIQUE_ID_MAX  16
#define MIMHID_PONG_FLAG_UNIQUE_ID 0x01

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
 *   5..6 IN endpoint size  7..8 bcdHID  9 country code  10 boot protocol (0 none, 1 keyboard, 2 mouse)
 *   11 idle rate  12 flags (reserved, zero)  13..14 OUT endpoint size (0: none)
 *   15 interface string length, then its bytes
 * NODE_DATA payload after the sequence number:
 *   0 node id  1..2 offset  3.. descriptor bytes
 * FEATURE payload after the sequence number:
 *   0 node id  1 report id  2.. bytes (without the report id), at most max_feature_bytes
 * EVENT payload: kind, then
 *   OUTPUT_REPORT  interface, length (two bytes), the report
 *   SET_REPORT     interface, report type, report id, the bytes the target sent (cut to what
 *                  the board holds)
 */

#endif /* MIMHID_PROTO_H */
