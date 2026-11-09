#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
"""Names, versions, and the exact asset set of a GitHub Release."""

from __future__ import annotations

from pathlib import Path
import re

import version_files

PROJECT = "libmimhid"
REPOSITORY = "nemarpuc/libmimhid"
REPOSITORY_URL = f"https://github.com/{REPOSITORY}"
VERSION_RE = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")

TARGETS = (
    ("linux", "x86_64"),
    ("linux", "aarch64"),
    ("macos", "arm64"),
    ("macos", "x86_64"),
    ("windows", "x86_64"),
    ("windows", "arm64"),
)
# Every binary archive carries both library kinds; find_package(mimhid COMPONENTS ...) picks one.
COMPONENTS = ("shared", "static")

# Firmware images published with the library; libaoahid has no firmware.
FIRMWARE_SUFFIXES = {
    "rp2040": ("uf2",),
    "rp2350": ("uf2",),
    "nrf52840": ("hex", "bin"),
    "stm32f4": ("hex", "bin"),
    "esp32s3": ("bin",),
}


class ReleaseError(RuntimeError):
    pass


def check_target(platform: str, arch: str) -> None:
    if (platform, arch) not in TARGETS:
        raise ReleaseError(f"unsupported release target: {platform}/{arch}")


def check_version(version: str) -> None:
    if VERSION_RE.fullmatch(version) is None:
        raise ReleaseError("version must have the exact form MAJOR.MINOR.PATCH")


def base_name(version: str, platform: str, arch: str) -> str:
    check_target(platform, arch)
    check_version(version)
    middle = f"{platform}-{arch}"
    if platform == "linux":
        middle += "-ubuntu22.04"
    return f"{PROJECT}-{version}-{middle}"


def archive_name(version: str, platform: str, arch: str) -> str:
    extension = "zip" if platform == "windows" else "tar.gz"
    return f"{base_name(version, platform, arch)}.{extension}"


def archive_names(version: str) -> list[str]:
    return [archive_name(version, platform, arch) for platform, arch in TARGETS]


def sbom_name(version: str) -> str:
    """The one SPDX document that describes every binary archive of the release."""
    check_version(version)
    return f"{PROJECT}-{version}.spdx.json"


def source_name(version: str) -> str:
    return f"{PROJECT}-{version}-source.tar.gz"


def firmware_names(version: str) -> list[str]:
    return [
        f"mimhid_firmware-{target}-{version}.{suffix}"
        for target, suffixes in FIRMWARE_SUFFIXES.items()
        for suffix in suffixes
    ]


def firmware_notices_name(version: str) -> str:
    return f"mimhid_firmware-THIRD_PARTY_NOTICES-{version}.txt"


def expected_assets(version: str) -> list[str]:
    """Every file a release uploads: 6 binary archives, one SPDX document for all of them, the
    source archive, the manifest, the checksums, the firmware images, and the firmware
    third-party notices file."""
    names = archive_names(version)
    names.append(sbom_name(version))
    names.append(source_name(version))
    names.extend(firmware_names(version))
    names.append(firmware_notices_name(version))
    names.extend(["release-manifest.json", "SHA256SUMS"])
    return sorted(names)


def read_versions(root: Path) -> dict[str, str]:
    """The version recorded in every file that carries one (the list is version_files.SITES)."""
    try:
        return version_files.assembled_versions(root)
    except (OSError, version_files.VersionFileError) as error:
        raise ReleaseError(str(error)) from error


def newest_dated_version(root: Path) -> str:
    """The version of the first CHANGELOG section that carries a release date (YYYY-MM-DD)."""
    text = (root / "CHANGELOG.md").read_text(encoding="utf-8")
    match = re.search(r"^## \[([0-9]+\.[0-9]+\.[0-9]+)\] - [0-9]{4}-[0-9]{2}-[0-9]{2}[ \t]*$", text, re.MULTILINE)
    if match is None:
        raise ReleaseError("CHANGELOG.md has no dated release section")
    return match.group(1)


def changelog_section(root: Path, version: str) -> tuple[str, str]:
    """Returns (date, body) of the CHANGELOG section for version."""
    text = (root / "CHANGELOG.md").read_text(encoding="utf-8")
    match = re.search(
        rf"^## \[{re.escape(version)}\] - (\S+)[ \t]*\n(.*?)(?=^## \[|\Z)", text, re.MULTILINE | re.DOTALL
    )
    if match is None:
        raise ReleaseError(f"CHANGELOG.md has no section for {version}")
    return match.group(1), match.group(2).strip()
