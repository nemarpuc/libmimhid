#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
"""Check that dist/ holds exactly the release's assets, then write the manifest and checksums."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

import generate_spdx
import release_names


def collect(dist: Path, version: str, source_sha: str) -> None:
    expected = set(release_names.expected_assets(version)) - {"release-manifest.json", "SHA256SUMS"}
    present = {p.name for p in dist.iterdir() if p.is_file()} - {"release-manifest.json", "SHA256SUMS"}
    missing, extra = sorted(expected - present), sorted(present - expected)
    if missing or extra:
        raise release_names.ReleaseError(f"asset set mismatch; missing {missing}, unexpected {extra}")
    assets = []
    for name in sorted(expected):
        path = dist / name
        if path.stat().st_size == 0:
            raise release_names.ReleaseError(f"{name} is empty")
        assets.append({"name": name, "size": path.stat().st_size, "sha256": generate_spdx.sha256_file(path)})
    for asset in assets:
        # An SPDX sidecar must describe the archive of the same name.
        if asset["name"].endswith(".spdx.json"):
            document = json.loads((dist / asset["name"]).read_text(encoding="utf-8"))
            stem = asset["name"][: -len(".spdx.json")]
            archive = next(a for a in assets if a["name"] in (stem + ".tar.gz", stem + ".zip"))
            if document.get("name") not in (stem + ".tar.gz", stem + ".zip") or document["name"] != archive["name"]:
                raise release_names.ReleaseError(f"{asset['name']} describes the wrong archive")
    manifest = {
        "project": release_names.PROJECT,
        "version": version,
        "repository": release_names.REPOSITORY,
        "source_commit": source_sha,
        "assets": assets,
    }
    (dist / "release-manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    lines = [f"{a['sha256']}  {a['name']}" for a in assets]
    manifest_hash = generate_spdx.sha256_file(dist / "release-manifest.json")
    lines.append(f"{manifest_hash}  release-manifest.json")
    (dist / "SHA256SUMS").write_text("\n".join(sorted(lines, key=lambda l: l.split("  ", 1)[1])) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dist", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--source-sha", required=True)
    args = parser.parse_args()
    try:
        collect(args.dist, args.version, args.source_sha)
    except release_names.ReleaseError as error:
        print(f"release collection failed: {error}", file=sys.stderr)
        return 1
    print(f"{len(release_names.expected_assets(args.version))} assets in {args.dist}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
