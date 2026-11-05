#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
"""Guard the pre-release storage-format policy.

The project deliberately keeps every on-disk format at ``format_version = 1``
until the first public release. The format is edited in place while it is
unfrozen; the generated ``schema hash`` (not a version number) identifies the
build. This gate fails if someone starts a "v2/v3" chain before the freeze.

Run by ``scripts/ci.sh`` and locally before committing a format change:

    python3 scripts/check_format_versions.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# (path, regex, human description). Every capture must be "1".
CHECKS = [
    (
        "include/refrain/world/model.hpp",
        r"chunk_format_version\s*=\s*(\d+)",
        "chunk field-set version",
    ),
    (
        "include/refrain/world/rwf.hpp",
        r"container_version\s*=\s*(\d+)",
        "RWF container version",
    ),
    (
        "include/refrain/world/store.hpp",
        r'meta_format\s*=\s*"refrain\.world-meta/(\d+)"',
        "world meta format",
    ),
    (
        "schema/storage/chunk.rsc",
        r"^version\s+(\d+)\s*;",
        "chunk schema module version",
    ),
    (
        "schema/storage/chunk.rsc",
        r"format_version:\s*u16\s*=\s*(\d+)",
        "chunk field-set schema default",
    ),
    (
        "src/world/palette_table.cpp",
        r"table_version\{(\d+)\}",
        "RFPT palette table version",
    ),
    (
        "src/world/object_store.cpp",
        r"index_version\{(\d+)\}",
        "RFOS object index version",
    ),
    (
        "src/data/content_segment.cpp",
        r"segment_format_version\{(\d+)\}",
        "RFSG segment version",
    ),
    (
        "src/data/content_index.cpp",
        r"blob_table_version\{(\d+)\}",
        "RFBT blob table version",
    ),
    (
        "src/data/content_index.cpp",
        r"resolution_index_version\{(\d+)\}",
        "RFRX resolution index version",
    ),
]


def main() -> int:
    failures: list[str] = []
    for relative, pattern, label in CHECKS:
        path = ROOT / relative
        if not path.is_file():
            failures.append(f"{relative}: missing ({label})")
            continue
        text = path.read_text(encoding="utf-8")
        matches = re.findall(pattern, text, flags=re.MULTILINE)
        if not matches:
            failures.append(f"{relative}: cannot find {label}")
            continue
        for value in matches:
            if value != "1":
                failures.append(
                    f"{relative}: {label} is {value}, expected 1 ("
                    "storage formats are frozen only at the first public release)"
                )
    if failures:
        print("format-version guard FAILED:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print(f"format-version guard: {len(CHECKS)} checks, all 1 (pre-release)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
