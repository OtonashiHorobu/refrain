# Ruined portal generation

A ruined portal is a template piece with a small amount of terrain work
around it. The reference version reads the structure's setup list, samples a
template and a vertical placement, then places one template piece.

## Start

1. Pick a setup with the weighted roll over `setups`; a single entry skips the
   roll. `air_pocket` is sampled from the setup probability (`0` and `1` are
   constants, otherwise one float).
2. Roll the template: a 5% chance picks one of the three giant portals,
   otherwise one of the ten normal portals.
3. Read the template, pick a random rotation and a mirror (`none` or
   `front_back`), and use the template's half-size as the rotation pivot.
4. Build the template bounding box at the chunk's world position, take the
   centre, and read `WORLD_SURFACE_WG` (or `OCEAN_FLOOR_WG`) below it. The
   projected Y comes from the vertical placement:
   - `in_nether`: random ranges around 27..100;
   - `in_mountain`: 70..surfaceY;
   - `underground`: minY + 15..surfaceY;
   - `partly_buried`: surfaceY - height + 2..8;
   - surface and ocean floor: surfaceY.
5. Lower the projected Y until at least three of the four bottom corners see
   an opaque block in the placement heightmap.
6. Add one template piece with the chosen template, transform, vertical
   placement and properties. `cold` is set when the biome is cold enough to
   snow at the origin; the other properties come from the setup.

## Processors

The piece ignores structure blocks (and air when there is no air pocket) and
then runs, in order:

- a rule processor for gold (30% air), lava (netherrack/magma depending on
  cold or ocean floor) and netherrack (7% magma unless cold);
- a block-age processor with the setup mossiness;
- a protected-block processor over `#minecraft:features_cannot_replace`;
- a lava-submerged processor that restores lava under non-full shapes;
- optionally a blackstone replacement processor.

## Post process

After the template is placed the piece:

- spreads netherrack/magma around the centre with a distance probability
  table, optionally above the surface following the terrain height;
- adds netherrack drip columns below portal netherrack;
- adds vines on full faces and jungle leaves above netherrack when
  `overgrown`.
