#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
"""Deterministic archive of the tagged tree."""

from __future__ import annotations

import argparse
import gzip
import io
from pathlib import Path
import subprocess
import sys
import tarfile

import release_names


def package_source(root: Path, version: str, commit: str, out_dir: Path) -> Path:
    epoch = int(subprocess.check_output(["git", "-C", str(root), "show", "-s", "--format=%ct", commit], text=True))
    prefix = f"{release_names.PROJECT}-{version}"
    raw = subprocess.check_output(["git", "-C", str(root), "archive", "--format=tar", commit])
    out = io.BytesIO()
    with tarfile.open(fileobj=io.BytesIO(raw)) as source, tarfile.open(fileobj=out, mode="w", format=tarfile.PAX_FORMAT) as tar:
        for member in sorted(source.getmembers(), key=lambda m: m.name):
            if member.name == "pax_global_header":
                continue
            member.name = f"{prefix}/{member.name}"
            member.uid = member.gid = 0
            member.uname = member.gname = ""
            member.mtime = epoch
            member.pax_headers = {}
            if member.isfile():
                member.mode = 0o755 if member.mode & 0o111 else 0o644
                tar.addfile(member, source.extractfile(member))
            else:
                member.mode = 0o755 if member.isdir() else member.mode
                tar.addfile(member)
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / release_names.source_name(version)
    with path.open("wb") as raw_out:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw_out, mtime=0, compresslevel=9) as stream:
            stream.write(out.getvalue())
    return path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--version", required=True)
    parser.add_argument("--commit", default="HEAD")
    parser.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args()
    print(package_source(args.root, args.version, args.commit, args.out_dir))
    return 0


if __name__ == "__main__":
    sys.exit(main())
