#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
"""Reject a release whose tag, versions, or CHANGELOG disagree."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

import release_names


def validate(root: Path, tag: str, allow_undated: bool = False) -> tuple[str, str]:
    if not tag.startswith("v"):
        raise release_names.ReleaseError(f"tag {tag!r} must start with 'v'")
    version = tag[1:]
    if release_names.VERSION_RE.fullmatch(version) is None:
        raise release_names.ReleaseError(f"tag {tag!r} is not vMAJOR.MINOR.PATCH")
    for path, found in release_names.read_versions(root).items():
        if found != version:
            raise release_names.ReleaseError(f"{path} says {found}, the tag says {version}")
    date, notes = release_names.changelog_section(root, version)
    if not allow_undated and re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}", date) is None:
        raise release_names.ReleaseError(
            f"CHANGELOG.md section {version} must carry a release date (YYYY-MM-DD), found {date!r}"
        )
    if not notes:
        raise release_names.ReleaseError(f"CHANGELOG.md section {version} is empty")
    return version, notes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--tag", required=True)
    parser.add_argument("--notes-out", type=Path)
    parser.add_argument("--allow-undated", action="store_true",
                        help="dry run: accept a CHANGELOG section without a release date")
    args = parser.parse_args()
    try:
        version, notes = validate(args.root, args.tag, args.allow_undated)
    except release_names.ReleaseError as error:
        print(f"release validation failed: {error}", file=sys.stderr)
        return 1
    if args.notes_out is not None:
        args.notes_out.parent.mkdir(parents=True, exist_ok=True)
        args.notes_out.write_text(notes + "\n", encoding="utf-8")
    print(f"release {version} is consistent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
