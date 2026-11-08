# Ocean monument generation

An ocean monument is a single `MonumentBuilding` piece that itself emits a
fixed room graph of smaller pieces. The reference structure is one chunk
centred candidate: the building starts 29 blocks west and north of the chunk,
so it spans several chunks.

## Start

1. Reject the candidate unless all biomes within 29 blocks of the chunk
   centre are in `#minecraft:required_ocean_monument_surrounding`.
2. Place the building at `(chunk.minX - 29, 39, chunk.minZ - 29)` with a
   random horizontal orientation.
3. The start height comes from `OCEAN_FLOOR_WG`.

## Building

`MonumentBuilding` owns a 58 x 23 x 58 box. It builds a 5 x 3 x 4 room grid
plus a second floor and a third-floor row, links neighbouring rooms, marks the
source and core rooms, and then fits the remaining room definitions in this
order:

- double XY, double YZ, double Z, double X, double Y;
- simple top room, simple room.

The entry room and core room are always present. Two wing rooms and a
penthouse are appended after the fitted rooms. Child boxes are offset by
`(9, 0, 22)` inside the building box.

## Pieces

The room fitters create simple, double and top room pieces. Every piece writes
the monument palette (prismarine, prismarine bricks, dark prismarine, sea
lanterns and water) into its own box, with the shared `OceanMonumentPiece`
helpers providing the floor, roof, pillars, openings and random block choices.

The building writes its own outer shell, then each child piece writes its
interior. The order of child pieces is the order in which they appear in the
building list, so the shared random stream follows the fitter order above.

## Oracle

Generate a fixed deep-ocean world and force-load the chunks covering a
monument candidate. Compare the building chunks block for block and compare
the four final heightmaps.
