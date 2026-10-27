// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Wire protocol v3 client: framing, the line rate, acknowledged commands, and board events. It
 * knows nothing about HID; the layers above give it bytes. */
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
    std::uint16_t max_report_bytes{};
    std::uint16_t max_descriptor_bytes{};
    std::uint16_t max_feature_bytes{};
    std::uint16_t max_endpoint_bytes{};
    std::uint16_t max_frame_payload{};
    std::uint16_t max_control_data_bytes{};
    std::uint8_t max_control_rules{};
    bool has_unique_id{};
    std::uint8_t chip{};
    bool usb_high_speed{};
    std::uint8_t baud_index{}; // the rate the board is on, in kBaudRates
    std::uint16_t baud_mask{}; // bit i: the board can run at kBaudRates[i]
    std::uint8_t unique_id_length{};
    std::array<std::uint8_t, MIMHID_UNIQUE_ID_MAX> unique_id{};
};

inline constexpr std::array<std::uint32_t, MIMHID_BAUD_COUNT> kBaudRates = MIMHID_BAUD_RATES;

// The index of baud in kBaudRates, or -1.
[[nodiscard]] int baud_index_of(std::uint32_t baud) noexcept;

struct Frame {
    std::uint8_t type{};
    std::uint16_t length{};
    std::array<std::uint8_t, MIMHID_PROTO_MAX_PAYLOAD> payload; // not zeroed: length says what is in it
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

enum class Handshake : std::uint8_t {
    ok,
    no_board,     // nothing answered at any rate
    rate_refused, // the board cannot make the rate, or the port refused to be set to it
    rate_failed,  // both sides switched, but the pings did not come through
    io_error,
};

// Talks to a board on a port that has no reader thread: finds it, and agrees a line rate with
// it. The port starts at the boot rate, and every call leaves the port and the board on the same
// rate. wait_ms bounds each wait for an answer.
class Handshaker final {
  public:
    Handshaker(SerialPort& port, std::uint32_t wait_ms) noexcept : port_(port), wait_ms_(wait_ms) {}

    // Looks for the board at the boot rate, then at every rate of the table (a board left on a
    // fast rate by a host that went away). Leaves both on the boot rate, except for a board on
    // another protocol version (out->protocol), which is left where it answered.
    bool find(BoardInfo* out);
    // From the boot rate, switches to baud and checks that three pings each way come through.
    // On failure both are back on the boot rate.
    Handshake switch_to(std::uint32_t baud, BoardInfo* info);
    // From the boot rate, the fastest rate the board makes that switch_to settles; both are
    // left on it. The boot rate when no other works, 0 when the board stopped answering.
    std::uint32_t fastest(BoardInfo* info);
    // Back to the boot rate.
    bool to_boot(BoardInfo* info);
    // The rate the port is on.
    [[nodiscard]] std::uint32_t rate() const noexcept { return rate_; }

  private:
    bool ping(BoardInfo* out);
    CommandStatus set_board_baud(std::uint8_t index);
    Handshake try_rate(std::uint8_t index, BoardInfo* info);

    SerialPort& port_;
    std::uint32_t wait_ms_;
    std::uint32_t rate_{MIMHID_BOOT_BAUD};
    std::uint8_t seq_{0x80};
};

struct BoardEvent {
    std::uint8_t kind{};
    std::size_t length{};
    std::array<std::uint8_t, MIMHID_PROTO_MAX_PAYLOAD> data; // not zeroed: length says what is in it
};

class Link final {
  public:
    using EventHandler = std::function<void(const BoardEvent&)>;

    Link() = default;
    ~Link();
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    // Opens the port at the boot rate, for a Handshaker on port(), then start().
    OpenResult open(const char* path);
    [[nodiscard]] SerialPort& port() noexcept { return port_; }
    void start(EventHandler handler);
    void close();

    // Pings until the board answers or the timeout passes.
    bool ping(std::uint32_t timeout_ms, BoardInfo* out);

    // Sends a command frame (a sequence number is prepended) and waits for its ACK or NAK. An
    // attempt that heard nothing back is sent again unchanged, with the same sequence number, so
    // the board answers it again instead of running it twice (and a late answer still counts).
    CommandResult command(std::uint8_t type, const std::uint8_t* payload, std::size_t length,
                          std::uint32_t timeout_ms, int attempts);

    // Sends a commit command with reconnect delay and waits for ACK or NAK.
    CommandResult commit(std::uint16_t delay_ms, std::uint32_t timeout_ms);

    // Asserts physical D+ High (connect) and waits for ACK or NAK.
    CommandResult connect(std::uint32_t timeout_ms);

    // Asserts physical D+ Low (disconnect) and waits for ACK or NAK.
    CommandResult disconnect(std::uint32_t timeout_ms);

    // Sends a CONTROL_REPLY without waiting for the board's ACK or NAK: the board drops a reply it
    // no longer waits for. It only writes, so the event handler may call it.
    IoStatus send_control_reply(std::uint8_t token, std::uint8_t action, const std::uint8_t* data,
                                std::size_t length, std::uint32_t timeout_ms,
                                std::int32_t* os_error);

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
