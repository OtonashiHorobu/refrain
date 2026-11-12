# Running a server

`refrain-server` runs a world headless for a fixed number of ticks or until the
console receives `stop`. It listens for protocol 777 clients using the
`network.toml` settings.

## Start

A fresh install starts with no world: the server creates one from a seed,
generates the spawn area and then listens.

```bash
build/Release/bin/refrain-server --seed 12345
```

`--seed` only applies when the world is created; an existing world keeps the
seed it was created with. The world directory defaults to `world` (override
with `--world` or `world.path` in the configuration), and an existing world is
opened as is.

```bash
build/Release/bin/refrain-server \
    --gamepack gamepack/gamepack.toml \
    --world /path/to/world \
    --config-dir config \
    --ticks 200
```

The server enters the tick loop as soon as it starts: it creates the world
directory when it does not exist yet. `--check` stops after the composition, the
mod lock and the content layers have been verified, without creating a world or
starting the loop, which is what a configuration check or a CI smoke wants.
`--ticks 0` runs until `stop`, EOF on stdin or a scripted stop command.

## Inputs

| Option | Meaning |
|---|---|
| `--gamepack <file>` | composition file; defaults to the bundled gamepack |
| `--config-dir <dir>` | host config directory; each `*.toml` becomes a read-only host runtime namespace (`refrain.toml` is `host`, `mods/*.toml` is `mod/<id>`); empty disables |
| `--world <dir>` | RWF world directory |
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
| `--content-store <dir>` | enable the content store and startup maintenance |
| `--mods <dir>` | instance root with `mods/<id>/manifest.toml`, repeatable |

`--dump-gamepack` prints the resolved composition and exits.

## Configuration

Configuration lives in one TOML file per namespace under `--config-dir`:

```text
config/
  network.toml      bind_host, port, compression, view distance, status, online mode
  world.toml        path, level_name, allow_nether
  runtime.toml      tick/watchdog/autosave knobs
  storage.toml      zstd and compaction policy
  providers/<id>.toml   implementation-specific options
  mods/<id>.toml        options of mods that do not provide a service
```

Precedence, high to low: CLI overrides, user TOML, imported properties, code
defaults. A missing file is an empty layer; `network.toml` example:

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
```

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
| `dig <x> <y> <z>` | remove a block |
| `place <x> <y> <z> <block>` | place a block state |
| `give <item> <count>` | add items to the inventory |
| `inv` | print the inventory |
| `spawn <type> <x> <y> <z>` | spawn an entity |
| `entities` | list entities |
| `kill <entity-id>` | remove an entity |
| `palette-gc` | rebuild the world palette while the world is quiesced |
| `schedules` | list periodic maintenance tasks and their counters |
| `help` | list commands |

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

The server speaks the 26.3 protocol (777), so an unmodified client joins with
the address the server prints on startup (`network: listening on ...`). Set
`online_mode = false` in `network.toml` to accept any username; enable it when
the server is reachable from outside the machine.

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
light. Export writes the vanilla Anvil layout expected by the matching
DataVersion.

`world palette-gc` rewrites a closed world into a sibling directory with a
compacted palette and swaps it in; the source world is left untouched if the
rebuild fails.

## Crash reporting

`--crash-dir <dir>` enables local minidump capture. Crash dumps are written to
that directory and remain local.
