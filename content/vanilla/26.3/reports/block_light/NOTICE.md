# Notice for game-derived data

The files in this directory are derived from Minecraft Java Edition and are

    Copyright (c) Mojang Studios / Microsoft Corporation.

They are **not** covered by the MIT license that applies to the rest of this
repository. No license is granted to copy, redistribute, sublicense or reuse
this data. It is included only so that this server can interoperate with a
locally owned copy of the game, and it should be removed if you do not own one.

`light.json` records, for every block state, the block light level the state
emits, the light levels a path loses when it enters the state, and the six face
shapes the lighting engine uses for shape-based light occlusion. `MANIFEST.json`
records the game version, the data version, the counts, the digest of the
report and the digest of the server archive it was extracted from.

The report is derived from the block registry of the jar named by
`MANIFEST.json`; it is not part of the repository license and is not shipped as
part of any game asset.
