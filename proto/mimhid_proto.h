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
 * receives or sends it (a HID Report ID included) and are not acknowledged: 0x41..0x4F to an IN
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
            every staged descriptor, string, control rule and endpoint setting */
#define MIMHID_T_DESC_BEGIN 0x11 /* announces one descriptor (see below) */
#define MIMHID_T_DESC_DATA 0x12  /* a chunk of the announced descriptor (see below) */
#define MIMHID_T_COMMIT                                                                            \
    0x13                      /* apply: disconnect, rebuild descriptors, reconnect, then ACK;      \
                                  payload after sequence number: [detach: 1 byte] [delay_ms: 2 bytes little-endian] */
/* 0x14 was FEATURE: unused since the board's HID driver went (a Feature report answer is the data
 * of an ANSWER control rule). The number is not reused in this release. */
#define MIMHID_T_RESET 0x15   /* drop every queued report */
#define MIMHID_T_REMOTE_WAKEUP 0x16
#define MIMHID_T_SET_BAUD 0x17 /* payload after sequence number: the rate, u32 little endian */
#define MIMHID_T_RULE                                                                              \
    0x18                            /* stages one control rule (see "Control rules" below) */
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
#define MIMHID_T_BOOTLOADER                                                                        \
    0x20 /* payload after sequence number: the four bytes MIMHID_BOOTLOADER_KEY. The board takes  \
            its USB device off the target, acknowledges, and restarts into the chip's own         \
            bootloader, where new firmware can be written; NAK_UNSUPPORTED on a chip that has     \
            none the firmware can enter, NAK_PARAM for another payload */
#define MIMHID_BOOTLOADER_KEY "BOOT"
#define MIMHID_T_RULES_APPLY                                                                       \
    0x21 /* payload after sequence number: flags (bit 0: report every control transfer), rule     \
            count. Makes the staged rules the active ones without a COMMIT (see below) */
#define MIMHID_T_RULE_DATA                                                                         \
    0x22 /* payload after sequence number: rule index, then the new bytes of an active ANSWER     \
            rule (see below) */
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
/* 5 to 8 were SET_REPORT, SET_PROTOCOL, SET_IDLE and OUTPUT_REPORT, which the board's HID driver
 * made. A HID request is now a control transfer taken by an ACCEPT rule, and an Output report on
 * an OUT endpoint is an endpoint data frame. The numbers are not reused in this release. */
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
/*
 * Control transfers. Every request the target sends on endpoint 0 (the setup packet of a
 * control transfer) is looked up in a table of rules, which the host writes. The first rule that
 * matches decides what the board does; a request no rule matches is left to the board's USB stack,
 * which answers the standard requests it can and stalls the rest (USB 2.0 9.2.7).
 *
 * RULE (staged) payload after the sequence number:
 *   0 rule index  1 bmRequestType mask  2 bmRequestType value  3 bRequest mask  4 bRequest value
 *   5..6 wValue mask  7..8 wValue value  9..10 wIndex mask  11..12 wIndex value  13 action
 *   then by action (MIMHID_RULE_*):
 *     ANSWER           14..15 capacity (u16 LE): the answer memory the rule reserves for as long
 *                      as the table is active  16..17 length (u16 LE, at most the capacity): the
 *                      bytes it answers with when it starts. Those bytes follow in DESC_BEGIN and
 *                      DESC_DATA with the kind MIMHID_DESC_ANSWER and the rule index as index
 *                      (language 0, flags 0); an answer of length 0 needs neither.
 *     ANSWER_LAST_SENT 14 endpoint address (an IN endpoint)  15 filter (0 or 1)  16 first byte
 *     ASK              14..15 timeout in ms (1..MIMHID_CONTROL_TIMEOUT_MAX)  16 action after the
 *                      timeout (MIMHID_REPLY_STALL or MIMHID_REPLY_DROP)
 *     ACCEPT, STALL    nothing more
 *   A request matches a rule when each field ANDed with its mask equals the value. The rules are
 *   numbered from 0 in the order they are tried. The staged table is a stack: a RULE with index i
 *   (at most the number of rules staged) replaces rule i and forgets every rule after it, so the
 *   host writes the table in order. DESC_RESET forgets the staged rules. The table and its answer
 *   bytes are limited by the board (control rule count and answer bytes in the PONG); more is
 *   NAK_OVERFLOW. A value outside its mask, an unknown action, an ANSWER or ANSWER_LAST_SENT to a request that is not
 *   device to host (bit 7 of bmRequestType in both mask and value), an ASK without its timeout or
 *   with another action after it, or a LAST_SENT with an endpoint that is not an IN address is
 *   NAK_PARAM.
 * RULES_APPLY payload after the sequence number: 0 flags (bit 0: report every control transfer)
 *   1 rule count n. The first n staged rules (the rest are forgotten) become the active table
 *   without a COMMIT, and nothing held under the old table is kept: it goes to the USB stack as if
 *   no rule had matched it. NAK_PARAM when n is more than the staged rules; NAK_STATE when an ANSWER
 *   rule lacks its bytes, or an ANSWER_LAST_SENT rule names an endpoint that is not an IN endpoint
 *   of the active configuration; NAK_BUSY when the board is answering a control transfer from the
 *   active answer memory (try again). A COMMIT applies all the staged rules with the descriptors
 *   (flag 0, nothing reported beyond what the rules say), under the same checks (NAK_STATE).
 *   A board starts with no rules.
 * RULE_DATA payload after the sequence number: 0 rule index  1.. bytes. Replaces the bytes of an
 *   ACTIVE rule of action ANSWER, any number of them up to the capacity the rule was staged with,
 *   whatever length it has now (the rule then answers with that many); NAK_PARAM for another
 *   rule, NAK_OVERFLOW for more bytes than the capacity, NAK_BUSY when the board is answering from
 *   that rule at the moment. The staged copy changes too, so a later
 *   RULES_APPLY or COMMIT keeps it.
 *
 * Actions of a rule:
 *   ANSWER            returns its bytes, cut to wLength, straight from the answer memory
 *   ANSWER_LAST_SENT  returns the last transfer the board started on the endpoint (with the filter:
 *                     the last whose first byte is the given one), cut to wLength; stalls when there
 *                     is none. RULES_APPLY and COMMIT forget what the changed rules hold; RESET
 *                     forgets all of it
 *   ACCEPT            takes the data stage (up to MIMHID_CONTROL_DATA_MAX) and completes the
 *                     transfer; the host is told with CONTROL_SETUP (flag accepted) and, when
 *                     there was a data stage, CONTROL_DATA. Nothing is awaited
 *   ASK               holds the transfer and asks the host (CONTROL_SETUP with the flag held),
 *                     which answers with CONTROL_REPLY within the timeout
 *   STALL             stalls
 *
 * EVENT CONTROL_SETUP payload: kind, token, flags, the 8 setup bytes as they came (little
 *   endian wValue, wIndex, wLength). flags: bit 0 held (a CONTROL_REPLY with this token is
 *   awaited), bit 1 too long (a host-to-device data stage longer than the board takes, not held;
 *   reported for an ACCEPT rule, and for any request when every transfer is reported), bit 2 fixed
 *   (SET_ADDRESS and SET_CONFIGURATION change the board's own USB state, so they are reported and
 *   never held), bit 3 lost (reports before this one were dropped: the board's queue was full),
 *   bit 4 accepted (an ACCEPT rule took it; a CONTROL_DATA with this token follows when it has a
 *   data stage). Rules other than ACCEPT and ASK are reported only when every transfer is.
 * EVENT CONTROL_DATA payload: kind, token, the bytes of the data stage. Sent after the board
 *   accepted a held host-to-device transfer with a data stage (the status stage waits for the
 *   second CONTROL_REPLY), or for an accepted one (nothing is awaited).
 * CONTROL_REPLY payload after the sequence number: 0 token  1 action (MIMHID_REPLY_*)  2.. the data
 *   stage of a device-to-host transfer (action ANSWER only; sent whole whatever wLength says).
 *   Acknowledged when the board still waits for that token; a NAK with MIMHID_NAK_STATE says it no
 *   longer does.
 *
 * A held transfer is answered by the host within its rule's timeout; after it the board does the
 * action the rule names (stall or drop). USB 2.0 (9.2.6.4) asks a device to return the first data
 * packet of a standard request within 500 ms, and a control transfer is allowed 5 s in all
 * (9.2.6.1); hosts may give up earlier, and a request without a data stage may be held to a shorter
 * time still.
 */
#define MIMHID_RULE_ANSWER 1
#define MIMHID_RULE_ANSWER_LAST_SENT 2
#define MIMHID_RULE_ACCEPT 3
#define MIMHID_RULE_ASK 4
#define MIMHID_RULE_STALL 5
/* The actions of CONTROL_REPLY, and the one a held transfer gets when no reply came in time. */
#define MIMHID_REPLY_LEAVE 0  /* the board's own handling, as if the host had not been asked */
#define MIMHID_REPLY_ANSWER 1 /* the data (device to host) or an empty status (otherwise) */
#define MIMHID_REPLY_STALL 2
#define MIMHID_REPLY_DROP                                                                          \
    3 /* silent drop: do not stall, do not arm, keep NAKing until host aborts */
/* USB 2.0 section 9.2.6.1: "USB sets an upper limit of 5 seconds as the upper limit for any
 * command to be processed." A rule may hold a request for that long and no longer. */
#define MIMHID_CONTROL_TIMEOUT_MAX 5000

#define MIMHID_CONTROL_F_HELD 0x01
#define MIMHID_CONTROL_F_TOO_LONG 0x02
#define MIMHID_CONTROL_F_FIXED 0x04
#define MIMHID_CONTROL_F_LOST 0x08
#define MIMHID_CONTROL_F_ACCEPTED 0x10

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
 *   4 reserved, zero  5 reserved, zero  6 flags (bit 0: has chip unique id)
 *   7 chip  8 usb speed (0 full, 1 high)
 *   9..12 baud the board is on (u32)  13..16 max_baud, the fastest rate its UART can make (u32)
 *   17..20 uart flags (u32): bit 0 break detection works, bits 1.. reserved, zero
 *   21..22 reserved, zero  23..24 max_report_bytes  25..26 reserved, zero
 *   27..28 max_endpoint_bytes (the largest endpoint packet)  29..30 max_frame_payload
 *   31..32 max_control_data_bytes (the data stage of a control transfer the host may read or
 *   answer)  33 max_control_rules (the table the host stages, Nodes' rules and the device's together)
 *   34 max_endpoint_number (the highest endpoint number the controller has; an endpoint address
 *   above it is refused)
 *   35 unique id length n (0..16)
 *   36 max_interfaces_any (interfaces of any class the board takes)  37..38 max_config_descriptor_bytes  39 max_strings (string
 *   descriptors, the board's own included)  40..41 max_transfer_bytes (the payload of one endpoint
 *   data frame)  42..43 endpoint_queue_slots (the slots of the pool the IN queues of the generic
 *   driver are cut from)  44 transfer_types (bit n set: the board has endpoints of transfer type
 *   n; USB 2.0 9.6.6 bmAttributes bits 1..0: bit 2 bulk, bit 3 interrupt)  45..46
 *   max_answer_bytes (the answer memory: the capacities of every ANSWER rule together, a HID report
 *   descriptor and the answers to its Feature reads among them)
 *   47.. the chip's unique id, n bytes
 * Bytes 0..3 are the same in every release. A reader ignores bytes past the ones it knows.
 */
#define MIMHID_PONG_MIN_LEN 47
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
 *   MIMHID_DESC_ANSWER        rule index      0         the bytes of an ANSWER control rule
 *
 * String index 0 (the language list) is never sent: the board answers it with the language ids
 * of the strings it holds. With MIMHID_DESC_F_SERIAL_FROM_ID on a string, the total length is 0 and
 * the board makes the string from its chip id (the characters are the hexadecimal digits of the
 * id). All other flag bits are zero.
 *
 * The kind ANSWER names a rule staged with RULE (an ANSWER rule of length 0 has no bytes to send):
 * DESC_BEGIN's total length must be the rule's length (NAK_PARAM otherwise), and DESC_DATA fills
 * the rule's bytes in the same way as any other descriptor.
 *
 * A DESC_BEGIN repeated with the same kind, index, language, total length and flags is
 * acknowledged again, and so is a DESC_DATA repeated with the same offset and bytes, since the
 * host sends a frame again when it did not see the answer. A chunk may start only at or before the
 * end of the bytes received so far.
 *
 * COMMIT checks the staged configuration once, and refuses it (NAK DESCRIPTOR, the previous
 * configuration kept) unless it holds together: every descriptor complete, every bLength at least
 * 2 and inside the blob, interface numbers 0, 1, 2, ... in order with alternate setting 0 only,
 * every interface, whatever its class (the board reads no class), with endpoints that are
 * interrupt or bulk, the class descriptors between them as given, interface association
 * descriptors that name the interfaces that follow, endpoint numbers the controller has and no
 * address twice, packet sizes the board takes, every IN endpoint with an EP_CONFIG, every ANSWER
 * rule with its bytes and every ANSWER_LAST_SENT rule on an IN endpoint of the configuration
 * (docs/PROTOCOL.md has the whole list).
 *
 * EP_CONFIG payload after the sequence number:
 *   0 endpoint address  1 depth  2 policy
 *   The address is that of an IN endpoint of the staged configuration. The depth is how many
 *   transfers may wait on the board, the one the controller is sending not counted (1 up to what
 *   its queue holds); policy says what happens when that many wait: DROP_NEWEST drops the transfer
 *   that arrives, DROP_OLDEST drops the oldest that is not being sent, WAIT is DROP_NEWEST on the
 *   board (the host
 *   counts and does not send more than the depth; MIMHID_EV_EP_DONE is how it learns what the
 *   board finished). A queue is cut from the board's pool of endpoint queue slots (see
 *   docs/LIMITS.md for what each policy costs); more than the pool holds is NAK OVERFLOW. Every IN
 *   endpoint needs one before COMMIT (NAK STATE otherwise).
 * EP_STALL payload after the sequence number: 0 endpoint address  1 stall (1) or clear (0)
 *   Stalling sets the Halt feature of the endpoint (USB 2.0 9.4.5): the endpoint answers STALL
 *   until it is cleared, by this command or by the target. On an IN endpoint it drops what waits
 *   and the transfer being sent. NAK STATE when the address is not an endpoint of the active
 *   configuration.
 * EP_FLUSH payload after the sequence number: 0 endpoint address (IN). Drops what waits on the
 *   board for it; a transfer already given to the controller completes. NAK STATE as for
 *   EP_STALL, NAK PARAM for an OUT endpoint (nothing waits on one).
 *
 * Endpoint data frames. 0x40 | n (host to board): one transfer for IN endpoint n of the active
 * configuration. The payload is the whole transfer: at most
 * max_transfer_bytes, and an empty payload is a zero-length packet (USB 2.0 5.8.3); a frame that
 * is not the last of a longer transfer is a multiple of the endpoint's packet size, so the target
 * sees one transfer. 0xC0 | n (board to host): a
 * packet the target sent on OUT endpoint n, as it came. The board arms the endpoint for the next
 * packet only after it has written that frame, so a busy link holds the target with NAKs and
 * nothing is lost.
 *
 * EVENT payload: kind, then
 *   TRANSFER_LOST  endpoint address, count (two bytes)
 *   EP_DONE        endpoint address, transfers completed (two bytes) since the last EP_DONE of
 *                  that endpoint; at most one per endpoint per pass of the board's USB side
 *   EP_HALT        endpoint address, halted (1: the target set the halt, 0: it cleared it)
 * (The interface of an event is the bInterfaceNumber of the staged configuration.)
 */
#define MIMHID_DESC_DEVICE 1
#define MIMHID_DESC_CONFIGURATION 2
#define MIMHID_DESC_STRING 3
#define MIMHID_DESC_ANSWER 5 /* 4 was the HID report descriptor, which the board no longer knows */
#define MIMHID_DESC_F_SERIAL_FROM_ID 0x01

#define MIMHID_EP_FULL_DROP_NEWEST 1
#define MIMHID_EP_FULL_DROP_OLDEST 2
#define MIMHID_EP_FULL_WAIT 3

#endif /* MIMHID_PROTO_H */
