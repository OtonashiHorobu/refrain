# Notice for game-derived data

This directory contains data derived from Minecraft Java Edition 26.3.

Tracked in the repository:

- `tables.rvtb` — compiled block-state, biome-preset, registry and item tables.
- `reports/biome_parameters/` — climate parameter points that select the
  overworld and nether biomes.
- `reports/block_defaults/` — default property values for blocks whose default
  state carries properties.
- `packets.json` — the packet catalog for protocol 777.
- `protocol_registry.json` — registry entries, ids and the order the server
  sends to clients.
- `README.md` and this notice.

Generated locally and not tracked:

- `data/` — the worldgen documents and the block and biome tags.
- `MANIFEST.json` — the game version, data version, entry count and content
  digest of the generated datapack.

The material is

    Copyright (c) Mojang Studios / Microsoft Corporation.

It is **not** covered by the MIT license that applies to the rest of this
repository and remains the intellectual property of Mojang Studios / Microsoft
Corporation. It is included solely for interoperability with a locally owned
copy of the game, under the terms of the
[Minecraft End User License Agreement](https://www.minecraft.net/en-us/eula)
and the
[Mojang Brand and Assets Guidelines](https://www.minecraft.net/en-us/usage-guidelines).

Vanilla Minecraft datapacks are not distributed in this repository. Generate
the datapack and its manifest from the `server.jar` you own:

```bash
scripts/fetch-content.sh <server.jar>
```

The script extracts the data entries with `refrain-tools extract` and packs the
worldgen documents and tags with `tools/worldgen/pack_content.py`. It writes
`content/vanilla/<version>/{data,MANIFEST.json}`; pass `--version` or
`--data-version` when they differ from the defaults. Both outputs are
game-derived data with the same terms and are not tracked by git.

Without them, the world generation data-driven tests are skipped.
