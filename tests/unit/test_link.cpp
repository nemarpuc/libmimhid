// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Covers the host's frame encoding and parsing without a serial port: which frame types the
 * parser takes from a board, and how it skips what is not a valid frame. */
#include "test.hpp"
#include "transport/link.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace {

using mimhid::transport::encode_frame;
using mimhid::transport::Frame;
using mimhid::transport::FrameParser;

std::vector<std::uint8_t> frame_bytes(const std::uint8_t type,
                                      const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out(payload.size() + 5U);
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
    for (const std::uint8_t type :
         {std::uint8_t{MIMHID_EV_SET_REPORT}, std::uint8_t{MIMHID_EV_OUTPUT_REPORT},
          std::uint8_t{MIMHID_T_PING}, std::uint8_t{MIMHID_T_COMMIT},
          std::uint8_t{MIMHID_T_REPORT_BASE}}) {
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
    bytes.back() = static_cast<std::uint8_t>(bytes.back() ^ 0xFFU);
    const std::vector<std::uint8_t> good = frame_bytes(MIMHID_T_ACK, {0x06U, 0x13U});
    bytes.insert(bytes.end(), good.begin(), good.end());
    const std::vector<Frame> frames = parse(bytes);
    MIMHID_CHECK(frames.size() == 1U);
    if (frames.size() == 1U)
        MIMHID_CHECK(frames[0].payload[0] == 0x06U);
}

} // namespace

void test_link() {
    test_board_frames_parse();
    test_long_frame_parses();
    test_other_types_are_skipped();
    test_bad_checksum_is_skipped();
}
