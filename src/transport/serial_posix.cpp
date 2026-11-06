// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
// Linux and macOS serial port: termios with non-blocking descriptors.
#include "transport/serial_port.hpp"

#include "transport/serial_baud.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/select.h>
#else
#include <poll.h>
#endif
#ifdef __linux__
#include <linux/serial.h>
#endif

namespace mimhid::transport {

namespace {

std::atomic<BreakObserver> g_break_observer{nullptr};

enum { WAIT_IN = 1, WAIT_OUT = 2, WAIT_ERR = 4 };

// poll() on tty devices is unreliable on macOS, so select() is used there.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int wait_fd(int fd, int events, std::uint32_t timeout_ms) noexcept {
#ifdef __APPLE__
    if (fd < 0 || fd >= FD_SETSIZE)
        return -1;
    fd_set rs, ws, es;
    FD_ZERO(&rs);
    FD_ZERO(&ws);
    FD_ZERO(&es);
    if (events & WAIT_IN)
        FD_SET(fd, &rs);
    if (events & WAIT_OUT)
        FD_SET(fd, &ws);
    FD_SET(fd, &es);
    struct timeval tv;
    tv.tv_sec = static_cast<time_t>(timeout_ms / 1000U);
    tv.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000U) * 1000U);
    int r = select(fd + 1, &rs, &ws, &es, &tv);
    if (r <= 0)
        return r;
    int out = 0;
    if (FD_ISSET(fd, &rs))
        out |= WAIT_IN;
    if (FD_ISSET(fd, &ws))
        out |= WAIT_OUT;
    if (FD_ISSET(fd, &es))
        out |= WAIT_ERR;
    return out;
#else
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events =
        static_cast<short>((events & WAIT_IN ? POLLIN : 0) | (events & WAIT_OUT ? POLLOUT : 0));
    pfd.revents = 0;
    int r = poll(&pfd, 1, static_cast<int>(timeout_ms));
    if (r <= 0)
        return r;
    int out = 0;
    if (pfd.revents & POLLIN)
        out |= WAIT_IN;
    if (pfd.revents & POLLOUT)
        out |= WAIT_OUT;
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
        out |= WAIT_ERR;
    return out;
#endif
}

#ifdef __APPLE__
// Not exposed without IOKit headers; same value as IOSSIOSPEED in <IOKit/serial/ioss.h>.
#ifndef IOSSIOSPEED
#define IOSSIOSPEED _IOW('T', 2, speed_t)
#endif
#endif

// Sets the rate of an open tty to any value the driver takes. A rate the driver rounds by more
// than 3 % is refused: the board would be talking to a port on another rate. False when refused.
bool apply_baud(int fd, std::uint32_t baud) noexcept {
#ifdef __APPLE__
    speed_t rate = baud;
    if (ioctl(fd, IOSSIOSPEED, &rate) == 0)
        return true;
    if (errno != ENOTTY)
        return false;
    // A pseudo-terminal keeps the rate in its termios, where any value is taken.
    struct termios tio;
    if (tcgetattr(fd, &tio) != 0)
        return false;
    cfsetispeed(&tio, rate);
    cfsetospeed(&tio, rate);
    return tcsetattr(fd, TCSANOW, &tio) == 0;
#else
    std::uint32_t actual = 0;
    if (!detail::linux_set_baud(fd, baud, &actual))
        return false;
    const std::uint64_t diff = actual > baud ? actual - baud : baud - actual;
    return diff * 100U <= static_cast<std::uint64_t>(baud) * 3U;
#endif
}

} // namespace

void set_break_observer(BreakObserver observer) noexcept { g_break_observer.store(observer); }

SerialPort::SerialPort() noexcept : handle_(-1) {}

SerialPort::~SerialPort() { close(); }

bool SerialPort::is_open() const noexcept { return handle_ >= 0; }

OpenResult SerialPort::open(const char* path, std::uint32_t baud) noexcept {
    OpenResult result;
    if (baud == 0U) {
        result.step = OpenStep::speed;
        return result;
    }
    int fd = ::open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        result.step = OpenStep::open;
        result.os_error = errno;
        return result;
    }

    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) {
        result.step = OpenStep::configure;
        result.os_error = errno;
        ::close(fd);
        return result;
    }

    cfmakeraw(&tio);
    // Any other rate is set after the base settings (IOSSIOSPEED, termios2).
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);
    tio.c_cflag |= (CS8 | CLOCAL | CREAD);
    tio.c_cflag &= ~static_cast<tcflag_t>(PARENB | CSTOPB | CRTSCTS);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        result.step = OpenStep::configure;
        result.os_error = errno;
        ::close(fd);
        return result;
    }
    if (baud != 115200U && !apply_baud(fd, baud)) {
        result.step = OpenStep::speed;
        result.os_error = errno;
        ::close(fd);
        return result;
    }
    // Another process holding the port (a running program, or a search) keeps it to itself.
    if (ioctl(fd, TIOCEXCL) != 0) {
        result.step = OpenStep::open;
        result.os_error = errno;
        ::close(fd);
        return result;
    }
#ifdef __linux__
    struct serial_struct ser {};
    if (ioctl(fd, TIOCGSERIAL, &ser) == 0) {
        ser.flags |= static_cast<int>(ASYNC_LOW_LATENCY);
        (void)ioctl(fd, TIOCSSERIAL, &ser);
    }
#endif
    tcflush(fd, TCIOFLUSH);
    handle_ = fd;
    result.ok = true;
    return result;
}

bool SerialPort::set_baud(std::uint32_t baud) noexcept {
    const int fd = static_cast<int>(handle_);
    if (fd < 0 || baud == 0U)
        return false;
    tcdrain(fd);
    if (!apply_baud(fd, baud))
        return false;
    tcflush(fd, TCIFLUSH);
    return true;
}

bool SerialPort::send_break(std::uint32_t ms) noexcept {
    const int fd = static_cast<int>(handle_);
    if (fd < 0)
        return false;
    tcdrain(fd);
    // TIOCSBRK holds the line low until TIOCCBRK; tcsendbreak would hold it for 250 to 500 ms.
    if (ioctl(fd, TIOCSBRK) != 0)
        return false;
    if (BreakObserver observer = g_break_observer.load())
        observer();
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    (void)ioctl(fd, TIOCCBRK);
    tcflush(fd, TCIFLUSH);
    return true;
}

void SerialPort::close() noexcept {
    if (handle_ >= 0) {
        // The flag outlives the descriptor on a tty whose other end stays open (a pty).
        (void)ioctl(static_cast<int>(handle_), TIOCNXCL);
        ::close(static_cast<int>(handle_));
        handle_ = -1;
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
IoStatus SerialPort::write_all(const std::uint8_t* data, std::size_t length, std::uint32_t stall_ms,
                               std::int32_t* os_error) noexcept {
    const int fd = static_cast<int>(handle_);
    if (fd < 0)
        return IoStatus::closed;
    std::size_t done = 0;
    while (done < length) {
        ssize_t n = ::write(fd, data + done, length - done);
        if (n > 0) {
            done += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN) {
            int w = wait_fd(fd, WAIT_OUT, stall_ms);
            if (w < 0 && errno == EINTR)
                continue; // a signal is not a failure; the stall clock starts again
            if (w > 0 && !(w & WAIT_ERR))
                continue;
            if (w == 0)
                return done == 0 ? IoStatus::timeout : IoStatus::short_write;
            if (w > 0) {
                // The adapter reports an error with no errno of its own to read.
                errno = EIO;
            }
        }
        if (os_error != nullptr)
            *os_error = errno;
        return errno == EIO || errno == ENXIO ? IoStatus::closed : IoStatus::error;
    }
    return IoStatus::ok;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
long SerialPort::read(std::uint8_t* buffer, std::size_t capacity,
                      std::uint32_t timeout_ms) noexcept {
    const int fd = static_cast<int>(handle_);
    if (fd < 0)
        return -1;
    int w = wait_fd(fd, WAIT_IN, timeout_ms);
    if (w < 0)
        return errno == EINTR ? 0 : -1;
    if (w == 0)
        return 0;
    if ((w & WAIT_ERR) && !(w & WAIT_IN))
        return -1; // the adapter was unplugged
    ssize_t n = ::read(fd, buffer, capacity);
    if (n < 0)
        return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
    if (n == 0)
        return -1;
    return static_cast<long>(n);
}

void SerialPort::drain() noexcept {
    if (handle_ >= 0) {
        tcdrain(static_cast<int>(handle_));
        tcflush(static_cast<int>(handle_), TCIFLUSH);
    }
}

void SerialPort::discard_input() noexcept {
    if (handle_ >= 0)
        tcflush(static_cast<int>(handle_), TCIFLUSH);
}

} // namespace mimhid::transport
