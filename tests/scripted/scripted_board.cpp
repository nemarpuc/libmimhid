// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* The scripted board (scripted_board.hpp). */
#include "scripted_board.hpp"

#include "mimhid.h"
#include "mimhid_proto.h"
#include "transport/frame.hpp"

#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

extern "C" std::uint32_t fake_board_pty_baud(int fd); // tests/sim/pty_baud.c

namespace scripted {

namespace tr = mimhid::transport;

namespace {

void put_u16(std::vector<std::uint8_t>* out, std::uint16_t value) {
    out->push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out->push_back(static_cast<std::uint8_t>(value >> 8));
}

void put_u32(std::vector<std::uint8_t>* out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
}

// Types that carry a sequence number and are answered with an ACK or NAK.
bool is_command(std::uint8_t type) {
    return type != MIMHID_T_PING && type != MIMHID_T_PROBE && type != MIMHID_T_CONTROL_REPLY &&
           !(type >= MIMHID_T_EP_IN_BASE && type <= MIMHID_T_EP_IN_LAST);
}

// The library's frames (the library's own parser takes only the board's types). Complete valid
// frames are moved to out; garbage is skipped a byte at a time.
void parse(std::vector<std::uint8_t>* buffer, std::vector<Received>* out) {
    std::size_t pos = 0;
    while (pos < buffer->size()) {
        const std::vector<std::uint8_t>& b = *buffer;
        if (b[pos] != MIMHID_SYNC) {
            ++pos;
            continue;
        }
        if (b.size() - pos < 3U)
            break;
        std::size_t header = 3U;
        std::size_t length = b[pos + 2];
        if ((length & 0x80U) != 0U) {
            if (b.size() - pos < 4U)
                break;
            length = (length & 0x7FU) | (static_cast<std::size_t>(b[pos + 3]) << 7);
            header = 4U;
        }
        if (length > MIMHID_PROTO_MAX_PAYLOAD) {
            ++pos;
            continue;
        }
        if (b.size() - pos < header + length + 2U)
            break;
        std::uint16_t crc = MIMHID_CRC16_INIT;
        for (std::size_t i = 1; i < header + length; ++i)
            crc = mimhid_crc16_update(crc, b[pos + i]);
        const std::size_t end = pos + header + length;
        if (b[end] != (crc & 0xFFU) || b[end + 1U] != (crc >> 8)) {
            ++pos;
            continue;
        }
        out->push_back(
            {b[pos + 1], std::vector<std::uint8_t>(b.begin() + static_cast<long>(pos + header),
                                                   b.begin() + static_cast<long>(end))});
        pos = end + 2U;
    }
    buffer->erase(buffer->begin(), buffer->begin() + static_cast<long>(pos));
}

} // namespace

Board::Board() {
    pong_.version[0] = MIMHID_VERSION_MAJOR;
    pong_.version[1] = MIMHID_VERSION_MINOR;
    pong_.version[2] = MIMHID_VERSION_PATCH;
}

Board::~Board() { stop(); }

bool Board::start() {
    master_ = posix_openpt(O_RDWR | O_NOCTTY);
    if (master_ < 0 || grantpt(master_) != 0 || unlockpt(master_) != 0)
        return false;
    const char* name = ptsname(master_);
    if (name == nullptr)
        return false;
    slave_ = name;
    termios tio{};
    tcgetattr(master_, &tio);
    cfmakeraw(&tio);
    tcsetattr(master_, TCSANOW, &tio);
    fcntl(master_, F_SETFL, fcntl(master_, F_GETFL) | O_NONBLOCK);
    stop_.store(false);
    thread_ = std::thread([this] { run(); });
    return true;
}

void Board::stop() {
    stop_.store(true);
    if (thread_.joinable())
        thread_.join();
    hang_up();
}

void Board::hang_up() {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (master_ >= 0) {
        ::close(master_);
        master_ = -1;
    }
}

void Board::set_policy(Policy policy) {
    std::lock_guard<std::mutex> lock(mutex_);
    policy_ = std::move(policy);
}

void Board::set_reading(bool reading) {
    reading_.store(reading);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!reading && thread_.joinable() && !paused_.load() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void Board::set_pong(const Pong& pong) {
    std::lock_guard<std::mutex> lock(mutex_);
    pong_ = pong;
}

void Board::set_carries(std::function<bool(std::uint32_t)> carries) {
    std::lock_guard<std::mutex> lock(mutex_);
    carries_ = std::move(carries);
}

void Board::send_bytes(const std::uint8_t* data, std::size_t length) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    std::size_t done = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (master_ >= 0 && done < length && std::chrono::steady_clock::now() < deadline) {
        const ssize_t n = ::write(master_, data + done, length - done);
        if (n > 0) {
            done += static_cast<std::size_t>(n);
            continue;
        }
        pollfd p{master_, POLLOUT, 0};
        (void)poll(&p, 1, 5);
    }
}

void Board::send_frame(std::uint8_t type, const std::uint8_t* payload, std::size_t length) {
    std::vector<std::uint8_t> frame(length + 8U);
    frame.resize(tr::encode_frame(type, payload, length, frame.data()));
    send_bytes(frame.data(), frame.size());
}

void Board::send_event(std::uint8_t kind, std::initializer_list<std::uint8_t> data) {
    std::vector<std::uint8_t> payload{kind};
    payload.insert(payload.end(), data.begin(), data.end());
    send_frame(MIMHID_T_EVENT, payload.data(), payload.size());
}

std::vector<Received> Board::received() {
    std::lock_guard<std::mutex> lock(mutex_);
    return received_;
}

std::size_t Board::count(std::uint8_t type) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t n = 0;
    for (const Received& r : received_)
        n += r.type == type ? 1U : 0U;
    return n;
}

bool Board::take(std::uint8_t type, Received* out, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    std::size_t index = 0;
    const bool found = cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
        for (index = 0; index < received_.size(); ++index) {
            if (!taken_[index] && received_[index].type == type)
                return true;
        }
        return false;
    });
    if (!found)
        return false;
    taken_[index] = true;
    if (out != nullptr)
        *out = received_[index];
    return true;
}

void Board::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    received_.clear();
    taken_.clear();
}

std::vector<std::uint8_t> Board::pong_payload() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pong_.raw)
        return *pong_.raw;
    std::vector<std::uint8_t> p(pong_.version, pong_.version + 3);
    p.push_back(pong_.ready);
    p.push_back(pong_.reserved);
    p.push_back(pong_.max_interfaces);
    p.push_back(pong_.flags);
    p.push_back(pong_.chip);
    p.push_back(pong_.high_speed);
    put_u32(&p, baud_.load());
    put_u32(&p, pong_.max_baud);
    put_u32(&p, pong_.uart_flags);
    put_u16(&p, pong_.max_descriptor_bytes);
    put_u16(&p, pong_.max_report_bytes);
    put_u16(&p, pong_.max_feature_bytes);
    put_u16(&p, pong_.max_endpoint_bytes);
    put_u16(&p, pong_.max_frame_payload);
    put_u16(&p, pong_.max_control_data_bytes);
    p.push_back(pong_.max_control_rules);
    p.push_back(pong_.max_endpoint_number);
    p.push_back(static_cast<std::uint8_t>(pong_.unique_id.size()));
    p.push_back(pong_.max_interfaces_any);
    put_u16(&p, pong_.max_config_descriptor_bytes);
    p.push_back(pong_.max_strings);
    put_u16(&p, pong_.max_transfer_bytes);
    put_u16(&p, pong_.endpoint_queue_slots);
    p.push_back(pong_.transfer_types);
    p.insert(p.end(), pong_.unique_id.begin(), pong_.unique_id.end());
    return p;
}

void Board::handle(const Received& frame) {
    if (frame.type == MIMHID_T_PING) {
        if (answer_pings_.load()) {
            const std::vector<std::uint8_t> pong = pong_payload();
            send_frame(MIMHID_T_PONG, pong.data(), pong.size());
        }
        return;
    }
    if (frame.type == MIMHID_T_PROBE) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++probes_;
        if (carries_ && !carries_(baud_.load()))
            probe_error_ = true;
        return;
    }
    if (!is_command(frame.type) || frame.payload.empty())
        return;

    std::optional<Reply> reply;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (policy_)
            reply = policy_(frame);
    }
    const std::uint8_t seq = frame.payload[0];
    std::uint32_t new_baud = 0;
    bool mount = false;
    if (!reply) {
        reply = Reply::ack();
        if (frame.type == MIMHID_T_SET_BAUD && frame.payload.size() >= 5U) {
            new_baud = tr::get_u32(&frame.payload[1]);
            put_u32(&reply->data, new_baud);
        } else if (frame.type == MIMHID_T_COUNTERS && frame.payload.size() >= 2U) {
            std::lock_guard<std::mutex> lock(mutex_);
            put_u16(&reply->data, static_cast<std::uint16_t>(probes_));
            put_u16(&reply->data, probe_error_ ? 1U : 0U); // bad CRC
            put_u16(&reply->data, 0U);
            put_u16(&reply->data, 0U);
            put_u16(&reply->data, 0U);
            if ((frame.payload[1] & MIMHID_COUNTERS_F_RESET) != 0U) {
                probes_ = 0;
                probe_error_ = false;
            }
        } else if (frame.type == MIMHID_T_COMMIT && frame.payload.size() >= 2U) {
            mount = frame.payload[1] == 0U && mount_on_commit_.load();
        }
    }
    switch (reply->kind) {
    case Reply::Kind::silent:
        return;
    case Reply::Kind::nak: {
        const std::uint8_t nak[3] = {seq, frame.type, reply->nak_code};
        send_frame(MIMHID_T_NAK, nak, sizeof(nak));
        return;
    }
    case Reply::Kind::ack: {
        std::vector<std::uint8_t> ack{seq, frame.type};
        ack.insert(ack.end(), reply->data.begin(), reply->data.end());
        send_frame(MIMHID_T_ACK, ack.data(), ack.size());
        break;
    }
    }
    if (new_baud != 0U)
        baud_.store(new_baud);
    if (mount)
        send_event(MIMHID_EV_MOUNTED, {});
}

void Board::run() {
    std::vector<std::uint8_t> pending;
    std::uint8_t buffer[4096];
    while (!stop_.load()) {
        int fd = -1;
        {
            std::lock_guard<std::mutex> lock(write_mutex_);
            fd = master_;
        }
        paused_.store(!reading_.load());
        if (fd < 0 || !reading_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, 5) <= 0 || (p.revents & POLLIN) == 0) {
            if ((p.revents & (POLLHUP | POLLERR)) != 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            pending.clear(); // a partial frame is given up when the line goes quiet
            continue;
        }
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n <= 0)
            continue;
        if (line_check_.load() && fake_board_pty_baud(fd) != baud_.load())
            continue; // the two ends run at different rates: what arrives is noise
        std::vector<Received> frames;
        pending.insert(pending.end(), buffer, buffer + n);
        parse(&pending, &frames);
        for (const Received& frame : frames) {
            if (frame.type != MIMHID_T_PING && frame.type != MIMHID_T_PROBE) {
                std::lock_guard<std::mutex> lock(mutex_);
                received_.push_back(frame);
                taken_.push_back(false);
                cv_.notify_all();
            }
            handle(frame);
        }
    }
}

} // namespace scripted
