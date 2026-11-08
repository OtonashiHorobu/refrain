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
| mineshaft | yes | yes | a single corridor still differs |
| jungle temple | yes | yes | yes |
| swamp hut | yes | synthetic | no |
| desert pyramid | yes | synthetic | no |
| igloo | yes | synthetic | no |
| shipwreck | yes | yes | yes |
| ocean ruin | yes | yes | yes |
| ruined portal | yes | yes | yes |
| woodland mansion | yes | piece list pinned | fence connection states open |

The ruined portal full-stage oracle uses a dry candidate. A portal that
intersects flowing lava or water is not stable after the reference world
settles.

The woodland mansion piece list is pinned by a digest over the template,
origin, rotation and mirror of all 484 pieces. The remaining block difference
is the connection state of a small number of fences, which needs the reference
collision-shape coverage model.

## Storage and runtime

- Proto chunk to storage round trip, including the Anvil export and import;
- generated structure chunk through the storage bridge and Anvil;
- deterministic parallel fill and fixed-biome structure generation;
- generator status ladder for terrain, features and full.

## Performance

`bench/` covers chunk fill, parallel fill, structure area and corridor
selection, template placement, feature freeze and decoration, the fixed-biome
structure generator, and the mansion layout.
