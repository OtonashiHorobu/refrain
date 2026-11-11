#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
"""Require a matching DCO sign-off on every commit in a range.

Every commit must carry a trailer of the form

    Signed-off-by: Name <email>

whose name and email equal the commit author. Such a trailer certifies the
Developer Certificate of Origin 1.1 (https://developercertificate.org/). The
comparison is exact; a sign-off from another person or address does not count.

Usage:
    python3 tools/check_dco.py main~3..main
    python3 tools/check_dco.py origin/main..HEAD

Exit code 0 = every commit is signed off, 1 = at least one commit is not,
2 = the range or repository could not be read.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SIGN_OFF = re.compile(r"^(.*?)\s*<([^<>]+)>$")


class GitError(RuntimeError):
    """A git command failed or returned unusable output."""


def git(*args: str) -> str:
    result = subprocess.run(
        ["git", *args],
        cwd=ROOT,
        capture_output=True,
        text=True,
        errors="replace",
        check=False,
    )
    if result.returncode != 0:
        message = result.stderr.strip() or f"git {' '.join(args)} failed"
        raise GitError(message)
    return result.stdout


def resolve(rev: str, label: str) -> str:
    try:
        return git("rev-parse", "--verify", f"{rev}^{{commit}}").strip()
    except GitError as error:
        raise GitError(
            f"{label} revision {rev!r} does not name a commit: {error}"
        ) from error


def parse_range(spec: str) -> tuple[str, str]:
    if "..." in spec or spec.count("..") != 1:
        raise GitError(f"range {spec!r} must have the form BASE..HEAD")
    base, separator, head = spec.partition("..")
    if not separator or not base or not head:
        raise GitError(f"range {spec!r} must have the form BASE..HEAD")
    return base, head


def commit_ids(base: str, head: str) -> list[str]:
    output = git("rev-list", "--reverse", f"{base}..{head}")
    return output.split()


def commit_header(commit: str) -> tuple[str, str, str, str]:
    output = git("show", "-s", "--format=%h%x00%s%x00%an%x00%ae", commit)
    fields = output.rstrip("\n").split("\0")
    if len(fields) != 4:
        raise GitError(f"cannot read the author of {commit}")
    short, subject, name, email = fields
    return short, subject, name, email


def sign_offs(commit: str) -> list[tuple[str, str]]:
    output = git(
        "show",
        "-s",
        "--format=%(trailers:key=Signed-off-by,valueonly)",
        commit,
    )
    parsed = []
    for line in output.splitlines():
        match = SIGN_OFF.match(line.strip())
        if match:
            parsed.append((match.group(1).strip(), match.group(2).strip()))
    return parsed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("range", metavar="BASE..HEAD", help="commit range to check")
    args = parser.parse_args()

    try:
        base_rev, head_rev = parse_range(args.range)
        base = resolve(base_rev, "base")
        head = resolve(head_rev, "head")
        commits = commit_ids(base, head)
    except GitError as error:
        print(f"dco: {error}", file=sys.stderr)
        return 2

    failures = []
    for commit in commits:
        try:
            short, subject, name, email = commit_header(commit)
            found = sign_offs(commit)
        except GitError as error:
            print(f"dco: {error}", file=sys.stderr)
            return 2
        if (name, email) not in found:
            failures.append((short, subject, name, email, found))

    if failures:
        print(
            f"dco: {len(failures)} of {len(commits)} commit(s) lack the "
            "author's Signed-off-by trailer",
            file=sys.stderr,
        )
        for short, subject, name, email, found in failures:
            print(f"  {short} {subject}", file=sys.stderr)
            print(f"    author: {name} <{email}>", file=sys.stderr)
            if found:
                for found_name, found_email in found:
                    print(
                        f"    found:  {found_name} <{found_email}> "
                        "(does not match the author)",
                        file=sys.stderr,
                    )
            else:
                print("    found:  none", file=sys.stderr)
        print(
            "dco: add the trailer with `git commit --amend -s` for the last "
            "commit or `git rebase --signoff BASE` for the branch",
            file=sys.stderr,
        )
        return 1

    print(f"dco: OK ({len(commits)} commit(s) signed off)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
