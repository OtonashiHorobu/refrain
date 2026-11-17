# Running a server

`refrain-server` runs a world headless for a fixed number of ticks or until the
console receives `stop`. It listens for protocol 777 clients using the
`network.toml` settings.

## First run

A fresh download or build starts like the vanilla server jar: no required
arguments. The first start writes `eula.toml` next to the working directory with
`eula = false`, prints the notice below and exits without creating a world or
opening a port:

```text
Refrain needs your acceptance of the Minecraft End User License Agreement before it can start.
Read the agreement at https://www.minecraft.net/en-us/eula.
Acceptance file: 'eula.toml'.
The file was created with eula = false.
To accept the agreement and start the server, run:
  refrain-server --accept-eula
No world was created and no port was opened.
```

A vanilla `eula.txt` present when `eula.toml` does not exist yet is imported
once, so an existing installation keeps the decision it already recorded. After
that, `eula.toml` is the only acceptance file the server reads.

Accept the agreement and start:

```bash
refrain-server --accept-eula
```

After acceptance the server:

1. fetches the official Minecraft Java Edition 26.3 server archive named by
   [Mojang's version manifest](https://piston-meta.mojang.com/mc/game/version_manifest_v2.json)
   over HTTPS;
2. verifies the archive against the SHA-1 the manifest publishes;
3. unpacks only the datapack documents and the structure templates into the
   data directory and writes `data/MANIFEST.json`, which records the game
   version, the DataVersion, the archive SHA-1 and a content digest;
4. creates `world` and `config`, generates the spawn area and listens on
   `0.0.0.0:25565`.

Later starts reuse `data` without touching the network. No game asset is
committed, redistributed or bundled with the release archive: only the unpacked
data stays on the machine that fetched it.

The first start prints a one-screen summary at info level: the version it
serves, the world and seed, where the game data came from, the generated
configuration files, the listening address and the ready line with the console
hint.

### Offline and pre-staged starts

`--no-data-download` never touches the network. It starts from an existing data
directory: the default `./data`, or the directory named by `--data-dir`.

To stage the data directory for an offline host, either copy an existing
`data` directory, or generate one from a `server.jar` you own:

```bash
scripts/fetch-content.sh /path/to/server.jar
refrain-server --data-dir content/vanilla/26.3 --no-data-download
```

The data directory layout is the content snapshot layout:

```text
data/
  MANIFEST.json           game version, DataVersion, archive SHA-1, content digest
  data/                   the datapack documents world generation reads
    <namespace>/worldgen/**       biomes, features, structures, carvers, noise
    <namespace>/tags/block/**     block tags
    <namespace>/tags/worldgen/**  biome and structure tags
  templates/              structure templates, <namespace>/structure/**.nbt
  reports/                the climate, default-state and block light
                          reports, when copied
  tables.rvtb             tracked block, item, registry and light tables, when
                          staged
  protocol_registry.json  tracked protocol registry, when staged
```

A data directory is accepted when its marker names this game version and the
datapack is present, or when it is a pre-staged `data/` tree without a marker. A
marker for another game version is stale: it is replaced when downloads are
allowed and is a `version_mismatch` error otherwise. A directory with no marker
that carries the datapack is used as-is and reported as pre-staged.

The tracked tables and reports ship with the release archive under
`content/vanilla/26.3`; a data directory that only holds the downloaded datapack
receives the reports from there on first start. The tracked tables carry the
block light table, so the runtime warns at startup when the tables it loaded
carry no light data: every block state then falls back to opaque and emitting
nothing, and lighting stays dark until a table with the block light report is
staged.

The release archive layout is:

```text
refrain-<version>-<platform>/
  bin/refrain-server
  bin/refrain-tools
  gamepack/gamepack.toml
  gamepack/presets/
  content/vanilla/26.3/      tracked tables, reports and protocol registry
  content/vanilla/NOTICE.md
  run-server.sh
```

The server searches the working directory, the executable's directory and up to
three parent directories for `gamepack/gamepack.toml` and
`content/vanilla/<version>`, so it starts the same way from an extracted
archive, from `build/Release/bin` in a checkout and from an instance directory
that carries its own composition. The files are not embedded in the binary: the
composition and the tracked tables already ship as inspectable files with their
own notice, and embedding them would duplicate several MiB per binary. A
build-tree or installed start therefore needs those directories next to the
binary; `--gamepack` and `--data-dir` override the search.

## Defaults

Every default is printed at startup and can be overridden by the matching flag
or TOML file:

| Setting | Default | Override |
|---|---|---|
| World directory | `./world` | `--world`, `world.path` |
| Configuration directory | `./config` | `--config-dir` |
| Game data directory | `./data`, or the installed content snapshot when it already holds the datapack | `--data-dir` |
| Composition | `gamepack/gamepack.toml` in the working directory, next to the executable or in the checkout root above it | `--gamepack` |
| Listening address | `0.0.0.0:25565` | `--bind`, `--port`, `bind_host` and `port` in `network.toml` |
| Online mode | on | `online_mode` in `network.toml` |
| View distance | 10 | `view_distance` in `network.toml` |
| Simulation distance | 10 | `simulation_distance` in `network.toml` |
| Maximum players | 20 | `status.max_players` in `network.toml` |
| MOTD | `A Refrain Server` | `status.motd` in `network.toml` |
| Game mode, difficulty | the values stored in the world | in-game commands |
| Compression threshold | 256 | `compression_threshold` in `network.toml` |

The first start writes `config/refrain.toml`, `config/network.toml`,
`config/world.toml`, `config/runtime.toml`, `config/storage.toml`,
`config/mods.toml`, `config/mods_admin.toml` and `config/permissions.toml` with
these defaults; existing files are never rewritten. `--import-properties` skips
the generated files, because the imported properties are a fallback layer below
the operator's TOML files.

## Start

A fresh world is created from a seed, its spawn area is generated and the
server then listens. `--seed` only applies when the world is created; an
existing world keeps the seed it was created with.

```bash
refrain-server --seed 12345
```

Start an existing world from any directory:

```bash
refrain-server --world /path/to/world --ticks 200
```

The server enters the tick loop as soon as it starts. `--check` stops after the
composition, the mod lock and the content layers have been verified, without
creating a world or starting the loop, which is what a configuration check or a
CI smoke wants. A run that only inspects the composition does not need the EULA.
`--ticks 0` runs until `stop`, EOF on stdin or a scripted stop command.

## Inputs

| Option | Meaning |
|---|---|
| `--gamepack <file>` | composition file; by default discovered under `gamepack/gamepack.toml` |
| `--config-dir <dir>` | host config directory, created with the defaults on first start; each `*.toml` becomes a read-only runtime namespace (`refrain.toml` is `host`, `mods/*.toml` is `mod/<id>`); empty disables |
| `--world <dir>` | RWF world directory (default `world`, or `world.path`) |
| `--data-dir <dir>` | game data directory (default `data`, or the installed content snapshot) |
| `--no-data-download` | never fetch the server archive; start only from staged or cached data |
| `--data-manifest-url <url>` | version manifest used to locate the server archive (default Mojang's launcher manifest); point it at a mirror |
| `--accept-eula` | record EULA acceptance and start |
| `--eula-file <file>` | EULA acceptance file (default `eula.toml`) |
| `--bind <address>` | listening address override (default `0.0.0.0` from `network.toml`) |
| `--port <n>` | listening port override (default `25565` from `network.toml`) |
| `--check` | verify configuration, composition and lock, then exit without starting |
| `--import-properties <file>` | import Java `server.properties` as a fallback layer (not written) |
| `--ticks <n>` | stop after n ticks, 0 disables the limit |
| `--script <file>` | tick-indexed commands, one `<tick> <command>` per line |
| `--cmd-file <file>` | commands executed at tick 0 |
| `--hash-every <n>` | log the deterministic state hash every n ticks |
| `--log-level <level>` | console log level |
| `--watchdog-timeout-ms <n>` | warn when a tick stalls longer than n milliseconds |
| `--autosave-interval-ticks <n>` | flush dirty chunks every n ticks, 0 disables |
| `--compact-interval-ticks <n>` | compact region files every n ticks, 0 disables |
| `--content-store <dir>` | enable the content store and startup maintenance; installed mods in it are part of the composition |
| `--mods <dir>` | content store root whose install receipts are part of the mod catalog, repeatable |
| `--allow-downgrade` | accept a mod version lower than the installed highest (one run) |
| `--replace-same-version` | accept the same mod `(id, version)` with a different archive digest (one run) |
| `--keep-signer-pin` | keep a mod's signer pin after uninstalling its last version |
| `--signature-policy <mode>` | override the install signature policy: `strict` or `trusted-any` |
| `--content-trusted-key <id>:<hex>` | add a trusted signing key (repeatable) |

`--dump-gamepack` prints the resolved composition and exits.

External mods load from content-store install receipts only: `--content-store`
contributes the server's own store and `--mods` names additional store roots. A
`mods/<id>/` directory is a packaging source (`refrain-tools mods pack`), never
a load path. The install/trust policy is `mods.toml` in the configuration
directory; the flags above override individual values for one run. See
[Mods and gamepacks](modding.md#installing-mods).

The server logs the build's mod profile and build id at startup:

```
[main/info] [cli]: starting Refrain 0.1.0-alpha [main@abc1234] with mod profile vanilla (build 0123456789abcdef)
```

A gamepack or `mods.lock` that pins a mod this binary was not built with fails
before any world is opened, naming the profile and the missing mod. Rebuild
with a profile that contains the mod or start with a composition that does not
pin it; see [Mods and gamepacks](modding.md#build-profiles).

## Configuration

Configuration lives in one TOML file per namespace under `--config-dir`:

```text
config/
  refrain.toml      host namespace; [logging] level sets the console log level
  network.toml      bind_host, port, compression, view distance, status,
                    online mode, broadcast budgets
  world.toml        path, level_name, allow_nether
  runtime.toml      tick/watchdog/autosave knobs
  storage.toml      zstd and compaction policy
  mods.toml             mod install/trust policy: [signature], [trust],
                        [install] and [rotation."<id>"] (see docs/modding.md)
  mods_admin.toml       mod management: [lifecycle] hot gates, [admin]
                        operators allowed to run /mods
  permissions.toml      permission policy served by the built-in permission
                        service: [defaults], [op], [grants], [groups]
  providers/<id>.toml   implementation-specific options
  mods/<id>.toml        options of mods that do not provide a service
```

Precedence, high to low: CLI overrides, user TOML, imported properties, code
defaults. A missing file is an empty layer. The console log level comes from
`--log-level`; when the flag is absent it is read from `[logging].level` in
`refrain.toml` before the logger starts. `network.toml` example:

```toml
bind_host = "0.0.0.0"
port = 25565
compression_threshold = 256
online_mode = true
transport_instance = "asio_tcp"   # provider instance name
protocol_instance = "java_777"    # provider instance name
rate_limit = 0          # packets/second per play connection; 0 disables
player_idle_timeout = 0 # minutes; 0 disables

[status]
motd = "A Refrain Server"
max_players = 20

[broadcast]
info_bytes_per_tick = 1048576
queued_info_bytes = 33554432
visibility_pairs_per_tick = 100000
spawns_per_tick = 8000

[chunk_stream]
max_connection_backlog = 1048576
max_total_backlog = 67108864
```

`[broadcast]` bounds the work one tick spends on the player list and on entity
visibility. The byte counts are tick work budgets, not wire limits:
`info_bytes_per_tick` is the encoded player-list fan-out a tick hands to the
transport, and a cohort that does not fit stays queued, in order, for the next
tick. `queued_info_bytes` is the backlog valve: above it the whole queue drains
in one tick even though that overruns the per-tick budget, because an unbounded
queue is the worse failure. `visibility_pairs_per_tick` and `spawns_per_tick`
spread the visibility sweep over as many ticks as it needs without dropping a
transition. The values shown are the defaults. A value below one is raised to
one, and the effective values are logged at startup, because a zero budget
would stop the corresponding pass from making progress.

`[chunk_stream]` bounds what the chunk stream may queue. A connection whose
write queue passes `max_connection_backlog` is left alone for that tick, so a
client that stops reading slows only its own view and catches up when it
resumes; `max_total_backlog` pauses the whole stream, generation included, once
every client is behind. The transport has its own per-connection cap above both
values and closes a connection that passes it, so these two decide when the
stream yields first, not when a client is dropped. The values shown are the
defaults, a value below one is raised to one, and the effective values are
logged at startup.

`storage.toml` holds the zstd policy and maintenance cadence:

```toml
zstd_level = 3
zstd_near_level = 1
zstd_mid_level = 3
zstd_far_level = 6
zstd_near_radius = 2
zstd_mid_radius = 8
autosave_interval_ticks = 6000
compact_interval_ticks = 0
```

`runtime.toml` holds the watchdog and other tick-loop policy:

```toml
watchdog_timeout_ms = 5000
```

`permissions.toml` is the policy of the built-in permission service. A node is
a dot-separated lowercase path such as `refrain.command.teleport` or
`refrain.mods.admin`; a rule may end in `.*` to cover a namespace and may start
with `!` to deny, and a deny wins over every grant. Subjects are
`player:<uuid>` (a profile name also matches as a fallback), `entity:<uuid>` and
`plugin:<mod id>`; a grant may list `@<group>` to join a group declared in
`[groups]`.

```toml
[defaults]
default = "deny"        # or "op": an operator at or above op.min_level holds
                        # every node

[op]
min_level = 1
nodes = ["*"]           # nodes every operator holds

[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.command.*", "!refrain.command.stop"]

[groups]
moderators = ["refrain.mods.admin"]
```

The console always holds every node. Rules resolve in this order: console, a
matching deny, a matching grant, an operator's `[op].nodes`, then
`[defaults].default`. Without the file (or without a mounted `permissions`
namespace) the service serves the built-in policy instead of failing to start:
the console holds every node and every other subject is denied. The service
publishes a monotonic revision for caches and re-reads the file when the
permission commands ask it to; until those land, an edit takes effect on the
next start. The generated first-start file grants operators every node and
denies every other subject until `[grants]` names it; remove `[op].nodes` to
grant operators nothing implicitly either.

A CLI flag that names the same setting is an explicit override and wins over
both the TOML file and imported properties.

To migrate a Java `server.properties`:

```bash
build/Release/bin/refrain-tools config import-properties \
    --in server.properties --out-dir config
```

`--import-properties` on `refrain-server` only installs the values at runtime as
the fallback layer; it never rewrites the TOML files.

## Console

The server reads commands from stdin. The scripted replay format uses the same
commands with a leading tick number. Dirty configuration is saved on shutdown;
`config reload` changes the store but running subsystems keep their startup
snapshot until restart.

## In-game commands

The server announces a small command tree after joining:

| Command | Effect |
|---|---|
| `/help` | list the available commands |
| `/list` | show connected players |
| `/say <message>` | broadcast a message to every player |
| `/gamemode <mode>` | change the calling player's game mode |
| `/defaultgamemode <mode>` | persist the world's default game mode |
| `/difficulty <0-3>` | persist and broadcast the world difficulty |
| `/hardcore <on\|off>` | persist hardcore for new logins |
| `/mods <command>` | list, verify, install, uninstall, reload, reload content data or manage the trust state of mods (operator only; see [Mod management](#mod-management)) |

| Command | Effect |
|---|---|
| `stop` | stop after the current tick |
| `config names` | list mounted configuration namespaces |
| `config <ns> get <key>` | print one typed value |
| `config <ns> set <key> <value>` | update the user layer in memory |
| `config save [ns]` | persist dirty namespaces (all when omitted) |
| `config reload [ns]` | re-read files; running subsystems need a restart |
| `save` | flush pending snapshots and commit the world |
| `tps` | print rolling tick work avg/p99/max |
| `net` | print connection/packet/byte counters |
| `hash` | print the state hash |
| `pos` | print the player position |
| `move <dx> <dy> <dz>` | move the player by a delta |
| `walk <dx> <dz>` | move the player on the horizontal plane |
| `jump` | apply a jump impulse |
| `gamemode <mode> [player]` | change a player's game mode immediately |
| `heal [player]` | restore the player to full health |
| `food [player] <0-20>` | set the food level |
| `damage [player] <amount>` | apply one damage hit |
| `xp [player] <points>` | add experience points |
| `dig <x> <y> <z>` | remove a block and wear the first damageable inventory stack |
| `place <x> <y> <z> <block>` | place a block state |
| `give <item> <count>` | add items to the inventory |
| `inv` | print the inventory |
| `spawn <type> <x> <y> <z>` | spawn an entity |
| `entities` | list entities |
| `kill <entity-id>` | remove an entity |
| `time query` | print the game time and the daylight clock |
| `time set <ticks\|day\|noon\|night\|midnight>` | move the daylight clock |
| `time add <ticks>` | add ticks to the daylight clock |
| `time pause` / `time resume` | stop or resume the daylight clock |
| `weather clear\|rain\|thunder [duration]` | switch the weather for a number of ticks |
| `mods <command>` | list, verify, install, uninstall, reload, reload content data or manage the trust state of mods (see [Mod management](#mod-management)) |
| `palette-gc` | rebuild the world palette while the world is quiesced |
| `schedules` | list periodic maintenance tasks and their counters |
| `help` | list commands |

## Mod management

Operators manage installed mods from the console and from an op-only in-game
command. Both frontends call the same application service, so the status lines,
the error text and the audit entry are identical whichever one ran the command.

| Command | Effect |
|---|---|
| `mods list` | list the running composition: id, version, loader, whether code runs, where the mod came from and the services it provides |
| `mods installed` | list the install receipts of the content store |
| `mods verify <package.rfmod>` | verify a package (structure, digests, signature, trust) without installing it |
| `mods install <package.rfmod> confirm` | install a package into the content store |
| `mods uninstall <id>[@<version>] confirm` | remove one or every installed version of a mod |
| `mods reload <id>[@<version>]` | reload a mod that is part of the running composition |
| `mods reload-data` | rebuild the datapack content view (recipes, loot tables, advancements, worldgen documents, assets) without restarting |
| `mods trust` | show the install policy, the trust anchors and the recorded signer pins |
| `mods trust pin <id> confirm` | pin the signer recorded by the highest installed version |
| `mods trust clear <id> confirm` | clear a signer pin |

`install`, `uninstall` and the trust changes are destructive and need the
literal `confirm` argument; without it the command is refused, audited and
changes nothing. Every command accepts `--dry-run`, which reports what would
happen without writing store state.

The command surface reads the install/trust policy from the same
`config/mods.toml` the startup bootstrap uses, and its own switches from
`config/mods_admin.toml`:

```toml
[lifecycle]
# Apply an install or uninstall to the running composition while the server
# runs. Production defaults are off: the store change lands on the next start.
hot_install = false
# Rebuild a loaded mod in place.
hot_reload = false
# Rebuild the datapack content view (data and assets) in place.
hot_reload_data = false

[admin]
# Player uuids allowed to run /mods. The server console is always allowed.
operators = []
```

The policy file is the source of truth for every runtime install. One-run
command-line overrides such as `--content-trusted-key` or `--allow-downgrade`
apply to the startup bootstrap only; the console and in-game commands always use
`config/mods.toml`.

The in-game command is gated by the `refrain.mods.admin` permission node and is
only announced to an operator. A player without it is denied with a message
naming the node; the command tree is filtered per player so an operator's tree
is never shown to another player. There is no permission service yet, so the
operator list in `mods_admin.toml` is the whole policy.

Installing and uninstalling change the content store immediately. Whether the
running composition also changes is controlled by `hot_install`; with it off
(the production default) the command reports `restart required` and the change
lands on the next start. Reload needs `hot_reload`. A hot change is applied by
the lifecycle registry at a tick boundary; a build without that registry
answers `unsupported`, naming the missing layer, instead of pretending the
composition changed.

### Hot lifecycle

The server builds the lifecycle service from the composition it is running and
from the catalog the start resolved: the same content-store roots, install
receipts and effective auto-scan policy. A staged change therefore follows the
same rules as the next start would, and the running composition only changes
when those rules resolve the mod into it. With `auto_scan` off and the mod not
listed by the gamepack, the install command keeps the receipt for the next
start and refuses with the reason instead of joining the running composition;
enable auto-scan or list the mod.

A staged change is published at a tick boundary. The registry switch is atomic
and no package or store IO happens there: reading and verifying the package,
opening a library and building the replacement providers all happen on the
command path before the change is staged. Until that boundary the running
composition keeps calling the current generation.

- `hot_install = true` makes `mods install` and `mods uninstall` stage their
  running-composition half after the store transaction. The store change is
  durable either way; a staging refusal leaves the receipt installed for the
  next start instead of rolling the store back.
- `hot_reload = true` makes `mods reload <id>` rebuild the active generation as
  the next one. After the commit, handles from the previous generation fail as
  stale; a call that started before the switch is drained for up to five
  seconds before the loader's unload hook runs.
- With a gate off nothing is staged. `mods install`/`mods uninstall` still
  change the content store and report `restart required`; `mods reload` is
  refused with the gate named. The change lands on the next start.

A successful reload or uninstall does not disconnect players and does not
unload chunks: the world keeps ticking with the same entities, chunks and
simulation state. What changes from the next tick on is which provider
implementations the tick loop and the other mods reach. Content and registry
data a mod contributed were read at start; rebuilding its code generation does
not re-read them. A native library is never unloaded while the process runs, so
reloaded code accumulates retired mappings (eight generations by default before
a further reload is refused).

Not every mod can be hot-switched. A change is refused with the reason named
when the mod runs statically linked code or provides a core service the running
process owns (`refrain:sim`, `refrain:world`, `refrain:storage`,
`refrain:net.protocol` or `refrain:runtime`). A mod that another active mod
still requires cannot be uninstalled. The refusal is audited, and the store
change stays durable for the next start.

Every operation writes one line through the `mod-audit` logger with the actor
(console, or the player uuid), the action, the mod id and version, the package
digest, the signer key id, the policy in force and the result. Refused and
failed operations are audited too:

```text
[mod-audit/info] console ran install on example.mod 1.0.0 from archive <blake3> signed by key 'first-party' under the strict policy, the result was ok after 4ms
```

### Content reload

The datapack content the server reads is one re-buildable content view: the
published content composition (when `--content-store` is used) with every
`--datapack` and `--dev` directory layered above it, i.e. the same stack the
next start would mount. `mods reload-data` rebuilds that stack on a background
thread, loads and validates it, and publishes the result at the next tick
boundary. The command returns immediately: it queues the rebuild, reports the
generation that stays live for the rest of the tick and never makes the tick
wait for a build.

The gate is `[lifecycle] hot_reload_data` in `config/mods_admin.toml`, off by
default. With the gate off the command is refused and names the file; with the
gate on a runtime that has no content view answers `unsupported` and names the
missing layer.

A reload replaces the content layer: recipes, loot tables, advancements,
worldgen documents, structure templates, tags and the file/asset layer every
layer-aware consumer reads. Readers take the live view once per tick and never
hold it across a tick boundary; the previous view is released when the last
reader drops its snapshot, so a tick that started on the old view finishes on
it.

A reload does not replace:

- the block, item and entity registries. The server freezes them when it
  starts and existing chunks keep the palette generation they were written
  with. A pack that adds, removes or edits a registry document
  (`data/<namespace>/block/...`, `data/<namespace>/item/...`,
  `data/<namespace>/entity_type/...`, or a shipped `tables.rvtb` /
  `protocol_registry.json`) is refused with `restart required`, naming the
  changed files, and the live view stays as it is;
- mod code and the mod composition. That is `mods reload <id>` under
  `hot_reload`, not `mods reload-data`;
- the chunk generator's own data snapshot. Chunk generation owns generator
  workers started from the game-data directory at boot, so changed worldgen
  documents become generation input after a restart in this build.

A failed rebuild (a malformed document, a missing layer, a validation error) is
rolled back: the live generation stays and the error names the layer and the
path. A reload does not disconnect players and does not unload chunks; a player
standing in generated terrain sees no change. Every successful publish logs one
line through the `content` logger:

```text
[content/info] published content view generation 2 with 2 content layers, 412 files, 18 recipes, 9 loot tables, 4 advancements, 121 worldgen documents, 0 structures and 36 tags
```

## Player state

The server owns its players' positions, health, hunger, experience and death
state; a client's movement packets are validated against the world and rejected
when they walk through a block, report a speed a walking or jumping player
cannot reach, or jump higher than the game allows. A rejected request is
answered with the authoritative position, so the client snaps back instead of
keeping the impossible position.

Falling hurts, drowning and the void hurt, hunger drains while sprinting and
regenerates health while saturation and food are high, and zero health shows the
death screen. The respawn request is accepted only from a dead player and
returns it to the world spawn point with full health and food. `/gamemode`
switches the rules on the next tick: creative takes no damage and may fly,
spectator passes through blocks, adventure and spectator cannot break or place
blocks.

A replay script exercises a deterministic path:

```text
0 place 0 64 0 minecraft:dirt
5 hash
10 stop
```

```bash
build/Release/bin/refrain-server \
    --gamepack gamepack/gamepack.toml \
    --world /path/to/world \
    --script replay.txt \
    --hash-every 5 \
    --ticks 20
```

Two runs over the same world and script produce the same hash for the same
tick.

## Client

The server speaks protocol 777 (Minecraft Java Edition 26.3), so an unmodified
client joins with the address the server prints on startup
(`[net]: listening on ...`). Set `online_mode = false` in `network.toml` to
accept any username; enable it when the server is reachable from outside the
machine.

The client is accepted when the following hold on a fresh join:

- the join screen leaves `Loading terrain` and the player stands on the
  generated terrain at the world spawn;
- the tab list shows the player, and a second client sees the first one move;
- opening the inventory shows the hotbar and the 27 main slots, and a
  left-click moves a stack between a slot and the cursor;
- closing the inventory with a stack on the cursor puts the stack back into
  the first free inventory slot.

The server owns the viewport: it streams `view_distance` chunks around the
player and tells the client to unload the chunks that leave that square, so a
client with a larger render distance still sees only the server viewport.

## World conversion

```bash
build/Release/bin/refrain-tools world import --from vanilla-world --to rwf-world
build/Release/bin/refrain-tools world export --from rwf-world --to vanilla-world
build/Release/bin/refrain-tools world info rwf-world
build/Release/bin/refrain-tools world fsck rwf-world
build/Release/bin/refrain-tools world diff world-a world-b
```

Import keeps chunk contents, block palette keys, block entities, biomes and
light. It also accepts a world saved by an older data version inside the
supported migration window (see [World data version
migration](#world-data-version-migration)): every record is fixed for its type
before it is converted, and the resulting RWF world records the current data
version. The import line reports the source data version, the number of fix
steps, how many records changed and the resulting data version. A world from
outside the window, a record whose version is newer than the current one, or a
corrupt record fails closed; the source world is never modified.

`--dry-run` validates the source the same way and prints the plan without
creating the target:

```bash
build/Release/bin/refrain-tools world import --from vanilla-world --to rwf-world --dry-run
```

Export writes the vanilla Anvil layout expected by the matching DataVersion.

`world palette-gc` rewrites a closed world into a sibling directory with a
compacted palette and swaps it in; the source world is left untouched if the
rebuild fails.

## World data version migration

A world directory records the game data version its records were written with.
This build reads one data version: a world saved by an older build is upgraded
before it is opened, and a world from a version outside the supported window is
refused with the recorded version and the window named in the error.

Anvil import and the RWF open path share the same built-in DataFixer: the fixes
for a record type are implemented once and applied by whichever path reads the
record, so importing an old Anvil world and opening an equivalent older RWF
world produce the same records. The window is a property of the fix table, not
of one storage format.

The upgrade runs automatically at startup through the built-in
`refrain.world.datafix` mod. It rewrites the version-sensitive records, then
stamps the new data version into the level record and the saved-data headers. A
world already at the current version is opened unchanged, and a second start
does no work. `refrain-tools world info` prints the recorded `data_version`, so
an operator can check a world before and after an upgrade.

The supported source window is data version 5013 through 5023: the two
reference schema changes registered after 5013 (the removed block-transformer
item component and the renamed buried-treasure map item name) are applied, and
no reference schema change exists between 5016 and 5023.

- **Back up the world directory before the first start of a build that
  upgrades it.** The upgrade rewrites records in place and there is no
  automatic rollback to the older version; a directory copy or a filesystem
  snapshot is the rollback path.
- The version stamp is written last. If the upgrade fails, the world stays
  recorded at its old version and the next start refuses it instead of serving
  half-upgraded records; the start that failed can simply be retried, because
  every record rewrite is idempotent.
- Upgrading from a version older than the supported window is refused, for
  both an Anvil import and an RWF open. The same applies to a world in a legacy
  storage layout the importer does not read yet, such as a pre-1.13 `.mcr`
  world or a pre-1.18 section layout. Open the world once in Minecraft 26.3 (or
  a newer build) to bring it forward, then import or start it with Refrain
  again. A world newer than this build is refused as well; do not copy a world
  back from a newer build.
- A corrupt record fails the whole operation: the import does not write the
  target world's metadata, so an interrupted or failed import is never opened
  as a half-upgraded world, and the source directory is left untouched.
- The migration is a mod. A custom gamepack that does not load it refuses an
  older world and leaves it untouched; only the current data version opens.

## Crash reporting

`--crash-dir <dir>` enables local minidump capture. Crash dumps are written to
that directory and remain local.

## See also

- [Building](building.md) for the toolchain, presets and build options behind
  the binaries used in this guide.
- [World generation data](worldgen.md) for the datapack and structure templates
  a generated world reads.
- [Mods and gamepacks](modding.md) for the composition `--gamepack` selects and
  the configuration namespaces mods read.
