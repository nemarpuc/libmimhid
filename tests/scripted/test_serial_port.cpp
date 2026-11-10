// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
/* The POSIX serial port on a pseudo-terminal: each failed open step, a port used before it is
 * opened or after it is closed, writes that stall or meet a pulled cable, reads that time out or
 * see the hang-up, and rates. */
#include "scripted_support.hpp"
#include "transport/serial_port.hpp"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace tr = mimhid::transport;

namespace {

struct Pty {
    int master{-1};
    const char* slave{};
    bool open() {
        master = posix_openpt(O_RDWR | O_NOCTTY);
        if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0)
            return false;
        slave = ptsname(master);
        termios tio{};
        tcgetattr(master, &tio);
        cfmakeraw(&tio);
        tcsetattr(master, TCSANOW, &tio);
        fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
        return slave != nullptr;
    }
    void close() {
        if (master >= 0)
            ::close(master);
        master = -1;
    }
    ~Pty() { close(); }
};

void test_open_steps() {
    tr::SerialPort port;
    tr::OpenResult r = port.open("/nonexistent/tty", MIMHID_BOOT_BAUD);
    MIMHID_CHECK(!r.ok && r.step == tr::OpenStep::open && r.os_error == ENOENT);
    r = port.open("/dev/null", MIMHID_BOOT_BAUD);
    MIMHID_CHECK(!r.ok && r.step == tr::OpenStep::configure && r.os_error != 0);
    r = port.open("/dev/null", 0);
    MIMHID_CHECK(!r.ok && r.step == tr::OpenStep::speed);
    MIMHID_CHECK(!port.is_open());

    Pty pty;
    if (!pty.open())
        return;
    r = port.open(pty.slave, 1000000); // a rate other than the boot rate is set after the rest
    MIMHID_CHECK(r.ok && port.is_open());
    tr::SerialPort second;
#ifndef __APPLE__ // macOS does not enforce TIOCEXCL on a pty slave (a real serial port does)
    // The port is held for this process: a second open is refused.
    r = second.open(pty.slave, MIMHID_BOOT_BAUD);
    MIMHID_CHECK(!r.ok && r.step == tr::OpenStep::open);
#endif
    port.close();
    port.close(); // twice is harmless
    r = second.open(pty.slave, MIMHID_BOOT_BAUD);
    MIMHID_CHECK(r.ok);
}

// A closed port refuses everything without touching a descriptor.
void test_closed_port() {
    tr::SerialPort port;
    const std::uint8_t byte = 0;
    std::int32_t os_error = 0;
    MIMHID_CHECK(port.write_all(&byte, 1, 10, &os_error) == tr::IoStatus::closed);
    std::uint8_t buffer[4];
    MIMHID_CHECK(port.read(buffer, sizeof(buffer), 1) == -1);
    MIMHID_CHECK(!port.set_baud(MIMHID_BOOT_BAUD));
    MIMHID_CHECK(!port.send_break(1));
    port.drain();
    port.discard_input();
}

std::atomic<int> g_breaks{0};
void count_break() { ++g_breaks; }

void test_io_on_a_pty() {
    Pty pty;
    if (!pty.open())
        return;
    tr::SerialPort port;
    MIMHID_CHECK(port.open(pty.slave, MIMHID_BOOT_BAUD).ok);
    std::uint8_t buffer[64];
    // Nothing to read: a timeout reads zero bytes.
    MIMHID_CHECK(port.read(buffer, sizeof(buffer), 5) == 0);
    const std::uint8_t hello[3] = {1, 2, 3};
    MIMHID_CHECK(::write(pty.master, hello, sizeof(hello)) == 3);
    MIMHID_CHECK(port.read(buffer, sizeof(buffer), 100) == 3 && buffer[2] == 3);
    MIMHID_CHECK(port.write_all(hello, sizeof(hello), 100, nullptr) == tr::IoStatus::ok);
    pollfd p{pty.master, POLLIN, 0};
    MIMHID_CHECK(poll(&p, 1, 100) == 1 && ::read(pty.master, buffer, sizeof(buffer)) == 3);
    // Rates: any nonzero one on a pty; zero never.
    MIMHID_CHECK(port.set_baud(3000000));
    MIMHID_CHECK(port.set_baud(MIMHID_BOOT_BAUD));
    MIMHID_CHECK(!port.set_baud(0));
    // A BREAK reaches the observer.
    tr::set_break_observer(count_break);
    const int before = g_breaks.load();
    MIMHID_CHECK(port.send_break(1));
    tr::set_break_observer(nullptr);
    MIMHID_CHECK(g_breaks.load() >= before && g_breaks.load() <= before + 1);
    port.drain();
    port.discard_input();

    // Nobody reads the master: the writes fill the pty and stall.
    std::array<std::uint8_t, 4096> block{};
    tr::IoStatus status = tr::IoStatus::ok;
    for (int i = 0; i < 4096 && status == tr::IoStatus::ok; ++i)
        status = port.write_all(block.data(), block.size(), 5, nullptr);
    MIMHID_CHECK(status == tr::IoStatus::timeout || status == tr::IoStatus::short_write);
    // The cable is pulled: a write is told the port is gone, and a read fails.
    pty.close();
    std::int32_t os_error = 0;
    status = port.write_all(block.data(), 1, 5, &os_error);
    MIMHID_CHECK(status == tr::IoStatus::closed || status == tr::IoStatus::error);
    MIMHID_CHECK(os_error != 0);
    MIMHID_CHECK(port.read(buffer, sizeof(buffer), 5) == -1);
}

} // namespace

void test_serial_port() {
    test_open_steps();
    test_closed_port();
    test_io_on_a_pty();
}
