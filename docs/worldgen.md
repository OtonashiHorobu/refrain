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

## Biome parameter reports

The overworld and nether biome selection tables are game data. They are
committed next to the datapack snapshot under
`content/vanilla/<version>/reports/biome_parameters/`:

```text
reports/biome_parameters/
  MANIFEST.json   game/data version, entry counts, sha256 of each report
  overworld.json  overworld climate parameter points, in selection order
  nether.json     nether climate parameter points, in selection order
```

Each report records the game version, data version, dimension id, entry count
and the ordered entries. An entry carries the biome id, the quantized minimum
and maximum of the six climate parameters, and the quantized offset. Worldgen
loads a report with `load_biome_parameters(data_root, dimension)`, where
`data_root` is the sibling `data` directory. The loader rejects a report whose
metadata, entry count, dimension or entries do not decode exactly, so a table
cannot silently drift. The reports are covered by the notice in
`content/vanilla/`.

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

## Dimensions

`dimension_profile_for(dimension_kind)` supplies the fixed parameters of the
overworld, the nether and the end: the level bounds, sea level, coordinate
scale, noise-settings path and material-rule path. The generator reads the
noise settings the profile names and uses their default block, default fluid
and generation geometry, so no dimension literal lives in the generation code.

The noise settings of the nether and the end are 128 blocks tall while their
level is 256 blocks tall. Fill and surface therefore run over y 0..127 and the
level above can only be air; heightmaps and the stored chunk still cover the
whole 256-block level. Their settings also select the legacy positional random
source, which the loader takes from the document rather than the caller.

The biome source is dimension-specific: the overworld and the nether select a
biome from their climate parameter report (`reports/biome_parameters/`), and
the end selects from its island bands. A fixed-biome source backs reference
worlds whose preset declares a single biome.

The nether and the end have no carver stage; carvers belong to the overworld
pipeline. The structure generator runs starts in every dimension: the nether
generates fortresses and the end generates end cities, next to the overworld
structure types. A structure start reads the dimension's noise settings through
the same profile the terrain stages use.

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
- nether and end terrain fill and surface rules against reference worlds of
  those dimensions, plus deterministic fixed-biome chunks of both dimensions;
- structure starts, payload trees, piece placement and full-stage
  generation for jungle temple, warm ocean ruin, beached shipwreck,
  buried treasure, mineshaft, ocean monument, stronghold, nether fortress,
  end city and a plains village start, including its feature pool elements;
- `locate_structure` against real seeds and reference heightmaps;
- the storage bridge, RWF round trip and Anvil export/import;
- parallel fill determinism and fixed-biome structure generator
  determinism;
- benchmarks for fill, parallel fill, structure selection, corridor
  placement, template placement, feature freeze and decoration.

Known gaps: structure feature placement has no climate source, so snow and
freeze feature modifiers are inert there. Capped processor lists,
block-entity rule modifiers and template entities are not implemented; end
city shulker and elytra data markers therefore leave no entities behind.
Bastion remnants, end gateways and the obsidian end platform are not
implemented; a `nether_complexes` draw that selects a bastion falls through to
the fortress entry. The nether climate samplers use the standard noise layout,
while the reference builds the temperature and vegetation noises of that
dimension with the legacy nether-biome initializer, so a nether world's biome
selection can differ from the reference even though the parameter table it
selects from does not.

Ruined portal starts place the template at the reference position and
orientation. A dry reference candidate at chunk (-65, 0) matches block for
block and heightmap for heightmap; a candidate that intersects flowing lava
is not a stable oracle after the reference world settles. Woodland mansion
pieces match the reference list exactly and its block oracle matches the
reference chunk. An ocean monument start plus every room, wing and penthouse
piece matches the reference world in the sixteen chunks the building covers.
A stronghold start plus every fitted corridor and room matches the reference
world in the sixty-three chunks the pieces cover, and the ring placement
reproduces the reference candidate chunks.

The dimension oracle of seed 1 matches nether chunks `(0,0)`, `(1,0)` and
`(0,1)` and end chunks `(0,0)` and `(1,1)` block for block and in all four
final heightmaps. Both reference worlds fix a real biome and strip its carvers
and features, so they contain exactly the fill and surface output of that
dimension. The deterministic dimension test repeats the fixed-biome chunks
without a reference world, using the snapshot this repository ships.

A second dimension reference world keeps one structure set and uses an empty
material rule, so a compared chunk contains terrain fill plus structure pieces
only. The full-stage test selects the dimension through
`REFRAIN_FULL_STAGE_DIMENSION` and takes the level bounds, noise settings and
sea level from the dimension profile; the reference fortress start and end city
start piece lists are compared separately. The fortress and end city reference
runs are documented in `tools/worldgen/oracle/README.md`.

