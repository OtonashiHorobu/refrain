#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
"""Line coverage from a gcov-instrumented build.

gcov resolves the notes file from the object name and the source from the
current directory, so it has to be called from the repository root with the
object path and the mirrored object directory. gcovr did not reproduce that
pairing here, which is why this walks the notes files itself.

    tools/coverage_report.py --build build/coverage/build/Release --root .
    tools/coverage_report.py --build ... --root . --top 20
    tools/coverage_report.py --build ... --root . --untouched
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

# A .gcov line is "<count>:<lineno>:<source>". The count is "-" when the line
# cannot execute, "#####" or "=====" when it never ran, and a number (possibly
# followed by "*") when it did.
COUNT = re.compile(r"^(\s*)(-|#####|=====|[0-9]+\*?):\s*[0-9]+:")


def parse_gcov(path: Path) -> tuple[int, int]:
    """Return (executable lines, executed lines) for one .gcov file."""
    executable = 0
    executed = 0
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = COUNT.match(line)
        if not match:
            continue
        count = match.group(2)
        if count == "-":
            continue
        executable += 1
        if count not in ("#####", "=====") and int(count.rstrip("*")) > 0:
            executed += 1
    return executable, executed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", required=True, help="build directory with .gcno files")
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--top", type=int, default=15, help="worst files to list")
    parser.add_argument("--untouched", action="store_true", help="list files with no executed line")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    build = Path(args.build).resolve()
    notes = sorted(build.rglob("*.gcno"))
    if not notes:
        print(f"no .gcno under {build}; build with -DREFRAIN_COVERAGE=ON", file=sys.stderr)
        return 2

    per_file: dict[str, list[int]] = defaultdict(lambda: [0, 0])
    # gcov writes its .gcov next to the working directory and resolves the
    # source relative to it, so the run happens in the repository root and the
    # produced file is removed straight away.
    for note in notes:
        # src/core/log.cpp.gcno -> source src/core/log.cpp, object log.cpp.o
        # CMakeFiles/<target>.dir/<source path>.gcno; the source path is what
        # follows the .dir/ component.
        relative = note.relative_to(build)
        parts = relative.parts
        marker = next((i for i, part in enumerate(parts) if part.endswith(".dir")), None)
        if marker is None:
            continue
        relative = Path(*parts[marker + 1:])
        object_name = relative.name.replace(".gcno", ".o")
        source = Path(str(relative).replace(".gcno", ""))
        subprocess.run(
            ["gcov", "-o", str(note.parent), object_name],
            cwd=root,
            capture_output=True,
            text=True,
            check=False,
        )
        produced = root / f"{source.name}.gcov"
        if produced.exists():
            executable, executed = parse_gcov(produced)
            if executable != 0 and str(source).startswith("src/"):
                slot = per_file[str(source)]
                slot[0] += executable
                slot[1] += executed
        # gcov writes one file per source it can attribute, which includes every
        # header it walked; none of those are read here, and leaving them behind
        # would litter the repository root with a few hundred generated files.
        for leftover in root.glob("*.gcov"):
            leftover.unlink()

    if not per_file:
        print("no coverage collected for src/", file=sys.stderr)
        return 2

    total_executable = sum(v[0] for v in per_file.values())
    total_executed = sum(v[1] for v in per_file.values())

    if args.untouched:
        empty = sorted(name for name, (e, x) in per_file.items() if x == 0)
        print(f"{len(empty)} file(s) with no executed line:")
        for name in empty:
            print(f"  {name}")
        return 0

    print(f"files:    {len(per_file)}")
    print(f"lines:    {total_executed}/{total_executable} "
          f"({100.0 * total_executed / total_executable:.1f}%)")
    print()
    print("lowest coverage:")
    ordered = sorted(per_file.items(), key=lambda kv: (kv[1][1] / kv[1][0], -kv[1][0]))
    for name, (executable, executed) in ordered[: args.top]:
        print(f"  {100.0 * executed / executable:5.1f}%  {executed:6d}/{executable:<6d} {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
