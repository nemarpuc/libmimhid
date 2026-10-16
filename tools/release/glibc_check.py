#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
"""Fail when a Linux library needs a newer glibc than the release baseline (2.35, Ubuntu 22.04)."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys

BASELINE = (2, 35)


def required_versions(objdump: str, library: Path) -> list[str]:
    output = subprocess.check_output([objdump, "-T", str(library)], text=True)
    return sorted(set(re.findall(r"\(?(GLIBC_[A-Za-z0-9._]+)\)?", output)))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--objdump", default="objdump")
    args = parser.parse_args()
    highest = (0, 0)
    for name in required_versions(args.objdump, args.library):
        match = re.fullmatch(r"GLIBC_([0-9]+)\.([0-9]+)(?:\.([0-9]+))?", name)
        if match is None:
            print(f"nonnumeric glibc requirement: {name}", file=sys.stderr)
            return 1
        highest = max(highest, (int(match.group(1)), int(match.group(2))))
    if highest > BASELINE:
        print(f"requires GLIBC_{highest[0]}.{highest[1]}, baseline is {BASELINE[0]}.{BASELINE[1]}", file=sys.stderr)
        return 1
    print(f"maximum glibc requirement: {highest[0]}.{highest[1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
