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

## Wiki

The guides in this directory are published to the repository
[wiki](https://github.com/OtonashiHorobu/refrain/wiki) by
`scripts/dev/sync-wiki.sh`. The files under `docs/` are the source of truth:
every wiki page names the repository file it comes from in a banner, so edits
belong in the repository and never on the wiki.

The wiki is a separate git repository, `refrain.wiki.git`. GitHub creates it
with the first wiki page, which has to be saved once in the web interface;
until then there is no repository to clone. After that first page, run the sync
from the repository root:

```bash
scripts/dev/sync-wiki.sh
```

The script writes one page per file under `docs/`, rebuilds the `Home` index,
commits only when the content changed and pushes. The check mode generates the
pages in a temporary directory and compares them with a local wiki checkout
(`--wiki-dir` or `REFRAIN_WIKI_DIR`); it never uses the network:

```bash
scripts/dev/sync-wiki.sh --check --wiki-dir ../refrain.wiki
```

Pushing needs a credential that may write the wiki repository. The
`GITHUB_TOKEN` a workflow receives is scoped to the source repository and
cannot push the wiki, so an unattended sync needs a user access token with
write access to the repository, or an equivalent credential, stored as a
repository secret. Pass it through `REFRAIN_WIKI_TOKEN`; the script never
writes it into a page or a remote URL.
