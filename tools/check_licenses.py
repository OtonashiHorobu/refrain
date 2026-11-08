#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
"""Reject copyleft SPDX identifiers in the permissively licensed tree."""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TEXT_SUFFIXES = {
    ".c", ".cc", ".cpp", ".h", ".hpp", ".hh", ".rs", ".py", ".sh",
    ".toml", ".md", ".rsc", ".ridl", ".json", ".yml", ".yaml",
}
FORBIDDEN = re.compile(r"SPDX-License-Identifier:\s*(A?GPL|LGPL)", re.IGNORECASE)


def tracked_files() -> list[pathlib.Path]:
    output = subprocess.check_output(
        ["git", "ls-files", "-z"], cwd=ROOT, text=True
    )
    return [ROOT / item for item in output.split("\0") if item]


def main() -> int:
    failures: list[str] = []
    for path in tracked_files():
        if path.suffix not in TEXT_SUFFIXES or not path.is_file():
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="ignore")
        except OSError as error:
            failures.append(f"{path.relative_to(ROOT)}: {error}")
            continue
        if FORBIDDEN.search(text):
            failures.append(
                f"{path.relative_to(ROOT)}: copyleft SPDX identifier"
            )
    if failures:
        for failure in failures:
            print(f"license: {failure}", file=sys.stderr)
        return 1
    print("license: OK (no copyleft identifiers in tracked sources)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
