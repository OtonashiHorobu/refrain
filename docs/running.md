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
4. creates `world`, `config`, `logs` and `crash`, generates the spawn area
   and listens on `0.0.0.0:25565`.

Later starts reuse `data` without touching the network. No game asset is
committed, redistributed or bundled with the release archive: only the unpacked
data stays on the machine that fetched it.

### Where the game data comes from

The datapack and the tracked artifacts (the table container, the climate
reports and the protocol registry) resolve separately. Each lookup uses the
first source that provides it:

1. the data directory named by `--data-dir`, or `./data` when it holds a valid
   marker or a staged datapack;
2. the content snapshot shipped next to the binary
   (`content/vanilla/<version>/`), which is how the release archive starts
   without a first-run fetch;
3. the official server archive, downloaded and verified on the first accepted
   start, unless `--no-data-download` disables the network;
4. generation from an operator-owned server archive with `--server-jar`, when
   a source checkout provides `tools/content/generate.sh` and its toolchain.

A world cannot be generated or served without the tracked table container,
the climate reports and the protocol registry. When no source provides them,
startup stops before creating or opening a world with one message that names
the missing file and the command that generates it. It never starts with an
empty block light table or a missing climate report.

The first start prints a one-screen summary at info level: the version it
serves, the world and seed, where the game data came from, the generated
configuration files, the listening address and the ready line with the console
hint.

### Offline and pre-staged starts

`--no-data-download` never touches the network. It starts from an existing data
directory, from the packaged content snapshot, or from data the server
generates from a local archive.

To stage the data directory for an offline host, either copy an existing
`data` directory, or generate one from a `server.jar` you own in a source
checkout:

```bash
tools/content/generate.sh --server-jar /path/to/server.jar \
    --out content/vanilla/26.3
refrain-server --data-dir content/vanilla/26.3 --no-data-download
```

The generation entry point runs the vanilla data generator and the light,
mining and climate probes through the existing tools, so it needs a Java
runtime (25 or newer for 26.3), Python 3, `unzip` and the `refrain-tools`
binary of the same build, and it reuses the protocol registry tracked in the
checkout. It writes `data/`, `reports/`, `tables.rvtb` and
`protocol_registry.json`. `scripts/fetch-content.sh` remains available when
only the datapack is needed.

When `--server-jar` names an archive and the tracked data is missing, the
server runs the same generation on the first start; a first-run download also
feeds it when the install ships no content snapshot. A package that ships
neither the content snapshot nor the source tooling therefore fails before the
world is created, naming the artifact to install.

The data directory layout is the content snapshot layout:

```text
data/
  MANIFEST.json           game version, DataVersion, archive SHA-1, content digest
  data/                   the datapack documents world generation reads
    <namespace>/worldgen/**       biomes, features, structures, carvers, noise
    <namespace>/tags/block/**     block tags
    <namespace>/tags/worldgen/**  biome and structure tags
  templates/              structure templates, <namespace>/structure/**.nbt
  reports/                the climate, default-state, block light and block
                          mining reports
  tables.rvtb             tracked block, item, registry, light, mining and tool
                          tables
  protocol_registry.json  tracked protocol registry
```

A data directory is accepted when its marker names this game version and the
datapack is present, or when it is a pre-staged `data/` tree without a marker. A
marker for another game version is stale: it is replaced when downloads are
allowed and is a `version_mismatch` error otherwise. A directory with no marker
that carries the datapack is used as-is and reported as pre-staged.

The tracked tables, reports and protocol registry ship with the release
archive under `content/vanilla/26.3`; a data directory that only holds the
downloaded datapack receives the reports from there on first start. The tracked
tables carry the block light table, so the runtime warns at startup when the
tables it loaded carry no light data: every block state then falls back to
opaque and emitting nothing, and lighting stays dark until a table with the
block light report is staged. A start instead refuses to proceed until the
tracked artifacts resolve, so a missing light table or climate report cannot
silently reach a generated world.

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

The archive carries the tracked data but not the generation tooling: an
incomplete package, or a binary moved away from its archive, fails closed and
names what is missing instead of starting half-configured. Unpack the archive
as a whole so the composition and the content snapshot stay next to the
binaries.

`logs/` and `crash/` are not part of the archive: the first start creates both
next to the process working directory. The release archives for Windows carry
`crashpad_handler.exe` beside `refrain-server.exe` when the package was built
with Crashpad (the MSVC package); the MinGW package uses the server's
Windows-native minidump fallback instead.

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
| Server archive for generation | none; a first-run download feeds it | `--server-jar` |
| Composition | `gamepack/gamepack.toml` in the working directory, next to the executable or in the checkout root above it | `--gamepack` |
| Listening address | `0.0.0.0:25565` | `--bind`, `--port`, `bind_host` and `port` in `network.toml` |
| Online mode | on | `online_mode` in `network.toml` |
| View distance | 10 | `view_distance` in `network.toml` |
| Simulation distance | 10 | `simulation_distance` in `network.toml` |
| Maximum players | 20 | `status.max_players` in `network.toml` |
| MOTD | `A Refrain Server` | `status.motd` in `network.toml` |
| Game mode, difficulty | the values stored in the world | in-game commands |
| Compression threshold | 256 | `compression_threshold` in `network.toml` |
| Log file | `./logs/refrain.log` (rotating, 16 MiB, 4 files) | none; the file sink is always attached |
| Crash database | `./crash` | `--crash-dir` |

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
`--ticks 0` runs until `stop` or a scripted stop command; end of file on stdin
only ends console input and does not stop the tick loop.

## Inputs

| Option | Meaning |
|---|---|
| `--gamepack <file>` | composition file; by default discovered under `gamepack/gamepack.toml` |
| `--config-dir <dir>` | host config directory, created with the defaults on first start; each `*.toml` becomes a read-only runtime namespace (`refrain.toml` is `host`, `mods/*.toml` is `mod/<id>`); empty disables |
| `--world <dir>` | RWF world directory (default `world`, or `world.path`) |
| `--data-dir <dir>` | game data directory (default `data`, or the installed content snapshot) |
| `--no-data-download` | never fetch the server archive; start only from staged, cached or generated data |
| `--server-jar <file>` | your own 26.3 server archive; generates missing tables and reports |
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
| `--log-level <level>` | console and file log level |
| `--crash-dir <dir>` | local crash database directory (default: `crash`) |
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
  mods_admin.toml       mod management: [lifecycle] hot gates
  permissions.toml      permission policy served by the built-in permission
                        service: [defaults], [op], [ops], [grants], [groups]
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
view_distance = 10          # 2..32; also sent to the client as chunk radius
simulation_distance = 10    # 2..32
transport_instance = "asio_tcp"   # provider instance name
protocol_instance = "java_777"    # provider instance name
rate_limit = 500        # packets/second per play connection; 0 disables
player_idle_timeout = 0 # minutes; 0 disables
login_timeout_ms = 30000
max_connections = 28    # default: [status].max_players + 8
pre_play_buffer_bytes = 65536
max_inbound_packets = 256
max_inbound_bytes = 1048576
max_inbound_packets_total = 8192
max_inbound_bytes_total = 8388608

[status]
enabled = true          # false: close status handshakes without a response
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

`view_distance` and `simulation_distance` must be between 2 and 32; any other
value stops startup with an error instead of being narrowed into a different
setting. `max_connections` caps the sockets the server accepts at once and
defaults to `[status].max_players` plus eight slots for the status list,
reconnects and the login burst; a connection above the cap is closed before it
gets a session. `login_timeout_ms` is the deadline for a connection to reach
play: handshake, status, login and configuration share it, so an
unauthenticated peer cannot hold a slot, a descriptor and a receive buffer
forever. `pre_play_buffer_bytes` bounds what one unauthenticated connection may
leave in its frame decoder; an ordinary handshake, status or login packet is a
few hundred bytes, so the default is far below the 2 MiB frame ceiling.
`[status].enabled` controls whether the server answers the server list ping:
when it is false a status handshake is closed without a response, so the MOTD,
version and player count are not disclosed. `[status].max_players` must be
between 0 and 1,000,000 and `auth_timeout_ms` between 1 and 60,000; a value
outside those ranges stops startup instead of being narrowed into a different
setting.

The play queue limits bound the work a tick has to catch up on.
`max_inbound_packets` and `max_inbound_bytes` apply per connection,
`max_inbound_packets_total` and `max_inbound_bytes_total` apply to every
connection together. A play packet that would pass either budget closes the
connection that sent it and is never queued, so a client flooding a server
whose tick is stalled cannot grow memory without bound. `rate_limit` is the
per-connection packet ceiling the token bucket enforces while a client is in
play; it defaults to 500 packets per second, well above the traffic of a
normal client, and 0 disables it. `online_mode` also bounds session
verification: the request runs on a small worker pool with a bounded queue
rather than on the network I/O thread, and one client address may start at most
eight login attempts per minute.

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
every client is behind. The transport holds 8 MiB of undelivered frames for one
connection and 64 MiB for all connections together, and closes a connection
whose queue would pass either ceiling; the per-connection cap is above both
stream values, so these two decide when the stream yields first, not when a
client is dropped. The values shown are the
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
a dot-separated lowercase path such as `refrain.command.gamemode` or
`refrain.mods.admin`; a rule may end in `.*` to cover a namespace and may start
with `!` to deny, and a deny wins over every grant. Subjects are
`player:<uuid>` (a profile name also matches as a fallback and warns at load),
`entity:<uuid>` and `plugin:<mod id>`; a grant may list `@<group>` to join a
group declared in `[groups]`.

Profile names are mutable. Online mode identifies an account by its profile
uuid, and a player who renames releases the old name for another account to
claim. A name-keyed entry (`player:<profile name>` in `[grants]` or a profile
name in `[ops].players`) follows the name, so a rename can transfer the grant
or the operator status to the account that claims the old name.

The resolver keeps the name fallback for compatibility and loading the policy
writes one warning for each name-keyed entry, naming the entry and recommending
the uuid form. Prefer the canonical lowercase profile uuid in online mode. The
join log line `player joined with id <uuid> as <name>` prints the profile uuid
of a joining player, and `permissions grant <name> <node>` resolves an online
name to that uuid before it writes the entry.

```toml
[defaults]
default = "deny"        # or "op": an operator holds every node
# Nodes every subject holds. The public command surface of the server.
nodes = ["refrain.command.help", "refrain.command.list",
         "refrain.command.me", "refrain.command.msg"]

[op]
# Vanilla ladder level that counts as an operator. It is reserved for a
# composition that supplies a real op level in the query.
min_level = 1
nodes = ["*"]           # nodes every operator holds

[ops]
# Players treated as operators, by canonical lowercase profile uuid. A profile
# name is accepted for compatibility and warns at load. The server console
# always holds every node.
players = []

[grants]
"player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["refrain.command.*", "!refrain.command.stop"]

[groups]
moderators = ["refrain.mods.admin"]

[contexts]
# A named predicate over the query context: it applies only when every entry
# matches. `world` and `gamemode` address those context fields; any other key
# matches one extra key/value pair a caller supplied.
creative = { gamemode = "creative" }
overworld = { world = "minecraft:overworld" }

[context_grants]
# Rules attached to a subject while one context applies. The subject key has
# the same shape as in `[grants]`, and `@group` joins a global group.
creative = { "player:069a79f4-44e9-4726-a5be-fca90e38aaf5" = ["!refrain.command.gamemode"] }
```

Context rules are additive. Every `[contexts]` predicate that matches the
query applies, and its `[context_grants]` rule set joins the subject's own rule
sets in the same deny-wins order: a matching deny in any applicable set
(global, group, operator, public default or context) denies, otherwise a
matching grant allows, otherwise `[defaults].default` decides. A context rule
therefore narrows or extends one subject's access for that context but cannot
override a deny. Group definitions stay global and single-level; a context rule
can join one with `@name`, and that membership, together with the group's rules,
applies only while the context matches. When several contexts match they are
scanned in name order, and `permissions check` names the first matching deny,
or the first matching grant, that decided.

A query context carries the player's world, its game mode and any extra
key/value pairs the caller supplies. The command bridge fills the game mode;
this server leaves the world key empty because the session does not track a
dimension, so a `world` predicate matches only a context a caller supplies
explicitly. A context with no `[contexts]` rule never changes an answer, and a
policy without `[contexts]`/`[context_grants]` answers the same way for every
context. A malformed context is rejected and the query is denied instead of
being matched: an oversized or empty key, a reserved key in the extra list or
a duplicate extra key all fail closed, so a caller that cannot supply a
well-formed context does not get a match.

The console always holds every node. Rules resolve in this order: console, a
matching deny, a matching grant, an operator's `[op].nodes` (an operator is a
player in `[ops].players` or at `[op].min_level` on the ladder), a grant in
`[defaults].nodes`, then `[defaults].default`. The rule sets of every matching
context are folded into the same order, as described above.

The generated first-start policy leaves the public command surface
(`refrain.command.help`, `refrain.command.list`, `refrain.command.me`,
`refrain.command.msg`) open to every player, grants `[ops].players` every node,
and denies everything else. A fresh install therefore announces and runs
`/help`, `/list`, `/me` and `/msg` for everyone; `/say`, the world and player
commands and `/mods` need an `[ops].players` entry or an explicit `[grants]`
rule. Remove an entry from `[defaults].nodes` to hide that command from
players, and add a node there to make another command public.

The old `config/mods_admin.toml` operator list is not imported: the file keeps
only its `[lifecycle]` gates. Move its entries into `[ops].players` to keep
them operators, or into a `[grants]` entry to allow one player one node. A stale
`[admin]` table is ignored with a warning at startup.

A composition without the `refrain:permissions` provider (for example a
gamepack that does not select `refrain.permissions`) keeps the vanilla
op-level semantics: every command node declares a fallback level next to its
node, and a player at or above that level may use the command.

`refrain:permissions` is a chain, and a gamepack may name wrapper providers in
front of the built-in resolver:

```toml
[providers]
permissions = { base = "refrain.permissions@^1.0.0", wrappers = ["com.example.permissions@^1.0.0"] }
```

A wrapper answers `allowed` or `denied` for the nodes it owns and
`not_handled` for every other node, which forwards the query to the next
provider until the base resolver answers. The wrappers are listed from the
innermost to the outermost, so the outermost wrapper sees the query first. A
wrapper can deny a node the base would grant and grant a node the base would
deny, which is how an external policy extends the built-in one. The served
revision is the base revision plus every wrapper revision, so a change in a
wrapper invalidates the command tree exactly like a policy change, and
`permissions reload` reloads the base and every wrapper. A wrapper the
gamepack names but the host did not register fails composition, so the running
server always has a complete chain. A running composition cannot change a chain
link yet: a staged wrapper reload or uninstall is refused because the wrapper's
provider slot is the default instance the base resolver occupies, so changing
the wrapper set is a restart-level change. The provider does refresh its link
set when the committed composition epoch changes and drops a wrapper the
registry no longer holds; that path keeps the served chain consistent once a
host can publish such a change, and the declared wrapper list is read on the
first answer after startup.

The service publishes a monotonic revision for caches. The administration
commands below re-read the file or change one rule at runtime, and every policy
change advances the revision, which invalidates the command-tree cache and makes
the server resend the tree to every player in play without a restart.

### Permission administration

The console line `permissions ...` and the in-game `/permissions` tree share one
implementation, so both frontends print the same lines, report the same errors
and write the same audit records. Every in-game node requires the
`refrain.permissions.admin` permission and falls back to the admin level in a
composition without the service; the console holds every node and can always run
the commands.

| Command | Effect |
|---|---|
| `permissions check <subject> <node>` | Answer allowed, denied or not-handled for one subject and node, name the deciding rule set (console, grant, deny, group, context, operator, public default or fallback) and print the player's world and game mode context; a node a chain wrapper decided is reported as such with the base resolver's own answer |
| `permissions grant <subject> <node>` | Add one rule to the subject's `[grants]` entry, write the configuration file and report the new revision |
| `permissions revoke <subject> <node>` | Remove one rule from the subject's `[grants]` entry, write the configuration file and report the new revision |
| `permissions groups` | List every `[groups]` entry with its rules and the subjects that joined it; a membership contributed by `[context_grants]` is listed with its context |
| `permissions reload` | Re-read `config/permissions.toml` and every chain wrapper, and report the old and new revision |

A `<subject>` is `console`, a player name, or one configured subject key:
`player:<uuid>`, `player:<profile name>`, `entity:<uuid>` or `plugin:<mod id>`.
A name resolves to the canonical account uuid: first through a live session,
then, on a server running with online mode disabled, through the deterministic
offline profile the login would create. A `player:<profile name>` key is
canonicalised the same way, so a change follows the account instead of the
mutable name; a name that resolves through the offline profile does not
authenticate anything, it only names the account a change targets. `grant`
refuses a subject whose name does not resolve to an account, so a new rule never
targets a mutable profile name; `revoke` still accepts the literal
`player:<profile name>` key, and prefers it when the policy holds one, so a
legacy name-keyed rule can be removed. An
online-mode server cannot map a name to an account without the account, so a
player who is not online is addressed by `player:<uuid>` there, and an unknown
bare name is an error. A `<node>` is a rule: a concrete node, a namespace
wildcard such as `refrain.command.*`, a bare `*`, or one of those prefixed with
`!` for an explicit deny. Granting a rule that is already present and revoking
one that is absent report the unchanged revision and write nothing.

A change edits the loaded configuration file and replaces it atomically before
it becomes visible to queries. Comments, table order and every unrelated entry
stay byte-for-byte intact; a document that expresses its grants as root-level
dotted keys is refused rather than rewritten, and the running policy then stays
unchanged. A provider built without a mounted `permissions` namespace serves the
built-in policy and refuses changes with `unsupported`, so the in-memory policy
and the durable file can never diverge. A failed write also leaves the running
policy and the revision untouched.

The configuration store mounted the namespace at startup and keeps its own
in-memory copy. The administration commands read and write the file directly, so
do not edit the same file through `config <ns> set` / `config save` while using
them; `permissions reload` re-reads whatever is on disk, and `config reload
permissions` refreshes the store copy after an administration change.

Every changing command appends one audit record through the
`permissions-audit` logger: the actor (console or player name and uuid), the
action, the subject, the node, the result and the resulting revision. Read-only
`check` and `groups` are not audited. The record never contains a filesystem
path. Write access to `config/permissions.toml` is therefore equivalent to
permission administration: keep it operator-owned, and remember that a `!`
deny for `refrain.permissions.admin` removes a player's access to this command
tree even if the command itself was granted earlier.

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
| `/time <...>`, `/weather <...>` | read and change the overworld clock and weather |
| `/tp <x y z>`, `/tp <target>`, `/tp <targets> <x y z>`, `/tp <targets> <destination>` | move the source or the selected entities (also `/teleport`); every target can be a bare player name or an entity selector |
| `/setblock <x y z> <block> [replace\|keep]` | place one block |
| `/fill <from> <to> <block> [replace\|keep\|hollow\|outline]` | edit a block region |
| `/clone <from> <to> <destination> [replace\|masked]` | copy a block region |
| `/give <targets> <item> [count]` | add items to the inventory of every selected player |
| `/me <action>` | broadcast an emote |
| `/msg <targets> <message>` | whisper to every selected player (also `/tell` and `/w`) |
| `/save-all [flush]` | flush dirty chunks and the clock state to the store |
| `/mods <command>` | list, verify, install, uninstall, reload, reload content data or manage the trust state of mods (requires `refrain.mods.admin`; see [Mod management](#mod-management)) |
| `/permissions <command>` | check, grant, revoke, list groups or reload the permission policy (requires `refrain.permissions.admin`; see [Permission administration](#permission-administration)) |

Entity selectors (`@p`, `@a`, `@r`, `@s`, `@e`, `@n`) and the
`limit`/`sort`/`distance`/`x`/`y`/`z`/`dx`/`dy`/`dz`/`name`/`gamemode`/`type`
filters are accepted wherever a command takes a target in the table above.
Selectors resolve against the players in play; a filter the server does not
model is rejected instead of ignored. The draw behind `@r` and `sort=random`
is derived from the command source, the world tick and the command text, so a
replay reproduces it. The full grammar and the v1 limits are in
[Commands](commands.md#entity-selectors).

Every in-game command declares a stable permission node. The policy decides who
holds it; the fallback level is what a composition without the permission
service compares a player's vanilla op level against. The console ignores the
nodes and may run every console command.

| Command | Permission node | Fallback level |
|---|---|---|
| `/help` | `refrain.command.help` | everyone |
| `/list` | `refrain.command.list` | everyone |
| `/say <message>` | `refrain.command.say` | gamemaster |
| `/gamemode <mode>` | `refrain.command.gamemode` | gamemaster |
| `/defaultgamemode <mode>` | `refrain.command.defaultgamemode` | gamemaster |
| `/difficulty <0-3>` | `refrain.command.difficulty` | gamemaster |
| `/time <...>` | `refrain.command.time` | gamemaster |
| `/weather <...>` | `refrain.command.weather` | gamemaster |
| `/hardcore <on\|off>` | `refrain.command.hardcore` | admin |
| `/tp <...>`, `/teleport <...>` | `refrain.command.teleport` | gamemaster |
| `/setblock <...>` | `refrain.command.setblock` | gamemaster |
| `/fill <...>` | `refrain.command.fill` | gamemaster |
| `/clone <...>` | `refrain.command.clone` | gamemaster |
| `/give <...>` | `refrain.command.give` | gamemaster |
| `/me <action>` | `refrain.command.me` | everyone |
| `/msg <targets> <message>`, `/tell`, `/w` | `refrain.command.msg` | everyone |
| `/save-all [flush]` | `refrain.command.save-all` | owner |
| `/mods <command>` | `refrain.mods.admin` | admin |
| `/permissions <command>` | `refrain.permissions.admin` | admin |

The tree snapshot and the completion list omit a node the player's subject may
not use, and the same check rejects the command at execution, so the three
paths agree. A denied command answers with the missing node instead of a
generic parse error.

The console reads the same commands without a permission check; its table:

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
| `permissions <command>` | check, grant, revoke, list groups or reload the permission policy (see [Permission administration](#permission-administration)) |
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
```

The policy file is the source of truth for every runtime install. One-run
command-line overrides such as `--content-trusted-key` or `--allow-downgrade`
apply to the startup bootstrap only; the console and in-game commands always use
`config/mods.toml`.

The in-game command is gated by the `refrain.mods.admin` permission node and is
only announced to a subject that holds it. A player without it is denied with a
message naming the node; the command tree is filtered per player, and the
completion list follows the same filter, so one player's tree is never shown to
another. The server console holds every node and keeps its unconditional
`mods` console command.

Who holds the node is decided in `config/permissions.toml`: list the player in
`[ops].players` to make them an operator, or grant the node to one player in
`[grants]`. The old `[admin] operators` list of `config/mods_admin.toml` is not
read; move its entries to `[ops].players` (or a `[grants]` rule) when upgrading.
A composition without the permission service falls back to the command's
declared admin level, so a provider-less development build keeps the previous
behaviour.

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
layer-aware consumer reads. Readers take the live snapshot once per tick and
never hold it across a tick boundary; the snapshot keeps the view and its
registry generation alive, so the previous view is released when the last
reader drops it and a tick that started on the old view finishes on it.

Chunk generation follows the published generation. A tile worker composes the
published view over the installed game-data directory when a publish advances
the generation, so a worldgen document, structure template or tag document the
view changes is generation input for the chunks produced after the publish.
Tag documents merge with the datapack rules for every tag kind: the installed
document holds the lowest-priority values, each view layer appends its values,
and a layer with `"replace": true` clears everything below it. A biome tag
therefore keeps the installed values a feature predicate or structure match
needs unless the view replaces it. A generator captures the generation it was
built from; the worker swaps the generator between tiles, so the previous
generator finishes the tile it is producing (up to `tile_size` squared chunks,
64 by default), queued tiles are served by the newest generation, and chunks
already written keep their content. A view that changes nothing chunk
generation reads costs no copy and produces byte-identical chunks, which is the
ordinary recipe or loot reload. The merged tree of a generation is a temporary
directory removed when the last generator that captured it is destroyed; a
leftover tree from a crashed process is swept when a process first materializes
a content generation.

Registry data follows the same rule with two boundaries:

- A change to block, item or entity registry documents
  (`data/<namespace>/block/...`, `data/<namespace>/item/...`,
  `data/<namespace>/entity_type/...`) is published as a new immutable registry
  generation together with the view. Its entry ids are assigned per registry
  kind in content-key order and every entry records its document digest, so the
  mapping is a pure function of the content and a snapshot keeps the generation
  it started with. The mapping is a data-layer identity: chunk generation
  resolves block names on demand from the reloadable worldgen documents and
  tag documents, and a stored palette keeps canonical block-state keys rather
  than numeric ids, so no remap is needed and no registry document change is
  generation-observable by itself. The commit names the changed class and says
  that generated chunk content changes only through worldgen and tag
  documents. A registry document path that cannot define an entry is refused as
  restart-required instead of publishing an unchanged mapping;
- A change to a registry container (`tables.rvtb` or
  `protocol_registry.json`) is refused with `restart required`, naming the
  changed files. The server froze the numeric identities of those tables and
  the wire registry catalog at startup, and a connected client received the old
  catalog at login. A pack that changes both classes is refused as one unit
  rather than applying the document half.

The spawn prefill of a fresh world runs before the first publish, so it takes
the composition-root boot view and binds the same generation the tile workers
will stream from: a spawn chunk and a streamed chunk of the same tile are
produced from one generation.

A reload does not replace mod code or the mod composition; that is
`mods reload <id>` under `hot_reload`, not `mods reload-data`.

The experimental per-chunk scheduler keeps its boot content after a reload: it
runs one frozen shared block-state table whose ids every worker generator has
already resolved, so it cannot be rebound safely. A composition that selects
that path also declares that it cannot bind registry generations, and a
registry-document reload is then refused as restart-required with the scheduler
named instead of reporting a publish that nothing rebinds. The tile path is the
production path and is rebound.

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

Survival mining is server-authoritative. The client reports the selected hotbar
slot and the start, stop and abort block actions; the server accumulates the
destroy progress from the block's destroy time and the held tool, shows the
destroy-stage animation to nearby players, writes air when the progress
completes and damages the tool. An incorrect tool mines slower, a block that
needs a tool tier leaves its break progress without the tool bonus, and
creative breaks immediately. Broken blocks do not drop items yet, and neither
placement rules nor neighbour updates are implemented.

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

## Logging and crash reporting

Every start attaches two log sinks at the level selected by `--log-level` or
`[logging].level`: the console (stderr) and a rotating file at
`logs/refrain.log`. The file sink keeps four files of 16 MiB each and rotates
the oldest out. The first start creates `logs/`, and a start where it cannot be
created continues with console logging only and says why; startup never fails
because of a diagnostic sink.

Crash reporting is also on by default. The local crash database is `crash/`
under the working directory; `--crash-dir <dir>` overrides the location. Crash
dumps and their metadata stay local. When the build enables Crashpad, the
server resolves `crashpad_handler` from its own directory (the release archive
ships it beside the server) and starts the handler as a child process. When
Crashpad is not attached, the platform fault handler is installed instead: it
writes `crash_marker.txt`, and on Windows it also writes a native `.dmp`
minidump through dbghelp. The server log says which reporter is active and
where the database lives.

## See also

- [Building](building.md) for the toolchain, presets and build options behind
  the binaries used in this guide.
- [World generation data](worldgen.md) for the datapack and structure templates
  a generated world reads.
- [Mods and gamepacks](modding.md) for the composition `--gamepack` selects and
  the configuration namespaces mods read.
