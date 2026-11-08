#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
"""Enforce the pre-release version freeze.

Until the first public release, service contracts and mod manifests stay at
1.0.0, and on-disk formats stay at version 1. Incompatible changes are made in
place and tracked by the schema hash, not by a new major/minor number.
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SERVICE_VERSION = re.compile(r"^version\s+(\S+);", re.MULTILINE)
MOD_VERSION = re.compile(r'^version\s*=\s*"([^"]+)"', re.MULTILINE)
FROZEN = "1.0.0"


def main() -> int:
    failures: list[str] = []
    for path in sorted((ROOT / "sdk/idl/refrain/services").glob("*.ridl")):
        match = SERVICE_VERSION.search(path.read_text(encoding="utf-8"))
        if match is None:
            failures.append(f"{path.relative_to(ROOT)}: missing service version")
        elif match.group(1) != FROZEN:
            failures.append(
                f"{path.relative_to(ROOT)}: service version "
                f"{match.group(1)} is not frozen at {FROZEN}"
            )
    for path in sorted((ROOT / "mods").glob("*/manifest.toml")):
        match = MOD_VERSION.search(path.read_text(encoding="utf-8"))
        if match is None:
            failures.append(f"{path.relative_to(ROOT)}: missing mod version")
        elif match.group(1) != FROZEN:
            failures.append(
                f"{path.relative_to(ROOT)}: mod version "
                f"{match.group(1)} is not frozen at {FROZEN}"
            )
    if failures:
        for failure in failures:
            print(f"contract-versions: {failure}", file=sys.stderr)
        return 1
    print("contract-versions: OK (services and mods frozen at 1.0.0)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
