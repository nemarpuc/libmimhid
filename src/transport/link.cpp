// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#include "transport/link.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

namespace mimhid::transport {

Link::~Link() { close(); }

OpenResult Link::open(const char* path) { return port_.open(path, MIMHID_BOOT_BAUD); }

void Link::start(EventHandler handler) {
    handler_ = std::move(handler);
    stop_.store(false);
    failed_.store(false);
    ready_.store(false);
    reader_ = std::thread([this] { reader_loop(); });
}

void Link::close() {
    if (reader_.joinable()) {
        stop_.store(true, std::memory_order_release);
        reader_.join();
    }
    port_.close();
}

void Link::reader_loop() {
    FrameParser parser;
    std::uint8_t buf[4096];
    while (!stop_.load(std::memory_order_acquire)) {
        // A partial frame is given up after the same quiet time the board allows itself.
        const long got = port_.read(buf, sizeof(buf), parser.pending() ? 10 : 50);
        if (got < 0) {
            failed_.store(true, std::memory_order_release);
            ready_.store(false, std::memory_order_release);
            std::lock_guard<std::mutex> lock(state_mutex_);
            state_cv_.notify_all();
            return;
        }
        auto dispatch = [this](const Frame& f) { handle_frame(f); };
        if (got == 0) {
            if (parser.pending())
                parser.idle(dispatch);
            continue;
        }
        parser.feed(buf, static_cast<std::size_t>(got), dispatch);
    }
}

void Link::handle_frame(const Frame& frame) {
    switch (frame.type) {
    case MIMHID_T_ACK:
    case MIMHID_T_NAK: {
        const std::size_t need = frame.type == MIMHID_T_ACK ? 2U : 3U;
        if (frame.length < need)
            break;
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (frame.payload[0] == pending_seq_ && frame.payload[1] == pending_type_ &&
            !reply_ready_) {
            reply_ready_ = true;
            reply_nak_ = frame.type == MIMHID_T_NAK;
            reply_code_ = reply_nak_ ? frame.payload[2] : 0;
            reply_data_length_ = 0;
            if (!reply_nak_) {
                reply_data_length_ = static_cast<std::uint8_t>(
                    std::min<std::size_t>(frame.length - 2U, reply_data_.size()));
                std::memcpy(reply_data_.data(), &frame.payload[2], reply_data_length_);
            }
            state_cv_.notify_all();
        }
        break;
    }
    case MIMHID_T_EVENT: {
        if (frame.length < 1)
            break;
        BoardEvent ev;
        ev.kind = frame.payload[0];
        ev.length = static_cast<std::size_t>(frame.length - 1);
        if (ev.length > 0) {
            std::memcpy(ev.data.data(), &frame.payload[1], ev.length);
        }
        if (ev.kind == MIMHID_EV_MOUNTED) {
            ready_.store(true, std::memory_order_release);
            std::lock_guard<std::mutex> lock(state_mutex_);
            state_cv_.notify_all();
        } else if (ev.kind == MIMHID_EV_UNMOUNTED) {
            ready_.store(false, std::memory_order_release);
        }
        if (handler_)
            handler_(ev);
        break;
    }
    default: {
        // A packet of the target on an OUT endpoint, as it came; not acknowledged.
        if (frame.type > MIMHID_T_EP_OUT_BASE && frame.type <= MIMHID_T_EP_OUT_LAST) {
            BoardEvent ev;
            ev.kind = frame.type;
            ev.length = frame.length;
            if (ev.length > 0)
                std::memcpy(ev.data.data(), frame.payload.data(), ev.length);
            if (handler_)
                handler_(ev);
        }
        break;
    }
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
CommandResult Link::command(std::uint8_t type, const std::uint8_t* payload, std::size_t length,
                            std::uint32_t timeout_ms, int attempts) {
    CommandResult result;
    if (length + 1U > MIMHID_PROTO_MAX_PAYLOAD) {
        result.status = CommandStatus::io_error;
        return result;
    }
    std::lock_guard<std::mutex> cmd(cmd_mutex_);
    std::uint8_t body[MIMHID_PROTO_MAX_PAYLOAD];
    std::uint8_t frame[6 + MIMHID_PROTO_MAX_PAYLOAD];
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        pending_seq_ = ++seq_;
        pending_type_ = type;
        reply_ready_ = false;
    }
    body[0] = pending_seq_;
    if (length != 0)
        std::memcpy(body + 1, payload, length);
    const std::size_t n = encode_frame(type, body, length + 1U, frame);
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (failed()) {
            result.status = CommandStatus::closed;
            return result;
        }
        {
            std::lock_guard<std::mutex> tx(tx_mutex_);
            const IoStatus io = port_.write_all(frame, n, timeout_ms, &result.os_error);
            if (io != IoStatus::ok) {
                result.status = io == IoStatus::short_write ? CommandStatus::short_write
                                : io == IoStatus::closed    ? CommandStatus::closed
                                : io == IoStatus::timeout   ? CommandStatus::timeout
                                                            : CommandStatus::io_error;
                return result;
            }
        }
        std::unique_lock<std::mutex> lock(state_mutex_);
        state_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                           [&] { return reply_ready_ || failed(); });
        if (reply_ready_) {
            result.status = reply_nak_ ? CommandStatus::nak : CommandStatus::ack;
            result.nak_code = reply_code_;
            result.data_length = reply_data_length_;
            result.data = reply_data_;
            return result;
        }
        if (failed()) {
            result.status = CommandStatus::closed;
            return result;
        }
        result.status = CommandStatus::timeout;
    }
    return result;
}

CommandResult Link::counters(bool reset, std::uint32_t timeout_ms, LinkCounters* out) {
    const std::uint8_t flags[1] = {static_cast<std::uint8_t>(reset ? MIMHID_COUNTERS_F_RESET : 0)};
    CommandResult result = command(MIMHID_T_COUNTERS, flags, 1, timeout_ms, 1);
    if (result.ok() && !decode_counters(result.data.data(), result.data_length, out))
        result.status = CommandStatus::io_error;
    return result;
}

CommandResult Link::connect(std::uint32_t timeout_ms) {
    const std::uint8_t payload[3] = {
        0, // detach: 0
        0, // delay_ms_low: 0
        0  // delay_ms_high: 0
    };
    return command(MIMHID_T_COMMIT, payload, sizeof(payload), timeout_ms, 1);
}

CommandResult Link::disconnect(std::uint32_t timeout_ms) {
    clear_ready();
    const std::uint8_t payload[3] = {1, // detach: 1
                                     0, 0};
    return command(MIMHID_T_COMMIT, payload, sizeof(payload), timeout_ms, 1);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
IoStatus Link::send_report(std::uint8_t endpoint_address, const std::uint8_t* report,
                           std::size_t length, std::uint32_t stall_ms, std::int32_t* os_error) {
    if (failed())
        return IoStatus::closed;
    if (length > MIMHID_PROTO_MAX_PAYLOAD)
        return IoStatus::error;
    std::uint8_t frame[6 + MIMHID_PROTO_MAX_PAYLOAD];
    const std::size_t n =
        encode_frame(static_cast<std::uint8_t>(MIMHID_T_EP_IN_BASE | (endpoint_address & 0x0F)),
                     report, length, frame);
    std::lock_guard<std::mutex> tx(tx_mutex_);
    return port_.write_all(frame, n, stall_ms, os_error);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
IoStatus Link::send_control_reply(std::uint8_t token, std::uint8_t action, const std::uint8_t* data,
                                  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                                  std::size_t length, std::uint32_t timeout_ms,
                                  std::int32_t* os_error) {
    if (failed())
        return IoStatus::closed;
    if (length + 3U > MIMHID_PROTO_MAX_PAYLOAD)
        return IoStatus::error;
    std::uint8_t body[3 + MIMHID_PROTO_MAX_PAYLOAD];
    std::uint8_t frame[6 + 3 + MIMHID_PROTO_MAX_PAYLOAD];
    body[0] = 0; // a sequence number nobody waits for
    body[1] = token;
    body[2] = action;
    if (length != 0)
        std::memcpy(body + 3, data, length);
    const std::size_t n = encode_frame(MIMHID_T_CONTROL_REPLY, body, length + 3U, frame);
    std::lock_guard<std::mutex> tx(tx_mutex_);
    return port_.write_all(frame, n, timeout_ms, os_error);
}

bool Link::wait_ready(std::uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(state_mutex_);
    state_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                       [&] { return ready() || failed(); });
    return ready();
}

} // namespace mimhid::transport
