#!/usr/bin/env python3
"""Extract structure templates from a Minecraft server jar.

The repository ships no game assets. World generation reads structure
templates from a directory laid out as `<root>/<namespace>/structure/...`;
this tool produces that directory from the jar the operator already owns,
so the extracted files live in a cache or data directory and never in the
repository.

Usage:
    extract_templates.py --server-jar /path/to/server.jar --out ~/.cache/refrain/templates
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import pathlib
import zipfile


def read_inner_jar(outer: zipfile.ZipFile) -> zipfile.ZipFile:
    version = json.loads(outer.read("version.json"))["id"]
    inner_path = f"META-INF/versions/{version}/server-{version}.jar"
    try:
        payload = outer.read(inner_path)
    except KeyError as error:
        raise SystemExit(f"server jar has no {inner_path}") from error
    return zipfile.ZipFile(io.BytesIO(payload))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server-jar", required=True, type=pathlib.Path)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--version", default=None)
    args = parser.parse_args()

    with zipfile.ZipFile(args.server_jar) as outer:
        version = args.version or json.loads(outer.read("version.json"))["id"]
        inner = read_inner_jar(outer)
        entries = sorted(
            name
            for name in inner.namelist()
            if name.startswith("data/")
            and "/structure/" in name
            and name.endswith(".nbt")
        )
        if not entries:
            raise SystemExit("server jar has no structure templates")
        digest = hashlib.sha256()
        count = 0
        for name in entries:
            relative = name[len("data/") :]
            destination = args.out / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            payload = inner.read(name)
            destination.write_bytes(payload)
            digest.update(relative.encode("utf-8"))
            digest.update(b"\0")
            digest.update(payload)
            count += 1

    manifest = {
        "game_version": version,
        "kind": "structure_templates",
        "entry_count": count,
        "content_sha256": digest.hexdigest(),
        "layout": "<namespace>/structure/**/*.nbt",
    }
    (args.out / "TEMPLATES.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    print(f"{args.out} ({count} templates)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
