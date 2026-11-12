// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libmimusb contributors
/* What speed a pty's termios says, for the fake board. On Linux a rate that is not one of the
 * B-constants is stored as BOTHER with the number beside it (struct termios2), which <termios.h>
 * cannot read, so this file uses the kernel's header alone. */
#include <stdint.h>

#ifdef __linux__
#include <asm/ioctls.h> /* TCGETS2; glibc's <sys/ioctl.h> pulls it in, musl's does not */
#include <asm/termbits.h>
#include <sys/ioctl.h>

uint32_t fake_board_pty_baud(int fd) {
    struct termios2 t;
    if (ioctl(fd, TCGETS2, &t) != 0)
        return 0;
    return (uint32_t)t.c_ospeed;
}
#else
#include <termios.h>

uint32_t fake_board_pty_baud(int fd) {
    struct termios t;
    if (tcgetattr(fd, &t) != 0)
        return 0;
    return (uint32_t)cfgetospeed(&t); /* macOS speeds are plain numbers */
}
#endif
