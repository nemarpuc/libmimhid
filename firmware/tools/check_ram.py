#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimusb contributors
"""Stops a firmware build whose static RAM leaves less than the stated reserve.

usage: check_ram.py <size tool> <elf> <ram bytes of the chip> <reserve bytes> [<bytes outside data+bss>]

The linker already refuses an image larger than the RAM region. This adds the margin the port
states in its build (the stack, the interrupt handlers and the next TinyUSB release grow into it),
and prints what the image takes, so a limit raised in mimusb_limits.h shows its cost.
data and bss are the sizes `size` reports (the Pico SDK's heap and stack areas and the STM32 BSP's are in
them). A port whose stack is not (the nRF52840 BSP's startup file keeps a .stack section of 0x4000 bytes
that the linker places at the end of RAM) states its size as the last argument.
"""
import subprocess
import sys


def main() -> int:
    if len(sys.argv) not in (5, 6):
        print(__doc__, file=sys.stderr)
        return 2
    size_tool, elf, ram, reserve = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    out = subprocess.run([size_tool, elf], check=True, capture_output=True, text=True).stdout
    fields = out.strip().splitlines()[-1].split()
    data, bss = int(fields[1]), int(fields[2])
    extra = int(sys.argv[5]) if len(sys.argv) == 6 else 0
    used = data + bss + extra
    free = ram - used
    print(f"RAM: {used} of {ram} bytes used (data {data}, bss {bss}, outside {extra}); free {free}, reserve {reserve}")
    if free < reserve:
        print(f"error: only {free} bytes of RAM are left, the port asks for {reserve}; lower a "
              f"limit in its mimusb_limits.h", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
