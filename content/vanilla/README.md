# Game-derived data

This directory holds data derived from Minecraft Java Edition 26.3: compiled
block-state, biome and registry tables, climate and default-state reports, the
packet catalog and the protocol registry. The material is Copyright (c) Mojang
Studios / Microsoft Corporation, is not covered by the MIT license of the
repository and is included solely for interoperability with a locally owned
copy of the game. See `NOTICE.md` for the full terms.

The repository tracks only the derived data:

```text
content/vanilla/<version>/
  tables.rvtb               compiled block-state, biome-preset and registry tables
  packets.json              packet catalog for protocol 777
  protocol_registry.json    registry entries, ids and client order
  reports/biome_parameters/ overworld and nether climate parameter points
  reports/block_defaults/   default property values per block
```

The datapack and its `MANIFEST.json` are generated into the same directory from
the `server.jar` you own and are not tracked by git:

```bash
scripts/fetch-content.sh <server.jar>
```

The script chains `refrain-tools extract` and `tools/worldgen/pack_content.py`,
writing `data/` with the worldgen documents and block and biome tags plus the
manifest. Pass `--version` or `--data-version` when they differ from the
defaults. Without the datapack, the world generation data-driven tests are
skipped.
