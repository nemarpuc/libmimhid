#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libmimhid contributors
"""Compare the C ABI declared in include/mimhid.h with a shared library's export table."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

EXPORT_RE = re.compile(r"MIMHID_API\s+[^;]*?\bMIMHID_CALL\s+(mimhid_[A-Za-z0-9_]+)\s*\(", re.DOTALL)


def declared(header: Path) -> set[str]:
    return set(EXPORT_RE.findall(header.read_text(encoding="utf-8")))


def pe_exports(path: Path) -> set[str]:
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE file")
    sections_count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    magic = struct.unpack_from("<H", data, optional)[0]
    directory = optional + (112 if magic == 0x20B else 96)
    rva, _size = struct.unpack_from("<II", data, directory)
    sections = []
    table = optional + optional_size
    for index in range(sections_count):
        virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from("<IIII", data, table + index * 40 + 8)
        sections.append((virtual_address, max(virtual_size, raw_size), raw_pointer))

    def offset(address: int) -> int:
        for start, size, raw in sections:
            if start <= address < start + size:
                return address - start + raw
        raise ValueError("RVA outside every section")

    if rva == 0:
        return set()
    export = offset(rva)
    count = struct.unpack_from("<I", data, export + 24)[0]
    names_rva = struct.unpack_from("<I", data, export + 32)[0]
    names = offset(names_rva)
    result = set()
    for index in range(count):
        name_rva = struct.unpack_from("<I", data, names + 4 * index)[0]
        start = offset(name_rva)
        result.add(data[start:data.index(b"\0", start)].decode("ascii"))
    return result


def nm_exports(path: Path, mac: bool, nm: str) -> set[str]:
    command = [nm, "-gU", str(path)] if mac else [nm, "-D", "--defined-only", str(path)]
    if shutil.which(nm) is None:
        raise ValueError(f"{nm} is not available")
    output = subprocess.check_output(command, text=True)
    names = set()
    for line in output.splitlines():
        symbol = line.split()[-1] if line.split() else ""
        symbol = symbol.split("@", 1)[0]
        if mac and symbol.startswith("_"):
            symbol = symbol[1:]
        names.add(symbol)
    return names


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--header", type=Path, required=True)
    parser.add_argument("--library", type=Path)
    parser.add_argument("--nm", default="nm", help="nm for the library's architecture")
    args = parser.parse_args()
    api = declared(args.header)
    if not api:
        print("no MIMHID_API function found in the header", file=sys.stderr)
        return 1
    if args.library is not None:
        head = args.library.read_bytes()[:4]
        if head[:2] == b"MZ":
            exported = pe_exports(args.library)
        else:
            exported = nm_exports(args.library, head in (b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe"), args.nm)
        # Anything outside the project prefix (version nodes, toolchain stubs) is not ABI.
        exported = {name for name in exported if name.startswith("mimhid_")}
        extra_all = {n for n in exported if n not in api}
        missing = api - exported
        if missing or extra_all:
            print(f"missing exports: {sorted(missing)}; undeclared exports: {sorted(extra_all)}", file=sys.stderr)
            return 1
    print(f"{len(api)} exported functions checked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
