#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
"""Create one deterministic binary archive (and its SPDX sidecar) from an install tree."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import gzip
import hashlib
import io
import json
from pathlib import Path
import stat
import sys
import tarfile
import zipfile

import generate_spdx
import release_names

DOC = "share/doc/libmimhid"


def collect(stage: Path, platform: str, variant: str) -> list[Path]:
    """The install-tree files that belong to the archive, relative to stage."""
    everything = sorted(p.relative_to(stage) for p in stage.rglob("*") if p.is_file() or p.is_symlink())
    if variant != "runtime":
        return everything
    keep = []
    for path in everything:
        text = path.as_posix()
        if text.startswith(DOC + "/"):
            keep.append(path)
        elif platform == "windows" and text.startswith("bin/") and text.endswith(".dll"):
            keep.append(path)
        elif platform == "linux" and text.startswith("lib/") and ".so" in path.name:
            keep.append(path)
        elif platform == "macos" and text.startswith("lib/") and path.name.endswith(".dylib"):
            keep.append(path)
    return keep


def validate_layout(stage: Path, platform: str, variant: str) -> None:
    required = [
        f"{DOC}/LICENSE",
        f"{DOC}/NOTICE",
        f"{DOC}/THIRD_PARTY_NOTICES.md",
        f"{DOC}/CHANGELOG.md",
    ]
    if variant in ("shared", "static"):
        required += ["include/mimhid.h", "include/mimhid.hpp"]
        flavor = "shared" if variant == "shared" else "static"
        required += [f"lib/cmake/mimhid/mimhid-{flavor}-targets.cmake", "lib/cmake/mimhid/mimhid-config.cmake"]
    if variant == "shared":
        names = {"linux": "lib/libmimhid.so", "macos": "lib/libmimhid.dylib", "windows": "bin/mimhid.dll"}
        required.append(names[platform])
        if platform == "windows":
            required.append("lib/mimhid.lib")
    if variant == "static":
        required.append("lib/mimhid_static.lib" if platform == "windows" else "lib/libmimhid.a")
    for relative in required:
        if not (stage / relative).exists():
            raise release_names.ReleaseError(f"required package file is missing: {relative}")
    if variant == "static":
        for path in stage.rglob("*"):
            if path.is_file() and (".so" in path.name or path.suffix in (".dll", ".dylib")):
                raise release_names.ReleaseError(f"static package contains a shared library: {path.name}")


def write_tar_gz(out: Path, root_name: str, stage: Path, files: list[Path], epoch: int) -> None:
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w", format=tarfile.PAX_FORMAT) as tar:
        directories: set[str] = set()
        for relative in files:
            for parent in list(relative.parents)[:-1]:
                directories.add(parent.as_posix())
        for directory in sorted(directories | {""}):
            info = tarfile.TarInfo(f"{root_name}/{directory}".rstrip("/"))
            info.type = tarfile.DIRTYPE
            info.mode = 0o755
            info.mtime = epoch
            tar.addfile(info)
        for relative in files:
            source = stage / relative
            info = tarfile.TarInfo(f"{root_name}/{relative.as_posix()}")
            info.mtime = epoch
            if source.is_symlink():
                info.type = tarfile.SYMTYPE
                info.linkname = str(source.readlink())
                info.mode = 0o777
                tar.addfile(info)
                continue
            data = source.read_bytes()
            executable = bool(source.stat().st_mode & stat.S_IXUSR) or relative.parts[0] == "bin"
            info.mode = 0o755 if executable or ".so" in source.name or source.suffix == ".dylib" else 0o644
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
    with out.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0, compresslevel=9) as stream:
            stream.write(buffer.getvalue())


def write_zip(out: Path, root_name: str, stage: Path, files: list[Path], epoch: int) -> None:
    stamp = datetime.fromtimestamp(max(epoch, 315532800), tz=timezone.utc).timetuple()[:6]
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for relative in files:
            info = zipfile.ZipInfo(f"{root_name}/{relative.as_posix()}", date_time=stamp)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, (stage / relative).read_bytes())


def archive_members(path: Path) -> list[tuple[str, str]]:
    members = []
    if path.name.endswith(".zip"):
        with zipfile.ZipFile(path) as archive:
            for info in sorted(archive.infolist(), key=lambda i: i.filename):
                if not info.is_dir():
                    members.append((info.filename, hashlib.sha256(archive.read(info)).hexdigest()))
    else:
        with tarfile.open(path) as tar:
            for info in sorted(tar.getmembers(), key=lambda i: i.name):
                if info.isfile():
                    members.append((info.name, hashlib.sha256(tar.extractfile(info).read()).hexdigest()))
    return members


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--platform", required=True)
    parser.add_argument("--arch", required=True)
    parser.add_argument("--variant", required=True, choices=release_names.VARIANTS)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--epoch", type=int, required=True, help="commit time, seconds since 1970")
    parser.add_argument("--build-metadata", action="append", default=[], metavar="KEY=VALUE")
    args = parser.parse_args()
    try:
        if len(args.source_sha) != 40 or any(c not in "0123456789abcdef" for c in args.source_sha):
            raise release_names.ReleaseError("source SHA must be 40 lowercase hexadecimal characters")
        base = release_names.base_name(args.version, args.platform, args.arch, args.variant)
        validate_layout(args.stage, args.platform, args.variant)
        metadata = {
            "project": release_names.PROJECT,
            "version": args.version,
            "platform": args.platform,
            "arch": args.arch,
            "variant": args.variant,
            "source_commit": args.source_sha,
            "source_epoch": args.epoch,
        }
        for item in args.build_metadata:
            key, _, value = item.partition("=")
            if not key or not _:
                raise release_names.ReleaseError(f"--build-metadata expects KEY=VALUE, got {item!r}")
            metadata[key] = value
        (args.stage / DOC).mkdir(parents=True, exist_ok=True)
        (args.stage / DOC / "build-metadata.json").write_text(
            json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")

        files = collect(args.stage, args.platform, args.variant)
        args.out_dir.mkdir(parents=True, exist_ok=True)
        out = args.out_dir / release_names.archive_name(args.version, args.platform, args.arch, args.variant)
        if args.platform == "windows":
            write_zip(out, base, args.stage, files, args.epoch)
        else:
            write_tar_gz(out, base, args.stage, files, args.epoch)
        if args.variant != "runtime":
            created = datetime.fromtimestamp(args.epoch, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
            document = generate_spdx.spdx_document(out, args.version, args.source_sha, created, archive_members(out))
            generate_spdx.write_spdx(out.with_name(base + ".spdx.json"), document)
    except release_names.ReleaseError as error:
        print(f"packaging failed: {error}", file=sys.stderr)
        return 1
    print(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
