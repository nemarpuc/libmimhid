#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
"""Fail when the documentation names a library identifier or a path that does not exist.

Two kinds of reference are checked in the Markdown files listed in DOCUMENTS:

- an identifier with the library's own prefix (mimhid_*, MIMHID_*, mimhid::*) inside
  backticks or a fenced code block must occur in the code: include/, src/, proto/,
  firmware/, tests/, examples/, bindings/, cmake/, tools/, .github/ or CMakeLists.txt. A '*' in
  the reference stands for any run of identifier characters, so `mimhid_spec_create_*`
  needs one match. mimhid::name is met by the literal text or by name in
  include/mimhid.hpp. A name CMake builds from a variable (mimhid_sim_${itfs}) is met
  by any reference that starts with the part before '${'.
- a backticked path that starts with one of PATH_ROOTS must exist ('*' is a glob).

tools/doc_reference_allowlist.txt lists deliberate exceptions, one reference per line,
each followed by '#' and the reason it is allowed.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
DOCUMENTS = (
    "docs/*.md",
    "README.md",
    "CONTRIBUTING.md",
    "tests/README.md",
    "tests/hw/README.md",
    "examples/README.md",
)
CODE_ROOTS = (
    "include", "src", "proto", "firmware", "tests", "examples", "bindings", "cmake", "tools", ".github",
)
CODE_FILES = ("CMakeLists.txt",)
CODE_SUFFIXES = frozenset(
    {".h", ".hpp", ".c", ".cpp", ".py", ".cmake", ".in", ".txt", ".map", ".exports", ".rs",
     ".cs", ".csproj", ".toml", ".json", ".sh", ".kt", ".kts", ".java", ".xml", ".rules", ".yml"}
)
PATH_ROOTS = (
    "src", "tests", "tools", "docs", "examples", "include", "bindings", "cmake", "udev", "firmware",
    "proto",
)
ALLOWLIST = Path("tools/doc_reference_allowlist.txt")
# This checker and its allowlist name references on purpose; they must not satisfy themselves.
SELF = (Path("tools/check_doc_references.py"), ALLOWLIST)

INLINE_CODE_RE = re.compile(r"(?<!`)`([^`\n]+)`(?!`)")
FENCE_RE = re.compile(r"^\s*(```|~~~)")
# A CMake name made from a variable, such as mimhid_sim_${itfs}: the part before "${".
GENERATED_RE = re.compile(r"(?<![A-Za-z0-9_])((?:mimhid_|MIMHID_)[A-Za-z0-9_]*)\$\{")
IDENTIFIER_RE = re.compile(r"(?<![A-Za-z0-9_])(?:mimhid::|mimhid_|MIMHID_)[A-Za-z0-9_*]*")
PATH_RE = re.compile(r"^(?:\.\./)*(?:" + "|".join(PATH_ROOTS) + r")/[A-Za-z0-9_.*/+-]*$")


@dataclass(frozen=True)
class Problem:
    document: str
    line: int
    reference: str
    message: str

    def __str__(self) -> str:
        return f"{self.document}:{self.line}: `{self.reference}`: {self.message}"


def documents(root: Path) -> list[Path]:
    found: set[Path] = set()
    for pattern in DOCUMENTS:
        found.update(path for path in root.glob(pattern) if path.is_file())
    return sorted(found)


def code_text(root: Path) -> str:
    skipped = {root / path for path in SELF}
    parts: list[str] = []
    candidates = [root / name for name in CODE_FILES]
    for directory in CODE_ROOTS:
        candidates.extend(sorted((root / directory).rglob("*")))
    for path in candidates:
        if path in skipped or not path.is_file() or "__pycache__" in path.parts:
            continue
        if path.suffix not in CODE_SUFFIXES and path.name not in CODE_FILES:
            continue
        try:
            parts.append(path.read_text(encoding="utf-8"))
        except (OSError, UnicodeDecodeError):
            continue
    return "\n".join(parts)


def read_allowlist(path: Path) -> tuple[set[str], list[str]]:
    """Return the allowed references and the lines that have no reason."""
    allowed: set[str] = set()
    errors: list[str] = []
    if not path.is_file():
        return allowed, errors
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        reference, _, reason = line.partition("#")
        if not reason.strip():
            errors.append(f"{path.name}:{number}: every entry needs '# <reason>'")
            continue
        allowed.add(reference.strip())
    return allowed, errors


def references(text: str) -> list[tuple[int, str, bool]]:
    """Yield (line, code text, inline) for every code span and fenced code line."""
    found: list[tuple[int, str, bool]] = []
    fenced = False
    for number, line in enumerate(text.splitlines(), start=1):
        if FENCE_RE.match(line):
            fenced = not fenced
            continue
        if fenced:
            found.append((number, line, False))
            continue
        found.extend((number, match.group(1), True) for match in INLINE_CODE_RE.finditer(line))
    return found


def generated_prefixes(code: str) -> tuple[str, ...]:
    return tuple(sorted(set(GENERATED_RE.findall(code))))


def identifier_exists(reference: str, code: str, facade: str,
                      prefixes: tuple[str, ...] = ()) -> bool:
    # A trailing '_' is a prefix written out (`MIMHID_ERR_`), as is a trailing '*'.
    name = reference + "*" if reference.endswith("_") else reference
    if name.startswith("mimhid::"):
        member = name[len("mimhid::") :]
        if "*" not in member and re.search(rf"\b{re.escape(member)}\b", facade):
            return True
    if any(name.startswith(prefix) and len(name) > len(prefix) for prefix in prefixes):
        return True
    pattern = re.escape(name).replace(r"\*", r"[A-Za-z0-9_]*")
    return re.search(rf"(?<![A-Za-z0-9_]){pattern}(?![A-Za-z0-9_])", code) is not None


def path_exists(root: Path, document: Path, reference: str) -> bool:
    cleaned = reference.split("#", 1)[0].rstrip(".,;:)")
    cleaned = re.sub(r":\d+(?:-\d+)?$", "", cleaned)
    base = document.parent if cleaned.startswith("../") else root
    target = (base / cleaned).as_posix()
    if "*" in cleaned:
        relative = Path(target).resolve().relative_to(root.resolve()).as_posix()
        return any(root.glob(relative))
    return Path(target).exists()


def find_problems(root: Path) -> list[Problem]:
    code = code_text(root)
    facade_path = root / "include/mimhid.hpp"
    facade = facade_path.read_text(encoding="utf-8") if facade_path.is_file() else ""
    prefixes = generated_prefixes(code)
    allowed, allowlist_errors = read_allowlist(root / ALLOWLIST)
    problems = [Problem(ALLOWLIST.as_posix(), 0, "", error) for error in allowlist_errors]
    for document in documents(root):
        relative = document.relative_to(root).as_posix()
        for number, span, inline in references(document.read_text(encoding="utf-8")):
            for match in IDENTIFIER_RE.finditer(span):
                reference = match.group(0)
                if reference in {"mimhid_", "MIMHID_", "mimhid::"} or reference in allowed:
                    continue
                if not identifier_exists(reference, code, facade, prefixes):
                    problems.append(Problem(relative, number, reference, "not found in the code"))
            candidate = span.strip()
            if inline and PATH_RE.match(candidate) and candidate not in allowed:
                if not path_exists(root, document, candidate):
                    problems.append(Problem(relative, number, candidate, "path does not exist"))
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args()
    problems = find_problems(args.root)
    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        print(f"doc references: {len(problems)} problem(s)", file=sys.stderr)
        return 1
    print(f"doc references: {len(documents(args.root))} documents checked")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
