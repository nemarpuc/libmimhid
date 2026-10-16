#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
"""Tests of the release tooling; they need nothing but the standard library."""

from pathlib import Path
import json
import sys
import tarfile
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/release"))

import collect_release  # noqa: E402
import package_release  # noqa: E402
import release_names  # noqa: E402
import validate_release  # noqa: E402

SHA = "0123456789abcdef0123456789abcdef01234567"


def fake_stage(root: Path, platform: str, variant: str) -> Path:
    stage = root / f"stage-{platform}-{variant}"
    files = {
        "include/mimhid.h": "h", "include/mimhid.hpp": "hpp",
        "share/doc/libmimhid/LICENSE": "MIT",
        "share/doc/libmimhid/NOTICE": "notice",
        "share/doc/libmimhid/THIRD_PARTY_NOTICES.md": "third-party",
        "share/doc/libmimhid/CHANGELOG.md": "log",
        "lib/cmake/mimhid/mimhid-config.cmake": "c",
        f"lib/cmake/mimhid/mimhid-{'shared' if variant != 'static' else 'static'}-targets.cmake": "t",
    }
    if variant == "static":
        files["lib/mimhid_static.lib" if platform == "windows" else "lib/libmimhid.a"] = "a"
    else:
        if platform == "windows":
            files["bin/mimhid.dll"] = "dll"
            files["lib/mimhid.lib"] = "lib"
        elif platform == "macos":
            files["lib/libmimhid.dylib"] = "dylib"
        else:
            files["lib/libmimhid.so"] = "so"
    for name, text in files.items():
        path = stage / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    return stage


class Names(unittest.TestCase):
    def test_asset_set(self):
        assets = release_names.expected_assets("1.2.3")
        self.assertEqual(len(assets), len(set(assets)))
        self.assertEqual(len(assets), 12 + 12 + 6 + 1 + 7 + 1 + 2)
        self.assertIn("libmimhid-1.2.3-linux-aarch64-ubuntu22.04-static.tar.gz", assets)
        self.assertIn("libmimhid-1.2.3-macos-arm64-runtime.tar.gz", assets)
        self.assertIn("libmimhid-1.2.3-windows-x86_64-shared.spdx.json", assets)
        self.assertIn("libmimhid-1.2.3-windows-arm64-runtime.zip", assets)
        self.assertIn("mimhid_firmware-rp2040-1.2.3.uf2", assets)
        self.assertIn("mimhid_firmware-THIRD_PARTY_NOTICES-1.2.3.txt", assets)

    def test_rejects_bad_input(self):
        with self.assertRaises(release_names.ReleaseError):
            release_names.archive_name("1.2", "linux", "x86_64", "shared")
        with self.assertRaises(release_names.ReleaseError):
            release_names.archive_name("1.2.3", "linux", "riscv", "shared")
        with self.assertRaises(release_names.ReleaseError):
            release_names.archive_name("1.2.3", "linux", "x86_64", "dev")


class Validate(unittest.TestCase):
    def test_current_tree_versions_agree(self):
        versions = set(release_names.read_versions(ROOT).values())
        self.assertEqual(len(versions), 1, versions)

    def test_bad_tags(self):
        for tag in ("0.3.0", "v0.3", "v9.9.9"):
            with self.assertRaises(release_names.ReleaseError):
                validate_release.validate(ROOT, tag)

    def test_changelog_must_be_dated(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "include").mkdir()
            (root / "firmware/common").mkdir(parents=True)
            (root / "LICENSE").write_text("MIT\n")
            (root / "NOTICE").write_text("NOTICE\n")
            (root / "THIRD_PARTY_NOTICES.md").write_text("THIRD_PARTY\n")
            (root / "include/mimhid.h").write_text(
                "#define MIMHID_VERSION_MAJOR 1\n#define MIMHID_VERSION_MINOR 2\n#define MIMHID_VERSION_PATCH 3\n")
            (root / "firmware/common/core.h").write_text(
                "#define FW_VERSION_MAJOR 1\n#define FW_VERSION_MINOR 2\n#define FW_VERSION_PATCH 3\n")
            (root / "CMakeLists.txt").write_text("project(libmimhid VERSION 1.2.3 LANGUAGES C)\n")
            (root / "CHANGELOG.md").write_text("# Changelog\n\n## [1.2.3] - unreleased\n\n- x\n")
            with self.assertRaises(release_names.ReleaseError):
                validate_release.validate(root, "v1.2.3")
            (root / "CHANGELOG.md").write_text("# Changelog\n\n## [1.2.3] - 2026-01-02\n\n- x\n\n## [1.2.2] - 2026-01-01\n\n- y\n")
            version, notes = validate_release.validate(root, "v1.2.3")
            self.assertEqual((version, notes), ("1.2.3", "- x"))
            (root / "CMakeLists.txt").write_text("project(libmimhid VERSION 1.2.4 LANGUAGES C)\n")
            with self.assertRaises(release_names.ReleaseError):
                validate_release.validate(root, "v1.2.3")
            (root / "CMakeLists.txt").write_text("project(libmimhid VERSION 1.2.3 LANGUAGES C)\n")
            (root / "THIRD_PARTY_NOTICES.md").unlink()
            with self.assertRaises(release_names.ReleaseError):
                validate_release.validate(root, "v1.2.3")


class Package(unittest.TestCase):
    def run_package(self, tmp, platform, variant, out):
        stage = fake_stage(Path(tmp), platform, variant)
        import subprocess
        arch = "x86_64"
        subprocess.check_call([sys.executable, str(ROOT / "tools/release/package_release.py"),
                               "--stage", str(stage), "--out-dir", str(out), "--version", "1.2.3",
                               "--platform", platform, "--arch", arch, "--variant", variant,
                               "--source-sha", SHA, "--epoch", "1700000000"],
                              stdout=subprocess.DEVNULL)
        return out / release_names.archive_name("1.2.3", platform, arch, variant)

    def test_deterministic_and_complete(self):
        with tempfile.TemporaryDirectory() as tmp:
            first = self.run_package(tmp, "linux", "shared", Path(tmp) / "a")
            second = self.run_package(tmp, "linux", "shared", Path(tmp) / "b")
            self.assertEqual(first.read_bytes(), second.read_bytes())
            with tarfile.open(first) as tar:
                names = tar.getnames()
            root = release_names.base_name("1.2.3", "linux", "x86_64", "shared")
            self.assertIn(f"{root}/lib/libmimhid.so", names)
            self.assertIn(f"{root}/share/doc/libmimhid/build-metadata.json", names)
            spdx = json.loads(first.with_name(root + ".spdx.json").read_text())
            self.assertEqual(spdx["spdxVersion"], "SPDX-2.3")
            self.assertEqual(spdx["name"], first.name)

    def test_windows_zip_and_runtime(self):
        with tempfile.TemporaryDirectory() as tmp:
            shared = self.run_package(tmp, "windows", "shared", Path(tmp) / "w")
            self.assertTrue(zipfile.is_zipfile(shared))
            runtime = self.run_package(tmp, "windows", "runtime", Path(tmp) / "w")
            with zipfile.ZipFile(runtime) as archive:
                names = archive.namelist()
            self.assertTrue(any(n.endswith("bin/mimhid.dll") for n in names))
            self.assertFalse(any(n.endswith("mimhid.h") for n in names))
            self.assertFalse(any(n.endswith(".lib") for n in names))

    def test_missing_file_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            stage = fake_stage(Path(tmp), "linux", "shared")
            (stage / "lib/libmimhid.so").unlink()
            with self.assertRaises(release_names.ReleaseError):
                package_release.validate_layout(stage, "linux", "shared")


class Collect(unittest.TestCase):
    def test_exact_set(self):
        with tempfile.TemporaryDirectory() as tmp:
            dist = Path(tmp)
            for name in release_names.expected_assets("1.2.3"):
                if name in ("release-manifest.json", "SHA256SUMS"):
                    continue
                if name.endswith(".spdx.json"):
                    stem = name[: -len(".spdx.json")]
                    archive = stem + (".zip" if "-windows-" in stem else ".tar.gz")
                    (dist / name).write_text(json.dumps({"name": archive}))
                else:
                    (dist / name).write_bytes(b"x")
            collect_release.collect(dist, "1.2.3", SHA)
            sums = (dist / "SHA256SUMS").read_text().splitlines()
            self.assertEqual(len(sums), len(release_names.expected_assets("1.2.3")) - 1)
            (dist / "stray.txt").write_text("x")
            with self.assertRaises(release_names.ReleaseError):
                collect_release.collect(dist, "1.2.3", SHA)
            (dist / "stray.txt").unlink()
            (dist / release_names.source_name("1.2.3")).unlink()
            with self.assertRaises(release_names.ReleaseError):
                collect_release.collect(dist, "1.2.3", SHA)


if __name__ == "__main__":
    unittest.main()
