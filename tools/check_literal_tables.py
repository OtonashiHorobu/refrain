#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
"""Guard against game tables duplicated as C++ literals (CI).

The game-derived tables live under ``content/vanilla`` and the runtime loads
them from there. A table compiled into ``src/`` or ``include/`` is a second
source of truth: it silently drifts from the content snapshot and a data fix
never reaches the runtime.

The check compares the resource locations named by each C++ source file with
the identifier sets of the content documents. A source file that names nearly
every identifier of one content document (at least 16 identifiers and 90% of
the document) is a duplicate of that table, because the same data is now
compiled in twice. Comment-only mentions are ignored: the scan strips C++
comments before collecting string literals, so a comment that explains a table
is not a copy of it.

The datapack documents under ``content/vanilla/*/data`` are only present when
the checkout has a locally generated pack; the tracked reports, the protocol
registry and the packet catalog are always scanned. A missing pack therefore
narrows the check but never disables it.

Usage:
    python3 tools/check_literal_tables.py [--root DIR] [--verbose]
    python3 tools/check_literal_tables.py --self-test

Exit code 0 = clean, 1 = a content table is duplicated in C++ source, 2 = the
check could not run (no content tables found).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CONTENT_DIR = Path("content/vanilla")
SOURCE_DIRS = ("src", "include")
SOURCE_SUFFIXES = {".cpp", ".cc", ".cxx", ".hpp", ".h", ".inl"}

# A content document needs this many identifiers before it counts as a table;
# smaller sets are incidental mentions rather than a table worth guarding.
MIN_TABLE = 16
# A source file duplicates a document when it names at least this fraction of
# the document's identifiers.
MIN_COVERAGE = 0.9

RESOURCE_LOCATION = re.compile(r"^[a-z0-9_.-]+:[a-z0-9_./-]+$")
CPP_IDENTIFIER = re.compile(r'"([a-z0-9_.-]+:[a-z0-9_./\[\]=,.-]+)"')


def strip_comments(text: str) -> str:
    """Removes C++ comments while keeping string and character literals."""
    out: list[str] = []
    index = 0
    length = len(text)
    while index < length:
        character = text[index]
        if character == '"':
            end = index + 1
            while end < length:
                if text[end] == "\\":
                    end += 2
                    continue
                if text[end] == '"':
                    end += 1
                    break
                end += 1
            out.append(text[index:end])
            index = end
            continue
        if character == "'":
            end = index + 1
            while end < length:
                if text[end] == "\\":
                    end += 2
                    continue
                if text[end] == "'":
                    end += 1
                    break
                end += 1
            out.append(text[index:end])
            index = end
            continue
        if character == "/" and index + 1 < length and text[index + 1] == "/":
            end = text.find("\n", index)
            if end < 0:
                break
            index = end
            continue
        if character == "/" and index + 1 < length and text[index + 1] == "*":
            end = text.find("*/", index + 2)
            if end < 0:
                break
            out.append("\n" * text.count("\n", index, end))
            index = end + 2
            continue
        out.append(character)
        index += 1
    return "".join(out)


def resource_locations(node: object) -> set[str]:
    """Every resource location mentioned anywhere in a JSON document."""
    found: set[str] = set()
    stack = [node]
    while stack:
        current = stack.pop()
        if isinstance(current, str):
            if RESOURCE_LOCATION.match(current):
                found.add(current)
        elif isinstance(current, dict):
            for key, value in current.items():
                if RESOURCE_LOCATION.match(key):
                    found.add(key)
                stack.append(value)
        elif isinstance(current, list):
            stack.extend(current)
    return found


def base_identifier(identifier: str) -> str:
    """Drops a block-state suffix: `minecraft:oak_log[axis=y]` -> the block."""
    return identifier.split("[", 1)[0]


def content_tables(root: Path) -> dict[str, set[str]]:
    """Identifier sets of the content documents, keyed by path relative to root.

    The walk follows the datapack symlink a lane worktree uses, and sorts every
    directory and file so the output order never depends on the file system.
    """
    tables: dict[str, set[str]] = {}
    content_root = root / CONTENT_DIR
    if not content_root.is_dir():
        return tables
    for dirpath, dirnames, filenames in os.walk(content_root, followlinks=True):
        dirnames.sort()
        for name in sorted(filenames):
            if not name.endswith(".json"):
                continue
            path = Path(dirpath) / name
            try:
                document = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, UnicodeDecodeError, json.JSONDecodeError):
                # A malformed content document is not this guard's contract:
                # the schema and data checks report it with the right context.
                continue
            identifiers = resource_locations(document)
            if len(identifiers) >= MIN_TABLE:
                tables[str(path.relative_to(root))] = identifiers
    return tables


def source_identifiers(path: Path) -> set[str]:
    text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
    return {
        base_identifier(match.group(1))
        for match in CPP_IDENTIFIER.finditer(text)
    }


def scan(root: Path) -> list[tuple[str, str, int, int]]:
    """Duplicates as (source path, content path, source count, shared count)."""
    tables = content_tables(root)
    if not tables:
        raise SystemExit(
            "literal-table guard: no content tables under "
            f"{root / CONTENT_DIR}; the content snapshot is missing"
        )
    duplicates: list[tuple[str, str, int, int]] = []
    for source_dir in SOURCE_DIRS:
        base = root / source_dir
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
                continue
            identifiers = source_identifiers(path)
            if len(identifiers) < MIN_TABLE:
                continue
            for content_path, table in tables.items():
                shared = identifiers & table
                if len(shared) < MIN_TABLE:
                    continue
                if len(shared) / len(table) >= MIN_COVERAGE:
                    duplicates.append(
                        (
                            str(path.relative_to(root)),
                            content_path,
                            len(identifiers),
                            len(shared),
                        )
                    )
    return duplicates


def self_test() -> int:
    """Proves the detector flags a copy, ignores a partial list and comments."""
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        biomes = [f"minecraft:test_biome_{index:02d}" for index in range(20)]
        report = root / "content/vanilla/26.3/reports/biome_parameters"
        report.mkdir(parents=True)
        (report / "overworld.json").write_text(
            json.dumps(
                {
                    "format": 1,
                    "dimension": "minecraft:overworld",
                    "entries": [{"biome": biome} for biome in biomes],
                }
            ),
            encoding="utf-8",
        )
        source = root / "src"
        source.mkdir()
        (source / "duplicate.cpp").write_text(
            "const char *const table[] = {\n"
            + "".join(f'    "{biome}",\n' for biome in biomes)
            + "};\n",
            encoding="utf-8",
        )
        (source / "partial.cpp").write_text(
            "const char *const table[] = {\n"
            + "".join(f'    "{biome}",\n' for biome in biomes[:10])
            + "};\n",
            encoding="utf-8",
        )
        (source / "commented.cpp").write_text(
            "// The table is data:\n"
            + "".join(f"// {biome}\n" for biome in biomes)
            + "const char *const table[] = {\n"
            '    "minecraft:unrelated_block",\n'
            "};\n",
            encoding="utf-8",
        )
        duplicates = scan(root)
        if len(duplicates) != 1:
            print(
                "literal-table guard self-test: expected one duplicate, got "
                f"{len(duplicates)}: {duplicates}",
                file=sys.stderr,
            )
            return 1
        if duplicates[0][0] != "src/duplicate.cpp":
            print(
                "literal-table guard self-test: flagged the wrong file: "
                f"{duplicates[0]}",
                file=sys.stderr,
            )
            return 1
    print(
        "literal-table guard self-test: copy detected, partial and "
        "comment-only lists ignored"
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run the detector against a synthetic duplicated table and exit",
    )
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    root = args.root.resolve()
    duplicates = scan(root)
    if duplicates:
        for source, content, source_count, shared in duplicates:
            print(
                f"{source}: {source_count} resource locations, {shared} shared "
                f"with {content}; the table is already content data",
                file=sys.stderr,
            )
        print(
            f"literal-table guard: {len(duplicates)} content table(s) "
            "duplicated in C++ source",
            file=sys.stderr,
        )
        return 1
    if args.verbose:
        print(
            f"literal-table guard: {len(content_tables(root))} content "
            "table(s) checked, no duplicates"
        )
    else:
        print(
            f"literal-table guard: {len(content_tables(root))} content "
            "table(s), 0 duplicate(s)"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
