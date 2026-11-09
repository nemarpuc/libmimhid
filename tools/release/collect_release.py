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


def check_sbom(dist: Path, version: str, digests: dict[str, str]) -> None:
    """The release's one SPDX document must describe exactly the binary archives in dist/, by
    checksum, each generated from the source archive in dist/."""
    name = release_names.sbom_name(version)
    try:
        document = json.loads((dist / name).read_text(encoding="utf-8"))
    except ValueError as error:
        raise release_names.ReleaseError(f"{name} is not JSON: {error}") from error
    if document.get("spdxVersion") != "SPDX-2.3" or document.get("name") != name[: -len(".spdx.json")]:
        raise release_names.ReleaseError(f"{name} is not the SPDX 2.3 document of release {version}")
    packages = {p.get("packageFileName"): p for p in document.get("packages", [])}
    expected = set(release_names.archive_names(version)) | {release_names.source_name(version)}
    if set(packages) != expected:
        raise release_names.ReleaseError(
            f"{name} describes {sorted(n for n in packages if n)}, the release has {sorted(expected)}")
    ids = {}
    for file_name, package in packages.items():
        checksums = {c.get("algorithm"): c.get("checksumValue") for c in package.get("checksums", [])}
        if checksums.get("SHA256") != digests[file_name]:
            raise release_names.ReleaseError(f"{name} has the wrong SHA-256 for {file_name}")
        ids[file_name] = package.get("SPDXID")
    relations = {(r.get("spdxElementId"), r.get("relationshipType"), r.get("relatedSpdxElement"))
                 for r in document.get("relationships", [])}
    source_id = ids[release_names.source_name(version)]
    for archive in release_names.archive_names(version):
        if ("SPDXRef-DOCUMENT", "DESCRIBES", ids[archive]) not in relations:
            raise release_names.ReleaseError(f"{name} does not describe {archive}")
        if (ids[archive], "GENERATED_FROM", source_id) not in relations:
            raise release_names.ReleaseError(f"{name} does not tie {archive} to the source archive")


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
    check_sbom(dist, version, {a["name"]: a["sha256"] for a in assets})
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
