// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Wire protocol v2 client: framing, acknowledged commands, and board events. It knows nothing
 * about HID; the layers above give it bytes. */
#pragma once

#include "mimhid_proto.h"
#include "transport/serial_port.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace mimhid::transport {

struct BoardInfo {
    std::uint8_t version[3]{};
    bool ready{};
    std::uint8_t protocol{};
    std::uint8_t max_nodes{};
    std::uint8_t max_interfaces{};
    std::uint8_t max_report_bytes{};
    std::uint16_t max_descriptor_bytes{};
    bool has_unique_id{};
};

struct Frame {
    std::uint8_t type{};
    std::uint8_t length{};
    std::array<std::uint8_t, MIMHID_MAX_FRAME_PAYLOAD> payload{};
};

// Splits a byte stream into valid frames, skipping garbage one byte at a time.
class FrameParser final {
  public:
    static constexpr std::size_t kCapacity = 1024;
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
    [[nodiscard]] std::uint8_t at(std::size_t i) const noexcept { return ring_[(tail_ + i) & kMask]; }
    bool next(Frame* out) noexcept;

    std::array<std::uint8_t, kCapacity> ring_{};
    std::size_t head_{0};
    std::size_t tail_{0};
};

[[nodiscard]] bool decode_pong(const Frame& frame, BoardInfo* out) noexcept;
std::size_t encode_frame(std::uint8_t type, const std::uint8_t* payload, std::size_t length,
                         std::uint8_t* out) noexcept;

#ifndef MIMHID_EV_OUTPUT_REPORT
#define MIMHID_EV_OUTPUT_REPORT 8
#endif

enum class CommandStatus : std::uint8_t { ack, nak, timeout, closed, io_error, short_write };

struct CommandResult {
    CommandStatus status{CommandStatus::closed};
    std::uint8_t nak_code{};
    std::int32_t os_error{};

    [[nodiscard]] bool ok() const noexcept { return status == CommandStatus::ack; }
    operator bool() const noexcept { return status == CommandStatus::ack; }
};

// Pings the port without starting a reader; used for discovery.
bool probe_port(const char* path, std::uint32_t baud, std::uint32_t timeout_ms, BoardInfo* out);

struct BoardEvent {
    std::uint8_t kind{};
    std::size_t length{};
    std::array<std::uint8_t, 256> data{};
};

class Link final {
  public:
    using EventHandler = std::function<void(const BoardEvent&)>;

    Link() = default;
    ~Link();
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    OpenResult open(const char* path, std::uint32_t baud, EventHandler handler);
    void close();

    // Pings until the board answers or the timeout passes.
    bool ping(std::uint32_t timeout_ms, BoardInfo* out);

    // Sends a command frame (a sequence number is prepended) and waits for its ACK or NAK.
    CommandResult command(std::uint8_t type, const std::uint8_t* payload, std::size_t length,
                          std::uint32_t timeout_ms, int attempts);

    // Sends a commit command with reconnect delay and waits for ACK or NAK.
    CommandResult commit(std::uint16_t delay_ms, std::uint32_t timeout_ms = 2000);

    // Asserts physical D+ High (connect) and waits for ACK or NAK.
    CommandResult connect(std::uint32_t timeout_ms = 2000);

    // Asserts physical D+ Low (disconnect) and waits for ACK or NAK.
    CommandResult disconnect(std::uint32_t timeout_ms = 2000);

    // Sends a report frame for a node; it is not acknowledged.
    IoStatus send_report(std::uint8_t node, const std::uint8_t* report, std::size_t length,
                         std::uint32_t stall_ms, std::int32_t* os_error);

    [[nodiscard]] bool ready() const noexcept { return ready_.load(std::memory_order_acquire); }
    void clear_ready() noexcept { ready_.store(false, std::memory_order_release); }
    bool wait_ready(std::uint32_t timeout_ms);
    [[nodiscard]] bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }
    void drain() noexcept { port_.drain(); }

  private:
    void reader_loop();
    void handle_frame(const Frame& frame);

    SerialPort port_;
    EventHandler handler_;
    std::thread reader_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> ready_{false};

    std::mutex tx_mutex_;    // one frame on the wire at a time
    std::mutex cmd_mutex_;   // one command in flight
    std::mutex state_mutex_; // guards the reply slot and the pong
    std::condition_variable state_cv_;
    std::uint8_t seq_{0};
    std::uint8_t pending_seq_{0};
    std::uint8_t pending_type_{0};
    bool reply_ready_{false};
    bool reply_nak_{false};
    std::uint8_t reply_code_{0};
    bool pong_ready_{false};
    BoardInfo pong_{};
};

} // namespace mimhid::transport
