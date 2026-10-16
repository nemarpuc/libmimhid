// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
// Linux and macOS serial port: termios with non-blocking descriptors.
#include "transport/serial_port.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
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

enum { WAIT_IN = 1, WAIT_OUT = 2, WAIT_ERR = 4 };

// poll() on tty devices is unreliable on macOS, so select() is used there.
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
#else
speed_t termios_speed(std::uint32_t baud) noexcept {
    switch (baud) {
    case 115200:
        return B115200;
    case 230400:
        return B230400;
#ifdef B460800
    case 460800:
        return B460800;
#endif
#ifdef B576000
    case 576000:
        return B576000;
#endif
    case 921600:
        return B921600;
#ifdef B1500000
    case 1500000:
        return B1500000;
#endif
#ifdef B2000000
    case 2000000:
        return B2000000;
#endif
    default:
        return B0;
    }
}
#endif

} // namespace

SerialPort::SerialPort() noexcept : handle_(-1) {}

SerialPort::~SerialPort() { close(); }

bool SerialPort::baud_supported(std::uint32_t baud) noexcept {
#ifdef __APPLE__
    switch (baud) {
    case 115200:
    case 230400:
    case 460800:
    case 576000:
    case 921600:
    case 1500000:
    case 2000000:
        return true;
    default:
        return false;
    }
#else
    return termios_speed(baud) != B0;
#endif
}

bool SerialPort::is_open() const noexcept { return handle_ >= 0; }

OpenResult SerialPort::open(const char* path, std::uint32_t baud) noexcept {
    OpenResult result;
    if (!baud_supported(baud)) {
        result.step = OpenStep::unsupported_baud;
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
#ifdef __APPLE__
    // Arbitrary rates are set with IOSSIOSPEED after the base termios settings.
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);
#else
    const speed_t speed = termios_speed(baud);
    cfsetispeed(&tio, speed);
    cfsetospeed(&tio, speed);
#endif
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
#ifdef __APPLE__
    speed_t rate = baud;
    // ENOTTY means a pseudo-terminal, which has no hardware rate to set.
    if (ioctl(fd, IOSSIOSPEED, &rate) != 0 && errno != ENOTTY) {
        result.step = OpenStep::speed;
        result.os_error = errno;
        ::close(fd);
        return result;
    }
#endif
#ifdef __linux__
    struct serial_struct ser{};
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

void SerialPort::close() noexcept {
    if (handle_ >= 0) {
        ::close(static_cast<int>(handle_));
        handle_ = -1;
    }
}

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
            if (w > 0 && !(w & WAIT_ERR))
                continue;
            if (w == 0)
                return done == 0 ? IoStatus::timeout : IoStatus::short_write;
        }
        if (os_error != nullptr)
            *os_error = errno;
        return errno == EIO || errno == ENXIO ? IoStatus::closed : IoStatus::error;
    }
    return IoStatus::ok;
}

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
        return (w & WAIT_ERR) ? -1 : 0;
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
