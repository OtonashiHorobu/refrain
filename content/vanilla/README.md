# Game-derived data

This directory holds data derived from Minecraft: block state tables, biome
parameters, registry listings and data pack snapshots. It is the only place in
the repository where such data may live.

The directory is empty until the content pipeline lands. When it does, it is
laid out per game version:

```text
content/vanilla/<version>/
  MANIFEST.json        game version, data version, entry count, content digest
  data/                worldgen documents and block/biome tags
  reports/             compact tables converted from the game's datagen reports
```

`MANIFEST.json` records the game and data version, the entry count and a
`content_sha256` digest over every entry name and its bytes. A regenerated
snapshot is compared against the committed digest; per-file hashes and the
source jar digest are not recorded.

The contents are not covered by the repository license; see `NOTICE.md` and the
root `NOTICE.md`. Data is regenerated from a locally owned copy of the game:

```bash
python3 tools/worldgen/pack_content.py --data-root <extracted>/data \
    --out content/vanilla/<version>
```

Removing the directory is supported: the build then requires the user's jar and
generates the snapshot at install time.
