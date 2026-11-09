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
scale, noise-settings path, material-rule path and whether the dimension has a
carver stage. The generator reads the noise settings the profile names and uses
their default block, default fluid and generation geometry, so no dimension
literal lives in the generation code.

The nether and the end have 128-block noise settings while their level is 256
blocks tall. Fill and surface therefore run over y 0..127 and the level above
can only be air; heightmaps and the stored chunk still cover the whole
256-block level. Their settings also select the legacy positional random
source, which the loader takes from the document rather than the caller.

The nether runs the carvers its biomes declare: the `nether_cave` document is a
cave carver with the nether generation bounds, a floor level and a large start
vertical radius. The end declares no carvers, so its pipeline has none. The
carver stage runs after the surface pass over the generation depth, not over the
full level height, and applies the resulting mask to the filled chunk.

The biome source is dimension-specific: the overworld and the nether select a
biome from their climate parameter report (`reports/biome_parameters/`), and
the end selects from its island bands. A fixed-biome source backs reference
worlds whose preset declares a single biome.

The structure generator runs starts in every dimension: the nether generates
fortresses, bastion remnants and fossils, and the end generates end cities,
next to the overworld structure types. A structure start reads the
dimension's noise settings through the same profile the terrain stages use.
Height providers sample over the generator's generation depth, which is shorter
than the level for the nether and the end, while jigsaw padding and distance
limits use the level bounds.

## Full-chunk generation

`worldgen::chunk_generator` is the production entry point that runs every
stage in one reusable object. `chunk_generator_config` names the content data
root, the extracted template root, the seed and the dimension kind. A fixed
biome key selects one biome everywhere; an empty key uses the dimension's biome
source. `generation_target` stops at `terrain` (biomes, fill, surface,
carvers), at `features` (terrain plus structure starts, references, placed
pieces and feature decoration), or at `full` (the features target plus the
final status). `structure_sets` restricts starts to the listed sets for oracle
runs, and an empty list enables every set. An empty template root still
generates procedural pieces and features; template-backed pieces stay empty.
Three reference-only switches reproduce a saved reference chunk that contains
its neighbours' feature writes: `decorate_neighbours` decorates the eight
neighbour chunks through the same region, `neighbours_before_target` selects
whether their writes come before or after the target's own features, and
`neighbours_in_generation_order` follows the order a force-load uses: every
chunk that precedes the target in ascending x and z decorates first, then the
target, then the rest. A mixed order matters for any feature that writes across
a border, such as an ore vein or a vegetation patch. A fixed biome is required
because every decorated chunk uses the target's biome plan.

One object owns the block state registry and its tags, the dimension profile,
the biome source, the surface rule state, the loaded carvers' runtime objects,
the feature registry, the structure data and template manager, and the terrain
engine. The immutable half of the datapack load — noise settings and their
density documents, material rule documents, climates, noise functions, block
tags, carvers, the feature data set and per-step plan, and the structure data —
is loaded once by `create` and shared by reference with every worker of a
batch. A worker keeps only its mutable state: the block state registry, the
multi-noise search, the template cache and the start-probe caches.

The stages run in the order the full-stage oracle uses. Structure starts are
resolved before fill because their terrain adaptation reaches the fill through
the beardifier; the start and reference records are written to the chunk after
the terrain stages. Targets that include structures or features fill the
target chunk and its eight neighbours; pieces of the target chunk's starts are
clipped into the owner and the neighbours so the fence, wall and stair repair
sees the complete building, and feature decoration runs last through the same
region. The final heightmaps are primed in one top-down pass per column when
the terrain status is reached and updated by every piece and feature write, so
they cover the finished chunk.

## Density sampling

The final density contains the two sampling layers the documents declare. An
interpolated node builds a coarse lattice over the requested block volume and
samplers fill the lattice corners: the overworld and the nether use a cell
width of four blocks on x and z and a cell height of eight, and the end uses
eight on x and z with a height of four. The lattice gains one extra row,
column or layer when the requested volume does not end on a cell boundary, and
the block values between corners are trilinearly interpolated in the
reference's float order, including its incremental y accumulation. A volume
whose steps equal the cell size and whose minimum lies on a cell boundary is
sampled directly, and a volume with a non-block step samples a block volume
and picks the samples.

Interpolation covers only the sub-expression the document wraps. The remaining
terms of the final density, such as the cave noises and the structure
beardifier, are evaluated for every block. A lattice over the whole final
density would therefore change generated blocks and is not used.

A referenced density document compiles once and every reference to it shares
the same graph. A `cache` node keeps the values its input sampled for one grid
in the sampling pass, and a grid that does not match replaces the entry, so a
context can be reused across chunks. The cache changes the amount of sampling,
not its results: cached and direct bulk sampling agree bit for bit. Scalar
sampling never reads the cached grid, because some nodes round the scalar and
bulk paths differently and terrain generation reads the bulk path.

The multi-noise search is reset before each chunk, which makes a chunk
independent of the chunks generated before it. `generate_batch` creates one
independent generator per worker and returns chunks in input order. Workers
are retained by the calling object because a returned chunk borrows its
block state table; the same applies to `generate`, whose chunk must not
outlive the generator. A batch hands each worker runs of consecutive
positions rather than single chunks, so the bounded per-worker start-probe
caches stay warm: each cached column holds the six unbearded heightmap rows
a start reads, or the vertical block column the fossil start scans. Those
columns are pure functions of the seed and data, so a cached probe produces
the same start as a freshly generated one.

`reference_world` reproduces the structure oracle's reference worlds: fill and
structure pieces only, with the surface pass, carvers and features omitted.
The chunk still reaches the configured target status. `reference_carvers` is a
second reference mode for `reference_world` runs: it keeps the real surface
pass and the dimension's carvers, while features stay off. Selecting
`generation_target::terrain` with it also disables structures, so the chunk is
exactly the fill, surface and carving output of the dimension. A production run
(without `reference_world`) always executes the dimension's carvers.

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
  those dimensions, a carved nether reference chunk compared through the
  runtime service, plus deterministic fixed-biome chunks of both dimensions;
- structure starts, payload trees, piece placement and full-stage
  generation for jungle temple, warm ocean ruin, beached shipwreck,
  buried treasure, mineshaft, ocean monument, stronghold, nether fortress,
  bastion remnant, nether fossil, end city, a plains village start, a
  pillager outpost, a taiga village, an ancient city, trial chambers and
  trail ruins, including their feature pool elements;
- `locate_structure` against real seeds and reference heightmaps;
- the storage bridge, RWF round trip and Anvil export/import;
- parallel fill determinism and fixed-biome structure generator
  determinism;
- the full-chunk generator against the reference full-stage worlds of a
  plains village, a woodland mansion, a stronghold, a pillager outpost, a
  taiga village, an ancient city and trial chambers, plus batch equality and
  the terrain/features/full status ladder;
- the configured feature, placement modifier, block predicate, state provider
  and tree placer families the standard biomes name, through synthetic unit
  tests and a content-driven test that creates one generator and generates one
  full chunk for every standard overworld, nether and end biome;
- fixed-biome feature reference worlds for cherry grove, plains, desert and
  badlands: the runtime runs the real features with structure starts disabled
  and decorates the target chunk and its eight neighbours, and the compared
  chunk matches the reference block for block and in all four final
  heightmaps. The spawn terrain of those worlds is ocean, so the chunks cover
  the underground families (ores, disks, geodes, lava lakes, springs) and
  their fluids. The reference command and environment variables are recorded
  in `tools/worldgen/oracle/README.md`;
- a fixed-biome warm ocean feature world reproduces its chunk block for block
  and in all four final heightmaps: chunk `(0,-1)` of the seed-1 reference
  world reaches zero with neighbour decoration in force-load order. The chunk
  exercises the weighted tall/short seagrass selection, sea pickles and the
  cross-border writes of both. Heightmap placements read the heightmap of the
  chunk that holds the candidate column, so a candidate the offset modifier
  spreads into a neighbour uses that neighbour's height;
- fixed-biome feature worlds for lush caves, deep dark and basalt deltas do
  not reach zero yet. Lush caves improves from 103 to 16 remaining blocks once
  a vegetation patch distributes its vegetation in the reference hash-set
  order, floods and plants a waterlogged patch the way the reference does, and
  skips edge columns when the patch declares no edge chance. The residual
  blocks come from the face-sturdy approximation and a few patch boundaries.
  The deep-dark sculk patch spreads veins from a non-vein source and grows
  more of the reference field, but its spread order still differs. The basalt
  delta and column features place different shapes. The best scanned chunks,
  their remaining mismatch counts and the commands are recorded in
  `tools/worldgen/oracle/README.md`;
- benchmarks for fill, parallel fill, structure selection, corridor
  placement, template placement, feature freeze and decoration.

Known gaps: the feature registry decodes every configured feature, placement
modifier, block predicate and state-provider type the shipped snapshot uses,
including the cherry, poplar, bending and upwards-branching tree placers, the
mangrove root placer and the pale moss and creaking heart decorators. The pine
and mega pine foliage placers read their canopy height provider under the name
each documents (`height` and `crown_height`); a pine placed feature that lacks
the provider places nothing instead of falling back to a default height. A biome
whose plan names an unknown type still fails `chunk_generator::create` with an
`unsupported` error instead of silently dropping that feature, and the
content-driven test fails when that happens. Predicates whose reference
definition reads collision geometry (sturdy faces and multiface attachment)
approximate a full face with the heightmap motion-blocking property, and the
unobstructed predicate admits every position because generation has no
entities, and the biome membership predicate accepts explicit biome ids but not
biome tags. Structure feature placement has no climate source, so snow and
freeze feature modifiers are inert there. Template entities are not placed;
end city shulker and elytra data markers, pillager cages and village animal
markers therefore leave no entities behind. Capped processor lists decode and
run their delegate over the whole piece, with constant and uniform limits; a
limit document that names another provider type is rejected instead of being
approximated. Rule block entity modifiers are decoded for `clear`,
`passthrough`, `append_static` and `append_loot`. A rule that names a loot
table keeps the seed drawn from its own positional stream, while a container
tag carried by the template still receives the piece stream's seed at
placement. Processor rule position predicates decode `always_true`,
`linear_pos` and `axis_aligned_linear_pos`; the two linear forms measure the
world position against the reference position of the structure start (the
centre of its first piece's box at the box floor), so the template-local
position does not participate. A missing predicate is `always_true` and draws
nothing. End gateways and the obsidian end platform are not implemented.

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

The jigsaw framework is validated on the overworld structures the structure
oracle adds: seed 1 places a pillager outpost start at chunk `(5, 20)`, a taiga
village start at chunk `(15, 8)`, an ancient city start at chunk `(9, 11)` and
a trial chambers start at chunk `(18, 14)`. The start piece lists match the
reference children exactly, including list elements, the outpost feature
plates, the ancient city's start anchor and the trial chambers pool aliases.
A full-stage reference chunk of each structure matches block for block and in
all four final heightmaps: the outpost and trial chambers start chunks, a taiga
village chunk one chunk south of the start and an ancient city chunk away from
its fluid pools. A trail ruins chunk exercises both capped archaeology lists:
the reference and runtime chunks agree on every block, heightmap and brushable
block entity, including the loot seed each capped selection stored. A trial
chambers chest chunk agrees on its container loot tables and seeds as well.
The clean chunks are chosen deliberately: reference worlds settle flowing
water and lava after saving, so a chunk whose pieces place fluids cannot be
compared block for block, and a feature element that writes into the compared
chunk from a neighbouring chunk depends on the order those chunks were
generated. The commands and environment variables are recorded in
`tools/worldgen/oracle/README.md`.

The nether structure oracle also compares a full-stage bastion remnant chunk
and a full-stage nether fossil chunk block for block and in all four final
heightmaps. The bastion start list and junctions match the reference for the
treasure, bridge, stable and housing/units branches; the fossil start and its
dried ghast placement match a soul-sand-valley reference chunk. A treasure
chunk crossed by the two high-rampart wall pieces matches block for block,
in all four final heightmaps and in its container block entities, which
exercises the axis-aligned linear position predicate. A hoglin stable chunk
matches the same way after wall sides stop connecting to hanging lanterns; the
four-block blackstone wall difference seen there was a final shape rescan
approximation, not a rule or position predicate difference. The dimension
oracle of seed 1 matches nether chunks `(0,0)`, `(1,0)` and `(0,1)` and end
chunks `(0,0)` and `(1,1)` block for block and in all four final heightmaps.
Both reference worlds fix a real biome and strip its carvers and features, so
they contain exactly the fill and surface output of that dimension. The
deterministic dimension test repeats the fixed-biome chunks without a reference
world, using the snapshot this repository ships.

A nether reference world keeps the fixed biome's real carvers and the real
material rule while features and structures stay off. The runtime service
reproduces a chunk with visible cave carving block for block and in all four
final heightmaps. `tools/worldgen/oracle/README.md` records the command and the
environment variables of that case.

The two nether climate noises use the historical nether-biome octave layout:
their generators come from legacy streams seeded with the world seed plus a
per-noise offset, and each octave stack follows the older descending octave
order. The other dimensions and every other noise document keep the standard
layout. A plain nether reference world (no data pack, no fixed biome) compares
the stored 4x4x4 biome grid of every full chunk in the region against the
runtime climate sampler, and a nether world that keeps the real multi-noise
biomes and carvers but strips features compares a carved terrain chunk block
for block and in all four final heightmaps. Both tests take the seed and, for
the terrain chunk, the chunk coordinate from the environment;
`tools/worldgen/oracle/README.md` records the commands.

A second dimension reference world keeps one structure set and uses an empty
material rule, so a compared chunk contains terrain fill plus structure pieces
only. The full-stage test selects the dimension through
`REFRAIN_FULL_STAGE_DIMENSION` and takes the level bounds, noise settings and
sea level from the dimension profile; the reference fortress start and end city
start piece lists are compared separately. The fortress and end city reference
runs are documented in `tools/worldgen/oracle/README.md`.

