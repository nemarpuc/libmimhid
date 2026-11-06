// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* Finding a board and agreeing the line rate with it, on a port that has no reader thread yet
 * (Link takes over once the rate is settled). See docs/PROTOCOL.md, "Line rate". */
#pragma once

#include "transport/link.hpp"

namespace mimhid::transport {

enum class Handshake : std::uint8_t {
    ok,
    no_board,     // nothing answered
    rate_refused, // the board cannot make the rate, or the port refused to be set to it
    rate_failed,  // both sides switched, but the link did not carry the test
    io_error,
};

// How hard a rate is tested: probe frames of probe_bytes, as many as fill line_ms of line time
// (at least min_frames).
struct LinkTest {
    std::uint32_t line_ms{};
    std::uint32_t min_frames{};
    std::uint32_t probe_bytes{};
};

// Talks to a board on a port that has no reader thread: finds it, and agrees a line rate with
// it. The port starts at the boot rate, and every call leaves the port and the board on the same
// rate. wait_ms bounds each wait for an answer.
class Handshaker final {
  public:
    Handshaker(SerialPort& port, std::uint32_t wait_ms) noexcept : port_(port), wait_ms_(wait_ms) {}

    // Looks for the board at the boot rate; a board a program left on a fast rate is brought back
    // with a BREAK. Leaves both on the boot rate, except for a board on another protocol version
    // (out->protocol), which is left where it answered.
    // hint_baud (0: none) is the rate the board may have been left on, for a port that cannot
    // send a BREAK.
    bool find(BoardInfo* out, std::uint32_t hint_baud = 0);
    // From the boot rate, moves to baud and tests it (pings, then a short burst of probe frames
    // whose arrival the board's counters confirm). On failure both are back on the boot rate.
    Handshake switch_to(std::uint32_t baud, BoardInfo* info);
    // From the boot rate, the fastest rate the link carries: a descending search from the
    // board's max_baud, bisected between the last failure and the first pass, and confirmed
    // by a longer burst. Both are left on it. The boot rate when no other works, 0 when the board
    // stopped answering.
    std::uint32_t fastest(BoardInfo* info);
    // Back to the boot rate.
    bool to_boot(BoardInfo* info);
    // The rate the port is on.
    [[nodiscard]] std::uint32_t rate() const noexcept { return rate_; }

  private:
    bool ping(BoardInfo* out);
    bool ping_legacy(BoardInfo* out);
    CommandStatus command(std::uint8_t type, const std::uint8_t* payload, std::size_t length,
                          std::uint8_t* data, std::uint8_t* data_length);
    bool recover(BoardInfo* info);
    bool run_test(const BoardInfo& board, const LinkTest& test);
    Handshake try_rate(std::uint32_t baud, const LinkTest& test, BoardInfo* info);

    SerialPort& port_;
    std::uint32_t wait_ms_;
    std::uint32_t rate_{MIMHID_BOOT_BAUD};
    std::uint32_t reply_extra_ms_{0}; // added to the wait for one answer
    std::uint8_t seq_{0x80};
};

} // namespace mimhid::transport
