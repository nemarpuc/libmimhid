#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
"""List the boards mimhid_discover finds, using only caller-supplied choices.

mimhid_discover messages every serial port on the host, so run this only when that is
acceptable. With no board attached it prints nothing and exits 0.
"""

from __future__ import annotations

import ctypes as c
import sys

from mimhid.native import MIMHID_OK, DiscoveryP, load


def text(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("utf-8", "replace")


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: enumerate.py LIBMIMHID_PATH HANDSHAKE_TIMEOUT_MS", file=sys.stderr)
        return 2
    try:
        timeout_ms = int(sys.argv[2])
    except ValueError:
        timeout_ms = 0
    if not 1 <= timeout_ms <= 0xFFFF_FFFF:
        print("HANDSHAKE_TIMEOUT_MS must be a positive uint32", file=sys.stderr)
        return 2
    library = load(sys.argv[1])
    discovery = DiscoveryP()
    result = library.mimhid_discover(timeout_ms, c.byref(discovery))
    if result != MIMHID_OK:
        print(library.mimhid_result_name(result).decode(), file=sys.stderr)
        return 1
    try:
        for index in range(library.mimhid_discovery_count(discovery)):
            info = library.mimhid_discovery_get(discovery, index).contents
            print(
                f"{text(info.port_name)} firmware "
                f"{info.version_major}.{info.version_minor}.{info.version_patch} "
                f"{text(info.description)}"
            )
    finally:
        library.mimhid_discovery_destroy(discovery)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
