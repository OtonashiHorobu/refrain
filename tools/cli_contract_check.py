#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
"""Frozen CLI/output contract check.

The strings in `test/fixtures/golden/cli-contract.txt` are what scripts/ci.sh,
the GitHub workflow and external scripts grep. Output drift is cheap to fix
when it fails here; it is expensive when a downstream grep silently stops
matching. Every entry also pins the output *stream* (stdout vs stderr), which
is part of the contract after the console log was moved to stderr.

Usage:
    python3 tools/cli_contract_check.py [--root .] [--bin-dir build/Release/bin]
                                        [--contract test/fixtures/golden/cli-contract.txt]
                                        [--only id1,id2] [--list]

Exit code 0 = every entry matched; 1 = at least one mismatch.
"""

from __future__ import annotations

import argparse
import shlex
import subprocess
import sys
from pathlib import Path

COLUMNS = ("id", "stream", "exit", "expect", "command")
STREAMS = {"stdout", "stderr", "any"}
EXITS = {"0", "nonzero"}
TIMEOUT_SECONDS = 120


class ContractError(Exception):
    pass


def parse_contract(path: Path) -> list[dict[str, str]]:
    entries: list[dict[str, str]] = []
    seen: set[str] = set()
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = [part.strip() for part in raw.split("|")]
        if len(parts) != len(COLUMNS):
            raise ContractError(
                f"{path}:{lineno}: expected {len(COLUMNS)} pipe-separated columns "
                f"({', '.join(COLUMNS)}), got {len(parts)}"
            )
        entry = dict(zip(COLUMNS, parts, strict=True))
        if entry["id"] in seen:
            raise ContractError(f"{path}:{lineno}: duplicate id '{entry['id']}'")
        seen.add(entry["id"])
        if entry["stream"] not in STREAMS:
            raise ContractError(
                f"{path}:{lineno}: stream must be one of {sorted(STREAMS)}"
            )
        if entry["exit"] not in EXITS:
            raise ContractError(
                f"{path}:{lineno}: exit must be one of {sorted(EXITS)}"
            )
        if not entry["command"]:
            raise ContractError(f"{path}:{lineno}: empty command")
        entries.append(entry)
    if not entries:
        raise ContractError(f"{path}: no contract entries")
    return entries


def expand(command: str, root: Path, bin_dir: Path) -> list[str]:
    substitutions = {
        "{root}": str(root),
        "{bin}": str(bin_dir),
        "{build}": str(bin_dir.parent),
    }
    for placeholder, value in substitutions.items():
        command = command.replace(placeholder, value)
    argv = shlex.split(command)
    # Bare program names are the build's binaries (`refrain-tools`,
    # `refrain-server`); resolve them against --bin-dir so the check does not
    # depend on PATH.
    if argv and "/" not in argv[0]:
        argv[0] = str(bin_dir / argv[0])
    return argv


def run_entry(entry: dict[str, str], root: Path, bin_dir: Path) -> str | None:
    argv = expand(entry["command"], root, bin_dir)
    try:
        completed = subprocess.run(
            argv,
            cwd=root,
            capture_output=True,
            text=True,
            errors="replace",
            timeout=TIMEOUT_SECONDS,
            check=False,
        )
    except FileNotFoundError as error:
        return f"cannot execute '{argv[0]}': {error}"
    except subprocess.TimeoutExpired:
        return f"timed out after {TIMEOUT_SECONDS}s"

    if entry["exit"] == "0" and completed.returncode != 0:
        return f"exit {completed.returncode}, expected 0 (stderr: {completed.stderr.strip()[:300]})"
    if entry["exit"] == "nonzero" and completed.returncode == 0:
        return "exit 0, expected a non-zero exit"

    if entry["stream"] == "stdout":
        text = completed.stdout
    elif entry["stream"] == "stderr":
        text = completed.stderr
    else:
        text = completed.stdout + completed.stderr

    expected = entry["expect"]
    if expected.startswith("!"):
        needle = expected[1:]
        if needle in text:
            return f"{entry['stream']} must not contain '{needle}'"
        return None
    if expected not in text:
        return f"{entry['stream']} does not contain '{expected}'"
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument(
        "--bin-dir", default="build/Release/bin", help="directory with the binaries"
    )
    parser.add_argument(
        "--contract",
        default="test/fixtures/golden/cli-contract.txt",
        help="contract file (relative to --root)",
    )
    parser.add_argument("--only", default="", help="comma-separated entry ids")
    parser.add_argument("--list", action="store_true", help="list entries and exit")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    bin_dir = Path(args.bin_dir)
    if not bin_dir.is_absolute():
        bin_dir = (root / bin_dir).resolve()
    contract = Path(args.contract)
    if not contract.is_absolute():
        contract = root / contract

    try:
        entries = parse_contract(contract)
    except (OSError, ContractError) as error:
        print(f"cli contract: {error}", file=sys.stderr)
        return 1

    selected = {item for item in args.only.split(",") if item}
    if selected:
        entries = [entry for entry in entries if entry["id"] in selected]
        if not entries:
            print(f"cli contract: no entry matches --only {args.only}", file=sys.stderr)
            return 1

    if args.list:
        for entry in entries:
            print(f"{entry['id']}\t{entry['stream']}\t{entry['exit']}\t{entry['command']}")
        return 0

    failures = 0
    print(f"CLI/output contract ({len(entries)} entries, {contract.relative_to(root)})")
    for entry in entries:
        problem = run_entry(entry, root, bin_dir)
        if problem is None:
            print(f"  PASS  {entry['id']}")
            continue
        failures += 1
        print(f"  FAIL  {entry['id']}: {problem}", file=sys.stderr)
    print(f"  {len(entries) - failures} passed, {failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
