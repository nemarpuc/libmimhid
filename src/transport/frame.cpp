// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#include "transport/frame.hpp"

#include <cstring>

namespace mimhid::transport {

std::size_t encode_frame(std::uint8_t type, const std::uint8_t* payload, std::size_t length,
                         std::uint8_t* out) noexcept {
    std::size_t n = 0;
    std::uint16_t crc = mimhid_crc16_update(MIMHID_CRC16_INIT, type);
    out[n++] = MIMHID_SYNC;
    out[n++] = type;
    if (length < 0x80U) {
        out[n++] = static_cast<std::uint8_t>(length);
        crc = mimhid_crc16_update(crc, static_cast<std::uint8_t>(length));
    } else {
        const auto low = static_cast<std::uint8_t>(0x80U | (length & 0x7FU));
        const auto high = static_cast<std::uint8_t>(length >> 7);
        out[n++] = low;
        out[n++] = high;
        crc = mimhid_crc16_update(crc, low);
        crc = mimhid_crc16_update(crc, high);
    }
    for (std::size_t i = 0; i < length; i++) {
        out[n++] = payload[i];
        crc = mimhid_crc16_update(crc, payload[i]);
    }
    out[n++] = static_cast<std::uint8_t>(crc & 0xFFU);
    out[n++] = static_cast<std::uint8_t>(crc >> 8);
    return n;
}

bool FrameParser::next(Frame* out) noexcept {
    for (;;) {
        // Find a sync byte.
        while (head_ != tail_ && ring_[tail_] != MIMHID_SYNC)
            tail_ = (tail_ + 1U) & kMask;
        const std::size_t available = size();
        if (available < 3U)
            return false;
        const std::uint8_t type = at(1U);
        const std::uint8_t len_low = at(2U);
        std::uint8_t len_high = 0;
        std::size_t header = 3U;
        std::size_t len = len_low;
        if ((len_low & 0x80U) != 0U) {
            if (available < 4U)
                return false;
            len_high = at(3U);
            len = static_cast<std::size_t>(len_low & 0x7FU) |
                  (static_cast<std::size_t>(len_high) << 7);
            header = 4U;
        }
        const bool board_type = type == MIMHID_T_PONG || type == MIMHID_T_ACK ||
                                type == MIMHID_T_NAK || type == MIMHID_T_EVENT;
        if (!board_type) {
            tail_ = (tail_ + 1U) & kMask;
            continue;
        }
        if (len > MIMHID_PROTO_MAX_PAYLOAD) {
            tail_ = (tail_ + 1U) & kMask;
            continue;
        }
        // A PONG of another protocol version is framed by that version's rules (protocols 1 to 6
        // end in one XOR byte): it is accepted to tell the user to flash the board, nothing more.
        if (type == MIMHID_T_PONG && len >= 5U && available >= header + 5U &&
            at(header + 4U) != MIMHID_PROTO_VERSION) {
            if (available < header + len + 1U)
                return false;
            std::uint8_t x = static_cast<std::uint8_t>(type ^ len_low ^ len_high);
            for (std::size_t i = 0; i < len; i++) {
                const std::uint8_t b = at(header + i);
                out->payload[i] = b;
                x = static_cast<std::uint8_t>(x ^ b);
            }
            if (x != at(header + len)) {
                tail_ = (tail_ + 1U) & kMask;
                continue;
            }
            out->type = type;
            out->length = static_cast<std::uint16_t>(len);
            tail_ = (tail_ + header + len + 1U) & kMask;
            return true;
        }
        if (available < header + len + 2U)
            return false;
        std::uint16_t crc = mimhid_crc16_update(MIMHID_CRC16_INIT, type);
        crc = mimhid_crc16_update(crc, len_low);
        if (header == 4U)
            crc = mimhid_crc16_update(crc, len_high);
        for (std::size_t i = 0; i < len; i++) {
            const std::uint8_t b = at(header + i);
            out->payload[i] = b;
            crc = mimhid_crc16_update(crc, b);
        }
        if (at(header + len) != static_cast<std::uint8_t>(crc & 0xFFU) ||
            at(header + len + 1U) != static_cast<std::uint8_t>(crc >> 8)) {
            tail_ = (tail_ + 1U) & kMask;
            continue;
        }
        out->type = type;
        out->length = static_cast<std::uint16_t>(len);
        tail_ = (tail_ + header + len + 2U) & kMask;
        return true;
    }
}

bool decode_counters(const std::uint8_t* data, std::size_t length, LinkCounters* out) noexcept {
    if (length < kCountersBytes)
        return false;
    auto u16 = [&](std::size_t i) {
        return static_cast<std::uint16_t>(data[2 * i] | (data[2 * i + 1] << 8));
    };
    out->probes_good = u16(0);
    out->bad_crc = u16(1);
    out->framing = u16(2);
    out->uart_overrun = u16(3);
    out->ring_overrun = u16(4);
    return true;
}

bool decode_pong(const Frame& frame, BoardInfo* out) noexcept {
    if (frame.type != MIMHID_T_PONG || frame.length < 5U)
        return false;
    // Every version starts with the firmware version, ready and the protocol version; a board
    // on another protocol is reported with those alone.
    if (frame.payload[4] != MIMHID_PROTO_VERSION) {
        *out = BoardInfo{};
        out->version[0] = frame.payload[0];
        out->version[1] = frame.payload[1];
        out->version[2] = frame.payload[2];
        out->ready = frame.payload[3] != 0;
        out->protocol = frame.payload[4];
        return true;
    }
    if (frame.length < MIMHID_PONG_MIN_LEN)
        return false;
    const std::uint8_t id_length = frame.payload[35];
    if (id_length > MIMHID_UNIQUE_ID_MAX || frame.length < MIMHID_PONG_MIN_LEN + id_length)
        return false;
    const auto& p = frame.payload;
    out->version[0] = p[0];
    out->version[1] = p[1];
    out->version[2] = p[2];
    out->ready = p[3] != 0;
    out->protocol = p[4];
    out->max_nodes = p[5];
    out->max_interfaces = p[6];
    out->has_unique_id = (p[7] & MIMHID_PONG_FLAG_UNIQUE_ID) != 0;
    out->chip = p[8];
    out->usb_high_speed = p[9] != 0;
    out->baud = get_u32(&p[10]);
    out->max_baud = get_u32(&p[14]);
    out->uart_flags = get_u32(&p[18]);
    out->max_descriptor_bytes = static_cast<std::uint16_t>(p[22] | (p[23] << 8));
    out->max_report_bytes = static_cast<std::uint16_t>(p[24] | (p[25] << 8));
    out->max_feature_bytes = static_cast<std::uint16_t>(p[26] | (p[27] << 8));
    out->max_endpoint_bytes = static_cast<std::uint16_t>(p[28] | (p[29] << 8));
    out->max_frame_payload = static_cast<std::uint16_t>(p[30] | (p[31] << 8));
    out->max_control_data_bytes = static_cast<std::uint16_t>(p[32] | (p[33] << 8));
    out->max_control_rules = p[34];
    out->unique_id_length = id_length;
    std::memcpy(out->unique_id.data(), &p[36], id_length);
    return true;
}

} // namespace mimhid::transport
