// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
#include "transport/frame.hpp"

#include "mimusb.h"

#include <cstring>

namespace mimusb::transport {

std::size_t encode_frame(std::uint8_t type, const std::uint8_t* payload, std::size_t length,
                         std::uint8_t* out) noexcept {
    std::size_t n = 0;
    std::uint16_t crc = mimusb_crc16_update(MIMUSB_CRC16_INIT, type);
    out[n++] = MIMUSB_SYNC;
    out[n++] = type;
    if (length < 0x80U) {
        out[n++] = static_cast<std::uint8_t>(length);
        crc = mimusb_crc16_update(crc, static_cast<std::uint8_t>(length));
    } else {
        const auto low = static_cast<std::uint8_t>(0x80U | (length & 0x7FU));
        const auto high = static_cast<std::uint8_t>(length >> 7);
        out[n++] = low;
        out[n++] = high;
        crc = mimusb_crc16_update(crc, low);
        crc = mimusb_crc16_update(crc, high);
    }
    for (std::size_t i = 0; i < length; i++) {
        out[n++] = payload[i];
        crc = mimusb_crc16_update(crc, payload[i]);
    }
    out[n++] = static_cast<std::uint8_t>(crc & 0xFFU);
    out[n++] = static_cast<std::uint8_t>(crc >> 8);
    return n;
}

bool FrameParser::next(Frame* out) noexcept {
    for (;;) {
        // Find a sync byte.
        while (head_ != tail_ && ring_[tail_] != MIMUSB_SYNC)
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
        // 0xC0 itself is endpoint 0, which has no data frame (mimusb_proto.h).
        const bool board_type = type == MIMUSB_T_PONG || type == MIMUSB_T_ACK ||
                                type == MIMUSB_T_NAK || type == MIMUSB_T_EVENT ||
                                (type > MIMUSB_T_EP_OUT_BASE && type <= MIMUSB_T_EP_OUT_LAST);
        if (!board_type) {
            tail_ = (tail_ + 1U) & kMask;
            continue;
        }
        if (len > MIMUSB_PROTO_MAX_PAYLOAD) {
            tail_ = (tail_ + 1U) & kMask;
            continue;
        }
        if (available < header + len + 2U)
            return false;
        std::uint16_t crc = mimusb_crc16_update(MIMUSB_CRC16_INIT, type);
        crc = mimusb_crc16_update(crc, len_low);
        if (header == 4U)
            crc = mimusb_crc16_update(crc, len_high);
        for (std::size_t i = 0; i < len; i++) {
            const std::uint8_t b = at(header + i);
            out->payload[i] = b;
            crc = mimusb_crc16_update(crc, b);
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
    // The firmware version and ready come first in every release.
    if (frame.type != MIMUSB_T_PONG || frame.length < 4U)
        return false;
    const auto& p = frame.payload;
    *out = BoardInfo{};
    out->version[0] = p[0];
    out->version[1] = p[1];
    out->version[2] = p[2];
    out->ready = p[3] != 0;
    // A board of another major or minor version is described by those alone.
    if (p[0] != MIMUSB_VERSION_MAJOR || p[1] != MIMUSB_VERSION_MINOR)
        return true;
    if (frame.length < MIMUSB_PONG_MIN_LEN)
        return false;
    const std::uint8_t id_length = p[35];
    if (id_length > MIMUSB_UNIQUE_ID_MAX || frame.length < MIMUSB_PONG_MIN_LEN + id_length)
        return false;
    out->compatible = true;
    out->has_unique_id = (p[6] & MIMUSB_PONG_FLAG_UNIQUE_ID) != 0;
    out->chip = p[7];
    out->usb_high_speed = p[8] != 0;
    out->baud = get_u32(&p[9]);
    out->max_baud = get_u32(&p[13]);
    out->uart_flags = get_u32(&p[17]);
    out->max_report_bytes = static_cast<std::uint16_t>(p[23] | (p[24] << 8));
    out->max_endpoint_bytes = static_cast<std::uint16_t>(p[27] | (p[28] << 8));
    out->max_frame_payload = static_cast<std::uint16_t>(p[29] | (p[30] << 8));
    out->max_control_data_bytes = static_cast<std::uint16_t>(p[31] | (p[32] << 8));
    out->max_control_rules = p[33];
    out->max_endpoint_number = p[34];
    out->max_interfaces_any = p[36];
    out->max_config_descriptor_bytes = static_cast<std::uint16_t>(p[37] | (p[38] << 8));
    out->max_strings = p[39];
    out->max_transfer_bytes = static_cast<std::uint16_t>(p[40] | (p[41] << 8));
    out->endpoint_queue_slots = static_cast<std::uint16_t>(p[42] | (p[43] << 8));
    out->transfer_types = p[44];
    out->max_answer_bytes = static_cast<std::uint16_t>(p[45] | (p[46] << 8));
    out->max_bos_bytes = static_cast<std::uint16_t>(p[47] | (p[48] << 8));
    out->unique_id_length = id_length;
    std::memcpy(out->unique_id.data(), &p[MIMUSB_PONG_MIN_LEN], id_length);
    return true;
}

} // namespace mimusb::transport
