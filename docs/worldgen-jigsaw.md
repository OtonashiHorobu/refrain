# Jigsaw structures

`minecraft:jigsaw` builds a structure from a start pool by repeatedly attaching
pool elements through their jigsaw block connectors. The start pool is chosen
by the structure document; each connector names the pool its replacement is
drawn from, so the shape of a generated structure is a graph walk whose random
draws, weight table and traversal order all define the result.

## Reference

The implementation follows the 26.3 sources `JigsawStructure.java`,
`JigsawPlacement.java`, `SinglePoolElement.java`, `ListPoolElement.java`,
`FeaturePoolElement.java`, `EmptyPoolElement.java`,
`StructureTemplatePool.java`, `PoolElementStructurePiece.java`,
`JigsawJunction.java`, `JigsawBlock.java`, `StructureTemplate.java`,
`StructurePlaceSettings.java`, `RuleProcessor.java`, `ProcessorRule.java`,
`JigsawReplacementProcessor.java`, `GravityProcessor.java`,
`BlockIgnoreProcessor.java` and `Beardifier.java`, plus the structure,
template-pool, processor-list and biome-tag documents under
`content/vanilla/`.

## Structure fields

The structure document decoder records

- `start_pool` and the optional `start_jigsaw_name`;
- `size` (the maximum attachment depth, default 6);
- `start_height`, a constant or uniform `HeightProvider` whose anchors may be
  absolute, above the dimension bottom, below its top or relative to sea level;
- `use_expansion_hack` (default true);
- the optional `project_start_to_heightmap`;
- `max_distance_from_center`, either a horizontal/vertical record or one value
  used for both axes;
- `pool_aliases`, `dimension_padding` and `liquid_settings`.

A `minecraft:jigsaw` document whose fields do not decode rejects the structure
instead of falling back to defaults.

## Start

1. Seed a legacy source with `setLargeFeatureSeed(seed, chunkX, chunkZ)` and
   sample `start_height`. The start position is
   `(chunkMinX, sampledHeight, chunkMinZ)`.
2. Draw the centre rotation and the centre element from the weight-expanded
   start pool. An empty element means the candidate has no structure.
3. With `start_jigsaw_name`, shuffle the centre element's jigsaw blocks and move
   the piece so the first block with that name sits at the start position.
4. Compute the centre bounding box, its centre column and, with
   `project_start_to_heightmap`, the first free height of that column. The piece
   moves so `box.minY + groundLevelDelta` equals that height. A start whose box
   violates `dimension_padding` is rejected.
5. The start anchor is `(centreX, bottomY + anchorOffset, centreZ)`; the biome
   test the structure framework applies sees that anchor.
6. The centre piece is the first piece. With depth 0 no attachment runs;
   otherwise a free-space shape is built from the horizontal/vertical maximum
   distance around the anchor minus the centre box.

## Attachment

Attachments are processed in priority order: the highest
`placement_priority` queue first, FIFO within one priority. For every jigsaw
block of the current piece, in shuffled order and stable-sorted by descending
`selection_priority`:

1. Resolve the connector's pool through the alias lookup. The target list is
   the pool's shuffled weight table at any depth below `size`, followed by the
   fallback pool's shuffled table at every depth.
2. For each target element and each shuffled rotation, compute the expansion
   hack: when enabled and the element is at most 16 blocks tall, the target box
   grows upward to the tallest pool reachable from the element's own connectors.
3. Try every target jigsaw block. A connection requires opposite fronts, equal
   tops for an aligned source and the source target naming the destination.
4. Both-rigid connections keep `sourceBoxY + deltaY`; any other pair uses the
   first free height of the source connector column. The target box must lie
   entirely in the free space; the free space then loses the target box.
5. Ground level deltas propagate as `sourceDelta - deltaY` for rigid targets.
   Both pieces record a `JigsawJunction`: the source side at the target jigsaw
   position, the target side at the source jigsaw position, each with its own
   projection and signed `deltaY`.
6. Attached pieces are appended to the piece list in attachment order and
   queued for their own depth. Pieces at the maximum depth still take fallback
   elements but do not attach further.

The start bounding box is the union of the piece boxes.

## Pools and elements

A `worldgen/template_pool` becomes a weighted table: every element is repeated
`weight` times in declaration order, so `getRandomTemplate` is one uniform draw
over the table and `getShuffledTemplates` is the reference's Fisher-Yates copy.
The decoder understands empty, single, legacy-single, list and feature
elements, per-element processor lists (referenced or inline), projection and
`override_liquid_settings`. A list applies its projection to every child, takes
its jigsaw blocks from the first child and its bounding box from the union of
non-empty children.

Pool aliases are resolved once per start from the level seed and the start
position. The alias random is a positional source forked from the level seed,
exactly like `PoolAliasLookup.create`, and the last binding for an alias wins.

## Pieces

`structure_piece` carries the element tree, the ground level delta, the liquid
setting and the junctions. Placement runs at the piece origin with no mirror and
rotation only:

- the position transform uses pivot `(0, 0, 0)`;
- processors run in the reference order: `block_ignore` (all-air for legacy
  elements, structure blocks otherwise), `jigsaw_replacement`, the element's
  processor list, then the projection's `gravity_processor` for
  `terrain_matching`;
- the gravity processor moves each processed block to the column height plus
  the template-local y, so later processors and the clipping check see the new
  position;
- `jigsaw_replacement` writes the jigsaw block's `final_state` and drops
  `minecraft:structure_void`; other processors are written before the state
  transform, and the final state is mirrored and rotated;
- block entities receive the placed position and a loot-table seed drawn from
  the piece random;
- feature elements are recorded as one-block pieces (so they take part in the
  free-space walk and the beardifier) but only place blocks when the caller
  supplies a placed-feature callback.

## Terrain adaptation

`collect_beard` reproduces `Beardifier.forStructuresInChunk`: for every start
whose adjusted bounding box reaches the chunk, pieces within twelve blocks are
included. Jigsaw pieces contribute a rigid beard only for the `rigid`
projection; every junction source within the chunk window contributes too, and
other piece types contribute their whole box. The union of those boxes is
inflated by 24 and bounds the sampled region. The fixed-biome structure
generator and the full-stage oracle bind that beardifier to the fill sampler;
fills without structures leave the context field unset and sample zero.

## Oracle

Seed 1 with a fixed `minecraft:plains` biome places the reference village start
at chunk `(15, 8)`; its 119 pieces match the reference start field for field,
including rotations, bounds, template origins, axes, ground level deltas and
junctions.

Chunk `(17, 8)` — covered by the village but more than 17 blocks from every
feature pool element — matches the reference block for block and in all four
heightmaps. The start chunk itself still differs: 64 blocks and 81 heightmap
entries, all caused by the eighteen `minecraft:oak`, `minecraft:flower_plain`
and `minecraft:pile_hay` feature pool elements whose placed features are not
generated. That is the one known jigsaw gap; a chunk outside their reach is
clean. The beached shipwreck, jungle temple, mineshaft, ocean monument, ocean
ruin, stronghold and woodland mansion full-stage oracles stay green.

## Remaining gaps

- Placed-feature elements need a feature-stage callback; they currently
  contribute geometry and beard data but no blocks.
- `capped` processor lists, block-entity rule modifiers and the position
  predicates beyond the default are not decoded; a processor list that contains
  one decodes to the empty list.
- Structure template entities are not spawned; the proto world keeps block
  entities only.

Reference world and test inputs, using the template cache extracted from the
server jar:

```bash
python3 tools/worldgen/oracle/generate.py \
    --server-jar /path/to/server.jar --seed 1 --structures \
    --fixed-biome minecraft:plains --settle 60 \
    --force-load-box 128 0 383 255 --port 25341 --output /tmp/t2jigsaw
REFRAIN_VANILLA_JIGSAW_REGION=/tmp/t2jigsaw/world/dimensions/minecraft/overworld/region/r.0.0.mca \
REFRAIN_VANILLA_WORLDGEN=/path/to/server/data \
REFRAIN_VANILLA_STRUCTURES=/path/to/templates \
    ./build/Release/bin/refrain-tests
```

The full-stage chunk uses `REFRAIN_FULL_STAGE_STRUCTURES=1`,
`REFRAIN_FULL_STAGE_CHUNK_X=17`, `REFRAIN_FULL_STAGE_CHUNK_Z=8`,
`REFRAIN_FULL_STAGE_START_CHUNK_X=15`,
`REFRAIN_FULL_STAGE_START_CHUNK_Z=8`,
`REFRAIN_FULL_STAGE_BIOME=minecraft:plains` and
`REFRAIN_FULL_STAGE_SEED=1`.
