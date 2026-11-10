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

# A hostile or corrupt jar is untrusted input: the extractor writes files, so
# it bounds the entry count and the decompressed bytes, and it refuses any
# entry whose name leaves the output directory.
max_entries = 20_000
max_entry_bytes = 16 * 1024 * 1024
max_total_bytes = 256 * 1024 * 1024


def read_inner_jar(outer: zipfile.ZipFile, version: str) -> zipfile.ZipFile:
    inner_path = f"META-INF/versions/{version}/server-{version}.jar"
    try:
        payload = outer.read(inner_path)
    except KeyError as error:
        raise SystemExit(f"server jar has no {inner_path}") from error
    return zipfile.ZipFile(io.BytesIO(payload))


def template_relative_path(name: str) -> pathlib.PurePosixPath:
    """Return the path below the output directory, or stop on an unsafe name."""
    relative = pathlib.PurePosixPath(name[len("data/") :])
    if (
        not relative.parts
        or relative.is_absolute()
        or ".." in relative.parts
        or "\\" in name
        or (relative.parts and ":" in relative.parts[0])
    ):
        raise SystemExit(f"server jar has an unsafe template path: {name}")
    return relative


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server-jar", required=True, type=pathlib.Path)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--version", default=None)
    args = parser.parse_args()

    with zipfile.ZipFile(args.server_jar) as outer:
        version = args.version or json.loads(outer.read("version.json"))["id"]
        inner = read_inner_jar(outer, version)
        entries = sorted(
            name
            for name in inner.namelist()
            if name.startswith("data/")
            and "/structure/" in name
            and name.endswith(".nbt")
        )
        if not entries:
            raise SystemExit("server jar has no structure templates")
        if len(entries) > max_entries:
            raise SystemExit(
                f"server jar has too many templates: {len(entries)}"
            )
        output_root = args.out.resolve()
        digest = hashlib.sha256()
        count = 0
        total_bytes = 0
        for name in entries:
            relative = template_relative_path(name)
            destination = (args.out / relative).resolve()
            if not destination.is_relative_to(output_root):
                raise SystemExit(f"template path escapes the output: {name}")
            size = inner.getinfo(name).file_size
            if size > max_entry_bytes:
                raise SystemExit(f"template entry is too large: {name}")
            total_bytes += size
            if total_bytes > max_total_bytes:
                raise SystemExit("server jar has too many template bytes")
            destination.parent.mkdir(parents=True, exist_ok=True)
            payload = inner.read(name)
            destination.write_bytes(payload)
            digest.update(str(relative).encode("utf-8"))
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
