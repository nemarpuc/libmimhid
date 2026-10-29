// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
#include "transport/link.hpp"

#include <chrono>
#include <cstring>
#include <thread>

namespace mimhid::transport {

std::size_t encode_frame(std::uint8_t type, const std::uint8_t* payload, std::size_t length,
                         std::uint8_t* out) noexcept {
    std::size_t n = 0;
    std::uint8_t x = type;
    out[n++] = MIMHID_SYNC;
    out[n++] = type;
    if (length < 0x80U) {
        out[n++] = static_cast<std::uint8_t>(length);
        x = static_cast<std::uint8_t>(x ^ length);
    } else {
        const auto low = static_cast<std::uint8_t>(0x80U | (length & 0x7FU));
        const auto high = static_cast<std::uint8_t>(length >> 7);
        out[n++] = low;
        out[n++] = high;
        x = static_cast<std::uint8_t>(x ^ low ^ high);
    }
    for (std::size_t i = 0; i < length; i++) {
        out[n++] = payload[i];
        x = static_cast<std::uint8_t>(x ^ payload[i]);
    }
    out[n++] = x;
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
                                type == MIMHID_T_NAK || type == MIMHID_T_EVENT ||
                                type == MIMHID_EV_OUTPUT_REPORT || type == MIMHID_EV_SET_REPORT;
        if (!board_type) {
            tail_ = (tail_ + 1U) & kMask;
            continue;
        }
        if (len > MIMHID_PROTO_MAX_PAYLOAD) {
            tail_ = (tail_ + 1U) & kMask;
            continue;
        }
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
}

int baud_index_of(std::uint32_t baud) noexcept {
    for (std::size_t i = 0; i < kBaudRates.size(); ++i) {
        if (kBaudRates[i] == baud)
            return static_cast<int>(i);
    }
    return -1;
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
    const std::uint8_t id_length = frame.payload[26];
    if (id_length > MIMHID_UNIQUE_ID_MAX || frame.length < MIMHID_PONG_MIN_LEN + id_length ||
        frame.payload[10] >= MIMHID_BAUD_COUNT)
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
    out->baud_index = p[10];
    out->baud_mask = static_cast<std::uint16_t>(p[11] | (p[12] << 8));
    out->max_descriptor_bytes = static_cast<std::uint16_t>(p[13] | (p[14] << 8));
    out->max_report_bytes = static_cast<std::uint16_t>(p[15] | (p[16] << 8));
    out->max_feature_bytes = static_cast<std::uint16_t>(p[17] | (p[18] << 8));
    out->max_endpoint_bytes = static_cast<std::uint16_t>(p[19] | (p[20] << 8));
    out->max_frame_payload = static_cast<std::uint16_t>(p[21] | (p[22] << 8));
    out->max_control_data_bytes = static_cast<std::uint16_t>(p[23] | (p[24] << 8));
    out->max_control_rules = p[25];
    out->unique_id_length = id_length;
    std::memcpy(out->unique_id.data(), &p[27], id_length);
    return true;
}

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
    const std::uint8_t payload[1] = {MIMHID_PROTO_VERSION};
    std::uint8_t frame[8];
    const std::size_t n = encode_frame(MIMHID_T_PING, payload, 1, frame);
    port_.discard_input();
    if (port_.write_all(frame, n, wait_ms_, nullptr) != IoStatus::ok)
        return false;
    return read_until(port_, wait_ms_, [&](const Frame& f) { return decode_pong(f, out); });
}

CommandStatus Handshaker::set_board_baud(std::uint8_t index) {
    const std::uint8_t seq = ++seq_;
    const std::uint8_t payload[2] = {seq, index};
    std::uint8_t frame[8];
    const std::size_t n = encode_frame(MIMHID_T_SET_BAUD, payload, 2, frame);
    port_.discard_input();
    if (port_.write_all(frame, n, wait_ms_, nullptr) != IoStatus::ok)
        return CommandStatus::io_error;
    CommandStatus status = CommandStatus::timeout;
    read_until(port_, wait_ms_, [&](const Frame& f) {
        if ((f.type != MIMHID_T_ACK && f.type != MIMHID_T_NAK) || f.length < 2 ||
            f.payload[0] != seq || f.payload[1] != MIMHID_T_SET_BAUD)
            return false;
        status = f.type == MIMHID_T_ACK ? CommandStatus::ack : CommandStatus::nak;
        return true;
    });
    return status;
}

bool Handshaker::to_boot(BoardInfo* info) {
    if (rate_ != MIMHID_BOOT_BAUD) {
        if (set_board_baud(0) != CommandStatus::ack)
            return false;
        if (!port_.set_baud(MIMHID_BOOT_BAUD))
            return false;
        rate_ = MIMHID_BOOT_BAUD;
    }
    return ping(info);
}

bool Handshaker::find(BoardInfo* out) {
    if (ping(out))
        return true;
    for (std::size_t i = kBaudRates.size() - 1; i > 0; --i) {
        if (!port_.set_baud(kBaudRates[i]))
            continue;
        rate_ = kBaudRates[i];
        if (ping(out)) {
            // A board on another protocol cannot be asked to change its rate; it stays found
            // where it answered.
            return out->protocol != MIMHID_PROTO_VERSION || to_boot(out);
        }
    }
    rate_ = MIMHID_BOOT_BAUD;
    port_.set_baud(MIMHID_BOOT_BAUD);
    return false;
}

Handshake Handshaker::try_rate(std::uint8_t index, BoardInfo* info) {
    const std::uint32_t baud = kBaudRates[index];
    // A rate the port cannot be set to is not asked of the board.
    if (!port_.set_baud(baud)) {
        port_.set_baud(MIMHID_BOOT_BAUD);
        return Handshake::rate_refused;
    }
    if (!port_.set_baud(MIMHID_BOOT_BAUD))
        return Handshake::io_error;
    const CommandStatus asked = set_board_baud(index);
    if (asked == CommandStatus::nak)
        return Handshake::rate_refused;
    if (asked != CommandStatus::ack)
        return Handshake::io_error;
    const auto switched = std::chrono::steady_clock::now();
    if (!port_.set_baud(baud))
        return Handshake::io_error;
    rate_ = baud;
    BoardInfo seen;
    bool carried = true;
    for (int i = 0; i < 3 && carried; ++i)
        carried = ping(&seen) && seen.baud_index == index;
    if (carried) {
        *info = seen;
        return Handshake::ok;
    }
    // The board goes back to the boot rate unless one of the pings reached it; the search
    // finds it either way.
    rate_ = MIMHID_BOOT_BAUD;
    if (!port_.set_baud(MIMHID_BOOT_BAUD))
        return Handshake::io_error;
    std::this_thread::sleep_until(switched +
                                  std::chrono::milliseconds(MIMHID_BAUD_CONFIRM_MS + 20));
    if (!ping(info) && !find(info))
        return Handshake::io_error;
    return Handshake::rate_failed;
}

Handshake Handshaker::switch_to(std::uint32_t baud, BoardInfo* info) {
    const int index = baud_index_of(baud);
    if (index < 0 || !(info->baud_mask & (1U << index)))
        return Handshake::rate_refused;
    return index == 0 ? Handshake::ok : try_rate(static_cast<std::uint8_t>(index), info);
}

std::uint32_t Handshaker::fastest(BoardInfo* info) {
    for (std::size_t i = kBaudRates.size() - 1; i > 0; --i) {
        if (!(info->baud_mask & (1U << i)) || !SerialPort::baud_supported(kBaudRates[i]))
            continue;
        const Handshake result = try_rate(static_cast<std::uint8_t>(i), info);
        if (result == Handshake::ok)
            return kBaudRates[i];
        if (result == Handshake::io_error)
            return 0U;
    }
    return MIMHID_BOOT_BAUD;
}

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
    case MIMHID_T_PONG: {
        BoardInfo info;
        if (decode_pong(frame, &info)) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            pong_ = info;
            pong_ready_ = true;
            state_cv_.notify_all();
        }
        break;
    }
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
        } else if (ev.kind == MIMHID_EV_OUTPUT_REPORT) {
            if (ev.length < 3)
                break;
        } else if (ev.kind == MIMHID_EV_SET_REPORT) {
            if (ev.length < 3)
                break;
        }
        if (handler_)
            handler_(ev);
        break;
    }
    case MIMHID_EV_OUTPUT_REPORT: {
        if (frame.length < 2)
            break;
        BoardEvent ev;
        ev.kind = MIMHID_EV_OUTPUT_REPORT;
        ev.length = frame.length;
        std::memcpy(ev.data.data(), frame.payload.data(), ev.length);
        if (handler_)
            handler_(ev);
        break;
    }
    case MIMHID_EV_SET_REPORT: {
        if (frame.length < 3)
            break;
        BoardEvent ev;
        ev.kind = MIMHID_EV_SET_REPORT;
        ev.length = frame.length;
        std::memcpy(ev.data.data(), frame.payload.data(), ev.length);
        if (handler_)
            handler_(ev);
        break;
    }
    default:
        break;
    }
}

bool Link::ping(std::uint32_t timeout_ms, BoardInfo* out) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::uint8_t payload[1] = {MIMHID_PROTO_VERSION};
    std::uint8_t frame[8];
    const std::size_t n = encode_frame(MIMHID_T_PING, payload, 1, frame);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            pong_ready_ = false;
        }
        std::lock_guard<std::mutex> tx(tx_mutex_);
        if (port_.write_all(frame, n, 100, nullptr) != IoStatus::ok)
            return false;
        std::unique_lock<std::mutex> lock(state_mutex_);
        const auto step = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        state_cv_.wait_until(lock, step < deadline ? step : deadline,
                             [&] { return pong_ready_ || failed(); });
        if (pong_ready_) {
            *out = pong_;
            return true;
        }
        if (failed())
            return false;
    }
    return false;
}

CommandResult Link::command(std::uint8_t type, const std::uint8_t* payload, std::size_t length,
                            std::uint32_t timeout_ms, int attempts) {
    CommandResult result;
    if (length + 1U > MIMHID_PROTO_MAX_PAYLOAD) {
        result.status = CommandStatus::io_error;
        return result;
    }
    std::lock_guard<std::mutex> cmd(cmd_mutex_);
    std::uint8_t body[MIMHID_PROTO_MAX_PAYLOAD];
    std::uint8_t frame[5 + MIMHID_PROTO_MAX_PAYLOAD];
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

CommandResult Link::commit(std::uint16_t delay_ms, std::uint32_t timeout_ms) {
    clear_ready();
    const std::uint8_t payload[3] = {
        0, // detach: 0
        static_cast<std::uint8_t>(delay_ms & 0xFF),
        static_cast<std::uint8_t>((delay_ms >> 8) & 0xFF),
    };
    return command(MIMHID_T_COMMIT, payload, sizeof(payload), timeout_ms, 1);
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

IoStatus Link::send_report(std::uint8_t node, const std::uint8_t* report, std::size_t length,
                           std::uint32_t stall_ms, std::int32_t* os_error) {
    if (failed())
        return IoStatus::closed;
    if (length > MIMHID_PROTO_MAX_PAYLOAD)
        return IoStatus::error;
    std::uint8_t frame[5 + MIMHID_PROTO_MAX_PAYLOAD];
    const std::size_t n = encode_frame(
        static_cast<std::uint8_t>(MIMHID_T_REPORT_BASE | (node & 0x0F)), report, length, frame);
    std::lock_guard<std::mutex> tx(tx_mutex_);
    return port_.write_all(frame, n, stall_ms, os_error);
}

IoStatus Link::send_control_reply(std::uint8_t token, std::uint8_t action, const std::uint8_t* data,
                                  std::size_t length, std::uint32_t timeout_ms,
                                  std::int32_t* os_error) {
    if (failed())
        return IoStatus::closed;
    if (length + 3U > MIMHID_PROTO_MAX_PAYLOAD)
        return IoStatus::error;
    std::uint8_t body[3 + MIMHID_PROTO_MAX_PAYLOAD];
    std::uint8_t frame[5 + 3 + MIMHID_PROTO_MAX_PAYLOAD];
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
