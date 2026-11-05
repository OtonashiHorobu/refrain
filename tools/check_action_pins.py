#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
"""Enforce full-SHA pinning for GitHub Actions used by this repository.

A mutable tag such as `actions/checkout@v4` lets an upstream tag move under the
build. Every `uses:` entry in `.github/workflows/*.yml` must reference a full
40-hex commit, optionally followed by a `# vX.Y.Z` comment. Local composite
actions (`./...`) are exempt.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORKFLOWS = ROOT / ".github" / "workflows"
USES = re.compile(r"^\s*(?:-\s*)?uses:\s*([^\s#]+)", re.MULTILINE)
PINNED = re.compile(r"^[^@]+@[0-9a-f]{40}$")


def main() -> int:
    violations: list[str] = []
    workflows = sorted(WORKFLOWS.glob("*.yml")) + sorted(WORKFLOWS.glob("*.yaml"))
    if not workflows:
        print("action pins: no workflows found", file=sys.stderr)
        return 1
    for path in workflows:
        for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            match = USES.search(line)
            if not match:
                continue
            target = match.group(1)
            if target.startswith("./"):
                continue
            if not PINNED.match(target):
                violations.append(f"{path.relative_to(ROOT)}:{lineno}: {target}")
    if violations:
        print("action pins: every external action must use a full 40-hex commit:", file=sys.stderr)
        for item in violations:
            print(f"  {item}", file=sys.stderr)
        return 1
    print(f"action pins: OK ({len(workflows)} workflow(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
