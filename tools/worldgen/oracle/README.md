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
