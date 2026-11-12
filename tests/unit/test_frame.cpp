// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Covers the host's frame encoding and parsing without a serial port: which frame types the
 * parser takes from a board, and how it skips what is not a valid frame. */
#include "mimhid.h"
#include "test.hpp"
#include "transport/frame.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

using mimhid::transport::encode_frame;
using mimhid::transport::Frame;
using mimhid::transport::FrameParser;

std::vector<std::uint8_t> frame_bytes(const std::uint8_t type,
                                      const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out(payload.size() + 6U);
    out.resize(encode_frame(type, payload.data(), payload.size(), out.data()));
    return out;
}

std::vector<Frame> parse(const std::vector<std::uint8_t>& bytes) {
    // On the heap: a parser holds a 16 KiB ring.
    const auto parser = std::make_unique<FrameParser>();
    std::vector<Frame> frames;
    parser->feed(bytes.data(), bytes.size(),
                 [&frames](const Frame& frame) { frames.push_back(frame); });
    return frames;
}

void test_board_frames_parse() {
    for (const std::uint8_t type : {std::uint8_t{MIMHID_T_PONG}, std::uint8_t{MIMHID_T_ACK},
                                    std::uint8_t{MIMHID_T_NAK}, std::uint8_t{MIMHID_T_EVENT}}) {
        const std::vector<Frame> frames = parse(frame_bytes(type, {0x01U, 0x02U, 0x03U}));
        MIMHID_CHECK(frames.size() == 1U);
        if (frames.size() == 1U) {
            MIMHID_CHECK(frames[0].type == type);
            MIMHID_CHECK(frames[0].length == 3U);
            MIMHID_CHECK(frames[0].payload[2] == 0x03U);
        }
    }
}

void test_long_frame_parses() {
    // A payload of 128 bytes or more takes a two-byte length.
    std::vector<std::uint8_t> payload(300U);
    for (std::size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<std::uint8_t>(i);
    const std::vector<Frame> frames = parse(frame_bytes(MIMHID_T_EVENT, payload));
    MIMHID_CHECK(frames.size() == 1U);
    if (frames.size() == 1U) {
        MIMHID_CHECK(frames[0].length == 300U);
        MIMHID_CHECK(frames[0].payload[299] == static_cast<std::uint8_t>(299U));
    }
}

void test_other_types_are_skipped() {
    // A board sends only PONG, ACK, NAK and EVENT. Event kinds (MIMHID_EV_*) travel inside an
    // EVENT frame and are no frame types; host-to-board types are not taken from a board either.
    std::vector<std::uint8_t> bytes;
    for (const std::uint8_t type : {std::uint8_t{MIMHID_T_RULE}, std::uint8_t{MIMHID_T_RULES_APPLY},
                                    std::uint8_t{MIMHID_T_PING}, std::uint8_t{MIMHID_T_COMMIT},
                                    std::uint8_t{MIMHID_T_EP_IN_BASE}}) {
        const std::vector<std::uint8_t> frame = frame_bytes(type, {0x00U, 0x01U, 0x02U});
        bytes.insert(bytes.end(), frame.begin(), frame.end());
    }
    const std::vector<std::uint8_t> event = frame_bytes(MIMHID_T_EVENT, {MIMHID_EV_MOUNTED});
    bytes.insert(bytes.end(), event.begin(), event.end());
    const std::vector<Frame> frames = parse(bytes);
    MIMHID_CHECK(frames.size() == 1U);
    if (frames.size() == 1U) {
        MIMHID_CHECK(frames[0].type == MIMHID_T_EVENT);
        MIMHID_CHECK(frames[0].length == 1U && frames[0].payload[0] == MIMHID_EV_MOUNTED);
    }
}

void test_bad_checksum_is_skipped() {
    std::vector<std::uint8_t> bytes = frame_bytes(MIMHID_T_ACK, {0x05U, 0x13U});
    bytes.back() = static_cast<std::uint8_t>(bytes.back() ^ 0xFFU); // the CRC's high byte
    const std::vector<std::uint8_t> good = frame_bytes(MIMHID_T_ACK, {0x06U, 0x13U});
    bytes.insert(bytes.end(), good.begin(), good.end());
    const std::vector<Frame> frames = parse(bytes);
    MIMHID_CHECK(frames.size() == 1U);
    if (frames.size() == 1U)
        MIMHID_CHECK(frames[0].payload[0] == 0x06U);
}

// CRC-16/CCITT-FALSE of "123456789" is 0x29B1 (the catalogued check value).
void test_crc_matches_the_standard() {
    std::uint16_t crc = MIMHID_CRC16_INIT;
    for (const char c : std::string("123456789"))
        crc = mimhid_crc16_update(crc, static_cast<std::uint8_t>(c));
    MIMHID_CHECK(crc == 0x29B1U);
}

// Two flipped bits in one bit position cancel in an XOR but not in a CRC.
void test_paired_bit_flips_are_caught() {
    std::vector<std::uint8_t> bytes = frame_bytes(MIMHID_T_ACK, {0x05U, 0x13U});
    bytes[3] = static_cast<std::uint8_t>(bytes[3] ^ 0x10U);
    bytes[4] = static_cast<std::uint8_t>(bytes[4] ^ 0x10U);
    MIMHID_CHECK(parse(bytes).empty());
}

void test_ack_data_is_kept() {
    const std::vector<Frame> frames =
        parse(frame_bytes(MIMHID_T_ACK, {0x05U, MIMHID_T_SET_BAUD, 0xC0U, 0xC6U, 0x2DU, 0x00U}));
    MIMHID_CHECK(frames.size() == 1U && frames[0].length == 6U);
}

// A PONG of another major or minor version decodes to its version and ready only: the host can
// tell the user to flash the board, nothing more.
void test_pong_of_another_version_is_recognised() {
    for (const std::uint8_t other :
         {std::uint8_t{MIMHID_VERSION_MINOR + 1U}, std::uint8_t{0xFFU}}) {
        const std::vector<Frame> frames =
            parse(frame_bytes(MIMHID_T_PONG, {MIMHID_VERSION_MAJOR, other, 3, 1}));
        MIMHID_CHECK(frames.size() == 1U);
        if (frames.size() == 1U) {
            mimhid::transport::BoardInfo info;
            MIMHID_CHECK(mimhid::transport::decode_pong(frames[0], &info) && !info.compatible &&
                         info.version[1] == other && info.version[2] == 3U && info.ready);
        }
    }
}

// The patch number does not change the wire: a board with another patch is compatible.
void test_pong_of_this_version_decodes_every_field() {
    std::vector<std::uint8_t> payload(MIMHID_PONG_MIN_LEN + 2U, 0U);
    // The bytes the old HID driver filled (5, 21..22, 25..26) are reserved and read as zero.
    payload[0] = MIMHID_VERSION_MAJOR;
    payload[1] = MIMHID_VERSION_MINOR;
    payload[2] = static_cast<std::uint8_t>(MIMHID_VERSION_PATCH + 5U);
    payload[3] = 1;
    payload[4] = 0;    // reserved
    payload[6] = 1;    // has a unique id
    payload[7] = 2;    // chip
    payload[9] = 0xC0; // baud 3,000,000
    payload[10] = 0xC6;
    payload[11] = 0x2D;
    payload[33] = 8;    // max_control_rules
    payload[34] = 6;    // max_endpoint_number
    payload[35] = 2;    // unique id length
    payload[36] = 12;   // max_interfaces_any
    payload[37] = 0x00; // max_config_descriptor_bytes 1024
    payload[38] = 0x04;
    payload[39] = 24;   // max_strings
    payload[40] = 0x40; // max_transfer_bytes 576
    payload[41] = 0x02;
    payload[42] = 32; // endpoint_queue_slots
    payload[43] = 0;
    payload[44] = 0x0C; // transfer types: bulk and interrupt
    payload[45] = 0x00; // max_answer_bytes 4096
    payload[46] = 0x10;
    payload[47] = 0x00; // max_bos_bytes 256
    payload[48] = 0x01;
    payload[MIMHID_PONG_MIN_LEN] = 0xAB;
    payload[MIMHID_PONG_MIN_LEN + 1] = 0xCD;
    const std::vector<Frame> frames = parse(frame_bytes(MIMHID_T_PONG, payload));
    MIMHID_CHECK(frames.size() == 1U);
    if (frames.size() == 1U) {
        mimhid::transport::BoardInfo info;
        MIMHID_CHECK(mimhid::transport::decode_pong(frames[0], &info) && info.compatible &&
                     info.max_answer_bytes == 4096U && info.max_bos_bytes == 256U &&
                     info.has_unique_id && info.chip == 2U && info.baud == 3000000U &&
                     info.max_control_rules == 8U && info.max_endpoint_number == 6U &&
                     info.max_interfaces_any == 12U && info.max_config_descriptor_bytes == 1024U &&
                     info.max_strings == 24U && info.max_transfer_bytes == 576U &&
                     info.endpoint_queue_slots == 32U && info.transfer_types == 0x0CU &&
                     info.unique_id_length == 2U && info.unique_id[0] == 0xABU &&
                     info.unique_id[1] == 0xCDU);
        // One byte short of the fixed part is refused.
        const std::vector<Frame> shorter = parse(frame_bytes(
            MIMHID_T_PONG,
            std::vector<std::uint8_t>(payload.begin(), payload.begin() + MIMHID_PONG_MIN_LEN - 1)));
        MIMHID_CHECK(shorter.size() == 1U && !mimhid::transport::decode_pong(shorter[0], &info));
    }
}

void test_counters_decode() {
    const std::uint8_t data[10] = {7, 0, 1, 0, 2, 0, 3, 0, 0xFF, 0xFF};
    mimhid::transport::LinkCounters c;
    MIMHID_CHECK(mimhid::transport::decode_counters(data, sizeof(data), &c));
    MIMHID_CHECK(c.probes_good == 7U && c.bad_crc == 1U && c.framing == 2U &&
                 c.uart_overrun == 3U && c.ring_overrun == 0xFFFFU && c.errors() == 65541U);
    MIMHID_CHECK(!mimhid::transport::decode_counters(data, 9U, &c));
}

} // namespace

void test_frame() {
    test_crc_matches_the_standard();
    test_paired_bit_flips_are_caught();
    test_ack_data_is_kept();
    test_pong_of_another_version_is_recognised();
    test_pong_of_this_version_decodes_every_field();
    test_counters_decode();
    test_board_frames_parse();
    test_long_frame_parses();
    test_other_types_are_skipped();
    test_bad_checksum_is_skipped();
}
