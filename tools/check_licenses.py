#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
"""Reject copyleft SPDX identifiers and missing license headers.

The tree is MIT-licensed, so an SPDX identifier that names a reciprocal or
copyleft family is a licensing error: (A)GPL, LGPL, MPL, EPL, CDDL and EUPL are
refused, in a plain identifier or as one alternative of an ``OR``/``AND``
expression. Every tracked file whose first line is a shebang is a distributed
source file as well, so it must carry both SPDX header lines; the check fails
instead of relying on review to notice one that is missing.

The built-in self-test runs on every invocation before the tracked tree is
scanned, so a broken matcher cannot silently pass the gate.

Usage: python3 tools/check_licenses.py [--self-test]
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TEXT_SUFFIXES = {
    ".c", ".cc", ".cpp", ".h", ".hpp", ".hh", ".rs", ".py", ".sh",
    ".toml", ".md", ".rsc", ".ridl", ".json", ".yml", ".yaml",
}
LICENSE_LINE = re.compile(r"SPDX-License-Identifier:[ \t]*(.+)$", re.MULTILINE)
# A copyleft family token anywhere in an SPDX expression. The lookarounds keep
# "GPL" from matching inside "LGPL"/"AGPL" while still accepting a trailing
# version or operator suffix ("MPL-2.0", "EUPL-1.2 OR MIT").
FORBIDDEN = re.compile(
    r"(?<![A-Za-z0-9-])(?:A?GPL|LGPL|MPL|EPL|CDDL|EUPL)(?![A-Za-z])",
    re.IGNORECASE,
)
COPYLEFT_FAMILIES = "AGPL, GPL, LGPL, MPL, EPL, CDDL and EUPL"
HEADER_LINES = 5

# Synthetic sources for the self-test: (name, lines, number of expected
# failures). They exercise the copyleft matcher in plain and in combined
# expressions, plus the header rule for files with and without a shebang. The
# marker is assembled from two strings so this guard's own source does not
# contain a synthetic license declaration that its scan would reject.
SPDX_TAG = "SPDX-License-" + "Identifier:"
COPYRIGHT_TAG = "SPDX-FileCopyrightText:"

SELF_TEST_FIXTURES: tuple[tuple[str, tuple[str, ...], int], ...] = (
    (
        "clean.py",
        (
            "#!/usr/bin/env python3",
            "# {license} MIT",
            "#",
            "# {copyright} Copyright (c) 2026 Example",
            "print('ok')",
        ),
        0,
    ),
    (
        "missing-header.py",
        (
            "#!/usr/bin/env python3",
            "print('no header')",
        ),
        1,
    ),
    (
        "mpl.c",
        (
            "// {license} MPL-2.0",
            "int main(void) { return 0; }",
        ),
        1,
    ),
    (
        "or-expression.c",
        (
            "// {license} MIT OR GPL-3.0-or-later",
            "int main(void) { return 0; }",
        ),
        1,
    ),
    (
        "epl.sh",
        (
            "#!/bin/sh",
            "# {license} EPL-2.0",
            "# {copyright} Copyright (c) 2026 Example",
        ),
        1,
    ),
    (
        "lgpl.c",
        (
            "// {license} LGPL-2.1-or-later",
            "int main(void) { return 0; }",
        ),
        1,
    ),
    (
        "agpl.c",
        (
            "// {license} AGPL-3.0-only",
            "int main(void) { return 0; }",
        ),
        1,
    ),
    (
        "cddl.c",
        (
            "// {license} CDDL-1.0",
            "int main(void) { return 0; }",
        ),
        1,
    ),
    (
        "eupl.c",
        (
            "// {license} EUPL-1.2",
            "int main(void) { return 0; }",
        ),
        1,
    ),
    (
        "permissive.c",
        (
            "// {license} Apache-2.0 OR MIT",
            "int main(void) { return 0; }",
        ),
        0,
    ),
    (
        "exception.c",
        (
            "// {license} Apache-2.0 WITH LLVM-exception",
            "int main(void) { return 0; }",
        ),
        0,
    ),
    (
        "data.json",
        (
            '{"no": "shebang", "so": "no header required"}',
        ),
        0,
    ),
)


def copyleft_identifier(text: str) -> str | None:
    """Return the first SPDX expression in `text` that names a copyleft family."""
    for match in LICENSE_LINE.finditer(text):
        expression = match.group(1).strip()
        if FORBIDDEN.search(expression):
            return expression
    return None


def missing_script_header(text: str) -> list[str]:
    """SPDX header lines a shebang script is missing (empty when compliant)."""
    lines = text.splitlines()
    if not lines or not lines[0].startswith("#!"):
        return []
    head = "\n".join(lines[:HEADER_LINES])
    missing: list[str] = []
    if "SPDX-License-Identifier:" not in head:
        missing.append("SPDX-License-Identifier")
    if "SPDX-FileCopyrightText:" not in head:
        missing.append("SPDX-FileCopyrightText")
    return missing


def check_source(relative: str, text: str) -> list[str]:
    """Failures for one source file; `relative` only labels the message."""
    failures: list[str] = []
    copyleft = copyleft_identifier(text)
    if copyleft is not None:
        failures.append(
            f"{relative}: copyleft SPDX identifier ({copyleft}); "
            f"the tree refuses {COPYLEFT_FAMILIES}"
        )
    missing = missing_script_header(text)
    if missing:
        failures.append(f"{relative}: shebang script is missing {' and '.join(missing)}")
    return failures


def self_test() -> int:
    failures: list[str] = []
    for name, lines, expected in SELF_TEST_FIXTURES:
        text = "\n".join(
            line.replace("{license}", SPDX_TAG).replace("{copyright}", COPYRIGHT_TAG)
            for line in lines
        )
        found = check_source(name, text)
        if len(found) != expected:
            failures.append(
                f"{name}: expected {expected} failure(s), got {len(found)}: {found}"
            )
    if failures:
        for failure in failures:
            print(f"license: self-test: {failure}", file=sys.stderr)
        return 1
    print(f"license: self-test OK ({len(SELF_TEST_FIXTURES)} fixtures)")
    return 0


def tracked_files() -> list[pathlib.Path]:
    output = subprocess.check_output(
        ["git", "ls-files", "-z"], cwd=ROOT, text=True
    )
    return [ROOT / item for item in output.split("\0") if item]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run only the built-in matcher and header-rule fixtures",
    )
    args = parser.parse_args()

    if self_test() != 0:
        return 1
    if args.self_test:
        return 0

    failures: list[str] = []
    for path in tracked_files():
        if path.suffix not in TEXT_SUFFIXES or not path.is_file():
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="ignore")
        except OSError as error:
            failures.append(f"{path.relative_to(ROOT)}: {error}")
            continue
        failures.extend(check_source(str(path.relative_to(ROOT)), text))
    if failures:
        for failure in failures:
            print(f"license: {failure}", file=sys.stderr)
        return 1
    print(
        "license: OK (no copyleft identifiers in tracked sources, "
        "every shebang script carries SPDX headers)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
