# Running a server

`refrain-server` runs a world headless for a fixed number of ticks or until the
console receives `stop`. It listens for protocol 777 clients using the
`network.toml` settings.

## Start

```bash
build/Release/bin/refrain-server \
    --gamepack gamepack/gamepack.toml \
    --world /path/to/world \
    --config-dir config \
    --ticks 200
```

`--world` is required to enter the tick loop. Without it the server validates
the composition and exits. `--ticks 0` runs until `stop`, EOF on stdin or a
scripted stop command.

## Inputs

| Option | Meaning |
|---|---|
| `--gamepack <file>` | composition file; defaults to the bundled gamepack |
| `--world <dir>` | RWF world directory |
| `--config-dir <dir>` | per-namespace TOML configuration; missing files keep defaults |
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

| Command | Effect |
|---|---|
| `stop` | stop after the current tick |
| `config names` | list mounted configuration namespaces |
| `config <ns> get <key>` | print one typed value |
| `config <ns> set <key> <value>` | update the user layer in memory |
| `config save [ns]` | persist dirty namespaces (all when omitted) |
| `config reload [ns]` | re-read files; running subsystems need a restart |
| `save` | flush pending snapshots and commit the world |
| `tps` | print tick timing |
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
