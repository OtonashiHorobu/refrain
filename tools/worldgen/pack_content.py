#!/usr/bin/env python3
"""Copy the worldgen datapack documents into the content snapshot.

The snapshot contains only the files world generation reads: every
`<namespace>/worldgen/**` document plus the block and biome tag trees. It is
game-derived data, so the output belongs under `content/vanilla/<version>/`
and carries the directory notice.

Usage:
    pack_content.py --data-root /path/to/server/data --out content/vanilla/26.3
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import shutil

VERSION = "26.3"
DATA_VERSION = 5023
KINDS = ("worldgen", "tags/block", "tags/worldgen")


def collect(data_root: pathlib.Path) -> list[tuple[str, pathlib.Path]]:
    files: list[tuple[str, pathlib.Path]] = []
    for namespace in sorted(data_root.iterdir()):
        if not namespace.is_dir():
            continue
        for kind in KINDS:
            base = namespace / kind
            if not base.is_dir():
                continue
            for path in sorted(base.rglob("*")):
                if path.is_file():
                    files.append(
                        (
                            f"{namespace.name}/{kind}/"
                            f"{path.relative_to(base).as_posix()}",
                            path,
                        )
                    )
    return files


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-root", required=True, type=pathlib.Path)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--version", default=VERSION)
    parser.add_argument("--data-version", type=int, default=DATA_VERSION)
    args = parser.parse_args()

    files = collect(args.data_root.resolve())
    if not files:
        raise SystemExit(f"no worldgen documents under {args.data_root}")

    target = args.out / "data"
    shutil.rmtree(target, ignore_errors=True)
    digest = hashlib.sha256()
    for name, path in files:
        destination = target / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, destination)
        digest.update(name.encode("utf-8"))
        digest.update(b"\0")
        digest.update(destination.read_bytes())
        digest.update(b"\0")

    args.out.mkdir(parents=True, exist_ok=True)
    manifest = {
        "game_version": args.version,
        "data_version": args.data_version,
        "datapack": "data",
        "content_sha256": digest.hexdigest(),
        "entry_count": len(files),
        "kinds": list(KINDS),
        "source": "server.jar datapack",
    }
    (args.out / "MANIFEST.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    print(f"{target} ({len(files)} files)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
