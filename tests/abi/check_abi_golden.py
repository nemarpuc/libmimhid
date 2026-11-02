#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
"""Compare the compiler-observed public C ABI with the recorded golden layout.

abi_layout.golden holds every size, field offset and constant that tests/abi/layout_oracle.c
prints for include/mimhid.h as released in 2.0.x. A change of any of them is an ABI change:
it fails here until the golden file is edited on purpose, together with SOVERSION. The
MIMHID_VERSION_* lines are left out because tools/release/sync_version.py and
validate_release.py already track the version.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

GOLDEN = Path(__file__).with_name("abi_layout.golden")
IGNORED_PREFIX = "MIMHID_VERSION_"


def parse(output: str, origin: str) -> dict[str, int]:
    result: dict[str, int] = {}
    for line in output.splitlines():
        if not line:
            continue
        key, separator, value = line.partition("=")
        if not separator or key in result:
            raise ValueError(f"{origin}: malformed or duplicate line: {line!r}")
        if key.startswith(IGNORED_PREFIX):
            continue
        result[key] = int(value)
    return result


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_abi_golden.py LAYOUT_ORACLE", file=sys.stderr)
        return 2
    completed = subprocess.run(
        [sys.argv[1]], check=True, text=True, stdout=subprocess.PIPE
    )
    observed = parse(completed.stdout, "layout oracle")
    expected = parse(GOLDEN.read_text(encoding="utf-8"), str(GOLDEN))
    failed = False
    for key in sorted(expected.keys() - observed.keys()):
        print(f"ABI oracle omitted {key}", file=sys.stderr)
        failed = True
    for key in sorted(observed.keys() - expected.keys()):
        print(f"ABI oracle added unknown entry {key}", file=sys.stderr)
        failed = True
    for key in sorted(expected.keys() & observed.keys()):
        if observed[key] != expected[key]:
            print(
                f"ABI drift: {key}: observed {observed[key]}, expected {expected[key]}",
                file=sys.stderr,
            )
            failed = True
    if failed:
        return 1
    print(f"C ABI matches the golden layout: {len(expected)} entries")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
