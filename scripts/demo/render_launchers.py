#!/usr/bin/env python3
"""Render the Windows demo launchers into a staged package directory.

The demo package assembly copies a staged client and server tree into one
directory. This script writes the four launchers from the templates next to it,
substituting the offline player UUID printed by the client's launch info. The
templates carry the parts that are easy to get wrong on Windows: the client
classpath is read with `for /f` instead of `set /p` (cmd truncates a `set /p`
assignment at 1023 characters), and the server launcher prints the process exit
code plus the default log and crash paths.

Usage:
    python3 scripts/demo/render_launchers.py --root <staged-package> --uuid <uuid>

The assembly tool may also import `render()` and call it directly.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

TEMPLATE_DIR = Path(__file__).resolve().parent / "launchers"
OFFLINE_UUID_PLACEHOLDER = "@OFFLINE_UUID@"
UUID_PATTERN = re.compile(
    r"^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-"
    r"[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"
)


def render(root: Path, offline_uuid: str) -> list[Path]:
    """Write every launcher template into `root` and return the written paths."""
    if not UUID_PATTERN.fullmatch(offline_uuid):
        raise ValueError(f"offline UUID is not a canonical UUID: {offline_uuid!r}")
    templates = sorted(TEMPLATE_DIR.glob("*.cmd.in"))
    if not templates:
        raise FileNotFoundError(f"no launcher templates under {TEMPLATE_DIR}")
    root.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    for template in templates:
        text = template.read_text(encoding="utf-8")
        text = text.replace(OFFLINE_UUID_PLACEHOLDER, offline_uuid)
        # cmd.exe needs CRLF line endings for reliable `^` continuation and
        # multi-line `if` blocks.
        data = text.replace("\r\n", "\n").replace("\n", "\r\n")
        target = root / template.name.removesuffix(".in")
        target.write_bytes(data.encode("utf-8"))
        written.append(target)
    return written


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root", required=True, type=Path, help="staged demo package directory"
    )
    parser.add_argument(
        "--uuid",
        required=True,
        help="offline player UUID from the staged client launch info",
    )
    args = parser.parse_args()
    try:
        written = render(args.root, args.uuid)
    except (OSError, ValueError) as error:
        parser.exit(1, f"error: {error}\n")
    print(f"demo launchers: wrote {len(written)} files to {args.root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
