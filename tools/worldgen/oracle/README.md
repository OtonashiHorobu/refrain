# Chunk oracle

`generate.py` starts the reference server on a world whose data pack removes
every stage that runs after terrain fill:

* the overworld preset uses one fixed biome instead of the multi-noise source,
  so no column takes the eroded-badlands or frozen-ocean extension;
* that biome declares no carvers and no features;
* the `minecraft:overworld` material rule is an empty sequence;
* `server.properties` disables structures.

The saved chunk therefore contains exactly the output of
`NoiseBasedChunkGenerator.doFill`: the default block, the aquifer's fluids and
air, plus the two write-time heightmaps. `test/worldgen/test_chunk_oracle.cpp`
compares that chunk against Refrain's fill stage block for block.

```bash
python3 tools/worldgen/oracle/generate.py \
    --server-jar /path/to/server.jar --seed 12345 --output /tmp/t2oracle
REFRAIN_VANILLA_WORLDGEN=/path/to/server/data \
REFRAIN_VANILLA_REGION=/tmp/t2oracle/world/dimensions/minecraft/overworld/region/r.0.0.mca \
    ./build/Release/bin/refrain-tests
```

`REFRAIN_VANILLA_ORACLE_SEED` overrides the seed (default `12345`) and must
match `--seed`. The test reads chunks `(0,0)`, `(1,0)`, `(0,1)`, `(1,1)` and
`(2,2)`; `generate.py` force-loads that 3x3 area before saving.

The surface oracle keeps the real material rule but pins the overworld to the
fixed `oracle:terrain` biome (no carvers or features), so the saved chunk is
terrain fill plus the real surface pass:

```bash
python3 tools/worldgen/oracle/generate.py --surface \
    --server-jar /path/to/server.jar --seed 12345 --output /tmp/t2surface
REFRAIN_VANILLA_WORLDGEN=/path/to/server/data \
REFRAIN_VANILLA_SURFACE_REGION=/tmp/t2surface/world/dimensions/minecraft/overworld/region/r.0.0.mca \
    ./build/Release/bin/refrain-tests
```

To exercise the surface pass against real multi-noise biomes, `--biomes` keeps
the normal overworld preset but strips carvers and features from every biome:

```bash
python3 tools/worldgen/oracle/generate.py --biomes \
    --server-jar /path/to/server.jar --seed 12345 --output /tmp/t2biomes
REFRAIN_VANILLA_WORLDGEN=/path/to/server/data \
REFRAIN_VANILLA_SURFACE_REGION=/tmp/t2biomes/world/dimensions/minecraft/overworld/region/r.0.0.mca \
REFRAIN_VANILLA_SURFACE_BIOMES=1 \
    ./build/Release/bin/refrain-tests
```

`--fixed-biome` changes the fixed biome of the surface world (the real biome is
copied with empty carvers/features), for example eroded badlands:

```bash
python3 tools/worldgen/oracle/generate.py --surface --fixed-biome minecraft:eroded_badlands \
    --server-jar /path/to/server.jar --seed 12345 --output /tmp/t2badlands
REFRAIN_VANILLA_WORLDGEN=/path/to/server/data \
REFRAIN_VANILLA_SURFACE_REGION=/tmp/t2badlands/world/dimensions/minecraft/overworld/region/r.0.0.mca \
REFRAIN_VANILLA_SURFACE_FIXED_BIOME=minecraft:eroded_badlands \
    ./build/Release/bin/refrain-tests
```

The biome oracle uses a plain overworld instead: `--plain` skips the data pack,
so the normal multi-noise biome source writes real biomes (surface rules,
features and structures still run but do not affect the biome grid):

```bash
python3 tools/worldgen/oracle/generate.py --plain \
    --server-jar /path/to/server.jar --seed 12345 --output /tmp/t2biome
REFRAIN_VANILLA_WORLDGEN=/path/to/server/data \
REFRAIN_VANILLA_BIOME_REGION=/tmp/t2biome/world/dimensions/minecraft/overworld/region/r.0.0.mca \
    ./build/Release/bin/refrain-tests
```

The same switch combines with `--dimension the_nether`: the normal nether
preset stays in place, so the stored biome grid comes from the real multi-noise
source and the climate noises use the historical nether-biome layout. The test
compares every chunk present in the region and takes the seed from the
environment, so neither a seed nor a chunk coordinate is fixed in the test:

```bash
python3 tools/worldgen/oracle/generate.py --plain \
    --dimension the_nether \
    --server-jar /path/to/server.jar --seed 1 --output /tmp/t2nether-biome
REFRAIN_VANILLA_WORLDGEN=/path/to/server/data \
REFRAIN_VANILLA_ORACLE_SEED=1 \
REFRAIN_VANILLA_NETHER_BIOME_REGION=/tmp/t2nether-biome/world/dimensions/minecraft/the_nether/region/r.0.0.mca \
    ./build/Release/bin/refrain-tests
```

The generator needs a JDK (26.3 requires Java 25 or newer) and a locally owned
`server.jar`; it never ships game data. The world it writes is a normal
vanilla world directory under `--output`, with the region files in
`world/dimensions/minecraft/overworld/region/`.

`--dimension the_nether|the_end` writes a reference world of that dimension
instead. The preset of the targeted dimension gets a fixed real biome
(`minecraft:nether_wastes` or `minecraft:the_end`) whose carvers and features
are stripped, the real material rule is kept, `allow-nether` is enabled, and
the force-load command runs inside the dimension. The region lands under
`world/dimensions/minecraft/<dimension>/region`:

```bash
python3 tools/worldgen/oracle/generate.py --dimension the_nether \
    --server-jar /path/to/server.jar --seed 1 --output /tmp/t2nether
REFRAIN_VANILLA_WORLDGEN=/path/to/server/data \
REFRAIN_VANILLA_ORACLE_SEED=1 \
REFRAIN_VANILLA_NETHER_REGION=/tmp/t2nether/world/dimensions/minecraft/the_nether/region/r.0.0.mca \
    ./build/Release/bin/refrain-tests

python3 tools/worldgen/oracle/generate.py --dimension the_end \
    --server-jar /path/to/server.jar --seed 1 --output /tmp/t2end
REFRAIN_VANILLA_WORLDGEN=/path/to/server/data \
REFRAIN_VANILLA_ORACLE_SEED=1 \
REFRAIN_VANILLA_END_REGION=/tmp/t2end/world/dimensions/minecraft/the_end/region/r.0.0.mca \
    ./build/Release/bin/refrain-tests
```

The dimension suites compare chunks `(0,0)`, `(1,0)` and `(0,1)` of the nether
and `(0,0)` and `(1,1)` of the end block for block and all four final
heightmaps. `--fixed-biome` overrides the real biome, and the dimension mode
rejects the overworld-only reference flags.

`--keep-carvers` combines with `--dimension the_nether`: the fixed real biome
keeps its real carvers while features stay stripped and structures stay
disabled. The saved chunks are therefore terrain fill, the real surface pass
and cave carving. The test takes every input from the environment, so no seed
or chunk coordinate is baked into it:

```bash
python3 tools/worldgen/oracle/generate.py --dimension the_nether \
    --keep-carvers --server-jar /path/to/server.jar --seed 1 \
    --output /tmp/t2nether-caves
REFRAIN_VANILLA_WORLDGEN=/path/to/server/data \
REFRAIN_NETHER_CARVER_REGION=/tmp/t2nether-caves/world/dimensions/minecraft/the_nether/region/r.0.0.mca \
REFRAIN_NETHER_CARVER_CHUNK_X=3 REFRAIN_NETHER_CARVER_CHUNK_Z=0 \
REFRAIN_NETHER_CARVER_SEED=1 \
REFRAIN_NETHER_CARVER_BIOME=minecraft:nether_wastes \
    ./build/Release/bin/refrain-tests \
    "the runtime service reproduces a reference nether carver chunk"
```

Chunk `(3,0)` is visibly carved: with the same seed, a reference world whose
default dimension mode strips the carvers differs in 6404 blocks in that
chunk. The runtime reproduces the carved chunk block for block and in all four
final heightmaps.

`--carvers` combines with `--dimension the_nether` to keep the normal
multi-noise nether preset and the biome carvers while every biome's features
are stripped. The saved chunks are the real terrain of the climate-selected
biomes: fill, surface and cave carving. The test reads the seed and the chunk
coordinate from the environment and compares the chunk block for block and in
all four final heightmaps:

```bash
python3 tools/worldgen/oracle/generate.py --carvers \
    --dimension the_nether --settle 40 \
    --server-jar /path/to/server.jar --seed 1 \
    --output /tmp/t2nether-multi
REFRAIN_VANILLA_WORLDGEN=/path/to/server/data \
REFRAIN_VANILLA_ORACLE_SEED=1 \
REFRAIN_VANILLA_NETHER_MULTI_REGION=/tmp/t2nether-multi/world/dimensions/minecraft/the_nether/region/r.0.0.mca \
REFRAIN_VANILLA_NETHER_CHUNK_X=0 REFRAIN_VANILLA_NETHER_CHUNK_Z=0 \
    ./build/Release/bin/refrain-tests \
    "nether multi-noise terrain matches the reference"
```

`--structures` combines with `--dimension`: the targeted dimension gets a real
fixed biome whose carvers and features are stripped, the material rule is
emptied, structures stay enabled and the force-load box runs inside the
dimension. `--only-structure-set` empties every other structure set so a
compared chunk contains one structure type only. The nether run below uses the
fortress candidate in set `minecraft:nether_complexes`; the end run fixes
`minecraft:end_highlands` because end cities only select from the end highlands
and midlands tags:

```bash
python3 tools/worldgen/oracle/generate.py --structures \
    --dimension the_nether --only-structure-set nether_complexes \
    --fixed-biome minecraft:nether_wastes --settle 40 \
    --force-load-box 208 -256 463 -1 \
    --server-jar /path/to/server.jar --seed 1 --output /tmp/t2nether-fortress
REFRAIN_VANILLA_WORLDGEN=/path/to/server/data \
REFRAIN_VANILLA_ORACLE_SEED=1 REFRAIN_VANILLA_STRUCTURES=/path/to/extracted/templates \
REFRAIN_VANILLA_FORTRESS_REGION=/tmp/t2nether-fortress/world/dimensions/minecraft/the_nether/region/r.0.-1.mca \
REFRAIN_VANILLA_FORTRESS_CHUNK_X=21 REFRAIN_VANILLA_FORTRESS_CHUNK_Z=-8 \
    ./build/Release/bin/refrain-tests "the fortress piece list matches the reference start"

REFRAIN_FULL_STAGE_STRUCTURES=1 REFRAIN_FULL_STAGE_DIMENSION=the_nether \
REFRAIN_FULL_STAGE_REGION=/tmp/t2nether-fortress/world/dimensions/minecraft/the_nether/region/r.0.-1.mca \
REFRAIN_FULL_STAGE_CHUNK_X=21 REFRAIN_FULL_STAGE_CHUNK_Z=-8 \
REFRAIN_FULL_STAGE_START_CHUNK_X=21 REFRAIN_FULL_STAGE_START_CHUNK_Z=-8 \
REFRAIN_FULL_STAGE_SEED=1 REFRAIN_FULL_STAGE_BIOME=minecraft:nether_wastes \
    ./build/Release/bin/refrain-tests "fill and structure placement reproduce the reference chunk"

python3 tools/worldgen/oracle/generate.py --structures \
    --dimension the_end --only-structure-set end_cities \
    --fixed-biome minecraft:end_highlands --settle 60 \
    --force-load-box -1296 -368 -1056 -128 \
    --server-jar /path/to/server.jar --seed 1 --output /tmp/t2end-city
REFRAIN_VANILLA_WORLDGEN=/path/to/server/data \
REFRAIN_VANILLA_ORACLE_SEED=1 REFRAIN_VANILLA_STRUCTURES=/path/to/extracted/templates \
REFRAIN_VANILLA_END_CITY_REGION=/tmp/t2end-city/world/dimensions/minecraft/the_end/region/r.-3.-1.mca \
REFRAIN_VANILLA_END_CITY_CHUNK_X=-73 REFRAIN_VANILLA_END_CITY_CHUNK_Z=-15 \
    ./build/Release/bin/refrain-tests "the end city piece list matches the reference start"

REFRAIN_FULL_STAGE_STRUCTURES=1 REFRAIN_FULL_STAGE_DIMENSION=the_end \
REFRAIN_FULL_STAGE_REGION=/tmp/t2end-city/world/dimensions/minecraft/the_end/region/r.-3.-1.mca \
REFRAIN_FULL_STAGE_CHUNK_X=-73 REFRAIN_FULL_STAGE_CHUNK_Z=-15 \
REFRAIN_FULL_STAGE_START_CHUNK_X=-73 REFRAIN_FULL_STAGE_START_CHUNK_Z=-15 \
REFRAIN_FULL_STAGE_SEED=1 REFRAIN_FULL_STAGE_BIOME=minecraft:end_highlands \
    ./build/Release/bin/refrain-tests "fill and structure placement reproduce the reference chunk"
```

Bastion remnants share the nether complexes set with fortresses. Seed 1 puts a
bastion start at chunk `(12, 0)`; the reference world force-loads its pieces and
keeps only `minecraft:nether_complexes`:

```bash
python3 tools/worldgen/oracle/generate.py --structures \
    --dimension the_nether --only-structure-set nether_complexes \
    --fixed-biome minecraft:nether_wastes --settle 40 \
    --force-load-box 160 0 271 63 \
    --server-jar /path/to/server.jar --seed 1 \
    --output /tmp/t2nether-bastion
REFRAIN_VANILLA_WORLDGEN=/path/to/server/data \
REFRAIN_VANILLA_ORACLE_SEED=1 REFRAIN_VANILLA_STRUCTURES=/path/to/templates \
REFRAIN_VANILLA_BASTION_REGION=/tmp/t2nether-bastion/world/dimensions/minecraft/the_nether/region/r.0.0.mca \
REFRAIN_VANILLA_BASTION_CHUNK_X=12 REFRAIN_VANILLA_BASTION_CHUNK_Z=0 \
    ./build/Release/bin/refrain-tests

REFRAIN_FULL_STAGE_STRUCTURES=1 REFRAIN_FULL_STAGE_DIMENSION=the_nether \
REFRAIN_FULL_STAGE_REGION=/tmp/t2nether-bastion/world/dimensions/minecraft/the_nether/region/r.0.0.mca \
REFRAIN_FULL_STAGE_CHUNK_X=12 REFRAIN_FULL_STAGE_CHUNK_Z=0 \
REFRAIN_FULL_STAGE_START_CHUNK_X=12 REFRAIN_FULL_STAGE_START_CHUNK_Z=0 \
REFRAIN_FULL_STAGE_SEED=1 REFRAIN_FULL_STAGE_BIOME=minecraft:nether_wastes \
    ./build/Release/bin/refrain-tests
```

The treasure branch also builds two high-rampart wall pieces that reach north
of that box. A second reference world force-loads those chunks and the
dedicated test compares one chunk the walls cross, including its container
block entities:

```bash
python3 tools/worldgen/oracle/generate.py --structures \
    --dimension the_nether --only-structure-set nether_complexes \
    --fixed-biome minecraft:nether_wastes --settle 40 \
    --force-load-box 160 -64 271 63 \
    --server-jar /path/to/server.jar --seed 1 \
    --output /tmp/t2nether-rampart

REFRAIN_VANILLA_WORLDGEN=/path/to/server/data \
REFRAIN_VANILLA_STRUCTURES=/path/to/templates \
REFRAIN_BASTION_RAMPART_FULL_STAGE_STRUCTURES=1 \
REFRAIN_BASTION_RAMPART_FULL_STAGE_DIMENSION=the_nether \
REFRAIN_BASTION_RAMPART_FULL_STAGE_REGION=/tmp/t2nether-rampart/world/dimensions/minecraft/the_nether/region/r.0.-1.mca \
REFRAIN_BASTION_RAMPART_FULL_STAGE_CHUNK_X=13 \
REFRAIN_BASTION_RAMPART_FULL_STAGE_CHUNK_Z=-2 \
REFRAIN_BASTION_RAMPART_FULL_STAGE_START_CHUNK_X=12 \
REFRAIN_BASTION_RAMPART_FULL_STAGE_START_CHUNK_Z=0 \
REFRAIN_BASTION_RAMPART_FULL_STAGE_SEED=1 \
REFRAIN_BASTION_RAMPART_FULL_STAGE_BIOME=minecraft:nether_wastes \
REFRAIN_BASTION_RAMPART_FULL_STAGE_TEMPLATE_DIRS=minecraft/structure/bastion \
    ./build/Release/bin/refrain-tests \
    "a bastion high rampart chunk matches the reference full-stage world"
```

The bastion start suite compares the complete piece list, including the pool
element, rotation, ground-level delta and junctions. The nether fossils set has
a two-chunk spacing, so seed 1 with a force-load box near the origin yields
several starts. The fossil biome tag only contains soul sand valleys, so the
fixed biome for that run is `minecraft:soul_sand_valley` rather than
`minecraft:nether_wastes`:

```bash
python3 tools/worldgen/oracle/generate.py --structures \
    --dimension the_nether --only-structure-set nether_fossils \
    --fixed-biome minecraft:soul_sand_valley --settle 40 \
    --force-load-box 0 0 127 63 \
    --server-jar /path/to/server.jar --seed 1 \
    --output /tmp/t2nether-fossil
REFRAIN_FULL_STAGE_STRUCTURES=1 REFRAIN_FULL_STAGE_DIMENSION=the_nether \
REFRAIN_FULL_STAGE_REGION=/tmp/t2nether-fossil/world/dimensions/minecraft/the_nether/region/r.0.0.mca \
REFRAIN_FULL_STAGE_CHUNK_X=2 REFRAIN_FULL_STAGE_CHUNK_Z=0 \
REFRAIN_FULL_STAGE_START_CHUNK_X=2 REFRAIN_FULL_STAGE_START_CHUNK_Z=0 \
REFRAIN_FULL_STAGE_SEED=1 REFRAIN_FULL_STAGE_BIOME=minecraft:soul_sand_valley \
    ./build/Release/bin/refrain-tests
```

A nether fossil reference chunk compares the fossil blocks, the ignored air and
structure blocks, and the dried ghast the piece may add at its floor. The same
command with chunk `(2, 2)` covers a rotated fossil whose piece spans two
chunks.

The start tests compare the complete piece list (bounds, orientation, piece id,
template name, rotation and overwrite flag) against the reference chunk. The
full-stage run compares the selected chunk block for block and all four final
heightmaps, including the `structure_generator` service path. The end city
world uses a fixed highlands biome, so its start anchors are the same terrain
corners the reference queries; the compared chunk has no block or heightmap
gap. Entity markers (shulkers and elytra) are outside the block comparison.

The structure oracle keeps the normal world preset and generates structures
for a fixed biome. `--force-load-box` moves the force-loaded area when the
structure candidates are not near the origin:

```bash
python3 tools/worldgen/oracle/generate.py --structures \
    --fixed-biome minecraft:plains --settle 60 \
    --force-load-box -272 -592 -240 -560 \
    --server-jar /path/to/server.jar --seed 1 --output /tmp/t2portal
REFRAIN_VANILLA_WORLDGEN=/path/to/server/data \
REFRAIN_VANILLA_STRUCTURES=/path/to/extracted/templates \
REFRAIN_FULL_STAGE_STRUCTURES=1 \
REFRAIN_FULL_STAGE_REGION=/tmp/t2portal/world/dimensions/minecraft/overworld/region/r.-1.-2.mca \
REFRAIN_FULL_STAGE_CHUNK_X=-16 REFRAIN_FULL_STAGE_CHUNK_Z=-36 \
REFRAIN_FULL_STAGE_SEED=1 REFRAIN_FULL_STAGE_BIOME=minecraft:plains \
    ./build/Release/bin/refrain-tests "fill and structure placement reproduce the reference chunk"
```

`REFRAIN_VANILLA_STRUCTURES` points at a directory containing the extracted
`<namespace>/structure/...nbt` templates. The full-stage test fills the chunk
and its neighbours, generates the starts, places the pieces and compares both
blocks and the final heightmaps.

An ocean monument owns its child pieces, so its start covers sixteen chunks.
Set `REFRAIN_FULL_STAGE_START_CHUNK_X` and
`REFRAIN_FULL_STAGE_START_CHUNK_Z` to place the start owned by another chunk
into the target chunk:

```bash
python3 tools/worldgen/oracle/generate.py --structures \
    --fixed-biome minecraft:deep_ocean --settle 60 \
    --force-load-box 128 304 287 463 \
    --server-jar /path/to/server.jar --seed 1 --output /tmp/t2monument
REFRAIN_VANILLA_WORLDGEN=/path/to/server/data \
REFRAIN_VANILLA_STRUCTURES=/path/to/extracted/templates \
REFRAIN_FULL_STAGE_STRUCTURES=1 \
REFRAIN_FULL_STAGE_REGION=/tmp/t2monument/world/dimensions/minecraft/overworld/region/r.0.0.mca \
REFRAIN_FULL_STAGE_CHUNK_X=10 REFRAIN_FULL_STAGE_CHUNK_Z=21 \
REFRAIN_FULL_STAGE_START_CHUNK_X=12 \
REFRAIN_FULL_STAGE_START_CHUNK_Z=23 \
REFRAIN_FULL_STAGE_SEED=1 REFRAIN_FULL_STAGE_BIOME=minecraft:deep_ocean \
    ./build/Release/bin/refrain-tests "fill and structure placement reproduce the reference chunk"
```

The monument candidate is chunk `(12, 23)`; its building covers chunks
`(10..13, 21..24)`.

Strongholds use concentric rings instead of a random spread. Seed 1 with a
fixed `minecraft:plains` biome places one at chunk `(108, 51)`; a force-load box
around chunks `(101..115, 44..58)` covers its pieces. Because the box is away
from the origin the generated regions are `r.3.0.mca` and `r.3.1.mca`; the
generator prints the region it found.

```bash
python3 tools/worldgen/oracle/generate.py --structures \
    --fixed-biome minecraft:plains --settle 60 \
    --force-load-box 1616 704 1855 943 \
    --server-jar /path/to/server.jar --seed 1 --output /tmp/t2stronghold
REFRAIN_VANILLA_WORLDGEN=/path/to/server/data \
REFRAIN_VANILLA_STRUCTURES=/path/to/extracted/templates \
REFRAIN_VANILLA_STRONGHOLD_REGION=/tmp/t2stronghold/world/dimensions/minecraft/overworld/region/r.3.1.mca \
REFRAIN_FULL_STAGE_STRUCTURES=1 \
REFRAIN_FULL_STAGE_REGION=/tmp/t2stronghold/world/dimensions/minecraft/overworld/region/r.3.1.mca \
REFRAIN_FULL_STAGE_CHUNK_X=104 REFRAIN_FULL_STAGE_CHUNK_Z=50 \
REFRAIN_FULL_STAGE_START_CHUNK_X=108 \
REFRAIN_FULL_STAGE_START_CHUNK_Z=51 \
REFRAIN_FULL_STAGE_SEED=1 REFRAIN_FULL_STAGE_BIOME=minecraft:plains \
    ./build/Release/bin/refrain-tests "fill and structure placement reproduce the reference chunk"
```

`worldgen/chunk-generator` runs the production service against the same
reference worlds. It reads the village case from the `REFRAIN_FULL_STAGE_*`
variables above and the mansion and stronghold cases from
`REFRAIN_RUNTIME_MANSION_*` and `REFRAIN_RUNTIME_STRONGHOLD_*` variables with
the same `_REGION`, `_CHUNK_X`, `_CHUNK_Z`, `_SEED`, `_BIOME` and `_DIMENSION`
suffixes. The optional `_STRUCTURE_SET` suffix restricts the generation to one
structure set. Each case compares the chunk and its four final heightmaps:

```bash
REFRAIN_VANILLA_WORLDGEN=/path/to/server/data \
REFRAIN_VANILLA_STRUCTURES=/path/to/extracted/templates \
REFRAIN_RUNTIME_MANSION_REGION=/tmp/t2mansion/world/dimensions/minecraft/overworld/region/r.1.0.mca \
REFRAIN_RUNTIME_MANSION_CHUNK_X=46 REFRAIN_RUNTIME_MANSION_CHUNK_Z=5 \
REFRAIN_RUNTIME_MANSION_SEED=1 REFRAIN_RUNTIME_MANSION_BIOME=minecraft:dark_forest \
REFRAIN_RUNTIME_STRONGHOLD_REGION=/tmp/t2stronghold/world/dimensions/minecraft/overworld/region/r.3.1.mca \
REFRAIN_RUNTIME_STRONGHOLD_CHUNK_X=108 REFRAIN_RUNTIME_STRONGHOLD_CHUNK_Z=51 \
REFRAIN_RUNTIME_STRONGHOLD_SEED=1 REFRAIN_RUNTIME_STRONGHOLD_BIOME=minecraft:plains \
REFRAIN_RUNTIME_STRONGHOLD_STRUCTURE_SET=minecraft:strongholds \
    ./build/Release/bin/refrain-tests "the runtime service reproduces"
```

### Jigsaw coverage: outpost, taiga village, ancient city, trial chambers and trail ruins

Five further jigsaw structures have reference starts and full-stage chunks.
The candidates come from `locate structure` in a probe world with the same
fixed biome; the force-load boxes below cover the start and the pieces the
compared chunk needs.

A pillager outpost uses the plains tag. Seed 1 places its start at chunk
`(5, 20)`; the box covers chunks `(0..11, 16..27)` and the start chunk holds
the base plate, the watchtower list element and every feature plate:

```bash
python3 tools/worldgen/oracle/generate.py --structures \
    --fixed-biome minecraft:plains --only-structure-set pillager_outposts \
    --settle 30 --force-load-box 0 256 191 447 \
    --server-jar /path/to/server.jar --seed 1 --output /tmp/t2outpost

REFRAIN_VANILLA_WORLDGEN=/path/to/server/data \
REFRAIN_VANILLA_STRUCTURES=/path/to/templates \
REFRAIN_JIGSAW_PILLAGER_OUTPOST_REGION=/tmp/t2outpost/world/dimensions/minecraft/overworld/region/r.0.0.mca \
REFRAIN_JIGSAW_PILLAGER_OUTPOST_CHUNK_X=5 \
REFRAIN_JIGSAW_PILLAGER_OUTPOST_CHUNK_Z=20 \
REFRAIN_JIGSAW_PILLAGER_OUTPOST_SEED=1 \
    ./build/Release/bin/refrain-tests \
    "the pillager outpost start matches the reference piece list"

REFRAIN_VANILLA_WORLDGEN=/path/to/server/data \
REFRAIN_VANILLA_STRUCTURES=/path/to/templates \
REFRAIN_JIGSAW_OUTPOST_FULL_STAGE_STRUCTURES=1 \
REFRAIN_JIGSAW_OUTPOST_FULL_STAGE_REGION=/tmp/t2outpost/world/dimensions/minecraft/overworld/region/r.0.0.mca \
REFRAIN_JIGSAW_OUTPOST_FULL_STAGE_CHUNK_X=5 \
REFRAIN_JIGSAW_OUTPOST_FULL_STAGE_CHUNK_Z=20 \
REFRAIN_JIGSAW_OUTPOST_FULL_STAGE_START_CHUNK_X=5 \
REFRAIN_JIGSAW_OUTPOST_FULL_STAGE_START_CHUNK_Z=20 \
REFRAIN_JIGSAW_OUTPOST_FULL_STAGE_SEED=1 \
REFRAIN_JIGSAW_OUTPOST_FULL_STAGE_BIOME=minecraft:plains \
    ./build/Release/bin/refrain-tests \
    "a pillager outpost chunk matches the reference full-stage world"
```

A taiga village only selects the taiga biome. Seed 1 places its start at
chunk `(15, 8)`. The full-stage case compares chunk `(15, 9)`, one chunk south
of the start, where every piece comes from the village templates; the patch of
taiga grass at the start chunk corner reads neighbouring structure blocks and
depends on the reference world's chunk generation order:

```bash
python3 tools/worldgen/oracle/generate.py --structures \
    --fixed-biome minecraft:taiga --only-structure-set villages \
    --settle 30 --force-load-box 128 0 303 159 \
    --server-jar /path/to/server.jar --seed 1 --output /tmp/t2village-taiga

REFRAIN_VANILLA_WORLDGEN=/path/to/server/data \
REFRAIN_VANILLA_STRUCTURES=/path/to/templates \
REFRAIN_JIGSAW_VILLAGE_TAIGA_REGION=/tmp/t2village-taiga/world/dimensions/minecraft/overworld/region/r.0.0.mca \
REFRAIN_JIGSAW_VILLAGE_TAIGA_CHUNK_X=15 \
REFRAIN_JIGSAW_VILLAGE_TAIGA_CHUNK_Z=8 \
REFRAIN_JIGSAW_VILLAGE_TAIGA_SEED=1 \
    ./build/Release/bin/refrain-tests \
    "the taiga village start matches the reference piece list"

REFRAIN_VANILLA_WORLDGEN=/path/to/server/data \
REFRAIN_VANILLA_STRUCTURES=/path/to/templates \
REFRAIN_JIGSAW_VILLAGE_FULL_STAGE_STRUCTURES=1 \
REFRAIN_JIGSAW_VILLAGE_FULL_STAGE_REGION=/tmp/t2village-taiga/world/dimensions/minecraft/overworld/region/r.0.0.mca \
REFRAIN_JIGSAW_VILLAGE_FULL_STAGE_CHUNK_X=15 \
REFRAIN_JIGSAW_VILLAGE_FULL_STAGE_CHUNK_Z=9 \
REFRAIN_JIGSAW_VILLAGE_FULL_STAGE_START_CHUNK_X=15 \
REFRAIN_JIGSAW_VILLAGE_FULL_STAGE_START_CHUNK_Z=8 \
REFRAIN_JIGSAW_VILLAGE_FULL_STAGE_SEED=1 \
REFRAIN_JIGSAW_VILLAGE_FULL_STAGE_BIOME=minecraft:taiga \
    ./build/Release/bin/refrain-tests \
    "a taiga village chunk matches the reference full-stage world"
```

An ancient city only selects the deep dark. Seed 1 places its start at chunk
`(9, 11)` and the box covers chunks `(0..11, 4..14)`. The start chunk contains
the city's water and lava pools, and the reference world lets those fluids
flow after saving, so the start chunk is not a stable block oracle. The
full-stage case therefore compares chunk `(11, 12)`, a dry chunk the start's
pieces cover:

```bash
python3 tools/worldgen/oracle/generate.py --structures \
    --fixed-biome minecraft:deep_dark --only-structure-set ancient_cities \
    --settle 40 --force-load-box 0 64 191 239 \
    --server-jar /path/to/server.jar --seed 1 --output /tmp/t2ancient

REFRAIN_VANILLA_WORLDGEN=/path/to/server/data \
REFRAIN_VANILLA_STRUCTURES=/path/to/templates \
REFRAIN_JIGSAW_ANCIENT_CITY_REGION=/tmp/t2ancient/world/dimensions/minecraft/overworld/region/r.0.0.mca \
REFRAIN_JIGSAW_ANCIENT_CITY_CHUNK_X=9 \
REFRAIN_JIGSAW_ANCIENT_CITY_CHUNK_Z=11 \
REFRAIN_JIGSAW_ANCIENT_CITY_SEED=1 \
    ./build/Release/bin/refrain-tests \
    "the ancient city start matches the reference piece list"

REFRAIN_VANILLA_WORLDGEN=/path/to/server/data \
REFRAIN_VANILLA_STRUCTURES=/path/to/templates \
REFRAIN_JIGSAW_ANCIENT_FULL_STAGE_STRUCTURES=1 \
REFRAIN_JIGSAW_ANCIENT_FULL_STAGE_REGION=/tmp/t2ancient/world/dimensions/minecraft/overworld/region/r.0.0.mca \
REFRAIN_JIGSAW_ANCIENT_FULL_STAGE_CHUNK_X=11 \
REFRAIN_JIGSAW_ANCIENT_FULL_STAGE_CHUNK_Z=12 \
REFRAIN_JIGSAW_ANCIENT_FULL_STAGE_START_CHUNK_X=9 \
REFRAIN_JIGSAW_ANCIENT_FULL_STAGE_START_CHUNK_Z=11 \
REFRAIN_JIGSAW_ANCIENT_FULL_STAGE_SEED=1 \
REFRAIN_JIGSAW_ANCIENT_FULL_STAGE_BIOME=minecraft:deep_dark \
    ./build/Release/bin/refrain-tests \
    "an ancient city chunk matches the reference full-stage world"
```

Trial chambers select from every overworld biome except oceans, mushroom
fields and the deep dark. Seed 1 places its start at chunk `(18, 14)`; the
structure uses a uniform start height, pool aliases and encapsulating terrain
adaptation, and the start chunk is dry:

```bash
python3 tools/worldgen/oracle/generate.py --structures \
    --fixed-biome minecraft:plains --only-structure-set trial_chambers \
    --settle 40 --force-load-box 224 160 367 303 \
    --server-jar /path/to/server.jar --seed 1 --output /tmp/t2trial

REFRAIN_VANILLA_WORLDGEN=/path/to/server/data \
REFRAIN_VANILLA_STRUCTURES=/path/to/templates \
REFRAIN_JIGSAW_TRIAL_CHAMBERS_REGION=/tmp/t2trial/world/dimensions/minecraft/overworld/region/r.0.0.mca \
REFRAIN_JIGSAW_TRIAL_CHAMBERS_CHUNK_X=18 \
REFRAIN_JIGSAW_TRIAL_CHAMBERS_CHUNK_Z=14 \
REFRAIN_JIGSAW_TRIAL_CHAMBERS_SEED=1 \
    ./build/Release/bin/refrain-tests \
    "the trial chambers start matches the reference piece list"

REFRAIN_VANILLA_WORLDGEN=/path/to/server/data \
REFRAIN_VANILLA_STRUCTURES=/path/to/templates \
REFRAIN_JIGSAW_TRIAL_FULL_STAGE_STRUCTURES=1 \
REFRAIN_JIGSAW_TRIAL_FULL_STAGE_REGION=/tmp/t2trial/world/dimensions/minecraft/overworld/region/r.0.0.mca \
REFRAIN_JIGSAW_TRIAL_FULL_STAGE_CHUNK_X=18 \
REFRAIN_JIGSAW_TRIAL_FULL_STAGE_CHUNK_Z=14 \
REFRAIN_JIGSAW_TRIAL_FULL_STAGE_START_CHUNK_X=18 \
REFRAIN_JIGSAW_TRIAL_FULL_STAGE_START_CHUNK_Z=14 \
REFRAIN_JIGSAW_TRIAL_FULL_STAGE_SEED=1 \
REFRAIN_JIGSAW_TRIAL_FULL_STAGE_BIOME=minecraft:plains \
    ./build/Release/bin/refrain-tests \
    "a trial chambers chunk matches the reference full-stage world"
```

A trail ruins start is underground and only selects the taiga-family biomes.
Seed 1 places its start at chunk `(0, -12)`; the box covers the start and every
piece the compared chunk needs. The chunk holds six brushable block entities,
three from the common capped list and three from the rare one, so the case
exercises capped delegates and the `append_loot` rule modifier:

```bash
python3 tools/worldgen/oracle/generate.py --structures \
    --fixed-biome minecraft:taiga --only-structure-set trail_ruins \
    --settle 25 --force-load-box -128 -288 143 -81 \
    --server-jar /path/to/server.jar --seed 1 --output /tmp/t2trail

REFRAIN_VANILLA_WORLDGEN=/path/to/server/data \
REFRAIN_VANILLA_STRUCTURES=/path/to/templates \
REFRAIN_TRAIL_RUINS_FULL_STAGE_STRUCTURES=1 \
REFRAIN_TRAIL_RUINS_FULL_STAGE_REGION=/tmp/t2trail/world/dimensions/minecraft/overworld/region/r.0.-1.mca \
REFRAIN_TRAIL_RUINS_FULL_STAGE_CHUNK_X=0 \
REFRAIN_TRAIL_RUINS_FULL_STAGE_CHUNK_Z=-12 \
REFRAIN_TRAIL_RUINS_FULL_STAGE_START_CHUNK_X=0 \
REFRAIN_TRAIL_RUINS_FULL_STAGE_START_CHUNK_Z=-12 \
REFRAIN_TRAIL_RUINS_FULL_STAGE_SEED=1 \
REFRAIN_TRAIL_RUINS_FULL_STAGE_BIOME=minecraft:taiga \
    ./build/Release/bin/refrain-tests \
    "a trail ruins chunk matches the reference full-stage world"
```

The trail ruins case compares loot-table block entities by default: every
reference block entity that stores `LootTable` must exist at the same position
with the same `LootTable` and `LootTableSeed`. The same comparison runs for a
trial chambers chest chunk; chunk `(21, 15)` with start chunk `(18, 14)` holds
two chests:

```bash
REFRAIN_VANILLA_WORLDGEN=/path/to/server/data \
REFRAIN_VANILLA_STRUCTURES=/path/to/templates \
REFRAIN_JIGSAW_TRIAL_LOOT_FULL_STAGE_STRUCTURES=1 \
REFRAIN_JIGSAW_TRIAL_LOOT_FULL_STAGE_REGION=/tmp/t2trial/world/dimensions/minecraft/overworld/region/r.0.0.mca \
REFRAIN_JIGSAW_TRIAL_LOOT_FULL_STAGE_CHUNK_X=21 \
REFRAIN_JIGSAW_TRIAL_LOOT_FULL_STAGE_CHUNK_Z=15 \
REFRAIN_JIGSAW_TRIAL_LOOT_FULL_STAGE_START_CHUNK_X=18 \
REFRAIN_JIGSAW_TRIAL_LOOT_FULL_STAGE_START_CHUNK_Z=14 \
REFRAIN_JIGSAW_TRIAL_LOOT_FULL_STAGE_SEED=1 \
REFRAIN_JIGSAW_TRIAL_LOOT_FULL_STAGE_BIOME=minecraft:plains \
    ./build/Release/bin/refrain-tests \
    "a trial chambers chest chunk matches the reference loot block entities"
```

Any other full-stage case enables the same comparison with a non-empty
`<PREFIX>_BLOCK_ENTITIES` variable.

The start cases read `_STRUCTURE`, `_BIOME` and `_TEMPLATE_DIRS` overrides when
the structure document differs from the defaults. Every candidate value comes
from the environment, so no seed or chunk coordinate is fixed in the tests.

The full-stage runs compare blocks and all four final heightmaps; the full
heightmap and runtime-service checks only run when the compared chunk owns the
start, as in the outpost case. `REFRAIN_FULL_STAGE_MISMATCHES` raises the
number of differing positions a failing comparison prints.

Other structure sets generate starts in neighbouring chunks of the reference
world. For a whole-structure sweep, replace every other
`worldgen/structure_set/*.json` in the generated datapack with an empty
`structures` list, delete the overworld region files, and rerun the server
with the same force-load box; chunks `(104..110, 49..57)` then reference only
the stronghold.
