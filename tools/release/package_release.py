#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimusb contributors
"""Create one deterministic binary archive from an install tree that holds both library kinds.

The SPDX document of the release covers every archive; generate_spdx.py writes it once all
archives are together."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import gzip
import io
import json
import re
from pathlib import Path
import shutil
import stat
import sys
import tarfile
import zipfile

import release_names

DOC = "share/doc/libmimusb"
SOURCE_ROOT = Path(__file__).resolve().parents[2]


def collect(stage: Path) -> list[Path]:
    """The install-tree files that belong to the archive, relative to stage."""
    return sorted(p.relative_to(stage) for p in stage.rglob("*") if p.is_file() or p.is_symlink())


SHARED_LIBRARY = {"linux": "lib/libmimusb.so", "macos": "lib/libmimusb.dylib", "windows": "bin/mimusb.dll"}
# With both kinds built the static library is called mimusb_static everywhere (CMakeLists.txt).
STATIC_LIBRARY = {"linux": "lib/libmimusb_static.a", "macos": "lib/libmimusb_static.a",
                  "windows": "lib/mimusb_static.lib"}
CMAKE_TRUE = {"ON", "TRUE", "1", "YES", "Y"}


def validate_layout(stage: Path, platform: str) -> None:
    """One archive per platform carries both library kinds and the package files of both."""
    required = [
        f"{DOC}/LICENSE",
        f"{DOC}/NOTICE",
        f"{DOC}/THIRD_PARTY_NOTICES.md",
        f"{DOC}/CHANGELOG.md",
    ]
    required.append(f"{DOC}/examples/README.md")
    required += ["include/mimusb.h", "include/mimusb.hpp"]
    required += ["lib/cmake/mimusb/mimusb-config.cmake", "lib/cmake/mimusb/mimusb-config-version.cmake"]
    required += [f"lib/cmake/mimusb/mimusb-{component}-targets.cmake" for component in release_names.COMPONENTS]
    required += ["lib/pkgconfig/mimusb.pc", "lib/pkgconfig/mimusb-static.pc"]
    required += [SHARED_LIBRARY[platform], STATIC_LIBRARY[platform]]
    if platform == "windows":
        required.append("lib/mimusb.lib")
    for relative in required:
        if not (stage / relative).exists():
            raise release_names.ReleaseError(f"required package file is missing: {relative}")
    # find_package(mimusb COMPONENTS x) finds x only when the configured tree built it, so the
    # installed config must say that both were built.
    config = (stage / "lib/cmake/mimusb/mimusb-config.cmake").read_text(encoding="utf-8")
    for component in release_names.COMPONENTS:
        match = re.search(rf"^set\(_MIMUSB_BUILT_{component.upper()} (\S+)\)", config, re.MULTILINE)
        if match is None or match.group(1).upper() not in CMAKE_TRUE:
            raise release_names.ReleaseError(f"the installed CMake package was not built with the {component} library")


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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--platform", required=True)
    parser.add_argument("--arch", required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--epoch", type=int, required=True, help="commit time, seconds since 1970")
    parser.add_argument("--build-metadata", action="append", default=[], metavar="KEY=VALUE")
    parser.add_argument("--source-root", type=Path, default=SOURCE_ROOT,
                        help="the source tree whose examples/ goes into the archive")
    args = parser.parse_args()
    try:
        if len(args.source_sha) != 40 or any(c not in "0123456789abcdef" for c in args.source_sha):
            raise release_names.ReleaseError("source SHA must be 40 lowercase hexadecimal characters")
        base = release_names.base_name(args.version, args.platform, args.arch)
        shutil.copytree(args.source_root / "examples", args.stage / DOC / "examples",
                        dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
        validate_layout(args.stage, args.platform)
        metadata = {
            "project": release_names.PROJECT,
            "version": args.version,
            "platform": args.platform,
            "arch": args.arch,
            "components": list(release_names.COMPONENTS),
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

        files = collect(args.stage)
        args.out_dir.mkdir(parents=True, exist_ok=True)
        out = args.out_dir / release_names.archive_name(args.version, args.platform, args.arch)
        if args.platform == "windows":
            write_zip(out, base, args.stage, files, args.epoch)
        else:
            write_tar_gz(out, base, args.stage, files, args.epoch)
    except release_names.ReleaseError as error:
        print(f"packaging failed: {error}", file=sys.stderr)
        return 1
    print(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
