# World generation data

Refrain contains no game assets. World generation reads two kinds of
data that come from the Minecraft version the server is built for: the
datapack documents that drive biomes, features, structures and carvers,
and the structure templates that provide the prebuilt pieces.

## Datapack snapshot

The datapack documents are copied into `content/vanilla/<version>/data`
by `tools/worldgen/pack_content.py`, which also writes `MANIFEST.json`
with the game version, data version, entry count and content digest:

```bash
python3 tools/worldgen/pack_content.py \
    --data-root /path/to/extracted/data \
    --out content/vanilla/26.3
```

The snapshot is game-derived data and is covered by the notice in
`content/vanilla/`; it is not part of the repository license.

## Structure templates

Structure templates are binary assets and are not part of the
repository. Extract them from the server jar the operator owns into a
cache directory:

```bash
python3 tools/worldgen/extract_templates.py \
    --server-jar /path/to/server.jar \
    --out ~/.cache/refrain/templates
```

The output uses the layout the template manager expects:

```
<root>/<namespace>/structure/<path>.nbt
```

`TEMPLATES.json` records the game version, template count and content
digest. Extract once per game version; the directory can be regenerated
at any time from the jar.

## Consuming the data

The worldgen layer loads the datapack snapshot from a data root and
templates from the extracted directory. Tests and oracles take both
locations from the environment so the same code paths run against the
files an installation would use.

## Custom feature reference worlds

The feature oracle compares a generated chunk against a reference world
whose biome lists curated placed features. To validate a feature that is not
part of that list, point the generator at a small datapack containing the
configured and placed feature and append it to the generated biome:

```bash
python3 tools/worldgen/oracle/generate.py \
    --server-jar /path/to/server.jar --seed 1 \
    --output /tmp/reference --features --features-only-extra \
    --extra-feature-pack /path/to/pack \
    --extra-feature oracle:my_feature
```

`--extra-feature-pack` merges the pack's `data` tree into the reference
world. `--features-only-extra` drops the curated list so the new feature
keeps its own decoration index and random stream. Run the feature oracle
with the same `REFRAIN_VANILLA_FEATURE_*` variables used by the curated
world; `REFRAIN_VANILLA_FEATURE_CHUNK_X`/`_Z` select one chunk and
`REFRAIN_VANILLA_FEATURE_BORDER_BLOCK` skips chunks whose reference output
touches a border, where features from neighbouring chunks write across the
chunk boundary and a single-chunk oracle cannot reproduce them.

### Root systems and vegetation patches

A root-system reference world can be built from two placed features: a
simple-block feature that writes a stone platform at a fixed position and a
root-system feature placed one block below it. The root system climbs
through the platform and delegates to a nested grass feature above it, so
the chunk exercises the ceiling scan, dirt replacement and hanging roots.
The same pattern works for vegetation patches: place a patch at a fixed
surface position and compare a single chunk. Set
`REFRAIN_VANILLA_FEATURE_CHUNK_X`/`_Z` to that chunk and point
`REFRAIN_VANILLA_FEATURE_REGION` and `REFRAIN_VANILLA_FEATURE_DATA` at the
generated world.

## Verification status

The reference-backed suites currently cover:

- terrain fill, biome selection, surface rules, carvers and feature
  decoration, including a whole-chunk feature oracle;
- structure starts, payload trees, piece placement and full-stage
  generation for jungle temple, warm ocean ruin, beached shipwreck,
  buried treasure and mineshaft;
- `locate_structure` against real seeds and reference heightmaps;
- the storage bridge, RWF round trip and Anvil export/import;
- parallel fill determinism and fixed-biome structure generator
  determinism;
- benchmarks for fill, parallel fill, structure selection, corridor
  placement, template placement, feature freeze and decoration.

Known gaps: the generated-start mineshaft full-stage case has a small
remaining difference in one corridor. Ruined portal starts place the
template at the reference position and orientation. A dry reference
candidate at chunk (-65, 0) matches block for block and heightmap for
heightmap; a candidate that intersects flowing lava is not a stable
oracle after the reference world settles. Woodland mansion pieces match the reference list exactly and its block
oracle matches the reference chunk.
Ocean monuments and strongholds do not have start or block
implementations yet.

