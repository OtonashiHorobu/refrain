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
- external WebAssembly mods beyond load: an installed package that names the
  wasm loader is validated, instantiated and its `on_load` hook runs, but the
  script host API and the call dispatch path a guest needs have not landed, so
  a wasm mod cannot answer server calls yet; native mods are loadable in full.

## Quickstart

Extract the release archive for your platform and start the server from the
archive directory:

```bash
./run-server.sh
```

The first start writes `eula.toml` next to the working directory with
`eula = false`, prints the EULA notice and exits without creating a world or
opening a port. Read the
[Minecraft End User License Agreement](https://www.minecraft.net/en-us/eula),
then accept it and start:

```bash
./run-server.sh --accept-eula
```

After acceptance the server fetches the official Minecraft Java Edition 26.3
server archive from Mojang's version manifest, verifies it against the published
SHA-1, unpacks the datapack and the structure templates into `./data`, creates
`./world` and `./config`, generates the spawn area and listens on
`0.0.0.0:25565`. Nothing from the archive is committed or redistributed; only
the unpacked data stays on this machine and later starts reuse it without the
network. `--no-data-download` forbids the fetch for offline hosts with a staged
`./data` directory.

Join with an unmodified Minecraft Java Edition 26.3 client at `localhost:25565`.
The default configuration uses online mode and verifies the player's session;
for a local test set `online_mode = false` in `config/network.toml`, which the
first start writes with the defaults.

From a source checkout, build the project, its dependencies and the test suite:

```bash
git clone https://github.com/OtonashiHorobu/refrain.git
cd refrain
scripts/bootstrap.sh
```

The script checks the toolchain, resolves every dependency through Conan,
configures the `conan-release` preset, builds, and runs the tests. Then start
the server the same way; it finds the bundled composition and the tracked game
data next to or above the binary:

```bash
build/Release/bin/refrain-server
```

Re-run the tests after a change with `./build/Release/bin/refrain-tests`.
`scripts/ci.sh --offline` runs the full local gate: vendor checks, the schema
drift check, the C ABI compile check, the unit tests, the CLI contract, the
runtime, first-run, diagnostics, extraction and crash-reporter smokes, and the
formatting check.

A checkout may stage the game data without the network, for example from a
`server.jar` you own:

```bash
scripts/fetch-content.sh /path/to/server.jar
build/Release/bin/refrain-server --data-dir content/vanilla/26.3 --no-data-download
```

Import an existing vanilla world into RWF and start the server on it:

```bash
build/Release/bin/refrain-tools world import \
    --from /path/to/vanilla-world --to /path/to/rwf-world
build/Release/bin/refrain-server --world /path/to/rwf-world
```

See [Running a server](docs/running.md) for the first-run flow, the defaults,
the configuration files, the console commands and the conversion tools.

## Features

- **Protocol.** Handshake, status, login, configuration and play over TCP, with
  compression, online and offline profiles, server-list status, tab-list
  features, viewport-based chunk streaming and rate and backlog budgets.
- **World generation.** Datapack-driven terrain, biome selection, surface
  rules, carvers, features, structures and template pieces for the overworld,
  the nether and the end. No game assets are distributed; the server unpacks the
  data locally from the game version it targets on the first accepted start.
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
