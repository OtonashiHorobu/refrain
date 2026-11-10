#!/usr/bin/env python3
"""Write the default block-state properties report for world generation.

The datapack names a block without properties when a feature places its
default state. The world generator needs to know the properties of that state
to reproduce state-dependent behaviour such as the second half of a double
plant. This tool reduces the data generator's `blocks.json` report to one
default property map per block and writes it into the locally generated
datapack snapshot.

Usage:
    java -DbundlerMainClass=net.minecraft.data.Main -jar server.jar --reports
    python3 tools/worldgen/block_defaults.py \
        --blocks generated/reports/blocks.json \
        --out content/vanilla/26.3/reports/block_defaults

The output is game-derived data, is generated locally and is not committed;
it carries its own notice.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib

VERSION = "26.3"
DATA_VERSION = 5023

NOTICE = """# Notice for game-derived data

The files in this directory are derived from Minecraft Java Edition and are

    Copyright (c) Mojang Studios / Microsoft Corporation.

They are **not** covered by the MIT license that applies to the rest of this
repository. No license is granted to copy, redistribute, sublicense or reuse
this data. It is included only so that this server can interoperate with a
locally owned copy of the game, and it should be removed if you do not own one.

`defaults.json` records, for every block whose default state carries
properties, the property values of that state. `MANIFEST.json` records the
game version, the data version and the digest of the file.
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--blocks", required=True, type=pathlib.Path)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    args = parser.parse_args()

    blocks = json.loads(args.blocks.read_text())
    defaults: dict[str, dict[str, str]] = {}
    for name, block in sorted(blocks.items()):
        found = [
            state.get("properties", {})
            for state in block.get("states", [])
            if state.get("default")
        ]
        if len(found) != 1:
            raise SystemExit(f"{name} does not have exactly one default state")
        if found[0]:
            defaults[name] = found[0]

    args.out.mkdir(parents=True, exist_ok=True)
    document = (
        json.dumps(
            {
                "game_version": VERSION,
                "data_version": DATA_VERSION,
                "blocks": defaults,
            },
            sort_keys=True,
            separators=(",", ":"),
        )
        + "\n"
    )
    (args.out / "defaults.json").write_text(document)
    (args.out / "NOTICE.md").write_text(NOTICE)
    manifest = {
        "data_version": DATA_VERSION,
        "files": {
            "defaults.json": {
                "bytes": len(document.encode("utf-8")),
                "block_count": len(defaults),
                "sha256": hashlib.sha256(document.encode("utf-8")).hexdigest(),
            }
        },
        "game_version": VERSION,
        "kind": "block_defaults",
        "notice": "../../../NOTICE.md",
    }
    (args.out / "MANIFEST.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
