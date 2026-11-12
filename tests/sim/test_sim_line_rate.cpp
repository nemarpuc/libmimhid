// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* The line rate: the rate search of mimusb_discover, a board left on a fast rate, BREAK and
 * adapters that round the rate. */
#include "sim_support.hpp"

#include "fill_values.h"
#include "mimusb_proto.h"
#include "transport/handshaker.hpp"
#include "transport/link.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace sim {

namespace {

// Frames written by hand on the slave end, as another program would.
void raw_frame(int fd, std::uint8_t type, const std::uint8_t* payload, std::uint8_t length) {
    std::uint8_t frame[16];
    const std::size_t n = mimusb::transport::encode_frame(type, payload, length, frame);
    MIMUSB_CHECK(write(fd, frame, n) == static_cast<ssize_t>(n));
}

bool raw_answer(int fd, std::uint8_t type) {
    std::uint8_t buf[256];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    std::size_t have = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const ssize_t n = read(fd, buf + have, sizeof(buf) - have);
        if (n > 0)
            have += static_cast<std::size_t>(n);
        for (std::size_t i = 0; i + 1 < have; ++i) {
            if (buf[i] == MIMUSB_SYNC && buf[i + 1] == type)
                return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

void set_speed(int fd, std::uint32_t baud) {
#ifdef __APPLE__
    const speed_t speed = baud; // speeds are plain numbers there
#else
    const speed_t speed = baud == 3000000U ? B3000000 : B115200;
#endif
    termios tio{};
    tcgetattr(fd, &tio);
    cfmakeraw(&tio);
    cfsetispeed(&tio, speed);
    cfsetospeed(&tio, speed);
    tcsetattr(fd, TCSANOW, &tio);
}

mimusb_device* open_at(std::uint32_t baud, mimusb_result* result) {
    mimusb_device_config o = base_options();
    o.transport.baud_rate = baud;
    mimusb_device* device = nullptr;
    *result = mimusb_device_open(g_slave.c_str(), &o, &device);
    return device;
}

std::uint32_t link_baud(const mimusb_device* device) {
    mimusb_device_info info{};
    return mimusb_device_board_info(device, &info) == MIMUSB_OK ? info.baud_rate : 0U;
}

// What mimusb_discover measures on each port, run on the pseudo-terminal.
std::uint32_t measure() {
    mimusb::transport::SerialPort port;
    if (!port.open(g_slave.c_str(), MIMUSB_BOOT_BAUD).ok)
        return 0;
    mimusb::transport::Handshaker handshake(port, 50);
    mimusb::transport::BoardInfo board;
    if (!handshake.find(&board))
        return 0;
    const std::uint32_t fastest = handshake.fastest(&board);
    return handshake.to_boot(&board) ? fastest : 0;
}

} // namespace

void test_line_rate() {
    mimusb_result r = MIMUSB_OK;
    // A pseudo-terminal carries no BREAK; the board is told of it directly.
    mimusb::transport::set_break_observer([] { fake_board_inject_errors(FAKE_BOARD_BREAK); });

    // The rate is the caller's: none is chosen for it.
    mimusb_device* device = open_at(0, &r);
    MIMUSB_CHECK(r == MIMUSB_ERR_UNSET_FIELD && device == nullptr);

    // The fastest rate the board makes; the board is left on the boot rate.
    MIMUSB_CHECK(measure() == 3000000U && fake_board_baud() == MIMUSB_BOOT_BAUD);
    device = open_at(3000000U, &r);
    MIMUSB_CHECK(r == MIMUSB_OK && link_baud(device) == 3000000U);
    MIMUSB_CHECK(fake_board_baud() == 3000000U);
    // The line lost nothing; the counters say so, and a reset zeroes them.
    mimusb_link_counters counters{};
    mimusb_link_counters_init(&counters);
    MIMUSB_CHECK(mimusb_device_link_counters(device, 1, kControlMs, &counters) == MIMUSB_OK);
    MIMUSB_CHECK(counters.bad_crc == 0 && counters.framing == 0 && counters.uart_overrun == 0 &&
                 counters.ring_overrun == 0);
    mimusb_device_close(device, kControlMs, kDrainMs);
    MIMUSB_CHECK(fake_board_baud() == MIMUSB_BOOT_BAUD);

    // The caller may ask for the measurement itself; the board is never left on a slower rate
    // than the one reported.
    device = open_at(MIMUSB_BAUD_FASTEST, &r);
    MIMUSB_CHECK(r == MIMUSB_OK && link_baud(device) == 3000000U);
    mimusb_device_close(device, kControlMs, kDrainMs);

    // An adapter that cannot go past 1,000,000 sends at that rate whatever it is set to: the
    // faster rates are tried and given up, and a rate within 2 % of 1,000,000 is measured.
    fake_board_set_adapter_limit(1000000U);
    const std::uint32_t limited = measure();
    MIMUSB_CHECK(limited >= 960000U && limited <= 1000000U &&
                 fake_board_baud() == MIMUSB_BOOT_BAUD);
    // A rate the adapter does not carry fails, and leaves the board on the boot rate.
    device = open_at(2000000U, &r);
    MIMUSB_CHECK(r == MIMUSB_ERR_UNSUPPORTED && device == nullptr);
    MIMUSB_CHECK(fake_board_baud() == MIMUSB_BOOT_BAUD);
    fake_board_set_adapter_limit(0);

    // A rate past what the board makes, and rates that are in no table.
    device = open_at(4000000U, &r);
    MIMUSB_CHECK(r == MIMUSB_ERR_UNSUPPORTED && device == nullptr);
    device = open_at(921600U, &r);
    MIMUSB_CHECK(r == MIMUSB_OK && link_baud(device) == 921600U && fake_board_baud() == 921600U);
    mimusb_device_close(device, kControlMs, kDrainMs);
    device = open_at(1234567U, &r);
    MIMUSB_CHECK(r == MIMUSB_OK && link_baud(device) == 1234567U && fake_board_baud() == 1234567U);
    mimusb_device_close(device, kControlMs, kDrainMs);
    MIMUSB_CHECK(fake_board_baud() == MIMUSB_BOOT_BAUD);

    // A program that went away left the board on 3,000,000: a BREAK brings it back.
    const int fd = open(g_slave.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    MIMUSB_CHECK(fd >= 0);
    set_speed(fd, 115200U);
    const std::uint8_t to_fast[5] = {0x55, 0xC0, 0xC6, 0x2D, 0x00}; // 3,000,000 = 0x002DC6C0
    raw_frame(fd, MIMUSB_T_SET_BAUD, to_fast, 5);
    MIMUSB_CHECK(raw_answer(fd, MIMUSB_T_ACK));
    set_speed(fd, 3000000U);
    raw_frame(fd, MIMUSB_T_PING, nullptr, 0);
    MIMUSB_CHECK(raw_answer(fd, MIMUSB_T_PONG));
    close(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(2 * MIMUSB_BAUD_CONFIRM_MS));
    MIMUSB_CHECK(fake_board_baud() == 3000000U);
    device = open_at(921600U, &r);
    MIMUSB_CHECK(r == MIMUSB_OK && link_baud(device) == 921600U);
    mimusb_device_close(device, kControlMs, kDrainMs);
    MIMUSB_CHECK(fake_board_baud() == MIMUSB_BOOT_BAUD);

#ifdef __linux__
    // Only one program has the port. A macOS pseudo-terminal does not keep TIOCEXCL, so this
    // is checked on Linux only.
    device = open_at(3000000U, &r);
    MIMUSB_CHECK(r == MIMUSB_OK);
    mimusb_device* second = open_at(3000000U, &r);
    MIMUSB_CHECK(r == MIMUSB_ERR_ACCESS && second == nullptr);
    mimusb_device_close(device, kControlMs, kDrainMs);
#endif
}

} // namespace sim
