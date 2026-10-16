#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
"""SPDX 2.3 SBOM sidecar for one binary archive."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

import release_names


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def spdx_document(archive: Path, version: str, source_sha: str, created: str,
                  members: list[tuple[str, str]]) -> dict:
    """members: (path inside the archive, sha256) for every regular file."""
    name = archive.name
    package_id = "SPDXRef-Package"
    files = []
    relationships = [
        {"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES",
         "relatedSpdxElement": package_id}
    ]
    for index, (path, digest) in enumerate(members):
        identifier = f"SPDXRef-File-{index}"
        files.append({
            "SPDXID": identifier,
            "fileName": path,
            "checksums": [{"algorithm": "SHA256", "checksumValue": digest}],
            "licenseConcluded": "MIT",
            "copyrightText": "Copyright (c) 2026 libmimhid contributors",
        })
        relationships.append({"spdxElementId": package_id, "relationshipType": "CONTAINS",
                              "relatedSpdxElement": identifier})
    return {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": name,
        "documentNamespace": f"{release_names.REPOSITORY_URL}/releases/{version}/{name}",
        "creationInfo": {"created": created, "creators": ["Tool: libmimhid-release-tools"]},
        "packages": [{
            "SPDXID": package_id,
            "name": release_names.PROJECT,
            "versionInfo": version,
            "downloadLocation": f"{release_names.REPOSITORY_URL}/releases/download/v{version}/{name}",
            "filesAnalyzed": True,
            "homepage": release_names.REPOSITORY_URL,
            "sourceInfo": f"built from commit {source_sha}",
            "licenseConcluded": "MIT",
            "licenseDeclared": "MIT",
            "copyrightText": "Copyright (c) 2026 libmimhid contributors",
            "packageVerificationCode": {
                "packageVerificationCodeValue": hashlib.sha1(
                    "".join(sorted(d for _, d in members)).encode()
                ).hexdigest()
            },
            "hasFiles": [f["SPDXID"] for f in files],
        }],
        "files": files,
        "relationships": relationships,
    }


def write_spdx(path: Path, document: dict) -> None:
    path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")
