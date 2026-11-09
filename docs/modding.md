# Mods and gamepacks

Refrain composes a server from mods. A mod owns content, data, scripts or a
service provider. A gamepack selects one provider per service, lists the mods
to load and fixes the composition for a world.

## Mod layout

```text
mods/<id>/
    manifest.toml
    data/       optional datapack resources
    assets/     optional client assets
    scripts/    optional scripts
    src/        optional C++ sources linked into the server
```

The directory name must equal the manifest `id`. A directory without a manifest
fails `refrain-tools mods check`; loose files are ignored.

## Manifest

```toml
[mod]
id = "example.content"
version = "1.0.0"
abi = "1"
side = "both"

[meta]
display_name = "Example content"
description = "Example datapack resources."
license = "MIT"

[provides]
services = ["example:content"]

[requires]
"refrain.core" = "^1.0.0"

[entry]
native = "libexample.so"
```

`[mod]` carries the identity, a SemVer 2.0.0 version, the integer ABI major and
the side (`server`, `client` or `both`). `[meta]` is display metadata and never
affects composition. `[provides]` lists service keys. `[requires]` maps mod ids
to version ranges; ranges support `=`, `>`, `>=`, `<`, `<=`, `^`, `~`, space for
conjunction and `||` for alternation. `[optional_requires]` marks dependencies
that may be absent.

Static mods bundled with the server omit `[entry]`; their code is linked into
the binary and paired with the manifest through the static registry. External
mods declare exactly one of `[entry].native` or `[entry].wasm`.

## Gamepack

```toml
[gamepack]
id = "refrain.vanilla"
version = "26.3.0"
side = "server"
abi = "1"
auto_scan = false

[mods]
list = [
    "refrain.data.vanilla@1.0.0",
    "refrain.world.vanilla@1.0.0",
]

[mods."refrain.data.vanilla"]
requires = ["refrain.content.vanilla@^1.0.0"]

[providers]
world = "refrain.world.vanilla@^1.0.0"
data = "refrain.data.vanilla@^1.0.0"
```

`[mods].list` pins the composition. `[providers]` maps a service name to the mod
that provides it, using a version range. With `auto_scan = true` the loader may
discover mods on disk; with `auto_scan = false` the gamepack is the only source
of truth and every required version must be listed.

A resolved composition is written to `mods.lock` next to the gamepack. The lock
records the selected versions, the provider for each service and a fingerprint
of the resolution. Loading fails when a pinned mod is missing or the
composition drifts; `refrain-server --update-lock` rewrites the lock for an
intentional change.

Validate and inspect a composition:

```bash
refrain-tools gamepack check --file gamepack/gamepack.toml
refrain-tools gamepack dump-plan --file gamepack/gamepack.toml
refrain-tools mods check --gamepack gamepack/gamepack.toml
```

## Providers

A provider implements a RIDL service and registers its service key. The
composition mode of the service decides whether one provider is active
(`exclusive`), one per instance name (`instanced`), a chain of wrappers
(`chain`) or merged contributions (`registry`). The provider receives calls
through a generated C++ wrapper; every struct carries its size and the ABI
version is checked on load.

Provider implementations live in `mods/<id>/src/` for first-party mods. Their
code is compiled into the server and registered through the static registry.
A provider that the gamepack selects but the binary does not link fails at
startup with the missing service key.

## Host runtime

The server exposes a stable runtime API to mods through sealed services.
`refrain:host.core` is registered by the host bootstrap before any mod factory
runs; it covers logging, time, host memory, read-only config namespaces and service
lookup/registration. `refrain-server --config-dir <dir>` mounts each TOML file
as a read-only namespace (`refrain.toml` is `host`, `mods/<id>.toml` is
`mod/<id>`); without the option config reads return `unsupported`. A sealed service is not a gamepack provider: a mod cannot
declare it in `[provides]` or replace it through `register_provider`. The C ABI
and C++ wrapper are generated from `sdk/idl/refrain/host/core.ridl`, so host
code and mods share the same versioned contract definitions.

## Content store

`refrain-server --content-store <dir>` enables the content store. Packages
placed in the inbox are verified, unpacked and installed into a
content-addressed store; the store publishes an index per composition and
tracks unreferenced content for garbage collection.

```bash
refrain-tools mods install --store /path/to/store package.rfmod
refrain-tools content index --store /path/to/store
```

`--content-gc off|dry-run|apply` selects the startup policy. In `apply` mode the
deletion queue is drained by a background worker; the tick thread never unlinks
files.

## Built-in image

The build packs every bundled mod into a single deterministic image:

```bash
refrain-tools builtin image --mods mods -o build/Release/generated/builtin.image
```

The image is embedded in the server binary. A deterministic build produces the
same image bytes, which the test suite relies on.
