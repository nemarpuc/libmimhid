// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* The serial wire protocol's frame format (mimusb_proto.h): encoding,
 * parsing, the PONG and the link counters. No port, no threads: pure functions and a parser. */
#pragma once

#include "mimusb_proto.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace mimusb::transport {

struct BoardInfo {
    std::uint8_t version[3]{};
    bool ready{};
    bool compatible{}; // firmware major.minor equal to the library's; false: only version and ready
                       // are set
    std::uint16_t max_report_bytes{};
    std::uint16_t max_endpoint_bytes{};
    std::uint8_t max_endpoint_number{}; // the highest endpoint number, in both directions
    std::uint16_t max_frame_payload{};
    std::uint16_t max_control_data_bytes{};
    std::uint8_t max_control_rules{};
    std::uint16_t max_bos_bytes{};
    std::uint16_t max_answer_bytes{};  // the bytes of every ANSWER control rule together
    std::uint8_t max_interfaces_any{}; // interfaces of every class
    std::uint16_t max_config_descriptor_bytes{};
    std::uint8_t max_strings{};
    std::uint16_t max_transfer_bytes{}; // one frame to an IN endpoint
    std::uint16_t endpoint_queue_slots{};
    std::uint8_t transfer_types{}; // bit n: transfer type n
    bool has_unique_id{};
    std::uint8_t chip{};
    bool usb_high_speed{};
    std::uint32_t baud{};       // the rate the board is on
    std::uint32_t max_baud{};   // the fastest rate its UART can make
    std::uint32_t uart_flags{}; // MIMUSB_PONG_UART_*
    std::uint8_t unique_id_length{};
    std::array<std::uint8_t, MIMUSB_UNIQUE_ID_MAX> unique_id{};
};

// What the board counted on its line since the last reset (see mimusb_proto.h).
struct LinkCounters {
    std::uint16_t probes_good{};
    std::uint16_t bad_crc{};
    std::uint16_t framing{};
    std::uint16_t uart_overrun{};
    std::uint16_t ring_overrun{};

    // Zero when the line lost nothing.
    [[nodiscard]] std::uint32_t errors() const noexcept {
        return static_cast<std::uint32_t>(bad_crc) + framing + uart_overrun + ring_overrun;
    }
};
inline constexpr std::size_t kCountersBytes = 2U * MIMUSB_COUNTER_COUNT;
[[nodiscard]] bool decode_counters(const std::uint8_t* data, std::size_t length,
                                   LinkCounters* out) noexcept;

struct Frame {
    std::uint8_t type{};
    std::uint16_t length{};
    // Not zeroed: length says what is in it.
    std::array<std::uint8_t, MIMUSB_PROTO_MAX_PAYLOAD> payload;
};

// Splits a byte stream into valid frames, skipping garbage one byte at a time.
class FrameParser final {
  public:
    // Room for a few frames of the longest payload the protocol allows.
    static constexpr std::size_t kCapacity = 16384;
    static constexpr std::size_t kMask = kCapacity - 1;

    // Feeds bytes; calls on_frame for every complete valid frame.
    template <typename F> void feed(const std::uint8_t* data, std::size_t length, F&& on_frame) {
        std::size_t offset = 0;
        Frame frame;
        while (offset < length) {
            const std::size_t free_space = (kCapacity - 1U) - size();
            if (free_space == 0U) {
                tail_ = (tail_ + 1U) & kMask;
                continue;
            }
            const std::size_t chunk =
                (length - offset < free_space) ? (length - offset) : free_space;
            for (std::size_t i = 0; i < chunk; ++i)
                ring_[(head_ + i) & kMask] = data[offset + i];
            head_ = (head_ + chunk) & kMask;
            offset += chunk;
            while (next(&frame))
                on_frame(frame);
        }
        if (length == 0U) {
            while (next(&frame))
                on_frame(frame);
        }
    }
    // Called when the line has been idle: an incomplete frame is abandoned one byte at a time.
    template <typename F> void idle(F&& on_frame) {
        if (!pending())
            return;
        tail_ = (tail_ + 1U) & kMask;
        feed(nullptr, 0, on_frame);
    }
    [[nodiscard]] bool pending() const noexcept { return head_ != tail_; }

  private:
    [[nodiscard]] std::size_t size() const noexcept { return (head_ - tail_) & kMask; }
    [[nodiscard]] std::uint8_t at(std::size_t i) const noexcept {
        return ring_[(tail_ + i) & kMask];
    }
    bool next(Frame* out) noexcept;

    std::array<std::uint8_t, kCapacity> ring_{};
    std::size_t head_{0};
    std::size_t tail_{0};
};

[[nodiscard]] bool decode_pong(const Frame& frame, BoardInfo* out) noexcept;
std::size_t encode_frame(std::uint8_t type, const std::uint8_t* payload, std::size_t length,
                         std::uint8_t* out) noexcept;

// Little-endian u32 as the wire carries it.
[[nodiscard]] inline std::uint32_t get_u32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
inline void put_u32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
}

} // namespace mimusb::transport
