# Refrain

Refrain is a server implementation of Minecraft Java Edition 26.3 (protocol 777,
DataVersion 5023), written in C++26. It is a headless dedicated server: it loads,
generates and stores worlds, runs a deterministic 20 Hz simulation, and speaks
the vanilla protocol, so an unmodified 26.3 client can join.

Refrain is not affiliated with, approved by or associated with Mojang or
Microsoft. It contains no Mojang code or assets. The game-derived tables under
`content/vanilla/` carry their own [notice](content/vanilla/NOTICE.md).
Minecraft is a trademark of Mojang Synergies AB.

NOT AN OFFICIAL MINECRAFT PRODUCT. NOT APPROVED BY OR ASSOCIATED WITH MOJANG OR
MICROSOFT.

## Status

Refrain is pre-release software. The server is usable for the features listed
below, but the gameplay surface is still partial and no compatibility promise is
made before the first release.

Implemented today:

- **Vanilla clients join.** The server speaks protocol 777 (Minecraft Java
  Edition 26.3), in online and offline mode. Status ping, login, configuration,
  play, chunk streaming, the player list, in-game commands and inventory and
  container interactions work with an unmodified client.
- **Reference-verified world generation.** The overworld, the nether and the end
  generate biomes, terrain, carvers, features and structures. Generation is
  checked against reference worlds produced from the target version, with the
  remaining known gaps recorded in
  [the verification status](docs/worldgen.md#verification-status).
- **Native world storage with Anvil interop.** RWF v1 stores worlds natively and
  imports and exports vanilla Anvil worlds losslessly for the fields the format
  carries, including zstd- and LZ4-compressed regions.
- **A deterministic headless runtime.** A 20 Hz tick loop, a plain console,
  scripted replays, a state hash, autosave, region recovery and a stall
  watchdog. The same world and command sequence produce the same hash at the
  same tick.
- **Mods and gamepack composition.** Manifests, version and dependency
  resolution, provider selection per service, a native mod entry ABI, signed
  packages and a content-addressed content store.
- **Schemas, contracts and tooling.** The RSC and RIDL schema compiler emits
  versioned C and C++ contract headers, reflection metadata and Rust bindings
  for the host contracts. `refrain-tools` covers world conversion, gamepack and
  mod validation, content packaging, protocol bots, packet capture, schema
  generation and diagnostics.

Not implemented yet:

- a graphical client; Refrain is a server only;
- protocol or game versions other than 26.3 (protocol 777);
- the full breadth of vanilla gameplay: many mechanics, commands and entity
  behaviours are still missing;
- external WebAssembly mods; the script host has not landed, so native mods are
  the only package type the loader accepts.

## Quickstart

Build the project, its dependencies and the test suite from a checkout:

```bash
git clone https://github.com/OtonashiHorobu/refrain.git
cd refrain
scripts/bootstrap.sh
```

The script checks the toolchain, resolves every dependency through Conan,
configures the `conan-release` preset, builds, and runs the tests. Re-run the
tests after a change with:

```bash
./build/Release/bin/refrain-tests
```

`scripts/ci.sh --offline` runs the full local gate: vendor checks, the schema
drift check, the C ABI compile check, the unit tests, the CLI contract, the
runtime, extraction and crash-reporter smokes, and the formatting check.

Import an existing vanilla world into RWF and start the server on it:

```bash
build/Release/bin/refrain-tools world import \
    --from /path/to/vanilla-world --to /path/to/rwf-world

build/Release/bin/refrain-server --world /path/to/rwf-world
```

Or let the server create a fresh world from a seed. Generating terrain reads the
datapack of the game version, which is not part of this repository. Generate it
once from a `server.jar` you own; the jar stays local and nothing from it is
committed:

```bash
scripts/fetch-content.sh /path/to/server.jar
build/Release/bin/refrain-server --seed 12345
```

An imported world starts without the datapack, but chunk generation outside its
stored chunks still needs it: without the data, generation fails and the log
reports `the chunk generator is unavailable`.

Join with an unmodified Minecraft Java Edition 26.3 client at
`localhost:25565`; the server prints its listening address on startup. The
default configuration uses online mode and verifies the player's session. For a
local test, disable the session check in `config/network.toml`:

```toml
online_mode = false
```

Both `--world` and `--seed` use the bundled `gamepack/gamepack.toml`. See
[Running a server](docs/running.md) for the configuration files, the console
commands and the conversion tools.

## Features

- **Protocol.** Handshake, status, login, configuration and play over TCP, with
  compression, online and offline profiles, server-list status, tab-list
  features, viewport-based chunk streaming and rate and backlog budgets.
- **World generation.** Datapack-driven terrain, biome selection, surface
  rules, carvers, features, structures and template pieces for the overworld,
  the nether and the end. No game assets are distributed; you generate the data
  locally from the game you own.
- **World format.** RWF v1 region files with A/B index commits and CRC32C
  checksums, lossless Anvil import and export, `world fsck`, compaction and an
  offline palette rebuild.
- **Runtime.** A fixed 20 Hz tick loop on a small thread set, a stdin console, a
  tick-indexed replay format, deterministic state hashes, autosave and a
  watchdog.
- **Mods.** Mod manifests with SemVer dependency ranges, gamepack composition
  with one provider per service, native C entry points, Ed25519 package
  signatures and a BLAKE3 content-addressed store.
- **Contracts.** Services declared in RIDL with versioned C ABI and C++ wrappers;
  data and packet layouts declared in RSC. Generated headers are checked for
  drift in CI.
- **Tooling.** `refrain-tools` converts worlds, validates gamepacks and mods,
  packs and verifies packages, runs protocol bots and load clients, records and
  diffs packet captures, generates schemas and extracts game data locally.
- **Tests.** Boost.UT suites, synthetic fixtures, golden files, fuzz smokes over
  every serverbound decoder, reference-backed world generation oracles and
  `scripts/soak.sh` load baselines.

## Requirements

Linux x86_64 is the supported development platform. Windows x86_64 builds are
compiled and tested in CI (MinGW and MSVC) and produce the Windows archives, but
they are not the primary development environment. macOS is not supported. The
server is headless and needs no GPU, display or audio device.

| Component | Requirement |
|---|---|
| Linux | x86_64, kernel 6.x or newer |
| Compiler | GCC 16 or newer (C++26) |
| CMake | 4.2 or newer |
| Conan | 2.31 or newer |
| Ninja | any recent version |
| Python | 3.11 or newer (tooling and Conan) |

All third-party libraries are resolved through Conan from
[`conan/locks/vendor.lock`](conan/locks/vendor.lock). Nothing is downloaded by
CMake. See [Building](docs/building.md) for the manual sequence, build options,
hardening and release archives.

## Repository layout

| Path | Contents |
|---|---|
| `include/refrain/` | Public headers, grouped by module |
| `src/` | Module implementations and the CLI entry points |
| `sdk/idl/` | RIDL service contract sources |
| `sdk/generated/` | Generated C ABI headers, C++ wrappers and Rust host bindings |
| `schema/` | RSC data and packet schemas |
| `mods/` | Bundled first-party mods and their manifests |
| `gamepack/` | Default composition and smoke presets |
| `content/vanilla/` | Game-derived tables with their own notice; the datapack is generated locally |
| `demo/` | Unbundled example mod |
| `test/` | Boost.UT suites, fixtures and golden files |
| `bench/` | Google Benchmark suites |
| `tools/` | Schema compiler, content and worldgen tooling, repository checks |
| `scripts/` | Bootstrap, CI, content fetch, soak and release scripts |
| `cmake/`, `conan/` | Build modules, in-repo recipes and the dependency lock |
| `.github/` | CI workflows and issue and pull request templates |
| `docs/` | User, developer and reference documentation |

## Documentation

The [documentation index](docs/README.md) groups every guide by use, development
and reference. Start there; the most-used entries are:

- [Running a server](docs/running.md) — command line, TOML configuration,
  console commands, client join behaviour and world conversion.
- [Building](docs/building.md) — prerequisites, bootstrap, build options,
  hardening and release archives.
- [World generation data](docs/worldgen.md) — generate the datapack and
  structure templates, and what the reference-backed tests cover.
- [Architecture](docs/architecture.md) — layers, contracts, composition,
  network, storage and the tick loop.

## Community and contributing

Development happens on GitHub. Read [CONTRIBUTING.md](CONTRIBUTING.md) for the
workflow, coding rules and commit convention; every commit carries a
`Signed-off-by` trailer. Report bugs and propose features through the issue
templates. Participation is governed by the
[Code of Conduct](CODE_OF_CONDUCT.md).

Report security problems through the private channel in
[SECURITY.md](SECURITY.md), never in a public issue. Accessibility barriers in
the documentation or the command-line tools are described in
[ACCESSIBILITY.md](ACCESSIBILITY.md), including how to report one.

## License

Refrain is distributed under the MIT License. See [LICENSE](LICENSE) for the
license text and [NOTICE.md](NOTICE.md) for third-party components and the
terms that cover the game-derived data under `content/vanilla/`.
