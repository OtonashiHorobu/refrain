# Bundled mods

Every subdirectory is one mod: `mods/<id>/manifest.toml`, optional `data/`,
`assets/`, `scripts/` and `src/`. The manifest `id` must equal the directory
name. Directories without a manifest fail `refrain-tools mods check`; loose
files such as this README are ignored.

The build packs every mod's manifest, data, assets and scripts into a single
deterministic RFSI image (`refrain-tools builtin image --mods mods`) that is
embedded in the server binary. Mods with C++ sources are linked statically and
register themselves through the static mod registry; the manifest version and
ABI must match the compiled constants, and a drift check rejects a mismatch at
load time.

Mods without `src/` are metadata-only. They contribute data, assets or scripts
and are completed when the corresponding capability exists.

| Mod | Provides | Contents |
|---|---|---|
| `refrain.admin.vanilla` | | administration data |
| `refrain.content.vanilla` | | content definitions |
| `refrain.data.vanilla` | `refrain:data.packs` | datapack resources |
| `refrain.proto.vanilla` | | protocol 777 definitions |
| `refrain.server.vanilla` | `refrain:runtime` | headless server runtime |
| `refrain.session.vanilla` | | session data |
| `refrain.sim.vanilla` | `refrain:sim` | simulation provider |
| `refrain.storage.rwf` | `refrain:storage` | RWF storage provider |
| `refrain.worldgen.vanilla` | | world generation data |
| `refrain.world.vanilla` | `refrain:world` | runtime world provider |

The default composition is `gamepack/gamepack.toml`. See
[../docs/modding.md](../docs/modding.md) for the manifest and gamepack formats.
