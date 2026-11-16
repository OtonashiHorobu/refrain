#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
"""Validate the on-disk format version policy.

Each custom container and storage format carries a monotonic integer version.
A breaking layout change bumps it and either migrates older data or refuses it
with the supported versions named; a version never resets or decreases. The
schema hash identifies the build and is not a version, so this gate checks
that every declared format version is a positive integer and that the chunk
field-set version agrees between its C++ constant and its schema default
rather than forcing one particular value.

Run by ``scripts/ci.sh`` and locally before committing a format change:

    python3 scripts/check_format_versions.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# (path, regex, human description); every capture is a positive integer.
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


def read_versions(
    failures: list[str],
) -> dict[tuple[str, str], int]:
    values: dict[tuple[str, str], int] = {}
    for relative, pattern, label in CHECKS:
        path = ROOT / relative
        if not path.is_file():
            failures.append(f"{relative}: missing ({label})")
            continue
        matches = re.findall(pattern, path.read_text(encoding="utf-8"),
                             flags=re.MULTILINE)
        if not matches:
            failures.append(f"{relative}: cannot find {label}")
            continue
        for value in matches:
            number = int(value)
            if number < 1:
                failures.append(
                    f"{relative}: {label} is {number}, expected a positive "
                    "integer (a breaking change bumps the version)"
                )
            values[(relative, label)] = number
    return values


def main() -> int:
    failures: list[str] = []
    values = read_versions(failures)
    model = values.get(
        ("include/refrain/world/model.hpp", "chunk field-set version"))
    schema = values.get(
        ("schema/storage/chunk.rsc", "chunk field-set schema default"))
    if model is not None and schema is not None and model != schema:
        failures.append(
            "chunk field-set version disagrees between model.hpp "
            f"({model}) and chunk.rsc ({schema})"
        )
    if failures:
        print("format-version guard FAILED:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    chunk = f", chunk field set {model}" if model is not None else ""
    print(f"format-version guard: {len(CHECKS)} checks, policy OK{chunk}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
