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


FIRMWARE_LICENSE_NOTES = """

---

### Firmware Binary Third-Party Notices

The host library (`libmimhid`) is licensed under the MIT License and has no third-party runtime library dependencies.

Prebuilt microcontroller firmware images (`mimhid_firmware-*`) incorporate third-party SDK and USB stack components under permissive licenses:

| Firmware asset | Third-party components & licenses |
| :--- | :--- |
| `mimhid_firmware-rp2040-*`, `mimhid_firmware-rp2350-*` | TinyUSB 0.18.0 (`MIT`), Raspberry Pi Pico SDK 2.2.0 (`BSD-3-Clause`, Copyright (c) 2020 Raspberry Pi (Trading) Ltd.), Arm CMSIS Core (`Apache-2.0`), Newlib C runtime |
| `mimhid_firmware-nrf52840-*` | TinyUSB 0.18.0 (`MIT`), Nordic Semiconductor `nrfx` (`BSD-3-Clause`, Copyright (c) 2017-2024 Nordic Semiconductor ASA), Arm CMSIS 5 (`Apache-2.0`), Newlib C runtime |
| `mimhid_firmware-stm32f4-*` | TinyUSB 0.18.0 (`MIT`), STMicroelectronics `cmsis_device_f4` & `stm32f4xx_hal_driver` (`BSD-3-Clause` / `Apache-2.0`, Copyright (c) 2016-2019 STMicroelectronics), Arm CMSIS 5 (`Apache-2.0`), Newlib C runtime |
| `mimhid_firmware-esp32s3-*` | TinyUSB 0.18.0 (`MIT`), Espressif ESP-IDF v5.4.2 (`Apache-2.0`, Copyright 2015-2025 Espressif Systems), Amazon FreeRTOS Kernel (`MIT`), Newlib C runtime |

Complete copyright notices, license texts, and warranty disclaimers for all firmware binaries are provided in the release asset `mimhid_firmware-THIRD_PARTY_NOTICES-{version}.txt` and in [`THIRD_PARTY_NOTICES.md`](https://github.com/nemarpuc/libmimhid/blob/v{version}/THIRD_PARTY_NOTICES.md) / [`NOTICE`](https://github.com/nemarpuc/libmimhid/blob/v{version}/NOTICE)."""


def validate(root: Path, tag: str, allow_undated: bool = False) -> tuple[str, str]:
    if not tag.startswith("v"):
        raise release_names.ReleaseError(f"tag {tag!r} must start with 'v'")
    version = tag[1:]
    if release_names.VERSION_RE.fullmatch(version) is None:
        raise release_names.ReleaseError(f"tag {tag!r} is not vMAJOR.MINOR.PATCH")
    for required_doc in ("LICENSE", "NOTICE", "THIRD_PARTY_NOTICES.md"):
        doc_path = root / required_doc
        if not doc_path.is_file() or doc_path.stat().st_size == 0:
            raise release_names.ReleaseError(f"required license document is missing or empty: {required_doc}")
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


def dry_run_tag(root: Path) -> tuple[str, bool, str]:
    """The tag a dry run validates as, whether its CHANGELOG section may be undated, and where the
    version came from. Normally that is the newest dated CHANGELOG section; when the version files
    have moved on to the next release (all of them agreeing), the version files are used and the
    section may still be undated."""
    dated = release_names.newest_dated_version(root)
    found = set(release_names.read_versions(root).values())
    if found == {dated}:
        return f"v{dated}", False, f"the newest dated CHANGELOG section ({dated}), which every version file carries"
    if len(found) != 1:
        raise release_names.ReleaseError(f"the version files disagree: {sorted(found)}")
    version = found.pop()
    return (f"v{version}", True,
            f"the version files ({version}); the newest dated CHANGELOG section is {dated}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--tag")
    mode.add_argument("--dry-run", action="store_true",
                      help="no tag: take the version from the CHANGELOG or the version files")
    parser.add_argument("--version-out", type=Path, help="write the validated version here")
    parser.add_argument("--summary-out", type=Path, help="append a Markdown line saying where the version came from")
    parser.add_argument("--notes-out", type=Path)
    parser.add_argument("--allow-undated", action="store_true",
                        help="dry run: accept a CHANGELOG section without a release date")
    args = parser.parse_args()
    try:
        tag, allow_undated, origin = args.tag, args.allow_undated, "the tag"
        if args.dry_run:
            tag, undated, origin = dry_run_tag(args.root)
            allow_undated = allow_undated or undated
        version, notes = validate(args.root, tag, allow_undated)
    except release_names.ReleaseError as error:
        print(f"release validation failed: {error}", file=sys.stderr)
        return 1
    if args.notes_out is not None:
        args.notes_out.parent.mkdir(parents=True, exist_ok=True)
        full_notes = notes + FIRMWARE_LICENSE_NOTES.format(version=version) + "\n"
        args.notes_out.write_text(full_notes, encoding="utf-8")
    if args.version_out is not None:
        args.version_out.write_text(version + "\n", encoding="utf-8")
    if args.summary_out is not None:
        with args.summary_out.open("a", encoding="utf-8") as summary:
            kind = "Dry run" if args.dry_run else "Release"
            summary.write(f"{kind} of libmimhid {version}: the version comes from {origin}.\n")
    print(f"release {version} is consistent (version from {origin})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
