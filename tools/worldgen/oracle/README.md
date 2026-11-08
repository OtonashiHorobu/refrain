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
