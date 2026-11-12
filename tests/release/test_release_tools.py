#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimusb contributors
"""Tests of the release tooling; they need nothing but the standard library."""

from pathlib import Path
import hashlib
import json
import re
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/release"))

import collect_release  # noqa: E402
import generate_spdx  # noqa: E402
import package_release  # noqa: E402
import release_names  # noqa: E402
import validate_release  # noqa: E402

SHA = "0123456789abcdef0123456789abcdef01234567"


def fake_stage(root: Path, platform: str) -> Path:
    """An install tree configured with both MIMUSB_BUILD_SHARED and MIMUSB_BUILD_STATIC."""
    stage = root / f"stage-{platform}"
    files = {
        "include/mimusb.h": "h", "include/mimusb.hpp": "hpp",
        "share/doc/libmimusb/LICENSE": "MIT",
        "share/doc/libmimusb/NOTICE": "notice",
        "share/doc/libmimusb/THIRD_PARTY_NOTICES.md": "third-party",
        "share/doc/libmimusb/CHANGELOG.md": "log",
        "lib/cmake/mimusb/mimusb-config.cmake": "set(_MIMUSB_BUILT_SHARED ON)\nset(_MIMUSB_BUILT_STATIC ON)\n",
        "lib/cmake/mimusb/mimusb-config-version.cmake": "v",
        "lib/cmake/mimusb/mimusb-shared-targets.cmake": "t",
        "lib/cmake/mimusb/mimusb-static-targets.cmake": "t",
        "lib/pkgconfig/mimusb.pc": "pc",
        "lib/pkgconfig/mimusb-static.pc": "pc",
    }
    if platform == "windows":
        files["bin/mimusb.dll"] = "dll"
        files["lib/mimusb.lib"] = "lib"
        files["lib/mimusb_static.lib"] = "a"
    else:
        files["lib/libmimusb.dylib" if platform == "macos" else "lib/libmimusb.so"] = "so"
        files["lib/libmimusb_static.a"] = "a"
    for name, text in files.items():
        path = stage / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    return stage


class Names(unittest.TestCase):
    def test_asset_set(self):
        assets = release_names.expected_assets("1.2.3")
        self.assertEqual(len(assets), len(set(assets)))
        # 6 archives, 1 SBOM, the source, 7 firmware images, the firmware notices, manifest, sums.
        self.assertEqual(len(assets), 6 + 1 + 1 + 7 + 1 + 2)
        self.assertIn("libmimusb-1.2.3-linux-aarch64-ubuntu22.04.tar.gz", assets)
        self.assertIn("libmimusb-1.2.3-windows-arm64.zip", assets)
        self.assertIn("libmimusb-1.2.3.spdx.json", assets)
        for gone in ("-shared", "-static", "-runtime"):
            self.assertFalse([a for a in assets if gone in a], gone)
        self.assertEqual(len([a for a in assets if a.endswith(".spdx.json")]), 1)
        self.assertIn("mimusb_firmware-rp2040-1.2.3.uf2", assets)
        self.assertIn("mimusb_firmware-THIRD_PARTY_NOTICES-1.2.3.txt", assets)

    def test_rejects_bad_input(self):
        with self.assertRaises(release_names.ReleaseError):
            release_names.archive_name("1.2", "linux", "x86_64")
        with self.assertRaises(release_names.ReleaseError):
            release_names.archive_name("1.2.3", "linux", "riscv")
        with self.assertRaises(release_names.ReleaseError):
            release_names.sbom_name("1.2")


class Validate(unittest.TestCase):
    @staticmethod
    def write_tree(root, version, cmake=None, binding=None):
        """Writes every file version_files.SITES reads; `cmake` and `binding` override one version."""
        major, minor, patch = version.split(".")
        binding = binding or version
        files = {
            "CMakeLists.txt": f"project(libmimusb VERSION {cmake or version} LANGUAGES C)\n",
            "include/mimusb.h": f"#define MIMUSB_VERSION_MAJOR {major}\n"
            f"#define MIMUSB_VERSION_MINOR {minor}\n#define MIMUSB_VERSION_PATCH {patch}\n",
            "firmware/common/core.h": f"#define FW_VERSION_MAJOR {major}\n"
            f"#define FW_VERSION_MINOR {minor}\n#define FW_VERSION_PATCH {patch}\n",
            "README.md": f"> Version {version}. Version 2.0.0 changed the API.\n",
            "bindings/python/pyproject.toml": f'version = "{binding}"\n',
            "bindings/csharp/MimUsb/MimUsb.csproj": f"<Version>{binding}</Version>\n",
            "bindings/rust/mimusb/Cargo.toml": f'version = "{binding}"\n'
            f'mimusb-sys = {{ path = "../mimusb-sys", version = "{binding}" }}\n',
            "bindings/rust/mimusb-sys/Cargo.toml": f'version = "{binding}"\n',
        }
        for path, text in files.items():
            (root / path).parent.mkdir(parents=True, exist_ok=True)
            (root / path).write_text(text)

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
            (root / "LICENSE").write_text("MIT\n")
            (root / "NOTICE").write_text("NOTICE\n")
            (root / "THIRD_PARTY_NOTICES.md").write_text("THIRD_PARTY\n")
            self.write_tree(root, "1.2.3")
            (root / "CHANGELOG.md").write_text("# Changelog\n\n## [1.2.3] - unreleased\n\n- x\n")
            with self.assertRaises(release_names.ReleaseError):
                validate_release.validate(root, "v1.2.3")
            (root / "CHANGELOG.md").write_text("# Changelog\n\n## [1.2.3] - 2026-01-02\n\n- x\n\n## [1.2.2] - 2026-01-01\n\n- y\n")
            version, notes = validate_release.validate(root, "v1.2.3")
            self.assertEqual((version, notes), ("1.2.3", "- x"))
            self.write_tree(root, "1.2.3", cmake="1.2.4")
            with self.assertRaises(release_names.ReleaseError):
                validate_release.validate(root, "v1.2.3")
            self.write_tree(root, "1.2.3", binding="1.2.2")  # a binding left on the old version
            with self.assertRaises(release_names.ReleaseError):
                validate_release.validate(root, "v1.2.3")
            self.write_tree(root, "1.2.3")
            (root / "THIRD_PARTY_NOTICES.md").unlink()
            with self.assertRaises(release_names.ReleaseError):
                validate_release.validate(root, "v1.2.3")


class DryRun(unittest.TestCase):
    def tree(self, root, version, changelog):
        for name in ("LICENSE", "NOTICE", "THIRD_PARTY_NOTICES.md"):
            (root / name).write_text("x\n")
        Validate.write_tree(root, version)
        (root / "CHANGELOG.md").write_text(changelog)

    def test_version_from_the_newest_dated_section(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.tree(root, "1.2.3", "# Changelog\n\n## [Unreleased]\n\n- next\n\n"
                      "## [1.2.3] - 2026-01-02\n\n- x\n\n## [1.2.2] - 2026-01-01\n\n- y\n")
            self.assertEqual(release_names.newest_dated_version(root), "1.2.3")
            tag, undated, origin = validate_release.dry_run_tag(root)
            self.assertEqual((tag, undated), ("v1.2.3", False))
            self.assertIn("CHANGELOG", origin)

    def test_version_files_ahead_of_the_changelog(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.tree(root, "1.3.0", "# Changelog\n\n## [1.3.0] - unreleased\n\n- next\n\n"
                      "## [1.2.3] - 2026-01-02\n\n- x\n")
            tag, undated, origin = validate_release.dry_run_tag(root)
            self.assertEqual((tag, undated), ("v1.3.0", True))
            self.assertIn("version files", origin)
            self.assertEqual(validate_release.validate(root, tag, undated), ("1.3.0", "- next"))
            # A tag run never accepts the undated section.
            with self.assertRaises(release_names.ReleaseError):
                validate_release.validate(root, tag)

    def test_command_line(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.tree(root, "1.2.3", "# Changelog\n\n## [1.2.3] - 2026-01-02\n\n- x\n")
            tool = [sys.executable, str(ROOT / "tools/release/validate_release.py"), "--root", str(root)]
            subprocess.check_call(tool + ["--dry-run", "--version-out", str(root / "v"),
                                          "--summary-out", str(root / "s")], stdout=subprocess.DEVNULL)
            self.assertEqual((root / "v").read_text(), "1.2.3\n")
            self.assertIn("Dry run of libmimusb 1.2.3", (root / "s").read_text())
            self.assertNotEqual(subprocess.call(tool + ["--dry-run", "--tag", "v1.2.3"],
                                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL), 0)
            self.assertNotEqual(subprocess.call(tool, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL), 0)


class SyncVersion(unittest.TestCase):
    def test_every_site_follows_the_new_version(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            Validate.write_tree(root, "1.2.3")
            tool = [sys.executable, str(ROOT / "tools/release/sync_version.py"), "--root", str(root), "--no-generate"]
            subprocess.check_call(tool + ["1.3.0"], stdout=subprocess.DEVNULL)
            self.assertEqual(set(release_names.read_versions(root).values()), {"1.3.0"})
            self.assertIn("Version 1.3.0.", (root / "README.md").read_text())
            self.assertNotEqual(subprocess.call(tool + ["1.3"], stderr=subprocess.DEVNULL), 0)


def make_archive(tmp, platform, out, arch="x86_64"):
    stage = fake_stage(Path(tmp), platform)
    subprocess.check_call([sys.executable, str(ROOT / "tools/release/package_release.py"),
                           "--stage", str(stage), "--out-dir", str(out), "--version", "1.2.3",
                           "--platform", platform, "--arch", arch,
                           "--source-sha", SHA, "--epoch", "1700000000"],
                          stdout=subprocess.DEVNULL)
    return out / release_names.archive_name("1.2.3", platform, arch)


class Package(unittest.TestCase):
    def run_package(self, tmp, platform, out, arch="x86_64"):
        return make_archive(tmp, platform, out, arch)

    def test_deterministic_and_complete(self):
        with tempfile.TemporaryDirectory() as tmp:
            first = self.run_package(tmp, "linux", Path(tmp) / "a")
            second = self.run_package(tmp, "linux", Path(tmp) / "b")
            self.assertEqual(first.read_bytes(), second.read_bytes())
            # One archive and nothing else: the SBOM is written later for the whole release.
            self.assertEqual([p.name for p in (Path(tmp) / "a").iterdir()], [first.name])
            with tarfile.open(first) as tar:
                names = tar.getnames()
                root = release_names.base_name("1.2.3", "linux", "x86_64")
                metadata = json.load(tar.extractfile(f"{root}/share/doc/libmimusb/build-metadata.json"))
            self.assertIn(f"{root}/share/doc/libmimusb/examples/README.md", names)
            self.assertIn(f"{root}/share/doc/libmimusb/examples/c/profiles/keyboard.c", names)
            self.assertEqual(metadata["components"], ["shared", "static"])
            self.assertNotIn("variant", metadata)

    def test_both_components_in_one_archive(self):
        with tempfile.TemporaryDirectory() as tmp:
            archive = self.run_package(tmp, "linux", Path(tmp) / "a")
            root = release_names.base_name("1.2.3", "linux", "x86_64")
            with tarfile.open(archive) as tar:
                names = set(tar.getnames())
            for relative in ("lib/libmimusb.so", "lib/libmimusb_static.a",
                             "lib/cmake/mimusb/mimusb-shared-targets.cmake",
                             "lib/cmake/mimusb/mimusb-static-targets.cmake",
                             "lib/cmake/mimusb/mimusb-config.cmake",
                             "lib/pkgconfig/mimusb.pc", "lib/pkgconfig/mimusb-static.pc"):
                self.assertIn(f"{root}/{relative}", names)
            macos = self.run_package(tmp, "macos", Path(tmp) / "m", arch="arm64")
            with tarfile.open(macos) as tar:
                names = set(tar.getnames())
            root = release_names.base_name("1.2.3", "macos", "arm64")
            self.assertIn(f"{root}/lib/libmimusb.dylib", names)
            self.assertIn(f"{root}/lib/libmimusb_static.a", names)

    def test_windows_zip(self):
        with tempfile.TemporaryDirectory() as tmp:
            archive = self.run_package(tmp, "windows", Path(tmp) / "w")
            self.assertTrue(zipfile.is_zipfile(archive))
            with zipfile.ZipFile(archive) as opened:
                names = opened.namelist()
            for relative in ("bin/mimusb.dll", "lib/mimusb.lib", "lib/mimusb_static.lib",
                             "lib/cmake/mimusb/mimusb-shared-targets.cmake",
                             "lib/cmake/mimusb/mimusb-static-targets.cmake"):
                self.assertTrue(any(n.endswith(relative) for n in names), relative)

    def test_missing_file_is_rejected(self):
        for relative in ("lib/libmimusb.so", "lib/libmimusb_static.a",
                         "lib/cmake/mimusb/mimusb-static-targets.cmake",
                         "lib/cmake/mimusb/mimusb-shared-targets.cmake", "lib/pkgconfig/mimusb-static.pc"):
            with tempfile.TemporaryDirectory() as tmp:
                stage = fake_stage(Path(tmp), "linux")
                (stage / relative).unlink()
                with self.assertRaises(release_names.ReleaseError, msg=relative):
                    package_release.validate_layout(stage, "linux")

    def test_config_must_say_both_were_built(self):
        # A tree configured with one kind installs a config that refuses the other component.
        with tempfile.TemporaryDirectory() as tmp:
            stage = fake_stage(Path(tmp), "linux")
            (stage / "lib/cmake/mimusb/mimusb-config.cmake").write_text(
                "set(_MIMUSB_BUILT_SHARED ON)\nset(_MIMUSB_BUILT_STATIC OFF)\n")
            with self.assertRaises(release_names.ReleaseError):
                package_release.validate_layout(stage, "linux")


def fake_dist(dist: Path, version: str = "1.2.3") -> None:
    """Every input of collect_release, with a valid SBOM over fake archive contents."""
    for name in release_names.expected_assets(version):
        if name not in ("release-manifest.json", "SHA256SUMS", release_names.sbom_name(version)):
            (dist / name).write_bytes(name.encode())
    archives = [(name, generate_spdx.sha256_file(dist / name),
                 [generate_spdx.member(f"{name}/include/mimusb.h", b"h")])
                for name in release_names.archive_names(version)]
    source = release_names.source_name(version)
    document = generate_spdx.spdx_document(version, SHA, "2023-11-14T22:13:20Z",
                                           (source, generate_spdx.sha256_file(dist / source)), archives)
    generate_spdx.write_spdx(dist / release_names.sbom_name(version), document)


class Sbom(unittest.TestCase):
    def test_one_document_for_every_archive(self):
        with tempfile.TemporaryDirectory() as tmp:
            dist = Path(tmp)
            stages = Path(tmp) / "stages"
            for platform, arch in release_names.TARGETS:
                make_archive(stages / f"{platform}-{arch}", platform, dist, arch=arch)
            (dist / release_names.source_name("1.2.3")).write_bytes(b"source")
            subprocess.check_call([sys.executable, str(ROOT / "tools/release/generate_spdx.py"),
                                   "--dist", str(dist), "--version", "1.2.3", "--source-sha", SHA,
                                   "--epoch", "1700000000"], stdout=subprocess.DEVNULL)
            document = json.loads((dist / "libmimusb-1.2.3.spdx.json").read_text())
            self.assertEqual(document["spdxVersion"], "SPDX-2.3")
            self.assertEqual(document["dataLicense"], "CC0-1.0")
            self.assertEqual(document["name"], "libmimusb-1.2.3")
            self.assertEqual(document["creationInfo"]["created"], "2023-11-14T22:13:20Z")
            packages = {p["packageFileName"]: p for p in document["packages"]}
            self.assertEqual(set(packages), set(release_names.archive_names("1.2.3")) | {"libmimusb-1.2.3-source.tar.gz"})
            ids = [p["SPDXID"] for p in document["packages"]] + [f["SPDXID"] for f in document["files"]]
            self.assertEqual(len(ids), len(set(ids)))
            for identifier in ids:
                self.assertRegex(identifier, r"^SPDXRef-[A-Za-z0-9.-]+$")
            relations = {(r["spdxElementId"], r["relationshipType"], r["relatedSpdxElement"])
                         for r in document["relationships"]}
            source_id = packages["libmimusb-1.2.3-source.tar.gz"]["SPDXID"]
            files = {f["SPDXID"]: f for f in document["files"]}
            for name in release_names.archive_names("1.2.3"):
                package = packages[name]
                self.assertEqual(package["checksums"],
                                 [{"algorithm": "SHA256", "checksumValue": generate_spdx.sha256_file(dist / name)}])
                self.assertIn(("SPDXRef-DOCUMENT", "DESCRIBES", package["SPDXID"]), relations)
                self.assertIn((package["SPDXID"], "GENERATED_FROM", source_id), relations)
                members = generate_spdx.archive_members(dist / name)
                self.assertEqual(len(package["hasFiles"]), len(members))
                self.assertTrue(any(m[0].endswith("lib/cmake/mimusb/mimusb-static-targets.cmake") for m in members))
                sha1s = sorted(next(c["checksumValue"] for c in files[i]["checksums"] if c["algorithm"] == "SHA1")
                               for i in package["hasFiles"])
                self.assertEqual(package["packageVerificationCode"]["packageVerificationCodeValue"],
                                 hashlib.sha1("".join(sha1s).encode()).hexdigest())
            # The document passes the same check collect_release applies.
            digests = {n: generate_spdx.sha256_file(dist / n)
                       for n in release_names.archive_names("1.2.3") + ["libmimusb-1.2.3-source.tar.gz"]}
            collect_release.check_sbom(dist, "1.2.3", digests)

    def test_missing_archive_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            dist = Path(tmp)
            (dist / release_names.source_name("1.2.3")).write_bytes(b"source")
            with self.assertRaises(release_names.ReleaseError):
                generate_spdx.release_document(dist, "1.2.3", SHA, 1700000000)


class Collect(unittest.TestCase):
    def test_exact_set(self):
        with tempfile.TemporaryDirectory() as tmp:
            dist = Path(tmp)
            fake_dist(dist)
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

    def test_sbom_must_match_the_archives(self):
        archive = release_names.archive_name("1.2.3", "macos", "arm64")
        with tempfile.TemporaryDirectory() as tmp:
            dist = Path(tmp)
            fake_dist(dist)
            (dist / archive).write_bytes(b"rebuilt after the SBOM was written")
            with self.assertRaises(release_names.ReleaseError):
                collect_release.collect(dist, "1.2.3", SHA)
        with tempfile.TemporaryDirectory() as tmp:
            dist = Path(tmp)
            fake_dist(dist)
            sbom = dist / release_names.sbom_name("1.2.3")
            document = json.loads(sbom.read_text())
            document["packages"] = [p for p in document["packages"] if p["packageFileName"] != archive]
            sbom.write_text(json.dumps(document))
            with self.assertRaises(release_names.ReleaseError):
                collect_release.collect(dist, "1.2.3", SHA)
        with tempfile.TemporaryDirectory() as tmp:
            dist = Path(tmp)
            fake_dist(dist)
            sbom = dist / release_names.sbom_name("1.2.3")
            document = json.loads(sbom.read_text())
            document["relationships"] = [r for r in document["relationships"]
                                         if r["relationshipType"] != "GENERATED_FROM"]
            sbom.write_text(json.dumps(document))
            with self.assertRaises(release_names.ReleaseError):
                collect_release.collect(dist, "1.2.3", SHA)


class Readme(unittest.TestCase):
    def test_asset_count_matches_the_release(self) -> None:
        # The README states how many assets a release uploads; it must follow release_names.
        text = (ROOT / "README.md").read_text(encoding="utf-8")
        match = re.search(r"exactly (\d+) uploaded assets", text)
        self.assertIsNotNone(match, "README does not state the number of uploaded assets")
        assert match is not None
        self.assertEqual(int(match.group(1)), len(release_names.expected_assets("1.2.3")))


if __name__ == "__main__":
    unittest.main()
