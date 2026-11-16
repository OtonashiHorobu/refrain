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
| Worldgen | `include/refrain/worldgen`, `src/worldgen` | datapack documents, biome sources, terrain, carvers, features, structures and templates |
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

### Composition epochs

The active composition is not frozen for the process lifetime. Sealed services
(`refrain:host.*` and the mod loader bootstrap) are sealed permanently, while an
ordinary service can be replaced in epochs: a new provider generation is staged
as a candidate, activated without publishing it, and then committed atomically
at the next tick boundary. Each `(service, instance)` slot carries a
monotonically increasing generation; callers cache a generation-stamped handle
for the tick and a handle from a retired generation fails with `stale` instead
of reaching code that is being unloaded. In-process callers read that handle
from the registry; a mod reads it from the sealed `refrain:host.core` table
through `query_service_handle`, and `validate_handle` answers whether the
cached generation is still committed.

Staging is the IO half (read and verify the package, open a shared library,
build the provider table) and runs on the command/installation thread. The
commit is the tick half: it swaps the staged slots, advances the epoch, drains
in-flight calls with a bounded wait and runs the old generations' unload hooks.
A failed staging attempt rolls back to the previous epoch. Native libraries are
never unloaded from the process; retired mappings are counted per mod and
capped, after which a reload requires a restart.

The composition epoch and a digest over its mod/provider generations are part
of the audit record, so a run with hot changes can be distinguished from a
static composition with the same mods.

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
  Movement is a request: the runtime validates it against the simulation's
  authoritative record and sends the position back when it is rejected.
- `chunk_tracker`: viewport changes and pending chunk subscription requests.

Player records live where the active world port keeps them. The first-party
composition runs the simulation in this process, so `play_session` forwards
every read and write to the simulation's player table without a copy. A
composition reached through the contracts keeps the table inside the provider:
the session's store holds a value projection refreshed by one bulk read per
tick (`player_count()` plus the indexed accessor) and turns the mutation calls
into the provider's connection-addressed player calls. Either way a packet's
connection lookup is answered from the session's view, not by a provider call
per packet, and the runtime never holds a pointer into a provider's table
across calls.

The network shell reports neutral events (`player_join`, `player_leave`,
`movement`, `player_input`, `flight`, `teleport_confirm`, `attack`,
`animation`, `play_ready`, `view_distance`) and provides a raw packet sink plus
the gameplay packet senders (teleport, health, experience, death screen, entity
events). It does not decide which entities exist, who can see them, how much
health a player has, or when a chunk view changes.

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

A world records the game data version its records were written with. The
storage layer reads and writes one version only; upgrading a world recorded
with an older version is a separate provider over the
`refrain:world.migration` contract, implemented by the bundled migration mod
and run by the runtime before the store opens. The migration rewrites the
version-sensitive records and stamps the new version last, so an interrupted
upgrade leaves the old version recorded and is retried by the next start. A
composition that omits the provider refuses an older world instead of reading
it with the wrong schema.

## Simulation

The simulation owns the loaded chunks, tickets, block registry, entity table and
inventory state. A tick advances player movement, entities, RNG and timers in a
fixed order. The tick loop runs at 20 Hz and derives a state hash from the
world contents and simulation state.

Determinism is a contract: the same world and the same command sequence produce
the same hash at the same tick. Timers are ordered by due tick and insertion
id. The event bus is single-threaded and preserves subscription order.

### Player physics and survival

The simulation owns each player's authoritative position, velocity, health,
hunger, air, experience and death state; the session projection and the
console read that one record. A client movement packet is a request, not a
position: `world::submit_player_move()` accepts it only when it is finite,
within the speed envelope, reachable through the block collision sweep (Y,
then X, then Z, with step-up over partial shapes) and not higher than a jump
can reach above the last ground contact. The runtime answers a rejected
request with a teleport back to the authoritative position, and the client's
acknowledgement of that teleport is adopted without a second validation. A
provider-owned simulation keeps its own movement rules; the runtime then only
projects the reported position for visibility and chunk streaming.

Each tick advances the record the same way on the ground, in air, in water and
in lava: input-driven acceleration, gravity, friction, a collision sweep, fall
distance tracking, and the landing damage. Survival state then ticks in a fixed
order: fall damage, movement exhaustion, drowning, void and suffocation damage,
hunger and natural regeneration or starvation. Creative and spectator players
take no damage, spectators ignore collision, and creative and spectator flight
is honoured only while the game mode allows it. Damage opens a short immunity
window, records its source and the attacking connection, and at zero health
moves the record into the dead state: the runtime sends the death screen and
the system chat message, and a respawn request is accepted only while the
record is dead. Respawning restores health, hunger, air and position at the
world spawn point; the level record supplies the spawn point, difficulty and
hardcore flag at boot.

Block collision is derived from the block-state key rather than a full
`VoxelShape` table, so the sweep understands full cubes, slabs, snow layers and
carpets; fences, walls and stairs collide as full blocks. That approximation
can over-restrict a move but never lets a player through a solid block, and it
is the one place where collision differs from the reference implementation.

### Lighting

Chunks carry sky and block light. The simulation owns one light engine over
the loaded columns: a column is lit when it joins the resident set (loaded from
the store or adopted from the generator), light propagates across column and
section borders, and a block edit updates light incrementally before the tick
that sends the chunk packet and the update packets runs. The light arrays live
in the column's storage record, so the chunk codec, the background writer and
the `level_chunk_with_light` encoder read one copy. The engine keeps a
deterministic order (ascending column position, then ascending section) for
every propagation and every drained change, and a column's light does not
depend on the order the generator delivered its neighbours.

Block light emission and sky dampening come from a curated block table inside
the engine: the shipped content tables carry block states but no light
properties. The table matches the reference for solid terrain, fluids, leaves
and the common sources; blocks outside it are opaque and emit nothing, so a
scene can only be darker than the reference, never brighter.

### Time and weather

The overworld clock advances on the tick grid: the game time counts every
server tick, the daylight clock follows the clock rate (paused when a command
stops it) and needs 24000 ticks for one day-night cycle. Once per second the
runtime broadcasts `set_time` with the game time and the clock state, which is
what lets the client interpolate the cycle between syncs; a joining client
receives the same snapshot in its login burst. Weather follows the reference
state machine: rain and thunder timers resample from the reference's uniform
ranges, the rain and thunder levels ease by 0.01 per tick and the runtime
broadcasts the level changes as game events. Clock and weather are persisted
into the world's saved data on flush and restored on the next open.

### Chunk residency

The chunk pipeline pins what the players can see. A column is resident while it
is inside at least one connection's viewport or holds a ticket (the spawn area
and any forced load); every other loaded column is unloaded at the end of the
tick. Unloading writes a dirty column to the store first and only then drops
it, so a later viewport that covers the column reads it back from the region
file instead of handing it to the generator again. The store index is published
at the normal commit points (autosave, save, shutdown), so the tick never waits
for an fsync. A clean column needs no store traffic at all and is dropped
immediately; at most a small number of dirty columns are written per tick, so a
burst of generated columns cannot turn into a long tick.

The bound is the union of the active viewport squares plus the ticket set. It
follows the view distances, so walking away from an area releases its memory
instead of growing the resident set for as long as the process lives. A viewport
pin covers columns that do not exist yet, which is what keeps a generated column
resident when it arrives.

The pass walks resident columns in ascending position order and unloads in that
order. `state_hash()` covers the loaded columns, so a replay has to unload the
same set at the same ticks; an order taken from a hash map or from task
completion would make the hash depend on thread timing.

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

## See also

- [Schema compiler](schema.md) for the RSC and RIDL sources behind the contracts.
- [Mods and gamepacks](modding.md) for the composition model and the host
  runtime API.
- [World generation data](worldgen.md) for the data the worldgen layer reads
  and how its output is verified.
