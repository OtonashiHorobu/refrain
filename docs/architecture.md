# Architecture

Refrain is a server-first implementation of the Minecraft Java Edition 26.3
protocol and world model. The code is layered so that platform code, contract
definitions and gameplay modules can evolve independently.

## Layers

| Layer | Path | Responsibility |
|---|---|---|
| Platform | `include/refrain/platform`, `src/platform` | files, memory mapping, threads, processes, dynamic loading |
| Kernel | `include/refrain/core`, `include/refrain/kernel`, `src/core`, `src/kernel` | errors, logging, time, memory, jobs, events, configuration, I/O, async, service registry, crash reporting |
| Contracts | `sdk/idl`, `sdk/generated` | RIDL service contracts, generated C ABI and C++ wrappers |
| Data | `include/refrain/data`, `src/data` | datapacks, registries, resource store, content-addressed store, cook artifacts |
| World | `include/refrain/world`, `src/world` | RWF world storage, chunk codec, Anvil import and export |
| Simulation | `include/refrain/sim`, `src/sim` | runtime world, block registry, entities, inventory, tick loop, replay |
| Mods | `include/refrain/mod`, `src/mod` | manifests, gamepack composition, discovery, resolution, loading, provider registry, static mod registry, startup content |
| Session | `include/refrain/session`, `src/session` | player table, entity visibility/movement, chunk subscriptions |
| Server runtime | `include/refrain/server`, `src/server` | headless tick loop, console, watchdog, per-namespace TOML configuration |
| Application | `include/refrain/app`, `src/app`, `src/main` | composition root, service layer shared by the CLIs, CLI entry points |
| Tools | `tools`, `src/main/tools.cpp` | schema compiler, world conversion, packaging, diagnostics |

A module may include the public headers of the layers below it. The platform
layer has no dependency on the kernel; the kernel has no dependency on data,
world or simulation code.

## Contracts

Services are declared in RIDL under `sdk/idl/`. The schema compiler emits a C
ABI header, a C++ wrapper header and reflection metadata into `sdk/generated/`.
Every service carries a version, side, thread affinity, composition mode and a
set of provided service keys.

A provider implements the C vtable and registers the service key. Consumers
receive a type-safe C++ wrapper that checks the ABI version and copies struct
sizes at the boundary. Structs crossing the boundary start with a `struct_size`
field; slices are borrowed views valid for the duration of a call.

Calls return `refrain::status` or `refrain::result`; error codes are part of the
contract. Exceptions never cross a contract boundary.

The composition mode of a service decides how many providers may be active:
`exclusive` (one), `instanced` (one per instance name), `chain` (wrappers that
forward selectively) and `registry` (contributions merged into a registry).
`sealed` is the host-owned mode: the process bootstrap registers the only
provider before mod loading, and a gamepack or mod cannot select or replace it.
The sealed host services (`refrain:host.core`, `refrain:host.platform`,
`refrain:host.kernel` and `refrain:host.metadata`) are the stable runtime API
offered to mods.

## Composition

A gamepack selects one provider per service and lists the mods to load. Each
mod carries a `manifest.toml` with its identity, dependencies and provided
services. The loader resolves the dependency graph, orders providers, checks
versions and ABI compatibility, then activates the composition.

First-party mods are linked into the server binary and register through the
static mod registry. Their data, assets and scripts travel in a deterministic
RFSI image embedded in the binary. External packages use the same manifest
model and are installed into the content store.

## Network

The network stack is split along three contracts:

- `refrain:transport` moves bytes. Providers declare reliability, ordering,
  message orientation, MTU and backpressure. The bundled provider is TCP.
- `refrain:net.protocol` turns bytes into packet ids and payloads. A provider
  declares an edition/family descriptor, canonical and accepted versions,
  required transport capabilities, framing/compression/encryption and the
  optional `tab_codec` extension. Packet semantics and the connection state
  machine stay in the network session shell; protocol providers do not own
  world or player state.
- `refrain:net.session` resolves offline and online player profiles. Online
  mode uses it for the `hasJoined` check; offline mode does not require a
  provider.

Session presentation and observation are optional feature services, never
network code:

- `refrain:session.status` supplies server-list description, online and max
  counts.
- `refrain:session.tab` supplies tab-list header and footer.
- `refrain:session.lifecycle` receives player join/leave and per-tick notifications.

The runtime enumerates every active instance in deterministic order. An empty
service list keeps the static `network.toml` status, sends no tab packet and
dispatches no lifecycle events.

The stable C++ facade for the current wire version is
`include/refrain/protocol/current.hpp`. Session and server code include this
facade; only protocol providers and client tooling name a concrete version.

### Session layer

`refrain::session` owns gameplay-side player and entity state:

- `play_session`: connection-scoped player records, entity id allocation,
  movement/health/game-mode updates, per-player inventory slots, visibility.
- `chunk_tracker`: viewport changes and pending chunk subscription requests.

The network shell reports neutral events (`player_join`, `player_leave`,
`movement`, `attack`, `animation`, `play_ready`, `view_distance`) and provides a
raw packet sink. It does not decide which entities exist, who can see them, how
much health a player has, or when a chunk view changes.

## World storage

RWF v1 is the native world format. A world directory holds a header, world
metadata, a world-level block palette, dimension dictionaries, region files and
auxiliary object stores.

Region files use a 4 KiB header, two 36 KiB index pages written alternately, and
an append-only data area of compressed records. A record holds the chunk
sections, block entities, biomes and light. The world palette maps canonical
block-state keys to compact identifiers; it is append-only and compacted only
by an offline rebuild.

Anvil import and export convert between RWF and the vanilla region layout,
including zstd and LZ4 compressed regions, block palette keys, block entities,
biomes and light. The conversion is lossless for the fields the format carries.

## Simulation

The simulation owns the loaded chunks, tickets, block registry, entity table and
inventory state. A tick advances player movement, entities, RNG and timers in a
fixed order. The tick loop runs at 20 Hz and derives a state hash from the
world contents and simulation state.

Determinism is a contract: the same world and the same command sequence produce
the same hash at the same tick. Timers are ordered by due tick and insertion
id. The event bus is single-threaded and preserves subscription order.

### Generation failures

A generation task that fails reports the columns of its tile. The pipeline
forgets them so the streamer requests them again, and a column gets a fixed
number of attempts before it is left unserved for that viewport visit. That
keeps a permanently failing position from being requested once per tick, and it
means a client is never left waiting on an answer that will not arrive. A worker
whose generator cannot be created fails its tasks the same way instead of
exiting, so the queue drains and every affected column is reported.

### Tick budget

The simulation thread bounds the work a tick can take from state that arrives
asynchronously. Three budgets keep a login storm from turning into a
second-long tick:

- the network shell delivers at most 64 join events per tick and carries the
  remaining events to later ticks in queue order;
- the broadcaster queues the player-list updates of a tick, encodes one body
  per cohort, and hands at most 1 MiB to the transport per tick; the rest
  follows in order on later ticks. A body that goes to several connections is
  framed once by the transport, and each connection still takes its own
  ciphertext;
- the visibility pass re-evaluates only the players whose chunk, view distance
  or presence changed since the previous tick, in both directions, and stops
  at a pair budget and a spawn budget, so a dense fill spreads over several
  ticks. A full reconcile of every viewer runs every 600 ticks (thirty seconds)
  as the safety net for a pair whose distance test changed while both players
  stayed inside their chunks;
- the movement fan-out walks a reverse tracking index, so a mover is encoded
  once and appended only to the viewers that actually track it.

A player that waits a few ticks for its tab-list entry or its entity spawn
cannot observe the delay; a tick that applies the whole backlog can.

## Threading

The server uses a small fixed set of threads: the main thread owns startup and
the console, the simulation thread owns the tick loop and the world, and I/O
threads perform encoding, writes and content deletion. Jobs are dispatched
through the kernel job system. Ownership of a world or content store is never
shared between the simulation thread and an I/O thread without an explicit
handoff.

## Error handling

Functions return `refrain::result<T>` or `refrain::status`. Errors carry a code,
a message and the source location of the failing check. Allocation failures and
library exceptions are caught at the boundary where a library is called and
converted to error values.
