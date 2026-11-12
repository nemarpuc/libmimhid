// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* A board played by the test, on the master end of a pseudo-terminal. It speaks the serial
 * protocol of mimhid_proto.h well enough to be opened and configured, and every answer can be
 * replaced: a NAK with any code, silence, a short or malformed frame, a reader that stops reading,
 * or a cable that is pulled. The firmware core (tests/e2e) shows what a real board does; this
 * shows what the library does when a board, an adapter or a line misbehaves. Test-only. */
#pragma once

#include "mimhid.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace scripted {

// A frame the board received.
struct Received {
    std::uint8_t type{};
    std::vector<std::uint8_t> payload;
};

// What the board answers to a command frame.
struct Reply {
    enum class Kind : std::uint8_t { ack, nak, silent };
    Kind kind{Kind::ack};
    std::uint8_t nak_code{};
    std::vector<std::uint8_t> data; // after seq and type in an ACK

    static Reply ack(std::vector<std::uint8_t> data = {}) {
        return {Kind::ack, 0, std::move(data)};
    }
    static Reply nak(std::uint8_t code) { return {Kind::nak, code, {}}; }
    static Reply silent() { return {Kind::silent, 0, {}}; }
};

// Called for every command frame (one with a sequence number) before the board's own answer;
// nullopt keeps the board's own.
using Policy = std::function<std::optional<Reply>(const Received&)>;

// What the PONG says. The baud field is the board's current rate and is filled in by the board.
struct Pong {
    std::uint8_t version[3]{MIMHID_VERSION_MAJOR, MIMHID_VERSION_MINOR, MIMHID_VERSION_PATCH};
    std::uint8_t ready{1};
    std::uint8_t reserved{0}; // byte 4 of the PONG: reserved, zero (as are bytes 5, 21..22, 25..26)
    std::uint8_t flags{0x01}; // MIMHID_PONG_FLAG_UNIQUE_ID
    std::uint8_t chip{0};
    std::uint8_t high_speed{0};
    std::uint32_t max_baud{3000000};
    std::uint32_t uart_flags{0x01};
    std::uint16_t max_report_bytes{64};
    std::uint16_t max_endpoint_bytes{64};
    std::uint16_t max_frame_payload{1024};
    std::uint16_t max_control_data_bytes{256};
    std::uint8_t max_control_rules{64};
    std::uint8_t max_endpoint_number{7};
    std::uint8_t max_interfaces_any{15};
    std::uint16_t max_config_descriptor_bytes{1024};
    std::uint8_t max_strings{24};
    std::uint16_t max_transfer_bytes{64};
    std::uint16_t endpoint_queue_slots{64};
    std::uint8_t transfer_types{0x0C}; // bulk and interrupt
    std::uint16_t max_answer_bytes{4096};
    std::vector<std::uint8_t> unique_id{1, 2, 3, 4, 5, 6, 7, 8};
    // When set, sent as the whole PONG payload instead of the fields above.
    std::optional<std::vector<std::uint8_t>> raw;
};

class Board final {
  public:
    Board();
    ~Board();
    Board(const Board&) = delete;
    Board& operator=(const Board&) = delete;

    // Opens the pseudo-terminal and starts answering. False when no pseudo-terminal is available.
    bool start();
    void stop();
    // The path the library opens.
    [[nodiscard]] const char* path() const noexcept { return slave_.c_str(); }
    // The board end of the cable disappears: the master is closed.
    void hang_up();

    void set_policy(Policy policy);
    void set_pong(const Pong& pong);
    void set_answer_pings(bool answer) { answer_pings_.store(answer); }
    // Whether the link carries a rate; a rate it does not carry loses probe frames.
    void set_carries(std::function<bool(std::uint32_t)> carries);
    // While not reading, what the library writes waits in the pseudo-terminal until it is full.
    // Returns once the board has stopped reading.
    void set_reading(bool reading);
    // With the line check on, bytes pass only while the library's end of the pseudo-terminal is
    // set to the board's rate, as on a real UART (off by default: any rate passes).
    void set_line_check(bool check) { line_check_.store(check); }
    // The board's rate, as if a program had left it there.
    void set_baud(std::uint32_t baud) { baud_.store(baud); }
    // What a BREAK does to a board: back to the boot rate.
    void take_break() { baud_.store(115200); }
    // A MOUNTED event follows every COMMIT that attaches (true by default).
    void set_mount_on_commit(bool mount) { mount_on_commit_.store(mount); }

    // Sends one frame, or raw bytes, to the library.
    void send_frame(std::uint8_t type, const std::uint8_t* payload, std::size_t length);
    void send_event(std::uint8_t kind, std::initializer_list<std::uint8_t> data);
    void send_bytes(const std::uint8_t* data, std::size_t length);

    // The frames received so far, oldest first.
    std::vector<Received> received();
    std::size_t count(std::uint8_t type);
    // Waits for the first received frame of this type that no earlier call took.
    bool take(std::uint8_t type, Received* out, int timeout_ms = 2000);
    void clear();
    [[nodiscard]] std::uint32_t baud() const noexcept { return baud_.load(); }

  private:
    void run();
    void handle(const Received& frame);
    std::vector<std::uint8_t> pong_payload();

    int master_{-1};
    std::string slave_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> answer_pings_{true};
    std::atomic<bool> reading_{true};
    std::atomic<bool> paused_{false};
    std::atomic<bool> mount_on_commit_{true};
    std::atomic<bool> line_check_{false};
    std::atomic<std::uint32_t> baud_{115200};
    std::mutex write_mutex_;

    std::mutex mutex_; // guards everything below
    std::condition_variable cv_;
    Policy policy_;
    Pong pong_;
    std::function<bool(std::uint32_t)> carries_;
    std::vector<Received> received_;
    std::vector<bool> taken_;
    std::uint32_t probes_{0};
    bool probe_error_{false};
};

} // namespace scripted
