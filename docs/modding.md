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
    lib/        optional native entry libraries, by platform triple
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
loader = "native"
native = "lib/x86_64-linux/libexample.so"
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
that may be absent. A service id with a namespace other than
`refrain` is a custom contract: a mod that registers a provider for it must
also declare it in `[contracts]`. The descriptor format and its evolution rules
are under [Mod-defined contracts](#mod-defined-contracts).

Static mods bundled with the server omit `[entry]`; their code is linked into
the binary, paired with the manifest through the static registry and executed
by the implicit `builtin` loader. External packages are installed into a
content store and read back through their install receipt: the runtime catalog
is built from receipts, so an installed package's manifest and data are exactly
the installed bytes.

Every external manifest declares `[entry].loader`, the loader that executes the
package, and carries that loader's payload. External native mods use
`loader = "native"` with `[entry].native` and implement the C entry ABI in
`include/refrain/mod/entry.h`; the native loader reads the member from the
installed package, calls `refrain_mod_main`, validates the descriptor ABI, and
invokes `on_load` with the sealed host core table. External WASM mods use
`loader = "wasm"` with `[entry].wasm` and are executed by the sandboxed wasm
loader when the build links the WebAssembly runtime; [WASM mods](#wasm-mods)
describes the guest ABI, the budgets and the build switch. [Mod
loaders](#mod-loaders) describes the contract and the dispatch rules.

## Mod loaders

A loader is the execution environment a mod package is loaded through. Every
package is dispatched through one active instance of the instanced
`refrain:mod.loader` service. The host bootstraps its own `builtin` and
`native` loaders (plus the script loaders a build enables), and a trusted mod
that the gamepack declares as a loader provider can add a namespaced instance
during the same startup load. A package never chooses its loader by itself: the
manifest names the loader and the gamepack plan decides whether a
package-provided one exists.

### Declaring a loader

`[entry].loader` names the loader. A reserved first-party id (`native`,
`wasm`, `lua`, `bukkit`) is unqualified; a package-provided loader uses a
namespaced id such as `com.example.loader`. `builtin` is reserved for the host
itself and cannot be declared: a package without an `[entry]` table is internal
and is loaded by the builtin loader.

The loader id must match the payload shape of a first-party loader: `native`
requires `[entry].native`, `wasm` requires `[entry].wasm`. A package-provided
loader defines its own payload keys; this parser only knows the shared ones and
the loader validates the rest.

An external manifest must declare `[entry].loader`; a manifest with an
`[entry]` table but no loader is rejected and the error names the missing
field. There is no second way to express an entry point.

A gamepack that declares loader providers must also declare every namespaced
loader its packages name (`[loaders]`, see
[Package-provided loaders](#package-provided-loaders)); naming a namespaced
loader id that the plan does not declare fails composition resolution and the
error lists the declared ids. A gamepack without `[loaders]` keeps accepting
the host bootstrap's own namespaced loaders, which is what embedders and tests
register before `load()`.

### Dispatch and fail-closed rules

The host resolves a manifest's loader id to the active `refrain:mod.loader`
provider instance with that name before any mod code runs. The provider
declares its capabilities first; the host validates the loader id, the loader
ABI major, the unload semantics, the sandbox flag and the declared provider,
re-checks them before every dispatch and records the declared unload semantics
on the loaded mod.

The dispatch refuses to guess:

- a loader id with no active instance fails the load, naming the id and the
  active loader ids; an unknown namespaced id is `not_found`, a first-party
  loader that is not built into this host is `unsupported`;
- a manifest naming `wasm` in a build without the WebAssembly runtime, and a
  manifest naming `lua` in every build, are refused by the registered provider
  with `unsupported` and a message naming the disabled or missing runtime;
- a loader whose capabilities change after registration fails the next load
  instead of silently reconfiguring, and the diagnostic names the provider mod;
- a sandboxed loader cannot register providers on behalf of its guest: the
  registration is refused and the load fails with the sandbox diagnostic;
- a declared loader can only be registered by the mod package the gamepack
  names as its provider, and every other registration attempt is refused;
- a mod cannot provide, replace or shadow `builtin`, `native`, `wasm`, `lua`,
  `bukkit` or a `refrain.` loader id.

### Package-provided loaders

A trusted mod can be the execution environment other packages run in. Two
declarations are required and both are validated before any guest code runs:

1. the gamepack plan selects the launcher with a `[loaders]` entry keyed by
   the loader id, for example

   ```toml
   [loaders]
   "com.example.loader" = "com.example.scripts@^1.0.0"
   ```

   Loader ids contain dots, so every key is a quoted literal key. The value is
   the providing mod and the version range its installed version must satisfy.
   `builtin`, `native`, `wasm`, `lua`, `bukkit` and any `refrain.` id are
   refused here, so a gamepack can never make a package the environment for
   host-owned loaders.

2. the provider mod declares the service in its own manifest and registers
   the instance from its factory while the host is loading:

   ```toml
   [provides]
   services = ["refrain:mod.loader"]
   ```

   ```cpp
   // statically linked mod factory; the table is the provider's own generated
   // `refrain_services_mod_loader_vtable` and its state pointer
   auto on_load(refrain::mod::host &host) -> refrain::status {
       const refrain::mod::provider_api api{
           .vtable = &loader_vtable,
           .self = &loader_state,
           .abi_major = REFRAIN_SERVICES_MOD_LOADER_ABI_MAJOR,
       };
       return host.register_provider("refrain:mod.loader", "com.example.loader",
                                     api);
   }
   ```

   An installed native package has no C++ host symbol: its entry point
   (`include/refrain/mod/entry.h`) receives the sealed `refrain:host.core`
   table and registers through the stable C surface of that contract.

   ```c
   refrain_host_core_provider_ref api = {
       .struct_size = REFRAIN_HOST_CORE_PROVIDER_REF_SIZE,
       .vtable = &loader_vtable,   /* the loader's own generated vtable */
       .self = &loader_state,
       .abi_major = REFRAIN_SERVICES_MOD_LOADER_ABI_MAJOR,
   };
   const refrain_str service = {(const uint8_t *)"refrain:mod.loader",
                                sizeof("refrain:mod.loader") - 1};
   const refrain_str instance = {(const uint8_t *)"com.example.loader",
                                 sizeof("com.example.loader") - 1};
   host_core->register_provider(host_core_self, service, instance, &api);
   ```

The host loads loader providers in a bootstrap phase before the rest of the
composition: the declared providers and their transitive hard dependencies are
loaded first, in plan load order. The set is derived from the plan and the
manifests and does not depend on discovery or hash-map order. A loader provider
itself must run through the host-owned `builtin` or `native` loader: a
sandboxed guest (`wasm`, `lua`) can never become a loader, and a
package-provided loader cannot host another package-provided loader. The host
refuses a provider mod whose package source is neither the built-in image nor
an installed, receipt-checked package: a directory tree is not an install, so
the signature and install policy has already decided the package is trusted
before it is loaded.

The registration is gated by the same checks first-party loaders get: the
capability table must name the instance it is registered on, report the loader
ABI major the host implements, declare a known unload guarantee and name the
providing mod in the `provider` field of the capability struct. The loader
service is sealed after the first composition epoch, so a loader instance is
registered once per process: a second registration of the same id is
`already_exists` and the plan cannot add, remove or repoint a loader at
runtime. One provider mod may register several loader ids; each registration
carries its own capability table and names its own instance.

A package naming a declared loader loads after the provider: resolution adds a
dependency edge from the provider to every package that names its loader, so
the load order is the same as if the package declared the dependency itself.
A cycle between loader providers and the packages they order is rejected at
composition resolution with the cycle members named. A `[loaders]` entry whose
provider is missing, out of range or does not declare `refrain:mod.loader`
fails resolution before any package is loaded.

The lifecycle guarantee is a refusal: a loader provider is part of the
bootstrap composition, so it cannot be hot-unloaded or reloaded while the
process runs, whether or not it currently has live guests. A staging request
that removes a declared provider, changes the loader plan or reloads the
provider fails with `unsupported` and names the loader and provider mod; the
running epoch is untouched. A loader that changes its capabilities after
registration is detected on the next dispatch and the load fails with
`version_mismatch`, naming the loader and the provider mod.

The loader contract hands the provider the guest manifest subset and a package
root; it does not forward a package's custom `[entry]` payload keys, because
the parser keeps only the shared entry fields and the loader validates its own
layout. A loader therefore defines a fixed member layout for its guests, for
example a `data/payload.bin` member that the package walker records.

`package_root` is empty for a manifest read from the built-in image. For an
installed package it is the directory that holds the install receipt, so
`<package_root>/<version>.json` is the receipt and the guest members are the
receipt-checked content-store blobs. Host-linked code reads them through the
package API (`make_receipt_package_source()` in `refrain/mod/package.hpp`,
which verifies the receipt and the member hashes); an external native loader
receives the path only and implements the read against the receipt and the
content store itself.

### Loader boundaries

| Loader | Execution | Unload semantics | Status |
|---|---|---|---|
| `builtin` | host-owned: pairs the built-in image manifests with the statically linked code and calls the factory of every selected provider | `restart_required` | shipped |
| `native` | host-owned: materialises the installed `[entry].native` member out of the content store, checks the entry ABI, opens the shared library and calls the entry point | `retire` | shipped |
| `wasm` | host bootstrap: materialises the installed `[entry].wasm` member from the receipt-checked content store, validates the guest ABI, instantiates the module in a fresh store with size/memory/instruction budgets and calls its `on_load` hook | `clean` | shipped when built (`REFRAIN_WITH_WASM`) |
| `lua` | fail-closed placeholder: no Lua runtime ships, so every package naming `lua` is refused with the missing runtime named | `clean` | fail-closed |
| `com.example.loader` | package-provided: the declared provider mod registers a `refrain:mod.loader` instance and runs its guests itself | declared by the provider | via `[loaders]` |
| `bukkit` | optional standalone mod that embeds a JVM and manages its own plugin directory | declared by that mod | via `[loaders]` |

The `builtin` and `native` loaders are host bootstrap code: they wrap the
static mod registry and the dynamic-library entry ABI the host already owns, so
they are registered by the host itself instead of shipping as a mod. A
package-provided loader ships as a mod that declares `refrain:mod.loader` in
`[provides]` and is selected by the gamepack `[loaders]` table; the same table
can select a standalone implementation such as a future Bukkit bridge.

The native loader copies the entry library into a private host-owned directory
before opening it: an installed package keeps its members in the content store,
so the manifest path is a package member and never a filesystem path next to a
manifest. Each load materialises a new private copy under a unique path (the
member digest plus a per-load sequence), so a reload can never resolve to a
mapping of an earlier generation; the bytes are the installed,
receipt-checked bytes.

The loader's unload semantics bound what the host may release: a `clean`
loader releases the guest completely, a `retire` loader keeps its resources
mapped and only stops dispatching, and a `restart_required` loader cannot be
unloaded while the process runs. The hot lifecycle below honours exactly the
declared guarantee.

### WASM mods

A WASM mod is an installed `.rfmod` package whose manifest names the wasm
loader and declares the module member:

```toml
[entry]
loader = "wasm"
wasm = "scripts/example.wasm"
```

The wasm loader reads the member back from the content store through the
install receipt, so the bytes it compiles are exactly the installed bytes; a
missing, oversized or corrupt member fails the load before any guest code
runs. The guest ABI is deliberately narrow:

- the module has **no host imports**. A module that declares one is refused as
  a sandbox violation, and there is no path from a guest to a loader table, the
  service registry or a host provider. Host imports will be added only through
  a restricted script API, never as a raw provider table;
- it must export `on_load: () -> i32`, called before the mod becomes active,
  and may export `on_unload: () -> i32`, called on unload. A return value of
  zero reports success; a non-zero `on_load` return fails the load;
- all guest state lives in the module's own linear memory, which is released
  when the instance is destroyed.

Each load gets a fresh store with explicit budgets: a maximum module member
size, a linear-memory ceiling, an instruction budget for the load path and a
wall-clock load deadline. A guest that exhausts a budget is stopped and the
load fails closed instead of leaving a half-loaded mod behind. A `clean`
unload destroys the store and releases the module, memory and instruction
budget; a reload builds the replacement generation first and unloads the old
instance after the composition epoch commits, so a staged change never frees
code the running composition still calls. Module code only runs during
instantiation, `on_load` and `on_unload` in this slice; per-call execution
budgets arrive with the script host API and the call dispatch path it needs.

The wasm loader ships with the server composition when the build links the
WebAssembly runtime (`with_wasm=True`, the default). A build configured with
`-o with_wasm=False` registers the loader as an explicit fail-closed provider:
every manifest naming `wasm` is refused with `unsupported` and a message
naming the disabled build. The `lua` loader is registered the same way as an
explicit fail-closed provider: it names the missing Lua runtime, and no Lua
runtime ships with the server.

`refrain-tools mods load` prints the registered loaders and, for every loaded
mod, the loader that executed it and the recorded unload semantics.

## Build profiles

Two layers decide what runs:

| Layer | Decides | Where |
|---|---|---|
| Build profile | which first-party mods exist in the binary | `profiles/<name>.toml` |
| Gamepack | which providers of that set are active and how they compose | `gamepack/*.toml` |

A gamepack can only select mods the binary was built with. The default build
profile is `vanilla`, which contains every bundled mod. A profile lists mod ids
without versions; the build resolves the manifests under `mods/`:

```toml
name = "minimal"
description = "Content and data layer only."
gamepack = "gamepack/presets/data-only.toml"
mods = ["refrain.content.vanilla", "refrain.data.vanilla"]
dynamic = []
```

The build uses that one list to compile the mod sources, to pack the built-in
image and to validate the default gamepack, then records the profile name, the
resolved mod list and a build id in the binary and in `mods.lock`. The `dynamic`
list names mods compiled as shared modules loaded through the native loader
instead of being linked in; the mod code and manifest are the same. The loader
resolves the module's runtime calls from the host executable, so a module may
only use runtime functions the host already links; the shared runtime library
that lifts this restriction is future work.

Check a profile before building (this reads the profile and the mod tree, so it
works with any `refrain-tools` binary):

```bash
refrain-tools mods check --profile minimal
refrain-tools gamepack check --profile minimal
```

Add `--built` to compare the profile file with the mod set compiled into the
binary you are running, and `--image <builtin.image>` to verify the built-in
image contains exactly that set:

```bash
refrain-tools mods check --profile vanilla --built \
    --image build/Release/generated/builtin.image
```

A composition that asks for a mod the binary does not contain fails before the
server starts, naming the profile and the mod:

```
mod 'refrain.world.vanilla' is not built into this binary (profile=minimal,
build 0123456789abcdef); gamepack 'gamepack/gamepack.toml' pins it. Rebuild
with a profile that includes it or use a composition without it
```

The same check applies to a `mods.lock` written by a different build: a lock is
refused when its profile or build id does not match the running binary, because
the two binaries do not contain the same mods.

### Adding a bundled mod

1. Add `mods/<id>/manifest.toml` (and `src/` when the mod has code).
2. Add the id to the profiles that should contain it. `vanilla` lists every
   bundled mod; a new optional feature belongs in a dedicated profile
   (`profiles/1.17.toml` is the template for a version-specific set).
3. Pin it in the default gamepack of each profile that contains it, and add a
   `[providers]` binding when it provides a service.
4. If the mod is a provider whose code should be hot-reloadable, add the id to
   the profile's `dynamic` list. Dynamic modules are built from the same sources
   and manifest; there is no second source tree.

The build validates the profile/gamepack pair at configure time and the CI
profile matrix runs the same checks for `minimal` and `dynamic`.

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
    "com.example.scripts@1.0.0",
]

[mods."refrain.data.vanilla"]
requires = ["refrain.content.vanilla@^1.0.0"]

[providers]
world = "refrain.world.vanilla@^1.0.0"
data = "refrain.data.vanilla@^1.0.0"

[loaders]
"com.example.loader" = "com.example.scripts@^1.0.0"
```

`[mods].list` pins the composition. `[providers]` maps a service name to the mod
that provides it, using a version range; it cannot bind the loader service
(`refrain:mod.loader`), because package-provided loaders are declared in
`[loaders]` and the host-owned ones come from the host bootstrap. `[loaders]`
declares the package-provided loader instances a composition trusts, keyed by
loader id and pointing at the providing mod. Both are optional. With
`auto_scan = true` the resolved composition also includes every mod installed
in the server's content store
(`--content-store`, plus any `--mods` store roots); with `auto_scan = false` the
gamepack is the only source of truth and every required version must be listed.

A resolved composition is written to `mods.lock` next to the gamepack. The lock
records the selected versions, the provider for each service, a fingerprint of
the resolution and the build profile and build id that wrote it. Loading fails
when a pinned mod is missing, the composition drifts or the lock was written by
a different build; `refrain-server --update-lock` rewrites the lock for an
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
version is checked on load. A mod can also define its own contract
instead of implementing a RIDL contract; [Mod-defined
contracts](#mod-defined-contracts) covers that path.

Provider implementations live in `mods/<id>/src/` for first-party mods. Their
code is compiled into the server and registered through the static registry (or
loaded from a shared module when the profile lists the mod as dynamic). The
build profile decides which first-party providers exist; a provider the
gamepack selects from outside that set fails before startup with the profile
diagnostic described under [Build profiles](#build-profiles), not with a
missing service key.

### Mod-defined contracts

A mod can publish a service contract of its own without a server patch. The
contract is data in the package manifest: the defining mod declares the service
id, its ABI version, the generated vtable layout and the range of contract
versions it promises compatibility with. Other mods declare the range they
consume, and every provider is reached through the same `query_service` and
`query_service_handle` API as a first-party service.

Custom service ids use the `namespace:name` form with a namespace other than
`refrain`; `refrain` stays reserved for the host runtime and the first-party
contracts generated from RIDL.

#### Contract descriptor

```toml
[provides]
services = ["example:shop"]

[contracts."example:shop"]
version = "1.0.0"
vtable_size = 32
compatible = ">=1.0.0 <2.0.0"
trusted_providers = ["example.shop.extra"]
```

`version` is the contract ABI version, not the mod version. Its three
components are copied into the generated vtable header (`abi_major`,
`abi_minor`, `abi_patch`) and must be a release SemVer with a non-zero major.
`vtable_size` is the generated vtable `struct_size`. The host records both,
checks every provider table against them, and never reads the contract
functions.

`compatible` is the range of contract versions the declaration covers, as a
SemVer range. It defaults to the whole major line
(`>=<major>.0.0 <<major+1>.0.0`) and must include the declared version.
Consumers must require a range inside it, so a package cannot depend on an
evolution band the owner does not promise.

`trusted_providers` is optional. The declaring mod is the contract owner and
may register a provider for the service; another mod may do so only when the
owner lists it here. Every other provider is refused.

A consumer declares the range it uses:

```toml
[requires]
"example.shop" = "^1.0.0"

[contract_requires]
"example:shop" = "^1.0.0"
```

`[contract_requires]` maps a custom service id to the range the consumer
accepts. The hard dependency in `[requires]` is what orders the load: a
consumer that calls the provider from its own `on_load` must require the
provider mod, so the provider is loaded first. The host checks the requirement
after the whole composition is assembled and fails closed when no declaring mod
is present.

#### Registration

The gamepack binds the custom service by its full id; the value is the provider
mod and its version range:

```toml
[providers]
"example:shop" = "example.shop@^1.0.0"
```

A provider registers for a custom service through `register_provider` like any
other provider. The request passes only when all of these hold:

| Check | Failure |
|---|---|
| the manifest `[provides]` lists the service | `permission_denied` |
| the gamepack plan binds the service to the mod | `permission_denied` |
| a loaded mod declares the contract | `permission_denied` |
| the registering mod is the owner or in `trusted_providers` | `permission_denied` |
| the vtable header matches `version` and `vtable_size` | `version_mismatch` |
| the service is neither sealed nor in the reserved namespace | `permission_denied` |

A provider whose vtable header disagrees with the descriptor fails with
`version_mismatch` naming the declared version, the header version and the two
sizes. When the provider is not the declaring mod, the declaration owner must
be loaded first; list it in the provider's `[requires]` so the load order
guarantees it. A hot epoch re-checks every still-active custom provider table
against the declaration it republishes, so reloading a declaration cannot leave
an older trusted table serving the new contract. Reserved host ids cannot be shadowed by a custom declaration: a
`[contracts]` entry in the `refrain` namespace is rejected when the manifest is
parsed.

#### Consumption

`query_service` and `query_service_handle` address a custom service by id; the
returned `provider_ref` is cast to the client wrapper generated from the
contract RIDL. Nothing in the runtime path changes. A consumer caches the
handle for one tick, calls `validate_handle` at the tick boundary and
re-queries after a `stale` result. A replaced or unloaded provider reports
`stale` for cached handles and `not_found` for new queries.

Version conformance happens before activation. After assembling the whole
composition the host requires the declared contract version to satisfy every
consumer's required range, requires that range to lie inside the declaring
mod's `compatible` band, and fails with `version_mismatch` naming both mods and
both versions. Two mods declaring the same service id fail at load, naming both
mods; a differing ABI is a `version_mismatch` and an identical declaration is an
`already_exists` conflict.

The requirement is checked when the composition is assembled, not on every
query. Any mod that knows a custom service id can call `query_service` without
declaring `[contract_requires]`, so the manifest declaration is a composition
guarantee rather than a query-time authorization check; a deployment that needs
that isolation must restrict the packages it installs.

`refrain-tools mods check --mods <dir>` prints every descriptor it parsed
(`contracts: example:shop@1.0.0 vtable 32 compatible ...`), so a malformed
declaration fails the tool before a server ever loads the package.

#### Version evolution

Custom contracts follow the same SemVer 2.0.0 policy as RIDL contracts. A major
bump is a breaking change and consumers must require the new major. A minor
bump appends vtable slots, increases `vtable_size` and leaves the existing
slots unchanged, so a consumer can keep requiring an older minor while the
owner widens `compatible` to accept both. A patch bump changes no layout and no
ABI field. A released version number is never reused for different content.

Ship the contract RIDL source and its generated headers in the package and
regenerate the headers whenever the contract changes; `version` and
`vtable_size` in the manifest must be updated in the same step. A provider that
forgets the bump fails registration instead of silently mismatching the
declaration.

#### Sandbox limit

A sandboxed loader refuses provider registration on behalf of its guest. A
scripted guest cannot publish a custom contract provider and never receives a
raw provider table: the refusal is `permission_denied` and names the loader and
the service, and the guest reaches the host only through its import layer. A
custom contract package may still ship data for a sandboxed guest, but a
provider table stays a native-host capability until a script host API defines
the mapping.

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
### Permission nodes and queries

`refrain:permissions` is a `chain` service provided by the built-in
`refrain.permissions` mod. A query names a subject, a node and an optional
context and is answered with `allowed`, `denied` or `not_handled`; a chain
wrapper answers `not_handled` for nodes it does not own and the built-in
default resolver answers the rest. The default gamepack binds the built-in mod
as the chain `base`.

Nodes are dot-separated lowercase paths. A first-party feature declares its
nodes under `refrain.<area>` (for example `refrain.command.teleport` or
`refrain.mods.admin`); a third-party mod uses its own reverse-domain root, for
example `com.example.mymod.home`. At least two segments are required. A
configured rule may end in `.*` to cover a namespace and may start with `!` to
deny, and a deny wins over every grant:

```toml
# config/permissions.toml
[defaults]
default = "deny"        # or "op": an operator holds every node
nodes = ["refrain.command.help", "refrain.command.list"]

[op]
min_level = 1           # vanilla ladder level that counts as an operator
nodes = ["refrain.mods.admin"]

[ops]
players = ["<uuid>", "Notch"]   # player identities that count as operators

[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.command.*", "!refrain.command.stop"]
"player:Notch" = ["@moderators"]
"plugin:com.example.mymod" = ["refrain.world.edit"]

[groups]
moderators = ["refrain.permissions.admin"]
```

Rules resolve in this order: the console holds every node; a matching deny
denies; a matching grant allows; an operator's `[op].nodes` set allows; a grant
in `[defaults].nodes` allows; `[defaults].default` decides. An operator is a
player listed in `[ops].players` (canonical lowercase uuid or profile name) or
one at or above `[op].min_level` on the vanilla ladder supplied in the query.
`[defaults].nodes` is the public surface every subject holds; it is how the
built-in command tree keeps `/help` and `/list` available on a fresh install
while management commands stay grant-only. A grant joins a group with `@name`;
groups are single-level in v1. Without a mounted `permissions` configuration
namespace the resolver serves the built-in policy: the console holds every node
and every other subject is denied, so a composition that adds the service
before the operator writes a policy fails closed instead of granting players
access.

The built-in command tree is the reference consumer of this policy: every
in-game command declares a stable node (`refrain.command.<name>` and
`refrain.mods.admin` below the feature namespace), and the tree, the
suggestion list and the execution check all consult the same node. A command
node also declares a vanilla ladder fallback, which decides when no provider is
active; the complete node table is in
[Running a server](running.md#in-game-commands). A command denied by the policy
is absent from the tree and from completion, and the execution answers with the
missing node text. Management commands use a feature namespace instead of the
command namespace: the permission administration commands require
`refrain.permissions.admin`, and the built-in permission service administers its
own policy through a host-side API that the contract does not expose. A chain
wrapper can answer queries and add nodes in front of the base resolver, but it
cannot change the base policy; a composition without the built-in mod has no
`/permissions` management surface and refuses changes as unsupported.

A mod declares its nodes by using them: there is no registration call, only the
namespace convention above. To query from mod code, look the provider up
through the sealed host table and bind the consumer helper (header-only, no
engine symbol):

```cpp
#include <refrain/contracts/host/core.hpp>
#include <refrain/permissions/client.hpp>

// `context` is the `refrain_mod_context` passed to on_load; `host_core` stays
// valid for the process lifetime.
const refrain::host::core::provider host{*context->host_core,
                                         context->host_core_self};
const auto service = host.query_service("refrain:permissions", "");
if (service) {
    const auto client = refrain::permissions::client::bind(service->vtable,
                                                           service->self);
    const auto who = refrain::permissions::subject::player(profile_uuid,
                                                           profile_name,
                                                           op_level);
    if (client.has(who, "com.example.mymod.home")) {
        // allowed, or the service has no opinion: the caller decides its own
        // fallback for not_handled
    }
    if (client.query(who, "refrain.mods.admin") ==
        refrain::permissions::permission_result::denied) {
        // explicitly denied
    }
}
```

`has()` reads `not_handled` as "maybe"; a caller that must separate "no
opinion" from "allowed" uses `query()` directly. Queries are read-only and may
run on any thread, so a consumer keeps the provider table for one composition
epoch and re-queries after a hot epoch. `revision()` is a monotonic policy
counter: a command tree or suggestion cache compares it and rebuilds when it
changed. `reload()` re-reads the configuration file and publishes a new
snapshot; it runs on the main thread and fails with the parser's diagnostic
without touching the published policy.

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

The manifest declares the native loader and the packaged member:

```toml
[entry]
loader = "native"
native = "lib/x86_64-linux/libexample.so"
symbol = "refrain_mod_main"   # optional
```

The native loader reads the member from the installed package (verifying it
against the install receipt), copies it into a private host directory the
dynamic loader can open, and validates the manifest entry ABI, the descriptor
`struct_size`, `abi_major` and `on_load` before calling `on_load`; failures
abort startup with the mod id. The shared library stays loaded for the process
lifetime and `on_unload` runs at shutdown, which is the loader's `retire`
semantics.

## Host runtime

The server exposes a stable runtime API to mods through sealed services.
`refrain:host.core` is registered by the host bootstrap before any mod factory
runs; it covers logging, time, host memory, read-only config namespaces and service
lookup/registration. A mod that keeps a provider lookup across ticks uses
`query_service_handle`, which returns the provider table together with the
committed `(service, instance)` generation, and re-checks it with
`validate_handle` at each tick boundary. A replaced or retired generation
reports `stale`, and the mod re-queries instead of calling the table it cached;
the sealed host tables report generation zero because they never change.
`query_service` stays the process-lifetime form for those sealed tables and
for providers a caller uses only within one call chain. `refrain:host.platform` adds random-access files,
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

The receipt is one schema, `refrain.mod-receipt/1`. It records the package
identity and digests, the signer identity (`key_id` and the BLAKE3
`public_key_fingerprint` of the verifying key), the policy that accepted the
install, the audit fields `installed_at` and `installer_version`, the
host-observed `origin` and one `path`/`size`/`blake3`/`mode` record per installed
file.

A store written by an older development build carries receipts with the retired
`refrain.mod-receipt/2` schema. They are not migrated: parsing fails closed, so
the runtime refuses the store and names the unsupported schema. Reinstall the
packages to recover it: remove the old `mods/` receipt tree and run
`mods install` for each package, which recreates the receipts with the current
schema and reuses the content blobs already in the store.

## Hot lifecycle

A running server can install, uninstall and reload a mod while the composition
keeps ticking. The unit of change is a composition **epoch**: a complete
provider-generation set that is built while the server runs and switched in one
atomic step at a tick boundary.

```text
staged -> active -> quiescing -> retired -> removed
```

- `staged`: the package is resolved and its loader instance is built, but the
  running composition does not call it yet. A failure here is rolled back and
  leaves the committed epoch untouched.
- `active`: the mod's providers belong to the committed registry generation;
  callers hold generation-stamped handles.
- `quiescing`: the generation stops accepting new calls. The lifecycle waits a
  bounded time (five seconds) for calls that are already inside it.
- `retired`: the loader's unload hook ran (or the mod was quarantined) and the
  generation's handles fail as `stale`. Native code may still be mapped.
- `removed`: the composition record and the install receipt are gone; content
  garbage collection may reclaim the package blobs.

Staging does the slow work outside the tick: reading the package, verifying a
receipt, opening a shared library, building the replacement provider table.
The publish is one call at the next tick boundary (the server runtime commits
before the tick body). It swaps every staged provider slot at once, advances
the composition epoch, retires the old generations and runs their unload hooks.
No package or store I/O happens inside the tick.

### Restart-required mods

Not every mod can be hot-switched. A change is refused with a message naming the
reason when the mod

- runs statically linked code (the `builtin` loader declares
  `restart_required`), or
- provides a core service the running process owns: `refrain:sim`,
  `refrain:world`, `refrain:storage`, `refrain:net.protocol` or
  `refrain:runtime`, or
- provides a package-provided loader declared in `[loaders]`: the loader
  provider is part of the bootstrap composition and cannot be unloaded or
  reloaded while the process runs, whether or not it has live guests.

Leaf services, data-only mods and providers behind a `clean` or `retire` loader
are hot-swappable. A mod that other active mods still require cannot be
uninstalled; uninstall or reload the dependency first.

### Data-only content reload

A data mod contributes a `data/` tree and no code. Its documents (recipes, loot
tables, advancements, worldgen documents, structure templates and tags) are the
datapack content layer, not the composition, so they reload on their own path:
`mods reload-data` (console or op `/mods reload-data`) rebuilds the layered
content view from the same stack the server came up with, loads and validates
it off the tick, and publishes it at the next tick boundary. Readers take the
live view once per tick, so the previous view stays mapped until the last
reader drops its snapshot and a tick that started on it finishes on it.

The gate is `[lifecycle] hot_reload_data` in `config/mods_admin.toml`, off by
default. A failed rebuild rolls back: the live generation stays and the error
names the layer and the path, which makes a broken edit visible without taking
the server down.

Block, item and entity registries are not part of the view. The server freezes
them at start and existing chunks keep the palette generation they were written
with; a pack whose reload would add, remove or edit a registry document
(`data/<namespace>/block/...`, `data/<namespace>/item/...`,
`data/<namespace>/entity_type/...`, or a shipped `tables.rvtb` /
`protocol_registry.json`) is refused with `restart required` and the changed
files are named. Mod code and the provider composition are the other half of a
reload and go through `mods reload <id>` and `hot_reload`, not `hot_reload_data`.
Chunk generation starts its workers from the game-data directory at boot, so
changed worldgen documents are picked up by newly generated chunks after a
restart in this build.

### Native retire and quarantine

A shared library is never `dlclose`d while the server runs: a call that started
before the epoch switch may still be inside the old mapping. A reload
materialises the replacement `.so` from its install receipt into a new unique
path, opens it, and retires the old mapping after the commit. Retired
generations stay mapped and are counted per mod; after eight generations a
further reload is refused and the operator must restart the server. The limit
is configurable by the embedding (`host::set_native_retire_limit`).

A mod whose `on_unload` reports a failure, or whose in-flight calls do not drain
within the bounded wait, is **quarantined**: the host skips its unload hook,
never calls it again, and refuses to install or reload that id until the process
restarts. A drain timeout quarantines instead of silently continuing to call an
implementation whose lifetime can no longer be proven.

Script loaders declare `clean` unload: destroying the guest runtime releases
its state completely, so they never accumulate retired mappings. The `wasm`
loader is hot-swappable when built and refuses packages in a build without the
runtime; the `lua` loader is an explicit fail-closed provider because no Lua
runtime ships with the server.

### Registry epochs and stale handles

The service registry keeps `(service, instance) -> provider generation`.
Sealed host services stay frozen forever; ordinary services accept a staged
activation, a staged retirement or a replacement candidate. Callers read a
handle with its generation and re-check it (`service_registry::validate`) when
crossing a composition boundary; a handle from a retired generation returns
`stale` instead of reaching freed code. Normal tick-path calls are direct
vtable calls; the generation check is a per-boundary check, not a per-call
lookup. The sealed host core ABI exposes the same state to mods:
`query_service_handle` returns the table with its generation and
`validate_handle` answers `stale` when the cached generation was replaced, so
a mod does not need in-process registry access to detect a hot reload. An
in-process consumer that keeps a table across ticks caches it through
`mod::provider_cache` and refreshes the cache at the tick boundary.

The composition epoch and a BLAKE3 digest over the committed mod generations,
provider generations and gamepack identity are part of the audit record, so a
run with hot changes can be told apart from an identical static composition.

### Application API

`app::mod_lifecycle` is the shared service the command surfaces call. It offers
`install(path)`, `uninstall(id[, version])`, `reload(id)` and `commit()`:

- `install` runs the content-store install transaction, re-resolves the
  catalog and stages the new composition. A refused staging step leaves the
  package installed, so the next server start loads it;
- `uninstall` stages the removal first and deletes the install receipt only
  after the staging step accepted it;
- `reload` rebuilds the active generation from its installed package;
- `commit` is what the tick loop calls; the command layer never publishes an
  epoch by itself.

`make_mod_lifecycle_port()` adapts the service to the shared command service's
`mod_lifecycle_port`. The command layer keeps parsing, permission checks,
durable store transactions and audit; the port stages the running-composition
half and reports that the change publishes at the next tick boundary. The
server composition root builds the service from the running host and the
catalog it resolved (content-store roots and the effective auto-scan policy)
and wires the port into `mod_management_context.lifecycle`; the runtime commits
staged changes at the next tick boundary. A server without a loaded host or a
content store leaves the port empty, and the `mods` commands then report that
the running composition was not changed instead of simulating one.

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

The built-in default is the production default for the server and for
`refrain-tools mods install`: a trusted signature is required and updates are
strict. With no `--policy-file` the tool applies that default and prints the
policy it used, so a missing policy file cannot be mistaken for a lax one. A
development store that installs unsigned packages opts out explicitly with a
policy file that sets `require = false`.

Update semantics are Android-style:

- **Signer continuity.** The first signed install pins the signer for the mod
  id: the receipt records the key id and the BLAKE3 fingerprint of the
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

The commands above operate on an offline store. The same operations are
available against a running server through `app::mod_lifecycle` (the command
surface calls it): the package is installed or the receipt removed immediately,
and the composition change is published at the next tick boundary. Native code
reloaded at runtime accumulates retired mappings as described in
[Hot lifecycle](#hot-lifecycle).

Uninstall removes the receipts and, for the last installed version, the signer
pin (`--keep-signer-pin` keeps it so a reinstall must use the same signer). The
CAS is append-only: the blobs stay until the next mark-sweep GC runs with no
root pointing at them. `--allow-empty-roots` is required when nothing is
installed any more, because an empty root set by accident would otherwise look
like a request to wipe the store.

## Managing mods on a running server

The server exposes the same catalog from its console and from the
`refrain.mods.admin`-gated `/mods` command, and both call one application
service, so a result, an error or an audit entry is identical whichever
frontend ran the command:

```text
mods list
mods installed
mods verify package.rfmod
mods install package.rfmod confirm
mods uninstall my.mod@1.0.0 confirm
mods reload my.mod
mods reload-data
mods trust
mods trust pin my.mod confirm
mods trust clear my.mod confirm
```

The management service reads the install/trust policy from the same
`config/mods.toml` the startup bootstrap uses, so a runtime install cannot
accept a package the next start would reject. `config/mods_admin.toml` holds
the hot lifecycle gates (`[lifecycle] hot_install`, `hot_reload`,
`hot_reload_data`). Who may run the in-game command is the
`refrain.mods.admin` permission node: grant it in `config/permissions.toml`,
either by listing the player in `[ops].players` or with a `[grants]` entry. The
console frontend is unconditional. The old `[admin] operators` list is not
read; a stale table is ignored with a warning so an old allowlist never
silently becomes a grant. The hot gates default off: installing or
uninstalling changes the content store at
once, but the running composition picks the change up on the next start unless
the gate is enabled. A hot install only publishes a mod the composition
resolves: with auto-scan off the gamepack has to list it, otherwise the receipt
stays installed for the next start.

`install`, `uninstall` and the trust changes require the literal `confirm`
argument; `--dry-run` reports without writing store state. Every operation,
including a refusal or a failure, writes one structured audit line through the
`mod-audit` logger with the actor, the action, the mod id/version, the package
digest, the signer key id, the policy and the result.

[Running a server](running.md#mod-management) documents the console and
in-game command tables, the permission node and the configuration defaults.

## Content store

`refrain-server --content-store <dir>` enables the content store. The startup
bootstrap scans the inbox, installs packages, publishes one resolution index per
composition and can garbage-collect unreferenced content.

`--content-gc off|dry-run|apply` selects the startup policy. In `apply` mode the
deletion queue is drained by a background worker; the tick thread never unlinks
files.

## Built-in image

The build packs the mods selected by the build profile into a single
deterministic image:

```bash
refrain-tools builtin image --mods mods --mod refrain.data.vanilla \
    -o build/Release/generated/builtin.image
```

`--mod <id>` (repeatable), `--mod-list <profile.toml>` and `--profile <name>`
select the included mods; without a selection every manifest in the tree is
packed, which the tooling fixtures use. The image is embedded in the server
binary. A deterministic build produces the same image bytes, which the test
suite and the profile consistency check rely on.

## See also

- [Schema compiler](schema.md) for RIDL service declarations and the generated
  C ABI and C++ wrappers a provider implements.
- [Running a server](running.md) for the CLI flags, configuration namespaces and
  content-store options that load a composition.
- [Architecture](architecture.md) for the composition and contract model in
  context.
