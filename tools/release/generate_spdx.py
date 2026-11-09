#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
"""One SPDX 2.3 document for a release: a package per binary archive, each generated from the
source package."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import sys
import tarfile
import zipfile

import release_names

COPYRIGHT = "Copyright (c) 2026 libmimhid contributors"
SOURCE_ID = "SPDXRef-Package-source"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


Member = tuple[str, str, str]  # (path inside the archive, sha256, sha1)


def member(path: str, data: bytes) -> Member:
    return (path, hashlib.sha256(data).hexdigest(), hashlib.sha1(data).hexdigest())


def archive_members(path: Path) -> list[Member]:
    """Every regular file of the archive, sorted by path."""
    members = []
    if path.name.endswith(".zip"):
        with zipfile.ZipFile(path) as archive:
            for info in sorted(archive.infolist(), key=lambda i: i.filename):
                if not info.is_dir():
                    members.append(member(info.filename, archive.read(info)))
    else:
        with tarfile.open(path) as tar:
            for info in sorted(tar.getmembers(), key=lambda i: i.name):
                if info.isfile():
                    members.append(member(info.name, tar.extractfile(info).read()))
    return members


def package_id(archive_name: str) -> str:
    """An SPDX identifier ([A-Za-z0-9.-]+) that names the archive's platform."""
    stem = re.sub(r"\.(tar\.gz|zip)$", "", archive_name)
    return "SPDXRef-Package-" + re.sub(r"[^A-Za-z0-9.-]", "-", stem)


def spdx_document(version: str, source_sha: str, created: str, source: tuple[str, str],
                  archives: list[tuple[str, str, list[Member]]]) -> dict:
    """source: (file name, sha256) of the source archive.
    archives: (file name, sha256, members) for every binary archive; members as archive_members."""
    download = f"{release_names.REPOSITORY_URL}/releases/download/v{version}"
    source_name, source_digest = source
    packages = [{
        "SPDXID": SOURCE_ID,
        "name": f"{release_names.PROJECT}-source",
        "versionInfo": version,
        "packageFileName": source_name,
        "downloadLocation": f"{download}/{source_name}",
        "checksums": [{"algorithm": "SHA256", "checksumValue": source_digest}],
        "filesAnalyzed": False,
        "homepage": release_names.REPOSITORY_URL,
        "sourceInfo": f"git archive of commit {source_sha}",
        "licenseConcluded": "NOASSERTION",
        "licenseDeclared": "MIT",
        "copyrightText": COPYRIGHT,
    }]
    files = []
    relationships = []
    for archive_name, digest, members in sorted(archives):
        identifier = package_id(archive_name)
        file_ids = []
        for index, (path, member_sha256, member_sha1) in enumerate(members):
            file_id = f"{identifier.replace('SPDXRef-Package-', 'SPDXRef-File-')}-{index}"
            file_ids.append(file_id)
            files.append({
                "SPDXID": file_id,
                "fileName": path,
                "checksums": [{"algorithm": "SHA1", "checksumValue": member_sha1},
                              {"algorithm": "SHA256", "checksumValue": member_sha256}],
                "licenseConcluded": "MIT",
                "copyrightText": COPYRIGHT,
            })
            relationships.append({"spdxElementId": identifier, "relationshipType": "CONTAINS",
                                  "relatedSpdxElement": file_id})
        packages.append({
            "SPDXID": identifier,
            "name": release_names.PROJECT,
            "versionInfo": version,
            "packageFileName": archive_name,
            "downloadLocation": f"{download}/{archive_name}",
            "checksums": [{"algorithm": "SHA256", "checksumValue": digest}],
            "filesAnalyzed": True,
            "homepage": release_names.REPOSITORY_URL,
            "sourceInfo": f"built from commit {source_sha}",
            "licenseConcluded": "MIT",
            "licenseDeclared": "MIT",
            "copyrightText": COPYRIGHT,
            "packageVerificationCode": {
                # SPDX 2.3 clause 7.9: SHA-1 over the sorted SHA-1 values of the files.
                "packageVerificationCodeValue": hashlib.sha1(
                    "".join(sorted(sha1 for _, _, sha1 in members)).encode()
                ).hexdigest()
            },
            "hasFiles": file_ids,
        })
        relationships.insert(0, {"spdxElementId": identifier, "relationshipType": "GENERATED_FROM",
                                 "relatedSpdxElement": SOURCE_ID})
        relationships.insert(0, {"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES",
                                 "relatedSpdxElement": identifier})
    name = release_names.sbom_name(version)[: -len(".spdx.json")]
    return {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": name,
        "documentNamespace": f"{release_names.REPOSITORY_URL}/releases/{version}/{release_names.sbom_name(version)}",
        "creationInfo": {"created": created, "creators": ["Tool: libmimhid-release-tools"]},
        "packages": packages,
        "files": files,
        "relationships": relationships,
    }


def release_document(dist: Path, version: str, source_sha: str, epoch: int) -> dict:
    """The document for the archives and the source archive already in dist/."""
    archives = []
    for archive_name in release_names.archive_names(version):
        path = dist / archive_name
        if not path.is_file():
            raise release_names.ReleaseError(f"{archive_name} is missing from {dist}")
        archives.append((archive_name, sha256_file(path), archive_members(path)))
    source = dist / release_names.source_name(version)
    if not source.is_file():
        raise release_names.ReleaseError(f"{source.name} is missing from {dist}")
    created = datetime.fromtimestamp(epoch, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    return spdx_document(version, source_sha, created, (source.name, sha256_file(source)), archives)


def write_spdx(path: Path, document: dict) -> None:
    path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dist", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--epoch", type=int, required=True, help="commit time, seconds since 1970")
    args = parser.parse_args()
    try:
        out = args.dist / release_names.sbom_name(args.version)
        write_spdx(out, release_document(args.dist, args.version, args.source_sha, args.epoch))
    except release_names.ReleaseError as error:
        print(f"SBOM generation failed: {error}", file=sys.stderr)
        return 1
    print(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
