# Stronghold generation

A stronghold is a flat list of fitted corridor and room pieces placed under a
concentric-ring candidate chunk. Unlike the ocean monument, every piece is a
separate start piece, so the chunk's piece order is the generation order.

## Ring placement

`minecraft:strongholds` uses `ConcentricRingsStructurePlacement` with
`count = 128`, `distance = 32`, `spread = 3` and preferred biomes
`#minecraft:stronghold_biased_to`. Ring positions are generated once per world
seed:

1. Seed a legacy random source with the level seed and draw the starting
   angle from `nextDouble() * 2 * pi`.
2. For each of the 128 rings draw a distance
   `4 * distance + distance * circle * 6 + (nextDouble() - 0.5) * distance * 2.5`
   around the current angle, then round `cos(angle) * distance` and
   `sin(angle) * distance` to chunk coordinates.
3. Fork the ring random and search the preferred biomes within a 112 block
   radius around `(chunkX * 16 + 8, 0, chunkZ * 16 + 8)`. The search samples the
   quart grid, keeps a reservoir sample of matching positions and returns the
   chunk of that position; a fixed biome source instead draws a uniform
   position inside the square and returns it immediately, and a failed search
   falls back to the unmodified ring chunk.
4. Advance the angle by `2 * pi / spread`. After every `spread` rings grow the
   circle and the spread (`spread += 2 * spread / (circle + 1)`, clamped to the
   remaining ring count) and add another random angle offset.

Candidates are the chunks in this position list.

## Start

`StrongholdStructure.findGenerationPoint` places the start at the chunk
position and has no biome test of its own; the structure's `#has_structure/
stronghold` biome list is checked at the chunk position by the caller.

Generation retries until the pieces contain a portal room:

1. Reseed a legacy random source with `setLargeFeatureSeed(seed + tries,
   chunkX, chunkZ)` and reset the piece weights.
2. Create the start stairs-down piece at `(chunkMinX + 2, 64, chunkMinZ + 2)`
   with a random horizontal orientation. Its box is 5 x 11 x 5.
3. Fit child pieces recursively from small doors. The weighted table is
   straight 40, prison hall 5/5, left turn 20, right turn 20, room crossing
   10/6, straight stairs down 5/5, stairs down 5/5, five crossing 5/4, chest
   corridor 5/4, library 10/2 (depth > 4) and portal room 20/1 (depth > 5).
   Exhausted or invalid weights are removed, the previous weight cannot repeat
   immediately, and five failed draws fall back to a filler corridor derived
   from the first colliding piece. Pieces must stay above y = 10 and within 112
   blocks of the start in x and z, at a recursion depth of at most 50.
4. The start's pending children are processed in random order and may add more
   pending children.
5. When a portal room was fitted, move every piece below sea level:
   `maxY = seaLevel - 10`, `top = ySpan + minY + 1`, optionally advance `top`
   by `nextInt(maxY - top)` and shift all pieces by `top - boundingBox.maxY`.
   The traversal repeats with the next try seed when no portal room was fitted.

## Pieces

The pieces are start/stairs down, straight, prison hall, left turn, right turn,
room crossing, straight stairs down, stairs down, five crossing, chest
corridor, library, portal room and filler corridor. Each writes its own
oriented box. The shared `SmoothStoneSelector` writes stone brick variants on
box edges (cracked, mossy, infested or plain chosen by `nextFloat()`) and cave
air in the interior; `skipAir` boxes only touch non-air blocks. Block states are
mirrored then rotated by the piece orientation (`SOUTH` mirrors left/right,
`WEST` mirrors left/right and rotates clockwise, `EAST` rotates clockwise,
`NORTH` does nothing), exactly like `StructurePiece.placeBlock`. Chests use the
stronghold loot tables and the portal room's twelve frame eyes are drawn from
the placement random; an all-eyes room places the end portal.

## Oracle

Seed 1 with a fixed `minecraft:plains` biome puts the first stronghold at chunk
`(108, 51)`; its pieces span chunks `(104..110, 49..57)`. A reference world with
the other structure sets disabled matches every one of those 63 chunks block
for block and in all four heightmaps. With the other structure sets enabled,
chunk `(104, 50)` only references the stronghold and also matches block for
block.
