// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#ifndef MIMHID_PROTO_H
#define MIMHID_PROTO_H

#include <stdint.h>

/*
 * Serial wire protocol, shared by the host library and the firmware. The firmware and the library
 * of one release agree on it: a host talks to a board whose version has the same major and minor
 * number as its own (the patch number never changes the wire).
 *
 *   frame  = SYNC TYPE LEN payload[LEN] CRC_LO CRC_HI
 *   LEN    = one byte when the payload is shorter than 128 bytes, else two: (0x80 | LEN & 0x7F)
 *            then LEN >> 7. A payload of up to 16383 bytes can be announced; the protocol
 *            allows MIMHID_PROTO_MAX_PAYLOAD, and a board says how much it takes in its PONG.
 *   CRC    = CRC-16/CCITT-FALSE (polynomial 0x1021, initial value 0xFFFF, no reflection, no final
 *            xor) of TYPE, every LEN byte and every payload byte; low byte first.
 *
 * The first four bytes of a PONG (the firmware version and ready) keep their place in every
 * release, so a host that reads a board of another major or minor version can say which one it
 * is; it does not talk to it. Change the frame format or a layout, and the minor number goes up.
 *
 * Command frames (0x01..0x3F) start their payload with a one-byte sequence number that the
 * board echoes in its ACK or NAK. Endpoint data frames carry a transfer exactly as the target
 * receives or sends it (Report ID included for HID) and are not acknowledged: 0x41..0x4F to an IN
 * endpoint (host to board), 0xC1..0xCF from an OUT endpoint (board to host).
 */

#define MIMHID_SYNC 0xA5

/* The frame check, shared by the host and the firmware. A 16-entry table over nibbles keeps it
 * small (32 bytes) and costs two lookups per byte. */
#define MIMHID_CRC16_INIT 0xFFFFu
static inline uint16_t mimhid_crc16_update(uint16_t crc, uint8_t byte) {
    static const uint16_t table[16] = {0x0000u, 0x1021u, 0x2042u, 0x3063u, 0x4084u, 0x50A5u,
                                       0x60C6u, 0x70E7u, 0x8108u, 0x9129u, 0xA14Au, 0xB16Bu,
                                       0xC18Cu, 0xD1ADu, 0xE1CEu, 0xF1EFu};
    crc = (uint16_t)((uint16_t)(crc << 4) ^ table[(crc >> 12) ^ (byte >> 4)]);
    crc = (uint16_t)((uint16_t)(crc << 4) ^ table[(crc >> 12) ^ (byte & 0x0Fu)]);
    return crc;
}

/* Host to board. */
#define MIMHID_T_PING 0x01 /* no payload, no sequence number */
#define MIMHID_T_DESC_RESET                                                                        \
    0x10 /* no payload after the sequence number: starts a new staged configuration, forgetting    \
            every staged descriptor, string, Feature answer and endpoint setting */
#define MIMHID_T_DESC_BEGIN 0x11 /* announces one descriptor (see below) */
#define MIMHID_T_DESC_DATA 0x12  /* a chunk of the announced descriptor (see below) */
#define MIMHID_T_COMMIT                                                                            \
    0x13                      /* apply: disconnect, rebuild descriptors, reconnect, then ACK;      \
                                  payload after sequence number: [detach: 1 byte] [delay_ms: 2 bytes little-endian] */
#define MIMHID_T_FEATURE 0x14 /* data returned for GET_REPORT of a feature report of an interface  \
                               */
#define MIMHID_T_RESET 0x15   /* drop every queued report */
#define MIMHID_T_REMOTE_WAKEUP 0x16
#define MIMHID_T_SET_BAUD 0x17 /* payload after sequence number: the rate, u32 little endian */
#define MIMHID_T_CONTROL_CONFIG                                                                    \
    0x18                            /* which control transfers of the target are reported or held */
#define MIMHID_T_CONTROL_REPLY 0x19 /* the answer to a held control transfer */
#define MIMHID_T_LOSS_CONFIG                                                                       \
    0x1A /* payload after sequence number: enabled (0/1), minimum interval in ms (u16 LE) */
#define MIMHID_T_EP_CONFIG                                                                         \
    0x1B /* payload after sequence number: endpoint address, transfers that may wait (1..), policy \
          */
#define MIMHID_T_PROBE                                                                             \
    0x1C /* link test: a frame of a pattern both ends know (see below); no sequence number,        \
            not acknowledged */
#define MIMHID_T_COUNTERS                                                                          \
    0x1D /* payload after sequence number: flags (bit 0: reset the counters after reading);        \
            the ACK carries the counters (see below) */
#define MIMHID_T_EP_STALL                                                                          \
    0x1E /* payload after sequence number: endpoint address, 1 stall or 0 clear the halt */
#define MIMHID_T_EP_FLUSH                                                                          \
    0x1F /* payload after sequence number: endpoint address; drops what waits on the board */
#define MIMHID_T_EP_IN_BASE                                                                        \
    0x40 /* 0x40 | endpoint number: data for that IN endpoint; 0x40 itself                         \
            (endpoint 0) is refused */
#define MIMHID_T_EP_IN_LAST 0x4F

/* Board to host. */
#define MIMHID_T_PONG 0x81
#define MIMHID_T_ACK 0x82   /* payload: seq, type, then the data of the command, if it has any */
#define MIMHID_T_NAK 0x83   /* payload: seq, type, code */
#define MIMHID_T_EVENT 0x84 /* payload: kind, ... */
#define MIMHID_T_EP_OUT_BASE                                                                       \
    0xC0 /* 0xC0 | endpoint number: a packet the target sent on that OUT endpoint, as it came;      \
            0xC0 itself (endpoint 0) is never sent. Not acknowledged. */
#define MIMHID_T_EP_OUT_LAST 0xCF

/* Events. */
#define MIMHID_EV_MOUNTED 1
#define MIMHID_EV_UNMOUNTED 2
#define MIMHID_EV_SUSPEND 3 /* data: remote wakeup enabled by the target (0/1) */
#define MIMHID_EV_RESUME 4
#define MIMHID_EV_SET_REPORT 5    /* data: interface, report type, report id, bytes... */
#define MIMHID_EV_SET_PROTOCOL 6  /* data: interface, protocol (0 boot, 1 report) */
#define MIMHID_EV_SET_IDLE 7      /* data: interface, idle rate */
#define MIMHID_EV_OUTPUT_REPORT 8 /* data: interface, length, bytes... */
#define MIMHID_EV_CONTROL_SETUP 9 /* data: token, flags, the 8 setup bytes */
#define MIMHID_EV_CONTROL_DATA 10 /* data: token, the data stage of a host-to-device transfer */
#define MIMHID_EV_TRANSFER_LOST                                                                    \
    11 /* data: endpoint address, count (u16 LE): transfers dropped, queue full; only after        \
          LOSS_CONFIG enabled it */
#define MIMHID_EV_EP_DONE                                                                          \
    12 /* data: endpoint address, transfers completed since the last such event (u16 LE); only for \
          endpoints with the wait policy */
#define MIMHID_EV_EP_HALT                                                                          \
    13 /* data: endpoint address, halted (1: the target set the halt, 0: it cleared it); only for  \
          endpoints of the generic driver */

/* NAK codes. */
#define MIMHID_NAK_PARAM 1
#define MIMHID_NAK_OVERFLOW 2
#define MIMHID_NAK_STATE 3
#define MIMHID_NAK_DESCRIPTOR 4
#define MIMHID_NAK_UNSUPPORTED 5
#define MIMHID_NAK_BUSY 6

/* Limits that are part of the wire format. An endpoint data frame names its IN endpoint in the low
 * four bits of its type (MIMHID_T_EP_IN_BASE | endpoint number), so endpoints 1 to 15 are
 * addressable. */
/* The largest payload a frame may carry on any board. A board's own limit is in its PONG, and
 * is never below MIMHID_MIN_FRAME_PAYLOAD (a DESC_DATA chunk fits); the firmware also refuses to
 * build with a limit below its longest configuration command (config.h). */
#define MIMHID_PROTO_MAX_PAYLOAD 4096
#define MIMHID_MIN_FRAME_PAYLOAD 255
#define MIMHID_DESC_DATA_CHUNK 240
/* The largest USB string descriptor payload: USB 2.0 Spec section 9.6.7 defines bLength as 1 byte
 * (<= 255). A string descriptor has a 2-byte header (bLength, bDescriptorType) followed by UNICODE
 * UTF-16LE characters (2 bytes each). Total length is 2 + 2 * N <= 254 (even), so N <= 126
 * characters. */
#define MIMHID_STRING_MAX 126
/* The longest string descriptor as it is on the bus: 2 + 2 * MIMHID_STRING_MAX bytes. */
#define MIMHID_STRING_DESC_MAX (2 + 2 * MIMHID_STRING_MAX)
#define MIMHID_CONTROL_RULES 8 /* rules a board holds at once */
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
#define MIMHID_CONTROL_LEAVE 0  /* the board's own handling, as if the host had not been asked */
#define MIMHID_CONTROL_ANSWER 1 /* the data (device to host) or an empty status (otherwise) */
#define MIMHID_CONTROL_STALL 2
#define MIMHID_CONTROL_DROP                                                                        \
    3 /* silent drop: do not stall, do not arm, keep NAKing until host aborts */
/* USB 2.0 section 9.2.6.1: "USB sets an upper limit of 5 seconds as the upper limit for any
 * command to be processed." A rule may hold a request for that long and no longer. */
#define MIMHID_CONTROL_TIMEOUT_MAX 5000

#define MIMHID_CONTROL_F_HELD 0x01
#define MIMHID_CONTROL_F_TOO_LONG 0x02
#define MIMHID_CONTROL_F_FIXED 0x04
#define MIMHID_CONTROL_F_LOST 0x08

/*
 * Line rate. A board starts at the boot rate, the one rate both ends know, and the host asks for
 * another with SET_BAUD (any rate up to the board's max_baud; there is no table). The board ACKs
 * at the rate it is on, with the rate it will actually run at (the divider rarely gives the exact
 * one), switches once the ACK has left, and keeps the new rate only when a valid frame arrives at
 * it within MIMHID_BAUD_CONFIRM_MS; otherwise it goes back to the boot rate. A switch to the
 * boot rate needs no confirmation.
 *
 * A UART BREAK (the line held low for longer than one character at the rate the board is on)
 * returns the board to the boot rate whatever rate it was on, and drops the bytes it holds. That
 * is how a host finds a board that a program left on a fast rate: no rate is searched.
 *
 * SET_BAUD payload after the sequence number: the rate (u32 little endian). NAK PARAM for 0, for
 * a rate above max_baud and for a rate whose divider is off by more than MIMHID_BAUD_TOLERANCE_PPM.
 * ACK data: the rate the board will run at (u32 little endian).
 */
#define MIMHID_BOOT_BAUD 115200u
#define MIMHID_BAUD_CONFIRM_MS 100
#define MIMHID_BAUD_TOLERANCE_PPM 20000u /* 2 % */

/*
 * Link counters. The board counts what went wrong on the line since the last reset, each in an
 * unsigned 16-bit value that stops at 65535:
 *   probes_good   PROBE frames whose pattern was right
 *   bad_crc       frames that had a sync byte, a known type and a possible length, but a wrong CRC
 *   framing       UART framing errors (not BREAKs)
 *   uart_overrun  bytes the UART lost because nobody read it in time
 *   ring_overrun  reads that found the receive ring three quarters full or more: a ring that
 *                 was lapped cannot be told from a quiet line afterwards, so this is the near
 *                 miss that comes first
 * A hardware counter that only latches ("an error happened since I last looked") is counted once
 * per look, so a count is a lower bound on the errors; zero is exact.
 * COUNTERS ACK data: the five values in this order, u16 little endian each.
 *
 * PROBE payload: n bytes (any length the board takes) where byte i is
 * (uint8_t)(i * 7 + 3). The host sends COUNTERS with the reset flag, then PROBE frames back to
 * back, then COUNTERS again: the link is as good as the counters say.
 */
#define MIMHID_COUNTER_COUNT 5
#define MIMHID_COUNTERS_F_RESET 0x01
#define MIMHID_PROBE_BYTE(i) ((uint8_t)((i) * 7u + 3u))

/* Chips, as the PONG names them. */
#define MIMHID_CHIP_UNKNOWN 0
#define MIMHID_CHIP_RP2040 1
#define MIMHID_CHIP_RP2350 2
#define MIMHID_CHIP_ESP32S3 3
#define MIMHID_CHIP_NRF52840 4
#define MIMHID_CHIP_STM32F4 5

/*
 * PONG payload (all single bytes unless noted; every wider value is little endian):
 *   0 fw_major  1 fw_minor  2 fw_patch  3 ready
 *   4 reserved, zero  5 max_hid_interfaces  6 flags (bit 0: has chip unique id)
 *   7 chip  8 usb speed (0 full, 1 high)
 *   9..12 baud the board is on (u32)  13..16 max_baud, the fastest rate its UART can make (u32)
 *   17..20 uart flags (u32): bit 0 break detection works, bits 1.. reserved, zero
 *   21..22 max_descriptor_bytes  23..24 max_report_bytes  25..26 max_feature_bytes
 *   27..28 max_endpoint_bytes (the largest endpoint packet)  29..30 max_frame_payload
 *   31..32 max_control_data_bytes (the data stage of a control transfer the host may read or
 *   answer)  33 max_control_rules
 *   34 max_endpoint_number (the highest endpoint number the controller has; an endpoint address
 *   above it is refused)
 *   35 unique id length n (0..16)
 *   36 max_interfaces_any (interfaces of any class the board takes, HID ones included; byte 5 is
 *   the number of HID interfaces)  37..38 max_config_descriptor_bytes  39 max_strings (string
 *   descriptors, the board's own included)  40..41 max_transfer_bytes (the payload of one endpoint
 *   data frame)  42..43 endpoint_queue_slots (the slots of the pool the IN queues of the generic
 *   driver are cut from)  44 transfer_types (bit n set: the board has endpoints of transfer type
 *   n; USB 2.0 9.6.6 bmAttributes bits 1..0: bit 2 bulk, bit 3 interrupt)
 *   45.. the chip's unique id, n bytes
 * Bytes 0..3 are the same in every release. A reader ignores bytes past the ones it knows.
 */
#define MIMHID_PONG_MIN_LEN 45
#define MIMHID_PONG_UART_BREAK 0x01u
#define MIMHID_UNIQUE_ID_MAX 16
#define MIMHID_PONG_FLAG_UNIQUE_ID 0x01

/*
 * The USB descriptors travel as the bytes the target reads. The host composes them; the board
 * checks them at COMMIT and serves them without looking inside beyond what its USB stack needs.
 *
 * DESC_BEGIN payload after the sequence number:
 *   0 kind  1 index  2..3 language id (u16 LE)  4..5 total length (u16 LE)  6 flags
 * DESC_DATA payload after the sequence number:
 *   0 kind  1 index  2..3 language id  4..5 offset (u16 LE)  6.. bytes, at most
 *   MIMHID_DESC_DATA_CHUNK of them
 *
 *   kind                      index           language  bytes
 *   MIMHID_DESC_DEVICE        0               0         the 18-byte device descriptor
 *   MIMHID_DESC_CONFIGURATION 0               0         the whole configuration descriptor,
 *                                                       wTotalLength bytes
 *   MIMHID_DESC_STRING        1..255          LANGID    the string descriptor as on the bus:
 *                                                       bLength, 0x03, UTF-16LE (USB 2.0 9.6.7)
 *   MIMHID_DESC_HID_REPORT    interface no.   0         the report descriptor of that HID interface
 *
 * String index 0 (the language list) is never sent: the board answers it with the language ids
 * of the strings it holds. With MIMHID_DESC_F_SERIAL_FROM_ID on a string, the total length is 0 and
 * the board makes the string from its chip id (the characters are the hexadecimal digits of the
 * id). All other flag bits are zero.
 *
 * MIMHID_DESC_HID_REPORT exists only while TinyUSB's HID driver serves the report descriptors
 * (tud_hid_descriptor_report_cb); it goes when HID moves to the board's own driver.
 *
 * A DESC_BEGIN repeated with the same kind, index, language, total length and flags is
 * acknowledged again, and so is a DESC_DATA repeated with the same offset and bytes, since the
 * host sends a frame again when it did not see the answer. A chunk may start only at or before the
 * end of the bytes received so far.
 *
 * COMMIT checks the staged configuration once, and refuses it (NAK DESCRIPTOR, the previous
 * configuration kept) unless it holds together: every descriptor complete, every bLength at least
 * 2 and inside the blob, interface numbers 0, 1, 2, ... in order with alternate setting 0 only,
 * every interface of any class whose endpoints are interrupt or bulk (a HID interface also has
 * its HID descriptor, one interrupt IN endpoint and at most one interrupt OUT endpoint, and a
 * report descriptor of the length that HID descriptor says), interface association descriptors
 * only before a group of non-HID interfaces, endpoint numbers the controller has and no address
 * twice, packet sizes the board takes, every IN endpoint of the generic driver with an EP_CONFIG (docs/PROTOCOL.md has
 * the whole list).
 *
 * FEATURE payload after the sequence number:
 *   0 interface number  1 report id  2.. bytes (without the report id), at most max_feature_bytes
 * EP_CONFIG payload after the sequence number:
 *   0 endpoint address  1 depth  2 policy
 *   The address is that of an IN endpoint of the staged configuration. The depth is how many
 *   transfers may wait on the board, the one the controller is sending not counted (1 up to what
 *   its queue holds); policy says what happens when that many wait: DROP_NEWEST drops the transfer
 *   that arrives, DROP_OLDEST drops the oldest that is not being sent, WAIT is DROP_NEWEST on the
 *   board (the host
 *   counts and does not send more than the depth; MIMHID_EV_EP_DONE is how it learns what the
 *   board finished). An HID endpoint takes DROP_NEWEST only in this release (NAK UNSUPPORTED).
 *   A queue of the generic driver is cut from the board's pool of endpoint queue slots (see
 *   docs/LIMITS.md for what each policy costs); more than the pool holds is NAK OVERFLOW. Every IN
 *   endpoint of the generic driver needs one before COMMIT (NAK STATE otherwise).
 * EP_STALL payload after the sequence number: 0 endpoint address  1 stall (1) or clear (0)
 *   Stalling sets the Halt feature of the endpoint (USB 2.0 9.4.5): the endpoint answers STALL
 *   until it is cleared, by this command or by the target. On an IN endpoint it drops what waits
 *   and the transfer being sent. NAK STATE when the address is not an endpoint of the generic
 *   driver in the active configuration (an HID endpoint is not).
 * EP_FLUSH payload after the sequence number: 0 endpoint address (IN). Drops what waits on the
 *   board for it; a transfer already given to the controller completes. NAK STATE as for
 *   EP_STALL, NAK PARAM for an OUT endpoint (nothing waits on one).
 *
 * Endpoint data frames. 0x40 | n (host to board): one transfer for IN endpoint n of the active
 * configuration. On a generic endpoint the payload is the whole transfer: at most
 * max_transfer_bytes, and an empty payload is a zero-length packet (USB 2.0 5.8.3); a frame that
 * is not the last of a longer transfer is a multiple of the endpoint's packet size, so the target
 * sees one transfer. On an HID endpoint an empty payload is dropped. 0xC0 | n (board to host): a
 * packet the target sent on OUT endpoint n, as it came. The board arms the endpoint for the next
 * packet only after it has written that frame, so a busy link holds the target with NAKs and
 * nothing is lost.
 *
 * EVENT payload: kind, then
 *   OUTPUT_REPORT  interface, length (two bytes), the report
 *   SET_REPORT     interface, report type, report id, the bytes the target sent (cut to what
 *                  the board holds)
 *   TRANSFER_LOST  endpoint address, count (two bytes)
 *   EP_DONE        endpoint address, transfers completed (two bytes) since the last EP_DONE of
 *                  that endpoint; at most one per endpoint per pass of the board's USB side
 *   EP_HALT        endpoint address, halted (1: the target set the halt, 0: it cleared it)
 * (The interface of an event is the bInterfaceNumber of the staged configuration.)
 */
#define MIMHID_DESC_DEVICE 1
#define MIMHID_DESC_CONFIGURATION 2
#define MIMHID_DESC_STRING 3
#define MIMHID_DESC_HID_REPORT 4
#define MIMHID_DESC_F_SERIAL_FROM_ID 0x01

#define MIMHID_EP_FULL_DROP_NEWEST 1
#define MIMHID_EP_FULL_DROP_OLDEST 2
#define MIMHID_EP_FULL_WAIT 3

#endif /* MIMHID_PROTO_H */
