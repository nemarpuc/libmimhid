// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
// Finding a board and agreeing the line rate with it (see handshaker.hpp).
#include "transport/handshaker.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

namespace mimhid::transport {

namespace {
constexpr std::uint32_t kBreakMs = 10;
constexpr LinkTest kScreen{0, 32, 96};
constexpr LinkTest kConfirm{250, 32, 128};
} // namespace

namespace {

// Reads frames until on_frame returns true or wait_ms passes.
template <typename F> bool read_until(SerialPort& port, std::uint32_t wait_ms, F&& on_frame) {
    FrameParser parser;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    bool done = false;
    std::uint8_t buf[64];
    auto take = [&](const Frame& f) { done = done || on_frame(f); };
    while (!done && std::chrono::steady_clock::now() < deadline) {
        const long got = port.read(buf, sizeof(buf), 5);
        if (got < 0)
            return false;
        if (got == 0)
            parser.idle(take);
        else
            parser.feed(buf, static_cast<std::size_t>(got), take);
    }
    return done;
}

} // namespace

bool Handshaker::ping(BoardInfo* out) {
    std::uint8_t frame[8];
    const std::size_t n = encode_frame(MIMHID_T_PING, nullptr, 0, frame);
    port_.discard_input();
    if (port_.write_all(frame, n, wait_ms_, nullptr) != IoStatus::ok)
        return false;
    return read_until(port_, wait_ms_, [&](const Frame& f) { return decode_pong(f, out); });
}

CommandStatus Handshaker::command(std::uint8_t type, const std::uint8_t* payload,
                                  std::size_t length, std::uint8_t* data,
                                  std::uint8_t* data_length) {
    const std::uint8_t seq = ++seq_;
    std::uint8_t body[16];
    std::uint8_t frame[32];
    body[0] = seq;
    std::memcpy(body + 1, payload, length);
    const std::size_t n = encode_frame(type, body, length + 1U, frame);
    port_.discard_input();
    if (port_.write_all(frame, n, wait_ms_, nullptr) != IoStatus::ok)
        return CommandStatus::io_error;
    CommandStatus status = CommandStatus::timeout;
    if (data_length != nullptr)
        *data_length = 0;
    read_until(port_, wait_ms_ + reply_extra_ms_, [&](const Frame& f) {
        if ((f.type != MIMHID_T_ACK && f.type != MIMHID_T_NAK) || f.length < 2 ||
            f.payload[0] != seq || f.payload[1] != type)
            return false;
        status = f.type == MIMHID_T_ACK ? CommandStatus::ack : CommandStatus::nak;
        if (status == CommandStatus::ack && data != nullptr && data_length != nullptr) {
            const std::size_t got = std::min<std::size_t>(f.length - 2U, 16U);
            std::memcpy(data, &f.payload[2], got);
            *data_length = static_cast<std::uint8_t>(got);
        }
        return true;
    });
    return status;
}

// Back to the boot rate by BREAK, which the board takes whatever rate it is on. The board also
// falls back by itself when the rate it was moved to is not confirmed in time, so the wait runs
// out that window before it gives up.
bool Handshaker::recover(BoardInfo* info) {
    rate_ = MIMHID_BOOT_BAUD;
    if (!port_.set_baud(MIMHID_BOOT_BAUD))
        return false;
    (void)port_.send_break(kBreakMs);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(MIMHID_BAUD_CONFIRM_MS + 30);
    do {
        if (ping(info))
            return true;
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

bool Handshaker::to_boot(BoardInfo* info) {
    if (rate_ == MIMHID_BOOT_BAUD)
        return ping(info);
    std::uint8_t boot[4];
    put_u32(boot, MIMHID_BOOT_BAUD);
    const bool asked = command(MIMHID_T_SET_BAUD, boot, 4, nullptr, nullptr) == CommandStatus::ack;
    if (asked && port_.set_baud(MIMHID_BOOT_BAUD)) {
        rate_ = MIMHID_BOOT_BAUD;
        if (ping(info))
            return true;
    }
    return recover(info);
}

bool Handshaker::find(BoardInfo* out, std::uint32_t hint_baud) {
    if (ping(out))
        return true;
    // Silent at the boot rate: a program may have left the board on a fast one.
    if (port_.send_break(kBreakMs) && ping(out))
        return true;
    // The port cannot send a BREAK (or the board did not take it): the caller may know the rate
    // the board was last on. Ask it back to the boot rate there.
    if (hint_baud > MIMHID_BOOT_BAUD && port_.set_baud(hint_baud)) {
        rate_ = hint_baud;
        if (ping(out)) {
            if (!out->compatible)
                return true;
            return to_boot(out);
        }
        rate_ = MIMHID_BOOT_BAUD;
        port_.set_baud(MIMHID_BOOT_BAUD);
    }
    return false;
}

bool Handshaker::run_test(const BoardInfo& board, const LinkTest& test) {
    LinkCounters counters;
    std::uint8_t data[16];
    std::uint8_t data_length = 0;
    const std::uint8_t reset[1] = {MIMHID_COUNTERS_F_RESET};
    if (command(MIMHID_T_COUNTERS, reset, 1, data, &data_length) != CommandStatus::ack)
        return false;

    const std::size_t probe_bytes =
        std::min<std::size_t>(test.probe_bytes, board.max_frame_payload);
    if (probe_bytes == 0U)
        return false;
    std::uint8_t pattern[MIMHID_PROTO_MAX_PAYLOAD];
    for (std::size_t i = 0; i < probe_bytes; ++i)
        pattern[i] = MIMHID_PROBE_BYTE(i);
    std::uint8_t one[8 + MIMHID_PROTO_MAX_PAYLOAD];
    const std::size_t frame_bytes = encode_frame(MIMHID_T_PROBE, pattern, probe_bytes, one);

    // As many frames as fill the test's line time, within what a 16-bit counter holds.
    std::uint64_t frames =
        static_cast<std::uint64_t>(test.line_ms) * rate_ / (10000U * frame_bytes);
    frames = std::max<std::uint64_t>(frames, test.min_frames);
    frames = std::min<std::uint64_t>(frames, 60000U);

    constexpr std::size_t kBatch = 32;
    std::vector<std::uint8_t> batch(kBatch * frame_bytes);
    for (std::size_t i = 0; i < kBatch; ++i)
        std::memcpy(&batch[i * frame_bytes], one, frame_bytes);
    const std::uint32_t stall = std::max<std::uint32_t>(wait_ms_, 200U);
    for (std::uint64_t left = frames; left != 0U;) {
        const std::size_t now = static_cast<std::size_t>(std::min<std::uint64_t>(left, kBatch));
        if (port_.write_all(batch.data(), now * frame_bytes, stall, nullptr) != IoStatus::ok)
            return false;
        left -= now;
    }
    // Written is not sent: the adapter may still hold up to a driver buffer of it, and the board
    // reads at the line rate. The answer waits for the rest of the burst.
    reply_extra_ms_ = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(10000U, 100U + frames * frame_bytes * 10000U / rate_));
    const std::uint8_t keep[1] = {0};
    const CommandStatus got = command(MIMHID_T_COUNTERS, keep, 1, data, &data_length);
    reply_extra_ms_ = 0;
    return got == CommandStatus::ack && decode_counters(data, data_length, &counters) &&
           counters.probes_good == frames && counters.errors() == 0U;
}

Handshake Handshaker::try_rate(std::uint32_t baud, const LinkTest& test, BoardInfo* info) {
    // A rate the port cannot be set to is not asked of the board.
    if (!port_.set_baud(baud)) {
        port_.set_baud(MIMHID_BOOT_BAUD);
        return Handshake::rate_refused;
    }
    if (!port_.set_baud(MIMHID_BOOT_BAUD))
        return Handshake::io_error;
    std::uint8_t ask[4];
    put_u32(ask, baud);
    std::uint8_t data[16];
    std::uint8_t data_length = 0;
    const CommandStatus asked = command(MIMHID_T_SET_BAUD, ask, 4, data, &data_length);
    if (asked == CommandStatus::nak)
        return Handshake::rate_refused;
    if (asked != CommandStatus::ack || data_length < 4U)
        return Handshake::io_error;
    if (!port_.set_baud(baud)) {
        rate_ = baud;
        return recover(info) ? Handshake::rate_refused : Handshake::io_error;
    }
    rate_ = baud;
    BoardInfo seen;
    bool carried = ping(&seen) && seen.baud == baud && ping(&seen) && ping(&seen);
    carried = carried && run_test(seen, test);
    if (carried) {
        *info = seen;
        return Handshake::ok;
    }
    return recover(info) ? Handshake::rate_failed : Handshake::io_error;
}

Handshake Handshaker::switch_to(std::uint32_t baud, BoardInfo* info) {
    if (baud == 0U || baud > info->max_baud)
        return Handshake::rate_refused;
    return baud == MIMHID_BOOT_BAUD ? Handshake::ok : try_rate(baud, kScreen, info);
}

std::uint32_t Handshaker::fastest(BoardInfo* info) {
    const std::uint32_t floor = MIMHID_BOOT_BAUD + MIMHID_BOOT_BAUD / 20U;
    std::uint32_t pass = 0;
    std::uint32_t fail = 0;
    // Coarse: down from the board's fastest rate in steps of 20 % to the first rate that carries.
    for (std::uint32_t r = info->max_baud; r > floor; r = r / 5U * 4U) {
        const Handshake h = try_rate(r, kScreen, info);
        if (h == Handshake::ok) {
            pass = r;
            break;
        }
        if (h == Handshake::io_error)
            return 0U;
        fail = r;
    }
    if (pass == 0U)
        return MIMHID_BOOT_BAUD;
    // Fine: between the last failure and the first pass, to 2 %.
    while (fail != 0U &&
           static_cast<std::uint64_t>(fail) * 100U > static_cast<std::uint64_t>(pass) * 102U) {
        if (!to_boot(info))
            return 0U;
        const std::uint32_t mid = pass + (fail - pass) / 2U;
        const Handshake h = try_rate(mid, kScreen, info);
        if (h == Handshake::ok)
            pass = mid;
        else if (h == Handshake::io_error)
            return 0U;
        else
            fail = mid;
    }
    // The rate must hold a longer burst; if not, 5 % lower, until one does.
    for (int step = 0; step < 8 && pass > floor; ++step) {
        if (rate_ != MIMHID_BOOT_BAUD && !to_boot(info))
            return 0U;
        const Handshake h = try_rate(pass, kConfirm, info);
        if (h == Handshake::ok)
            return pass;
        if (h == Handshake::io_error)
            return 0U;
        pass = pass / 20U * 19U;
    }
    return MIMHID_BOOT_BAUD;
}

} // namespace mimhid::transport
