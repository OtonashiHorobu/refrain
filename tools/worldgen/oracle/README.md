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

Other structure sets generate starts in neighbouring chunks of the reference
world. For a whole-structure sweep, replace every other
`worldgen/structure_set/*.json` in the generated datapack with an empty
`structures` list, delete the overworld region files, and rerun the server
with the same force-load box; chunks `(104..110, 49..57)` then reference only
the stronghold.
