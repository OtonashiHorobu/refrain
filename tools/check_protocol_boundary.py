#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
"""Keep version-specific protocol types out of session/server code.

Only the protocol facade and provider implementations may name a concrete
protocol version. The bot and protocol tests are separate client tooling and are
explicitly outside this boundary.
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SEARCH_ROOTS = [
    "src/server",
    "include/refrain/server",
    "src/app",
    "include/refrain/app",
    "mods/refrain.server.vanilla",
]
# src/net is the transport/session shell; bot.cpp is a standalone test client.
EXTRA_FILES = ["src/net/server.cpp", "src/net/status.cpp"]
VERSIONED = re.compile(r"(schema/protocol/\d+|protocol::v\d+)")


def checked_files() -> list[pathlib.Path]:
    files: list[pathlib.Path] = []
    for root in SEARCH_ROOTS:
        path = ROOT / root
        if path.is_file():
            files.append(path)
        elif path.is_dir():
            files.extend(
                item
                for item in path.rglob("*")
                if item.suffix in {".hpp", ".h", ".cpp", ".cc", ".c"}
            )
    for extra in EXTRA_FILES:
        path = ROOT / extra
        if path.is_file():
            files.append(path)
    return sorted(set(files))


def main() -> int:
    failures: list[str] = []
    for path in checked_files():
        text = path.read_text(encoding="utf-8", errors="ignore")
        for number, line in enumerate(text.splitlines(), 1):
            if VERSIONED.search(line):
                failures.append(
                    f"{path.relative_to(ROOT)}:{number}: {line.strip()}"
                )
    if failures:
        for failure in failures:
            print(f"protocol-boundary: {failure}", file=sys.stderr)
        return 1
    print("protocol-boundary: OK (no concrete version in session/server code)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
