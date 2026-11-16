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

This tree is a *source* layout: `refrain-tools mods pack` turns it into an
`.rfmod` package, and only an installed package can load at runtime. Dropping a
directory under an instance's `mods/` is not a load path (see
[Installing mods](#installing-mods)).

## Manifest

```toml
[mod]
id = "example.content"
version = "1.0.0"
abi = "1"
side = "both"
host_api = "1.0.0"          # optional: required host runtime API
host_capabilities = ["log"] # optional: required host capabilities

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
the side (`server`, `client` or `both`). It may also declare `host_api` (a full
SemVer version of the host runtime API the mod requires) and
`host_capabilities` (host runtime capability names such as `log` or `config`);
the loader rejects a major mismatch, a newer minor or an unknown capability
before any mod code runs. `[meta]` is display metadata and never
affects composition. `[provides]` lists service keys. `[requires]` maps mod ids
to version ranges; ranges support `=`, `>`, `>=`, `<`, `<=`, `^`, `~`, space for
conjunction and `||` for alternation. `[optional_requires]` marks dependencies
that may be absent.

Static mods bundled with the server omit `[entry]`; their code is linked into
the binary and paired with the manifest through the static registry. External
packages are installed into a content store and read back through their install
receipt: the runtime catalog is built from receipts, so an installed package's
manifest and data are exactly the installed bytes.

External native mods declare `[entry].native` and implement the C entry ABI in
`include/refrain/mod/entry.h`; the loader calls `refrain_mod_main`, validates
the descriptor ABI, and invokes `on_load` with the sealed host core table.
Native code loading through an installed package depends on the loader contract
that materialises the entry library from the content store; until it lands, a
receipt-based native mod fails startup with the loader's diagnostic instead of
loading from an uninstalled directory. External WASM mods (`[entry].wasm`) are
rejected until the script runtime lands.

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
that provides it, using a version range. With `auto_scan = true` the resolved
composition also includes every mod installed in the server's content store
(`--content-store`, plus any `--mods` store roots); with `auto_scan = false` the
gamepack is the only source of truth and every required version must be listed.

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

### Session features

A mod may implement optional session feature services instead of a packet
provider:

| Service | Contract | Purpose |
|---|---|---|
| `refrain:session.status` | `session_status.ridl` | dynamic MOTD, online and max player counts |
| `refrain:session.tab` | `session_tab.ridl` | tab-list header and footer |
| `refrain:session.lifecycle` | `session_lifecycle.ridl` | player join/leave and per-tick notifications |

Each service is `instanced`; select the mod under the matching gamepack
provider key (`session_status`, `session_tab`, `session_lifecycle`). The runtime
calls every active instance and uses the first handled view for status/tab.
Lifecycle providers are notification-only.

[`demo/refrain.session.dynamic`](../demo/refrain.session.dynamic) is an
unbundled example. It is intentionally outside `mods/` and is not selected by
the default gamepack; copy it into an instance mods root and bind it explicitly
when testing.
## External native entry

A native mod exports `refrain_mod_main` (or the symbol named by
`[entry].symbol`) with `REFRAIN_MOD_MAIN` and returns a process-lifetime
`refrain_mod_desc`:

```c
#include <refrain/mod/entry.h>

static refrain_status on_load(const refrain_mod_context *ctx, void **user) {
    /* query/register services through ctx->host_core */
    return REFRAIN_STATUS_OK;
}

static const refrain_mod_desc desc = {
    .struct_size = sizeof(refrain_mod_desc),
    .abi_major = REFRAIN_MOD_ABI_MAJOR,
    .on_load = &on_load,
    .on_unload = NULL,
};

REFRAIN_MOD_MAIN(refrain_mod_main) {
    (void)init;
    return &desc;
}
```

The host validates `struct_size`, `abi_major` and `on_load` before calling
`on_load`; failures abort startup with the mod id. The shared library stays
loaded for the process lifetime and `on_unload` runs at shutdown.

## Host runtime

The server exposes a stable runtime API to mods through sealed services.
`refrain:host.core` is registered by the host bootstrap before any mod factory
runs; it covers logging, time, host memory, read-only config namespaces and service
lookup/registration. `refrain:host.platform` adds random-access files,
read-only mmap and crash annotations; file handles are host-owned and
single-threaded. `refrain:host.kernel` adds background jobs and a process-local
event bus; job callbacks run on host workers and event callbacks run on the
publishing thread. `refrain:host.metadata` exposes the generated schema
metadata (types, fields, enum values and contract versions) as read-only data;
it is not C++ `reflect<T>`. Rust bindings for the sealed host contracts are
generated under `sdk/generated/rust/refrain_host.rs`. `refrain-server --config-dir <dir>` mounts `refrain.toml` as `host`, the
sibling `<name>.toml` files as their namespaces and `mods/<id>.toml` as
`mod/<id>`; without the option no namespace is mounted and config reads return
`unsupported`. A sealed service is not a gamepack provider: a mod cannot
declare it in `[provides]` or replace it through `register_provider`. The C ABI
and C++ wrapper are generated from `sdk/idl/refrain/host/core.ridl`, so host
code and mods share the same versioned contract definitions.

## Installing mods

Every external mod is installed into a content store before it can load. The
store layout is:

```text
<store>/
  mods/<id>/<version>.json          install receipt (the load source)
  mods/pins/<id>.json               pinned signer of a mod id
  content/blobs.idx                 content-addressed blob table
  content/segments/*.rfseg          sealed segments for small members
  content/resolutions/<composition>.idx   published content index
  content/.staging/                 install transaction debris (self-healing)
```

Install one package:

```bash
refrain-tools mods install package.rfmod --store /path/to/store \
    --key-hex <64-hex-public-key> --key-id first-party
refrain-tools mods installed --store /path/to/store
```

`--content-store <dir>` enables the same store for the server. Packages placed
in the inbox (`<store>/mods/inbox` by default) are verified, unpacked into a
staging directory, committed to the content-addressed store and recorded with a
receipt; the startup bootstrap then publishes one resolution index from the
installed receipts and may run a mark-sweep GC.

An install is one transaction:

1. verify the package (structure, per-file BLAKE3, Ed25519 signature);
2. check the update policy (see below);
3. unpack the verified members into `<store>/content/.staging/<id>-<version>-...`;
4. commit the members to the CAS in one batch;
5. publish the merged blob table and re-encode the resolution indexes;
6. write the receipt (the commit point: an install exists exactly when its
   receipt does);
7. write or refresh the signer pin;
8. remove the staging directory.

A crash or failure never leaves a half install: the previous receipt is
untouched and the next install prunes a staging directory whose owner process is
gone. Reinstalling the exact archive digest of an installed `(id, version)` is a
no-op.

## Trust, signatures and updates

Install-time policy lives in server configuration, not in command-line flags:
`config/mods.toml` is read before the startup bootstrap and every `mods install`
takes it with `--policy-file`. The command line can override individual values
for one invocation.

```toml
[signature]
require = true            # reject unsigned packages / unknown signers
policy = "strict"         # or "trusted-any" for a self-hosted store

[trust]                   # trust anchors, key id -> Ed25519 public key hex
"first-party" = "<64 lowercase hex characters>"

[install]
allow_downgrade = false
replace_same_version = false
keep_signer_pin = false

[rotation."example.mod"]  # explicit signer rotation allowlist
old_key_id = "example-2025"
new_key_id = "example-2026"
```

The server's built-in default is the production default: a trusted signature is
required and updates are strict. A development store may set `require = false`
and still install unsigned packages.

Update semantics are Android-style:

- **Signer continuity.** The first signed install pins the signer for the mod
  id: the receipt v2 records the key id and the BLAKE3 fingerprint of the
  verifying public key, and `mods/pins/<id>.json` records the pin. A later
  version must carry the same key id and fingerprint. A different signer is
  rejected unless a `[rotation."<id>"]` allowlist entry names the exact
  transition; the rotation moves the pin to the new key. `policy =
  "trusted-any"` accepts any trusted signer instead.
  A signed install accepted without a configured trust anchor records the key
  id but no fingerprint (the key bytes were never checked against an anchor),
  so configure the anchor to pin the key material as well as the name.
- **Signed state.** A signed mod cannot be updated by an unsigned package.
  Updating an unsigned mod with a signed package is allowed and pins the signer.
- **Version monotonicity.** A version lower than the highest installed version
  is rejected; `--allow-downgrade` accepts it for one run (audited).
- **Same-version content identity.** The same `(id, version)` with a different
  archive digest is rejected; `--replace-same-version` accepts it (audited).
- **Legacy installs.** A store installed before signer pinning (receipt v1)
  counts as unpinned. Updating such a mod is rejected until an administrator
  confirms the signer with `refrain-tools mods pin <id> --store <dir>` or passes
  `--accept-unpinned-signer` to the update.

Every override prints an audit line naming what was accepted.

## Updating and uninstalling

```bash
# Update: the same command, a newer package.
refrain-tools mods install newer.rfmod --store /path/to/store \
    --policy-file config/mods.toml

# Uninstall one version, or every installed version of the id.
refrain-tools mods uninstall my.mod@1.0.0 --store /path/to/store
refrain-tools mods uninstall my.mod --store /path/to/store

# Reclaim the now-unreferenced blobs.
refrain-tools content gc --store /path/to/store --apply --allow-empty-roots
```

Uninstall removes the receipts and, for the last installed version, the signer
pin (`--keep-signer-pin` keeps it so a reinstall must use the same signer). The
CAS is append-only: the blobs stay until the next mark-sweep GC runs with no
root pointing at them. `--allow-empty-roots` is required when nothing is
installed any more, because an empty root set by accident would otherwise look
like a request to wipe the store.

Receipts written by older builds are schema v1. The runtime keeps reading them,
and `refrain-tools mods receipts --store <dir>` reports the migration;
`--apply` rewrites them as v2 without confirming the signer (that stays an
explicit administrator decision).

## Content store

`refrain-server --content-store <dir>` enables the content store. The startup
bootstrap scans the inbox, installs packages, publishes one resolution index per
composition and can garbage-collect unreferenced content.

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

## See also

- [Schema compiler](schema.md) for RIDL service declarations and the generated
  C ABI and C++ wrappers a provider implements.
- [Running a server](running.md) for the CLI flags, configuration namespaces and
  content-store options that load a composition.
- [Architecture](architecture.md) for the composition and contract model in
  context.
