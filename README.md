# Refrain

Refrain is a clean-room server implementation of Minecraft Java Edition 26.3
(protocol 777, DataVersion 5023), written in C++26. The project targets a
server-first architecture: a dedicated server that loads worlds, runs a
deterministic simulation and exposes storage and content tooling through
stable contracts.

Refrain is not affiliated with, approved by or associated with Mojang or
Microsoft. It contains no Mojang code, assets or data. Minecraft is a trademark
of Mojang Synergies AB.

NOT AN OFFICIAL MINECRAFT PRODUCT. NOT APPROVED BY OR ASSOCIATED WITH MOJANG OR
MICROSOFT.

## Status

Refrain is pre-release software and does not implement the network protocol
yet, so a Minecraft client cannot connect. What runs today:

- world storage in the native Refrain World Format (RWF v1) with lossless
  import and export of vanilla Anvil worlds, including zstd and LZ4 regions;
- a headless 20 Hz server runtime with a plain console, scripted replay,
  deterministic state hashes, autosave and crash recovery;
- a content-addressed data store, datapack parsing, mod manifests and gamepack
  composition;
- a schema compiler that generates C and C++ contracts from RSC and RIDL
  sources;
- command-line tools for world conversion, content packaging, schema generation
  and diagnostics.

World generation, the network protocol, scripting hosts and the client are not
implemented.

## Requirements

| Component | Version |
|---|---|
| Linux x86_64 | kernel 6.x or newer |
| GCC | 16 or newer |
| CMake | 4.2 or newer |
| Conan | 2.31 or newer |
| Ninja | any recent version |
| Python | 3.11 or newer |

All third-party libraries are resolved through Conan from
[`conan/locks/vendor.lock`](conan/locks/vendor.lock). Nothing is downloaded by
CMake.

## Build

```bash
scripts/bootstrap.sh
```

The bootstrap script checks the toolchain, creates the Conan profile, builds
the vendored recipes, installs dependencies, configures and builds the project,
then runs the test suite. For the manual sequence and build options, see
[docs/building.md](docs/building.md).

Run the server against an RWF world for a fixed number of ticks:

```bash
build/Release/bin/refrain-server \
    --gamepack gamepack/gamepack.toml \
    --world /path/to/world \
    --ticks 200
```

Import a vanilla world into RWF first:

```bash
build/Release/bin/refrain-tools world import \
    --from /path/to/vanilla-world \
    --to /path/to/world
```

## Documentation

- [Building](docs/building.md) covers the toolchain, build presets and options.
- [Running a server](docs/running.md) covers the CLI, console commands and
  runtime configuration.
- [Testing](docs/testing.md) covers the test layout, fixtures and CI checks.
- [Architecture](docs/architecture.md) describes the module layers, contracts
  and storage model.
- [Schema compiler](docs/schema.md) documents RSC and RIDL and the generated
  headers.
- [Mods and gamepacks](docs/modding.md) documents the composition model.

## Repository layout

| Path | Contents |
|---|---|
| `include/refrain/` | Public headers, grouped by module |
| `src/` | Implementation of the modules |
| `sdk/idl/` | RIDL contract sources |
| `sdk/generated/` | Generated C and C++ contract headers |
| `schema/` | RSC data schemas |
| `test/` | Boost.UT test suites and fixtures |
| `bench/` | Google Benchmark suites |
| `tools/` | Schema compiler, vendor tooling and checks |
| `cmake/` | CMake modules |
| `conan/` | In-repo Conan recipes and the dependency lock |
| `mods/` | First-party mod manifests and data |
| `gamepack/` | Default gamepack composition |
| `scripts/` | Bootstrap, CI and version checks |
| `docs/` | User and developer documentation |

## Contributing and security

See [CONTRIBUTING.md](CONTRIBUTING.md) for the development workflow, coding
rules and the commit convention. Report security issues through the private
channel described in [SECURITY.md](SECURITY.md).

## License

Refrain is distributed under the MIT License. See [LICENSE](LICENSE) and
[NOTICE.md](NOTICE.md) for third-party components.
