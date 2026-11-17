# Refrain documentation

[Back to the project README](../README.md)

These guides cover running a built server, developing Refrain and the formats
and contracts it defines. Start with [Building](building.md) if the tree is not
built yet.

## Use

- [Running a server](running.md) — command line, per-namespace TOML
  configuration, console and in-game commands, client join behaviour, world
  conversion and crash reporting.
- [World generation data](worldgen.md) — generating the datapack and structure
  templates from a locally owned `server.jar`, the generation pipeline, and what
  the reference-backed tests cover.

## Develop

- [Building](building.md) — prerequisites, bootstrap, presets and options,
  hardening, release archives, local iteration and benchmarking.
- [Testing](testing.md) — unit suites, additional configurations, CI checks,
  fuzz smokes, protocol bot and load tools, soak baselines and acceptance.
- [Commands](commands.md) — the in-game command framework: typed argument
  parsers, client command tree and completion, permissions, how to add a
  command and the 26.3 coverage list.
- [Mods and gamepacks](modding.md) — mod manifests, gamepack composition,
  providers, the native entry ABI, the host runtime API and the content store.

## Reference

- [Architecture](architecture.md) — layers, contracts, composition, the network
  stack, storage, the simulation contract, threading and error handling.
- [Schema compiler](schema.md) — RSC and RIDL syntax, extensions, the generated
  C ABI and C++ wrappers, generation and versioning.

## Project documents

- [README](../README.md) — overview, status and quickstart.
- [CONTRIBUTING](../CONTRIBUTING.md) — workflow, coding rules, commit
  convention and the Developer Certificate of Origin.
- [SECURITY](../SECURITY.md) — supported versions, the private vulnerability
  reporting channel and scope.
- [ACCESSIBILITY](../ACCESSIBILITY.md) — accessibility of the documentation and
  the command-line tools, known limitations and how to report a barrier.
- [NOTICE](../NOTICE.md) — third-party components and the terms of the
  game-derived data.
- [Code of Conduct](../CODE_OF_CONDUCT.md) — expected behaviour and enforcement.
