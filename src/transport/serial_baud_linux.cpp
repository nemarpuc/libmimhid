// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimhid contributors
// Linux: any line rate through termios2 (BOTHER). <asm/termbits.h> redefines what <termios.h>
// declares, so this is the only file that includes it.
#include "transport/serial_baud.hpp"

#ifdef __linux__
#include <asm/ioctls.h> // TCGETS2 and TCSETS2; musl's <sys/ioctl.h> does not pull them in
#include <asm/termbits.h>
#include <sys/ioctl.h>

namespace mimhid::transport::detail {

namespace {
// glibc's ioctl takes the request as unsigned long, musl's as int; TCGETS2 has the top bit set.
#if defined(__GLIBC__)
using IoctlRequest = unsigned long;
#else
using IoctlRequest = int;
#endif
constexpr IoctlRequest kGet = static_cast<IoctlRequest>(TCGETS2);
constexpr IoctlRequest kSet = static_cast<IoctlRequest>(TCSETS2);
} // namespace

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool linux_set_baud(int fd, std::uint32_t baud, std::uint32_t* actual) noexcept {
    struct termios2 t;
    if (ioctl(fd, kGet, &t) != 0)
        return false;
    t.c_cflag &= ~static_cast<tcflag_t>(CBAUD | CIBAUD);
    t.c_cflag |= BOTHER;
    t.c_ispeed = baud;
    t.c_ospeed = baud;
    if (ioctl(fd, kSet, &t) != 0)
        return false;
    // What the driver kept. Some drivers (ftdi_sio) report the request, others (cp210x) the rate
    // their divider makes; the pings at that rate are the proof either way.
    if (ioctl(fd, kGet, &t) != 0)
        return false;
    *actual = static_cast<std::uint32_t>(t.c_ospeed);
    return true;
}

} // namespace mimhid::transport::detail
#endif
