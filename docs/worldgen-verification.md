# Worldgen verification

The reference-backed suites compare the generated blocks and heightmaps of a
chunk against a world produced by the reference server. The world generators
live under `tools/worldgen/oracle`; every test is selected by environment
variables and reads the data and template roots from the caller.

## Terrain

| Stage | Oracle |
|---|---|
| fill | plain fixed-biome world |
| biome source | overworld preset without a fixed biome |
| surface rules | real material rule, fixed biome |
| carvers | real biomes and carvers, no features |
| feature decoration | fixed biome with curated placed features |

## Structures

| Structure | Start | Blocks | Full stage |
|---|---|---|---|
| buried treasure | yes | yes | yes |
| mineshaft | yes | yes | yes |
| jungle temple | yes | yes | yes |
| swamp hut | yes | synthetic | no |
| desert pyramid | yes | synthetic | no |
| igloo | yes | synthetic | no |
| shipwreck | yes | yes | yes |
| ocean ruin | yes | yes | yes |
| ruined portal | yes | yes | yes |
| woodland mansion | yes | yes | yes |
| ocean monument | yes | yes | yes |
| stronghold | yes | yes | yes |

The mineshaft full-stage oracle matches the 141-piece reference start,
including the corridor random stream. The ruined portal full-stage oracle
uses a dry candidate. A portal that
intersects flowing lava or water is not stable after the reference world
settles.

The woodland mansion piece list is pinned by a digest over the template,
origin, rotation and mirror of all 484 pieces. Its block oracle matches the
reference chunk.

The ocean monument start is pinned by a digest over the room graph, fitted
piece order, wing and penthouse bounds. Candidate chunk `(12, 23)` of seed 1
with `minecraft:deep_ocean` matches blocks and all four heightmaps in every
chunk the 58x58 building covers: chunks `(10..13, 21..24)`.

The stronghold ring placement is pinned by a digest over all 128 candidate
chunks, and the fitted piece list by a digest over type, bounds, orientation,
generation depth, door type and room flags. Seed 1 with `minecraft:plains`
places the first stronghold at chunk `(108, 51)`; chunks `(104..110, 49..57)`
match the reference blocks and heightmaps. With the other structure sets
enabled, chunk `(104, 50)` references only the stronghold and also matches.

## Storage and runtime

- Proto chunk to storage round trip, including the Anvil export and import;
- generated structure chunk through the storage bridge and Anvil;
- deterministic parallel fill and fixed-biome structure generation;
- generator status ladder for terrain, features and full.

## Performance

`bench/` covers chunk fill, parallel fill, structure area and corridor
selection, template placement, feature freeze and decoration, the fixed-biome
structure generator, and the mansion layout.
