#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimusb contributors
"""Tests of tools/check_doc_references.py; they need nothing but the standard library."""

from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import check_doc_references as checker  # noqa: E402


def write(root: Path, relative: str, text: str) -> None:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


class DocReferenceTests(unittest.TestCase):
    def tree(self) -> Path:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        write(root, "include/mimusb.h", "int mimusb_device_open(void);\n#define MIMUSB_OK 0\n")
        write(root, "include/mimusb.hpp", "namespace mimusb { inline int device_open(); }\n")
        write(root, "cmake/tests.cmake", "add_executable(mimusb_sim_${itfs} a.c)\n")
        write(root, "src/a.cpp", "\n")
        return root

    def problems(self, root: Path, document: str) -> list[str]:
        write(root, "docs/PAGE.md", document)
        return [problem.reference for problem in checker.find_problems(root)]

    def test_existing_references_pass(self) -> None:
        root = self.tree()
        text = ("`mimusb_device_open`, `MIMUSB_OK`, `MIMUSB_`, `mimusb::device_open`, "
                "`mimusb_device_*`, `mimusb_sim_15`, `src/a.cpp`, `src/*.cpp`\n")
        self.assertEqual(self.problems(root, text), [])

    def test_missing_identifier_is_reported(self) -> None:
        root = self.tree()
        self.assertEqual(self.problems(root, "`mimusb_device_ping`\n"), ["mimusb_device_ping"])

    def test_identifier_in_a_fenced_block_is_checked(self) -> None:
        root = self.tree()
        text = "```c\nmimusb_device_open();\nMIMUSB_ERR_GONE;\n```\n"
        self.assertEqual(self.problems(root, text), ["MIMUSB_ERR_GONE"])

    def test_missing_path_is_reported(self) -> None:
        root = self.tree()
        self.assertEqual(self.problems(root, "`src/b.cpp` and `tests/x/`\n"), ["src/b.cpp", "tests/x/"])

    def test_generated_prefix_needs_a_suffix(self) -> None:
        root = self.tree()
        self.assertEqual(self.problems(root, "`mimusb_sim_`x `mimusb_simx`\n"), ["mimusb_simx"])

    def test_allowlist_needs_a_reason(self) -> None:
        root = self.tree()
        write(root, "tools/doc_reference_allowlist.txt", "MIMUSB_GONE # removed\nMIMUSB_BARE\n")
        found = checker.find_problems(root)
        self.assertEqual(len([p for p in found if "reason" in p.message]), 1)
        write(root, "docs/PAGE.md", "`MIMUSB_GONE`\n")
        self.assertEqual([p.reference for p in checker.find_problems(root) if p.reference], [])

    def test_the_repository_passes(self) -> None:
        self.assertEqual([str(problem) for problem in checker.find_problems(ROOT)], [])


if __name__ == "__main__":
    unittest.main()
