// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* The running connection to a board: a reader thread, acknowledged commands, report frames and
 * board events (the frame format is frame.hpp; finding the board and the line rate is
 * handshaker.hpp). It knows nothing about HID; the layers above give it bytes. */
#pragma once

#include "mimhid_proto.h"
#include "transport/frame.hpp"
#include "transport/serial_port.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace mimhid::transport {

enum class CommandStatus : std::uint8_t { ack, nak, timeout, closed, io_error, short_write };

struct CommandResult {
    CommandStatus status{CommandStatus::closed};
    std::uint8_t nak_code{};
    std::int32_t os_error{};
    // What follows seq and type in the ACK (SET_BAUD: the rate; COUNTERS: the counters).
    std::uint8_t data_length{};
    std::array<std::uint8_t, 16> data{};

    [[nodiscard]] bool ok() const noexcept { return status == CommandStatus::ack; }
    operator bool() const noexcept { return status == CommandStatus::ack; }
};

// kind is a MIMHID_EV_* value, or for a packet of the target on an OUT endpoint the type of its
// frame, MIMHID_T_EP_OUT_BASE | the endpoint number (data holds the packet, length may be 0).
struct BoardEvent {
    std::uint8_t kind{};
    std::size_t length{};
    // Not zeroed: length says what is in it.
    std::array<std::uint8_t, MIMHID_PROTO_MAX_PAYLOAD> data;
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

    // Sends a command frame (a sequence number is prepended) and waits for its ACK or NAK. An
    // attempt that heard nothing back is sent again unchanged, with the same sequence number, so
    // the board answers it again instead of running it twice (and a late answer still counts).
    CommandResult command(std::uint8_t type, const std::uint8_t* payload, std::size_t length,
                          std::uint32_t timeout_ms, int attempts);

    // Reads the board's line counters, and zeroes them when reset is true.
    CommandResult counters(bool reset, std::uint32_t timeout_ms, LinkCounters* out);

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
    IoStatus send_report(std::uint8_t endpoint_address, const std::uint8_t* report,
                         std::size_t length, std::uint32_t stall_ms, std::int32_t* os_error);

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
    std::mutex state_mutex_; // guards the reply slot
    std::condition_variable state_cv_;
    std::uint8_t seq_{0};
    std::uint8_t pending_seq_{0};
    std::uint8_t pending_type_{0};
    bool reply_ready_{false};
    bool reply_nak_{false};
    std::uint8_t reply_code_{0};
    std::uint8_t reply_data_length_{0};
    std::array<std::uint8_t, 16> reply_data_{};
};

} // namespace mimhid::transport
