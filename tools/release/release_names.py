#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
"""Names, versions, and the exact asset set of a GitHub Release."""

from __future__ import annotations

from pathlib import Path
import re

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
VARIANTS = ("shared", "static", "runtime")

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


def base_name(version: str, platform: str, arch: str, variant: str) -> str:
    check_target(platform, arch)
    if variant not in VARIANTS:
        raise ReleaseError(f"unsupported release variant: {variant!r}")
    if VERSION_RE.fullmatch(version) is None:
        raise ReleaseError("version must have the exact form MAJOR.MINOR.PATCH")
    middle = f"{platform}-{arch}"
    if platform == "linux":
        middle += "-ubuntu22.04"
    if variant == "runtime":
        # The shared and static names stay unchanged; the runtime bundle appends its role.
        return f"{PROJECT}-{version}-{middle}-runtime"
    return f"{PROJECT}-{version}-{middle}-{variant}"


def archive_name(version: str, platform: str, arch: str, variant: str) -> str:
    extension = "zip" if platform == "windows" else "tar.gz"
    return f"{base_name(version, platform, arch, variant)}.{extension}"


def source_name(version: str) -> str:
    return f"{PROJECT}-{version}-source.tar.gz"


def firmware_names(version: str) -> list[str]:
    return [
        f"mimhid_firmware-{target}-{version}.{suffix}"
        for target, suffixes in FIRMWARE_SUFFIXES.items()
        for suffix in suffixes
    ]


def expected_assets(version: str) -> list[str]:
    """Every file a release uploads: 12 archives, 12 SPDX sidecars, 6 runtime bundles, the
    source archive, the manifest, the checksums, and the firmware images."""
    names: list[str] = []
    for platform, arch in TARGETS:
        for variant in ("shared", "static"):
            archive = archive_name(version, platform, arch, variant)
            names.append(archive)
            names.append(archive.rsplit(".tar.gz", 1)[0].rsplit(".zip", 1)[0] + ".spdx.json")
        names.append(archive_name(version, platform, arch, "runtime"))
    names.append(source_name(version))
    names.extend(firmware_names(version))
    names.extend(["release-manifest.json", "SHA256SUMS"])
    return sorted(names)


def read_versions(root: Path) -> dict[str, str]:
    """The version recorded in every file that carries one."""
    header = (root / "include/mimhid.h").read_text(encoding="utf-8")
    parts = []
    for part in ("MAJOR", "MINOR", "PATCH"):
        match = re.search(rf"^#define MIMHID_VERSION_{part} ([0-9]+)$", header, re.MULTILINE)
        if match is None:
            raise ReleaseError(f"include/mimhid.h has no MIMHID_VERSION_{part}")
        parts.append(match.group(1))
    firmware = (root / "firmware/common/core.h").read_text(encoding="utf-8")
    fw = []
    for part in ("MAJOR", "MINOR", "PATCH"):
        match = re.search(rf"^#define FW_VERSION_{part} ([0-9]+)$", firmware, re.MULTILINE)
        if match is None:
            raise ReleaseError(f"firmware/common/core.h has no FW_VERSION_{part}")
        fw.append(match.group(1))
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"^project\(libmimhid VERSION ([0-9.]+)", cmake, re.MULTILINE)
    if match is None:
        raise ReleaseError("CMakeLists.txt has no project version")
    return {
        "include/mimhid.h": ".".join(parts),
        "firmware/common/core.h": ".".join(fw),
        "CMakeLists.txt": match.group(1),
    }


def changelog_section(root: Path, version: str) -> tuple[str, str]:
    """Returns (date, body) of the CHANGELOG section for version."""
    text = (root / "CHANGELOG.md").read_text(encoding="utf-8")
    match = re.search(
        rf"^## \[{re.escape(version)}\] - (\S+)[ \t]*\n(.*?)(?=^## \[|\Z)", text, re.MULTILINE | re.DOTALL
    )
    if match is None:
        raise ReleaseError(f"CHANGELOG.md has no section for {version}")
    return match.group(1), match.group(2).strip()
