#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
"""Validate the version policy for contracts and mod manifests.

Every RIDL contract and mod manifest carries a SemVer 2.0.0 version. A breaking
change bumps the major, an additive method or appended struct field bumps the
minor, and a fix bumps the patch. A service major starts at one: the emitted
ABI major is the contract major, and a zero major would disable the provider
ABI header check. This gate validates the shape of the declared versions; the
history (a version only moves forward, a released number is never reused for
different content) is a review rule.
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SEMVER = re.compile(
    r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)"
    r"(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$"
)
SERVICE_VERSION = re.compile(r"^\s*version\s+(\S+)\s*;", re.MULTILINE)
MOD_VERSION = re.compile(r'^version\s*=\s*"([^"]+)"', re.MULTILINE)


def check_version(
    text: str,
    pattern: re.Pattern[str],
    path: pathlib.Path,
    label: str,
    failures: list[str],
    require_major_one: bool,
) -> None:
    match = pattern.search(text)
    if match is None:
        failures.append(f"{path.relative_to(ROOT)}: missing {label}")
        return
    version = match.group(1)
    if not SEMVER.match(version):
        failures.append(
            f"{path.relative_to(ROOT)}: {label} {version} is not SemVer 2.0.0"
        )
        return
    if require_major_one and version.split(".", 1)[0] == "0":
        failures.append(
            f"{path.relative_to(ROOT)}: {label} {version} has major 0; a "
            "service contract starts at major one"
        )


def main() -> int:
    failures: list[str] = []
    contracts = 0
    for path in sorted((ROOT / "sdk/idl").rglob("*.ridl")):
        check_version(
            path.read_text(encoding="utf-8"),
            SERVICE_VERSION,
            path,
            "service version",
            failures,
            require_major_one=True,
        )
        contracts += 1
    mods = 0
    for path in sorted((ROOT / "mods").glob("*/manifest.toml")):
        check_version(
            path.read_text(encoding="utf-8"),
            MOD_VERSION,
            path,
            "mod version",
            failures,
            require_major_one=False,
        )
        mods += 1
    if failures:
        for failure in failures:
            print(f"contract-versions: {failure}", file=sys.stderr)
        return 1
    print(
        f"contract-versions: OK ({contracts} contracts, {mods} mod manifests, "
        "SemVer 2.0.0)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
