# Notice for game-derived data

The files under this directory are derived from Minecraft Java Edition and are

    Copyright (c) Mojang Studios / Microsoft Corporation.

They are **not** covered by the MIT license that applies to the rest of this
repository. No license is granted to copy, redistribute, sublicense or reuse
this data. It is included only so that this server can interoperate with a
locally owned copy of the game, and it should be removed if you do not own one.

The data is generated from a game installation the user owns; `MANIFEST.json`
records the game version, the data version and the hash of the source jar. This
directory contains no game code and no game assets beyond what the pipeline
derives.

Removing this directory does not affect the source code. Builds then require the
user's jar and generate the snapshot at install time.
