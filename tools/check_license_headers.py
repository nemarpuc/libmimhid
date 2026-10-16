#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
"""Require an SPDX-License-Identifier and copyright line in every source file.

The repository root LICENSE covers the whole tree, but a single file copied out
of it carries no license information of its own. Machine license scanners and
SBOM generators also work per file. This check therefore requires the two-line
SPDX header on every source file in the extensions listed below.

Data and markup formats are deliberately out of scope: JSON cannot carry a
comment.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
SPDX_LINE = "SPDX-License-Identifier: MIT"
COPYRIGHT_LINE = "Copyright (c) 2026 libmimhid contributors"

# Verbatim third-party build files kept in the repository carry their own
# upstream SPDX identifier and copyright line.
THIRD_PARTY_HEADERS: dict[str, tuple[str, str]] = {
    "firmware/pico_sdk_import.cmake": (
        "# SPDX-License-Identifier: BSD-3-Clause",
        "# Copyright 2020 (c) 2020 Raspberry Pi (Trading) Ltd.",
    ),
}

SLASH_COMMENT_SUFFIXES = frozenset({".c", ".h", ".cpp", ".hpp"})
HASH_COMMENT_SUFFIXES = frozenset({".py", ".cmake", ".sh"})
HASH_COMMENT_NAMES = frozenset({"CMakeLists.txt"})

EXCLUDED_DIRECTORIES = ("build", ".git", "__pycache__")


def in_scope(path: Path) -> bool:
    relative = path.relative_to(ROOT).as_posix()
    if any(
        relative == excluded
        or relative.startswith(excluded + "/")
        or f"/{excluded}/" in relative
        for excluded in EXCLUDED_DIRECTORIES
    ):
        return False
    return (
        path.suffix in SLASH_COMMENT_SUFFIXES
        or path.suffix in HASH_COMMENT_SUFFIXES
        or path.name in HASH_COMMENT_NAMES
    )


def sources() -> list[Path]:
    return sorted(path for path in ROOT.rglob("*") if path.is_file() and in_scope(path))


def header_prefix(path: Path) -> str:
    if path.suffix in SLASH_COMMENT_SUFFIXES:
        return "// "
    return "# "


def expected_header(path: Path) -> list[str]:
    relative = path.relative_to(ROOT).as_posix()
    if relative in THIRD_PARTY_HEADERS:
        return list(THIRD_PARTY_HEADERS[relative])
    prefix = header_prefix(path)
    return [prefix + SPDX_LINE, prefix + COPYRIGHT_LINE]


def check(path: Path) -> str | None:
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as error:
        return f"{path.relative_to(ROOT)}: unreadable ({error})"
    lines = text.splitlines()
    start = 1 if lines and lines[0].startswith("#!") else 0
    window = lines[start : start + 2]
    expected = expected_header(path)
    if window != expected:
        return (
            f"{path.relative_to(ROOT)}: expected the exact two-line header "
            f"{expected[0]!r} then {expected[1]!r}"
        )
    return None


def insert(path: Path) -> bool:
    relative = path.relative_to(ROOT).as_posix()
    if relative in THIRD_PARTY_HEADERS or check(path) is None:
        return False
    text = path.read_text(encoding="utf-8")
    prefix = header_prefix(path)
    header = f"{prefix}{SPDX_LINE}\n{prefix}{COPYRIGHT_LINE}\n"
    if text.startswith("#!"):
        shebang, separator, rest = text.partition("\n")
        path.write_text(shebang + separator + header + rest, encoding="utf-8")
    else:
        path.write_text(header + text, encoding="utf-8")
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--fix",
        action="store_true",
        help="Insert the header into every first-party file that is missing it.",
    )
    arguments = parser.parse_args()
    files = sources()
    if not files:
        print("check-license-headers: no source files were found", file=sys.stderr)
        return 1
    if arguments.fix:
        changed = [path for path in files if insert(path)]
        for path in changed:
            print(f"check-license-headers: added header to {path.relative_to(ROOT)}")
    failures = [message for message in (check(path) for path in files) if message]
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(f"check-license-headers: {len(files)} source files carry their SPDX headers")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
