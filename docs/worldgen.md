# World generation data

Refrain contains no game assets or datapack. World generation reads two kinds of
data that come from the Minecraft version the server is built for: the
datapack documents that drive biomes, features, structures and carvers,
and the structure templates that provide the prebuilt pieces.

## Datapack

The datapack documents that drive biomes, features, structures and carvers are
not part of the repository and are not shipped with the binary. On the first
start after the EULA is accepted, the server fetches the official server archive
of the target version from Mojang's version manifest, verifies its SHA-1 and
unpacks the worldgen documents, the block and biome tags and the structure
templates into the data directory (`./data` by default, see
[Running a server](running.md#first-run)). The used game data is then:

```text
data/
  MANIFEST.json   game version, DataVersion, archive SHA-1, content digest
  data/           the datapack `data` tree the generator reads
  templates/      structure templates, <namespace>/structure/**.nbt
  reports/        the tracked reports copied next to the datapack
```

The server archive is a bundler jar: the outer archive carries metadata and the
real server jar under `META-INF/versions/<version>/server-<version>.jar`, and the
`data/` tree lives in that inner jar. `refrain-tools extract` and the first-run
downloader both unwrap it, so a plain jar whose root holds `data/` also works.

To stage the data offline, generate it from a `server.jar` you own:

```bash
scripts/fetch-content.sh /path/to/server.jar
```

The script extracts the data entries with `refrain-tools extract` and packs the
worldgen documents and tags with `tools/worldgen/pack_content.py`. It writes
`content/vanilla/<version>/{data,MANIFEST.json}`; pass `--version` or
`--data-version` when they differ from the defaults. Point the server at that
directory with `--data-dir content/vanilla/<version> --no-data-download`, or let
it read the snapshot when it is installed next to the binary. Both the snapshot
and a fetched data directory are game-derived data covered by the notice in
`content/vanilla/`, are not part of the repository license and are not tracked
by git.

## Biome parameter reports

The overworld and nether biome selection tables are game data. The derived
reports ship under `content/vanilla/<version>/reports/biome_parameters/`:

```text
reports/biome_parameters/
  MANIFEST.json   game/data version, entry counts, sha256 of each report
  overworld.json  overworld climate parameter points, in selection order
  nether.json     nether climate parameter points, in selection order
```

Each report records the game version, data version, dimension id, entry count
and the ordered entries. An entry carries the biome id, the quantized minimum
and maximum of the six climate parameters, and the quantized offset. Worldgen
loads a report with `load_biome_parameters(data_root, dimension)`, where
`data_root` is the sibling `data` directory. The loader rejects a report whose
metadata, entry count, dimension or entries do not decode exactly, so a table
cannot silently drift. The reports are covered by the notice in
`content/vanilla/`.

## Structure templates

Structure templates are binary assets and are not part of the
repository. Extract them from the server jar the operator owns into a
cache directory:

```bash
python3 tools/worldgen/extract_templates.py \
    --server-jar /path/to/server.jar \
    --out ~/.cache/refrain/templates
```

The output uses the layout the template manager expects:

```
<root>/<namespace>/structure/<path>.nbt
```

`TEMPLATES.json` records the game version, template count and content
digest. Extract once per game version; the directory can be regenerated
at any time from the jar.

## Consuming the data

The worldgen layer loads the datapack from a data root and
templates from the extracted directory. Tests and oracles take both
locations from the environment so the same code paths run against the
files an installation would use.

## Block default states

A datapack names a plain block when a feature places the block's default
state, and a reference chunk palette writes that default state as the bare
block name. The property values of the default are not part of the datapack,
so the repository ships a derived report:
`reports/block_defaults/defaults.json` maps every block whose default state
carries properties to those values, and its `MANIFEST.json` records the game
version, data version, size and digest. `tools/worldgen/block_defaults.py`
reduces the data generator's `blocks.json` report to that document.

The generator loads the report when it is installed. A bare state key then
answers the state-dependent questions the reference asks its default state:
the simple-block feature places the second half of a double plant, and shape
families read the default properties a state key omits. Without the report the
generator keeps the spelling-only behaviour, so an older datapack still loads.

## Block light report

Light behaviour is block code, not a datapack document: which level a state
emits, how much a path loses when it enters the state, and which face shapes
the lighting engine uses for shape-based occlusion. The repository ships the
derived report under `content/vanilla/<version>/reports/block_light/` and
compiles it into the tracked table container:

```text
reports/block_light/
  light.json      per-state emission, dampening and occlusion shape references
  MANIFEST.json   game/data version, counts, report digest, server jar digest
```

`light.json` lists one record per block state - the game's dense state id, the
emitted light level, the light levels a path loses when it enters the state,
the two occlusion flags, and an index into the six face shapes - plus the face
box lists of every occlusion shape. A face is the shape the reference
lighting engine slices at the cell boundary for one direction, so the engine
can test whether two face shapes together cover a cell without modelling a
voxel shape.

Regenerate the report from the server jar you own, after the data generator
wrote its reports:

```bash
tools/light/extract.sh /path/to/server.jar generated/reports
```

The script unpacks the bundler jar, runs `tools/light/BlockLightProbe.java`
against the game's block registry and packs the result with
`tools/light/pack_report.py`; `pack_report.py --check
content/vanilla/<version>/reports/block_light` verifies the tracked copy
against its manifest without a jar. Import the tracked tables with the report
next to the data generator output:

```bash
build/Release/bin/refrain-tools vanilla import \
    -r generated/reports -o /tmp/refrain-tables --force
cp /tmp/refrain-tables/tables.rvtb content/vanilla/<version>/
cp -r generated/reports/block_light content/vanilla/<version>/reports/
```

The runtime reads the per-state records from the table container. A state the
table does not carry - a modded state, or a container imported without the
report - falls back to the documented default of opaque and emitting nothing;
it is never guessed from a block name. The report and the container are covered
by the notice in `content/vanilla/`.

## Block mining report

Mining behaviour is block and item code, not a datapack document: the destroy
time a state caches, whether a correct tool is required for drops, which tool
families mine a block efficiently, the harvest tier it needs, and the rule
list of every item's tool component. The repository ships the derived report
under `content/vanilla/<version>/reports/block_mining/` and compiles it into
the tracked table container:

```text
reports/block_mining/
  mining.json     per-block mining records, per-tool item records and rules
  MANIFEST.json   game/data version, counts, report digest, server jar digest
```

`mining.json` lists one row per block - the destroy time, the
correct-tool-for-drops flag, the tool families that mine it efficiently, the
harvest tier its drops need and the shears and sword speeds - and one record
per item that carries a tool component: its family, material, tier speed,
damage per block, stack limit, durability and ordered rules. The report also
carries the block tags those rules reference, and it compresses the rule list
into one tier speed per family and one per-block speed for shears and swords;
the probe re-evaluates the game's own speed and drop calls for every tool and
block pair and aborts if the compressed answer differs from the game.

Regenerate the report from the server jar you own, after the data generator
wrote its reports:

```bash
tools/block/extract.sh /path/to/server.jar generated/reports
```

The script unpacks the bundler jar, binds the vanilla datapack tags and item
components of the jar, runs `tools/block/MiningProbe.java` against the block
and item registries and packs the result with `tools/block/pack_report.py`;
`pack_report.py --check content/vanilla/<version>/reports/block_mining`
verifies the tracked copy against its manifest without a jar. Import the
tracked tables with the report next to the data generator output, as for the
block light report:

```bash
build/Release/bin/refrain-tools vanilla import \
    -r generated/reports -o /tmp/refrain-tables --force
cp /tmp/refrain-tables/tables.rvtb content/vanilla/<version>/
cp -r generated/reports/block_mining content/vanilla/<version>/reports/
```

The container carries one mining record per block and one tool record per item
with a tool component. A block or item the table does not carry - a modded
block, or a container imported without the report - falls back to the
documented default: the block is unbreakable and the item has no tool
component. The fallback is never guessed from a name. The report and the
container are covered by the notice in `content/vanilla/`.

## Dimensions

`dimension_profile_for(dimension_kind)` supplies the fixed parameters of the
overworld, the nether and the end: the level bounds, sea level, coordinate
scale, noise-settings path, material-rule path and whether the dimension has a
carver stage. The generator reads the noise settings the profile names and uses
their default block, default fluid and generation geometry, so no dimension
literal lives in the generation code.

The nether and the end have 128-block noise settings while their level is 256
blocks tall. Fill and surface therefore run over y 0..127 and the level above
can only be air; heightmaps and the stored chunk still cover the whole
256-block level. Their settings also select the legacy positional random
source, which the loader takes from the document rather than the caller.

The nether runs the carvers its biomes declare: the `nether_cave` document is a
cave carver with the nether generation bounds, a floor level and a large start
vertical radius. The end declares no carvers, so its pipeline has none. The
carver stage runs after the surface pass over the generation depth, not over the
full level height, and applies the resulting mask to the filled chunk.

The biome source is dimension-specific: the overworld and the nether select a
biome from their climate parameter report (`reports/biome_parameters/`), and
the end selects from its island bands. A fixed-biome source backs reference
worlds whose preset declares a single biome.

The structure generator runs starts in every dimension: the nether generates
fortresses, bastion remnants and fossils, and the end generates end cities,
next to the overworld structure types. A generator evaluates only the structure
sets whose structures can appear with one of the biomes its source produces,
which is the filter the reference applies when it loads a world's structure
sets. The filtered list keeps the data set's own order, and a set stays whole
when one of its entries is possible, so a weighted draw still consumes the same
entries in the same order. A set that names only other dimensions' biomes is
never scanned: its starts could not pass their biome check anyway, and a start
candidate pays for the height and block probes its generation performs before
that check. The loot tables a structure's containers
carry are built-in constants of the target version rather than datapack fields:
the 26.3 structure, template and processor documents name none for the buried
treasure, igloo, jungle temple, ocean ruin, shipwreck, desert pyramid,
stronghold, nether fortress and end city pieces, so the placers use the same
ids the reference piece classes name and consume the piece seed at the same
point. The feature families work the same way: the bonus chest names the spawn
bonus chest table and the monster room the simple dungeon table, both built-in
constants. A template container tag takes its seed from the piece stream during
placement, a template data-marker pass draws the seed the world keeps, while a
processor rule that appends a loot table draws the seed from the rule's own
positional stream. Reference chunks store both the table and the seed, so the
container oracles compare them directly. A structure start reads the
dimension's noise settings through the same profile the terrain stages use.
Height providers sample over the generator's generation depth, which is shorter
than the level for the nether and the end, while jigsaw padding and distance
limits use the level bounds.

## Full-chunk generation

`worldgen::chunk_generator` is the production entry point that runs every
stage in one reusable object. `chunk_generator_config` names the content data
root, the extracted template root, the seed and the dimension kind. A fixed
biome key selects one biome everywhere; an empty key uses the dimension's biome
source. `generation_target` stops at `terrain` (biomes, fill, surface,
carvers), at `features` (terrain plus structure starts, references, placed
pieces and feature decoration), or at `full` (the features target plus the
final status). `structure_sets` restricts starts to the listed sets for oracle
runs, and an empty list enables every set. An empty template root still
generates procedural pieces and features; template-backed pieces stay empty.
Three reference-only switches reproduce a saved reference chunk that contains
its neighbours' feature writes: `decorate_neighbours` decorates the neighbour
chunks through the same region, `neighbours_before_target` selects whether
their writes come before or after the target's own features, and
`neighbours_in_generation_order` follows the order a force-load uses: every
chunk that precedes the target in ascending x and z decorates first, then the
target, then the rest. A mixed order matters for any feature that writes across
a border, such as an ore vein or a vegetation patch. Every decorated chunk
builds its own candidate-biome plan from the palettes of its three-by-three, so
the mode works with the dimension's biome source as well as with a fixed
biome.

`neighbour_radius` widens the decorated neighbourhood: zero decorates only the
target, one the eight surrounding chunks (the default), and each further step
adds a ring. The runtime attaches one more terrain ring than it decorates,
because a decorated chunk reads the chunks its own generation region covers,
and it confines every decoration's writes to that chunk's eight neighbours, as
the reference generation region does. `neighbour_box` names the reference
world's generated chunk rectangle in absolute chunk coordinates: the whole
rectangle decorates in force-load order and chunks outside it stay terrain
only. A feature that spreads across more than one border needs the rectangle
form; a sculk patch is the example, because its charge cursors move up to
twelve blocks from the patch origin.

`neighbours_ticket_sweep` replaces the flat ascending order with the order a
force-load ticket queue runs. A forced chunk keeps its own two rings at the
full status, so the generation task of every force-loaded chunk runs the
feature layer of the three-by-three around it; the tickets propagated two
rings out are full chunks themselves and run one more layer each. The sweep
therefore decorates the rectangle expanded by three chunks, and the runtime
attaches one more terrain ring for the chunks every decoration reads.
`force_load_decoration_order` returns that sequence. A basalt delta, blob or
column reads and writes the chunks those dependency rings generated, so the
chunk that a saved reference world contains depends on this order.

### Tile generation

The production path generates a tile at a time instead of a chunk at a time.
`chunk_generator::generate_tile` produces the `tile_size` by `tile_size`
square whose minimum corner is the given origin and returns only that square;
`generate_area_tiled` covers an arbitrary chunk rectangle with whole
aligned tiles and returns the requested chunks. The tile and one ring around it
are decorated in force-load order, each chunk with its own position and its own
candidate-biome plan, and the writes land in the ring. The terrain fill adds one
further ring, because a features step reads its own three-by-three. A served
chunk therefore contains exactly the feature writes its eight neighbours make
across its border, which is what a saved reference world contains and what a
vanilla client expects to see.

The extra work per served chunk is `(N+2)^2 / N^2` for decoration and
`(N+4)^2 / N^2` for terrain, rather than the nine-times decoration cost of
staging every neighbour through a separate single-chunk generation; a larger
tile amortises the rings better but delays the first chunk of the tile, so the
edge is a trade-off between throughput and latency. The tile is also the unit
the runtime's distance-priority queue and admission credits count: a worker
generates one tile, the nearest chunks of a requested column are served as
part of their tile, and a chunk is never generated without its neighbours'
decoration.

One object owns the block state registry and its tags, the dimension profile,
the biome source, the surface rule state, the loaded carvers' runtime objects,
the feature registry, the structure data and template manager, and the terrain
engine. The immutable half of the datapack load — noise settings and their
density documents, material rule documents, climates, noise functions, block
tags, carvers, the feature data set and per-step plan, and the structure data —
is loaded once by `create` and shared by reference with every worker of a
batch. A worker keeps only its mutable state: the block state registry, the
multi-noise search, the template cache and the start-probe caches.

The stages run in the order the full-stage oracle uses. Structure starts are
resolved before fill because their terrain adaptation reaches the fill through
the beardifier; the start and reference records are written to the chunk after
the terrain stages. Targets that include structures or features fill the
target chunk and its eight neighbours; pieces of the target chunk's starts are
clipped into the owner and the neighbours so the fence, wall and stair repair
sees the complete building, and feature decoration runs last through the same
region. The final heightmaps are primed in one top-down pass per column when
the terrain status is reached and updated by every piece and feature write, so
they cover the finished chunk.

### Per-chunk status pipeline

A second generation path serves one chunk at a time from a per-generator proto
chunk cache. `chunk_generator::generate_chunk` materialises the requested chunk,
the eight neighbours whose feature writes cross into it, and one further terrain
ring, then returns a copy of the requested chunk at the configured target
status. `generate_chunk_area` serves a rectangle in ascending x then z order.
Both entry points are additive: `generate`, `generate_tile` and
`generate_area_tiled` keep their own path. The production runtime uses the tile
path; the per-chunk jobs reach it only through the opt-in scheduler below.

The per-generator cache holds the chunks one generator touched, keyed by chunk
position, and records two stages. A chunk at `terrain` has its biomes, fill, surface, carvers
and final heightmaps. A chunk at `decorated` has additionally placed its
structure pieces and run its feature step. A served chunk is sealed: later
decorations read it and their writes into it are dropped, the way a generation
region reads a completed chunk without modifying it. A repeat request therefore
returns the same blocks and the same digest without running terrain or
decoration again. On this path the cache is bounded by `proto_cache_capacity`;
entries over the bound are evicted in insertion order, and an evicted chunk loses
the feature writes its neighbours already made into it, so a caller keeps the
capacity above its working set. The scheduler below uses the same entry model
through one shared cache with a different eviction rule.

The dependency radii follow the reference status chain. The features step reads
and writes over a radius of one chunk, and its terrain dependency is one chunk,
so a served chunk needs its own three-by-three decorated and a five-by-five of
terrain. Structure starts keep the reference search radius of eight chunks. A
chunk is served only after every chunk of its three-by-three is decorated, so
the served copy contains the cross-border feature writes a saved world has; its
structure records are the starts it owns plus one reference for every start in
the radius-eight window whose box overlaps it.

The pipeline never serves a chunk whose three-by-three is incomplete, and it
decorates an undecorated chunk only after the earlier chunks of its decorated
neighbourhood were processed, so the write order is ascending x then z.
Terrain is reproducible from the seed, the data, the position and the starts
window of the request that first produced it: every fill resets the multi-noise
search, and the structure starts of a chunk are generated from a fresh biome
search too, so a cached start never depends on which chunk the worker scanned
before it. Feature
decoration of a cached chunk reads the palettes of the chunks around it from the
cache, which are the same palettes the reference possible-biome set uses. A
terrain fill reads the structure starts of the requesting chunk's window, which
is the reference dependency radius, and a chunk keeps the fill of the request
that first cached it.

A chunk served through the pipeline carries the configured `generation_target`
status. The `features` and `full` targets both settle the three-by-three ring
and differ only in the status field, because the pipeline always produces the
complete chunk; a `terrain` target uses the single-chunk path, which has no
neighbour writes to settle.

`worldgen::chunk_scheduler` turns the pipeline into a status-job scheduler over
one shared deterministic state. It owns the content's block-state table, one
proto chunk cache and the worker generators. Every worker's generator points its
pipeline at the shared cache, so a chunk's terrain, structures and feature
writes are materialised once instead of once per worker. Creating the scheduler
loads the content block table and the first worker generator, so an unsupported
content configuration fails at start instead of on the first demanded column.

The scheduler queues three status jobs per demanded column. `terrain` fills the
chunks the column publishes first; it is reproducible from the seed, the data,
the position and the column's starts window, and the demand that publishes a
chunk is fixed the first time it is needed, so any worker may run it and a
refill reproduces the same blocks. `decorated` places the structure
pieces of the column's three-by-three and runs its feature step, and `served`
copies the column out. The terrain jobs run on all workers; the decorated and
served jobs run as one ordered lane in demand order. A column is served only
after its whole three-by-three is decorated, and a served chunk is sealed, so a
later feature write into it is dropped exactly as on the serial per-generator
pipeline. The served content is therefore the serial pipeline's content, and the
worker count only decides how the terrain fills are spread over threads.

Two pieces of shared state make that content independent of worker order. The
block-state table is built from the content's block table container
(`tables.rvtb`, the sibling of the data directory) and frozen after the first
worker generator loaded. It interns every state of the table, in the table's key
order, and the bare block name of every default state, so a state key always
resolves to the same id regardless of which worker reaches it first; an unknown
key fails to intern instead of taking a first-use id. The shared builder
re-checks after interning that every state of the content's block table and the
bare name of every block is held, and the frozen table is checked before the
scheduler starts (`block_state_registry::verify_frozen`): the air state is id
0, every key resolves to its own id, and every default-state and tag entry
names an existing id. Those checks cover the table's internal consistency and
the keys the builder just interned; a generation-derived key that the content
table does not define still interns to `not_found`, and the dimension
generator's resolver falls back to the empty state.

The proto cache is shared under one mutex and records the first demanded column
that publishes each terrain chunk, so a chunk is filled from the dependency
window that the per-generator pipeline would have used. Its `capacity` bounds
the pristine terrain entries, the fills that no structure or feature has written
to yet: an insertion evicts pristine entries in ascending position order until
the count is back at the bound. The guarantee is stated at the scheduler level.
The scheduler pins at most one five-by-five dependency window and clamps
`capacity` to at least 25, so its inserts cannot hide enough pristine entries to
overshoot; a stats reader never sees the transient `capacity + 1` state inside
an insertion because the read and the insertion share the mutex. A caller that
pins more entries than `capacity`, or drives the cache without pinning its
dependency windows, must size `capacity` above its pinned set.

An evicted pristine entry is refilled from the dependency window of the demand
that first published it. That window decides the fill's structure beard as well
as its noise, so the refill recomputes the pieces the chunk already carried
instead of producing a seed-only fill; the original window is a function of the
demand sequence, so eviction cannot change a served chunk. The tests compare a
capacity of 25 against a non-evicting capacity over a demand gap larger than one
window and get equal digests. Pinned entries, which the ordered lane holds while
it reads its five-by-five dependency window, are never evicted, and the
pipeline never mutates an entry through a pointer the cache could drop.

A terrain entry that received a structure piece or a neighbour feature write is
no longer reproducible from the seed, so it is marked written and is not an
eviction candidate until it is decorated. Written and decorated entries carry
state that only an unload can drop, and a long viewport would grow them without
bound, so the caller declares the rectangle of positions it may demand next
(`chunk_scheduler::trim_outside`) and the entries more than two chunks outside
it, which no such demand can read or write, are unloaded. The experimental
runtime derives that window from the wanted set and the chains in flight on
every dispatch, so a walking viewport keeps the cached set inside its current
working set; a request that jumps outside the declared window rebuilds its
neighbourhood from the seed and can serve different blocks than an unbounded
cache, which is the accepted consequence of not keeping the whole session in
memory. Without a declared window the decorated and written entries stay cached
until `clear`.

The statistics are race free: the cache maintains its entry, pristine and
high-water counters under its own mutex, and the ordered lane publishes a stage
change, a written mark or a seal through locked cache methods instead of writing
the entry through an unlocked `find` pointer. A reader may therefore poll
`chunk_scheduler::stats` while the workers generate. The high-water counters are
what the memory bound is checked against: the published
`cache_peak_pristine_entries` is recorded after the insertion evicted back to the
bound and never exceeds `cache_capacity`, and `cache_peak_terrain_entries`
additionally covers the written dependency ring.

The worker loop follows the runtime contract: a worker waits on a condition
variable whose predicate is `chunk_scheduler::has_job`, takes one job with
`next_job`, and the completion sites notify the condition variable so another
worker can pick up the ordered lane or a queued fill. The tests drive the
scheduler as waiting workers rather than busy polling `next_job`.

The scheduler's tests pin its content gates on the development machine; these
are correctness digests, not throughput numbers. A sixteen-by-sixteen fill at
eight workers with the default capacity of 256 terms serves every column and
peaks at 125 pristine and 125 resident terrain entries (400 cached entries in
total, including the decorated ring), so the pristine bound holds with room to
spare. At one, four and eight workers the same four-by-four fill at capacity 25
serves identical digests, and an eight-by-eight fill served by the scheduler
equals the serial per-generator pipeline at capacity 128, where that generator's
own cache evicts sixteen entries. The residency numbers are indicative and not
comparable with the bench host figures below; throughput on the shared path is
measured separately before it can replace the tile path.

The scheduler is opt-in. `chunk_generation` drives it only when its
`experimental_chunk_pipeline` config flag is set, and the flag is off by default
and never set by the server composition. The production tile path stays the
default. The experimental path is measured by `refrain-bench-runtime
--scheduler`, which reports first-chunk and full-rectangle latency and hashes
the served set; its digest is identical at one, four and eight workers for the
benchmark rectangles, with fixed and natural biomes, and repeats run to run.

The batch entry point is a third, oracle-only form: every worker replays the
same per-chunk requests up to its own slice on its own cache, so the served
chunks equal the serial sequence for any worker count. The tile batch keeps
serving production.

The configured tile edge does not select content on the per-chunk path:
`generate_chunk`, `generate_chunk_area` and the scheduler ignore `tile_size`,
and the same request produces the same digest for tile edges two, four and
eight. The production tile path does use its edge for staging, and its content
only equals the single-chunk reference when a fixed biome makes the ring order
irrelevant; the natural-biome edge difference is pinned by a regression test as
a known property of that staging.

## Acceptance

The production tile path is accepted on a dedicated 12-thread x86-64 machine
(six physical cores, AVX-512) with a Release build at `x86-64-v4`, link-time
optimisation enabled and the CPU governor fixed at `performance`.
[Building](building.md#benchmarking-on-a-separate-host) documents the remote
benchmark target and its host lock, which keeps runs from overlapping. The runs
use the generated datapack without an extracted template cache, so
template-backed structure pieces are not placed in the full-target numbers.

The runtime generates tiles: a `chunk_generation` worker produces one whole
`tile_size` tile for the columns the runtime needs, and
`refrain-bench-runtime --tiles` measures the same entry point. A request smaller
than one tile still generates the whole 8x8 square, so its throughput counts
the requested chunks, not the 64 generated; a 64-chunk request is exactly one
tile. Throughput and scaling for the full target over 16 independent tiles
(1024 chunks), median of five repeats:

| workers | chunks/s | chunks/s per worker | speed-up |
|---:|---:|---:|---:|
| 1 | 52.1 | 52.1 | 1.00x |
| 4 | 179.5 | 44.9 | 3.44x |
| 8 | 216.5 | 27.1 | 4.15x |

One tile (64 chunks) generates at 52.5 chunks/s on one worker, consistent with
the per-worker figure over 16 tiles; the terrain target reaches 143.9 chunks/s
per worker. Every configuration stays above the 20 chunks/s per worker budget.
The eight-worker run scales sublinearly because the machine has six physical
cores, and a request below one tile cannot use more than one worker at all.
Peak resident set reached 85.5 MiB in the single-tile runs and 566 MiB in the
16-tile run at eight workers.

```bash
REFRAIN_BENCH_HOST=user@host REFRAIN_BENCH_SSH_KEY=<key> \
REFRAIN_BENCH_ARCH=x86-64-v4 REFRAIN_BENCH_REQUIRE=1 \
scripts/dev/bench-remote.sh --tiles --targets full \
    --chunks 1024 --workers 1,4,8 --repeat 5
```

The chunk digests are equal at one and eight workers, for fixed and natural
biomes, and repeated runs agree. `--chunk-hash` hashes the first worker count of
the list, so the gate is one invocation per worker count:

| workload | chunks | one worker | eight workers |
|---|---:|---|---|
| full, fixed biome | 64 | `df152fcd8fe1fe47` | `df152fcd8fe1fe47` |
| full, fixed biome | 1024 | `ec7c731572980cf0` | `ec7c731572980cf0` |
| full, natural biome | 1024 | `5af78bada48fcc9e` | `5af78bada48fcc9e` |

```bash
REFRAIN_BENCH_HOST=user@host REFRAIN_BENCH_SSH_KEY=<key> \
REFRAIN_BENCH_ARCH=x86-64-v4 REFRAIN_BENCH_REQUIRE=1 \
scripts/dev/bench-remote.sh --tiles --chunk-hash --targets full \
    --chunks 1024 --workers 1
```

The natural-biome case is the one the runtime generates with; the opt-in
scheduler has its own digest gate.

A fresh world fills its spawn area (49 chunks) before the listener accepts
joins, and the tick loop then generates the viewports of the joining clients.
Three runs of 20 clients at a view distance of six, over 60 seconds each,
measured a tick-work average of 2.06-2.11 ms, p99 of 35.5-36.9 ms and a maximum
of 45.2-46.9 ms against the 50 ms budget, with no missed ticks in 1280 ticks per
run. The spawn fill runs before the first tick, so the budget is met by the tick
loop of the same fresh-world run while the viewport fills.

```bash
REFRAIN_BIN=<bench build>/bin scripts/soak.sh --fresh --seed 99 \
    --bots 20 --seconds 60 --view-distance 6 --p99-ms 50
```

`--p99-ms 50` sets the harness gate to the tick budget; `soak.sh` otherwise
exits nonzero on its stricter join-path default of 15 ms, which this
fresh-world load exceeds without missing a tick.

## Density sampling

The final density contains the two sampling layers the documents declare. An
interpolated node builds a coarse lattice over the requested block volume and
samplers fill the lattice corners: the overworld and the nether use a cell
width of four blocks on x and z and a cell height of eight, and the end uses
eight on x and z with a height of four. The lattice gains one extra row,
column or layer when the requested volume does not end on a cell boundary, and
the block values between corners are trilinearly interpolated in the
reference's float order, including its incremental y accumulation. A volume
whose steps equal the cell size and whose minimum lies on a cell boundary is
sampled directly, and a volume with a non-block step samples a block volume
and picks the samples.

Interpolation covers only the sub-expression the document wraps. The remaining
terms of the final density, such as the cave noises and the structure
beardifier, are evaluated for every block. A lattice over the whole final
density would therefore change generated blocks and is not used.

A referenced density document compiles once and every reference to it shares
the same graph. A `cache` node keeps the values its input sampled for one grid
in the sampling pass, and a grid that does not match replaces the entry, so a
context can be reused across chunks. The cache changes the amount of sampling,
not its results: cached and direct bulk sampling agree bit for bit. Scalar
sampling never reads the cached grid, because some nodes round the scalar and
bulk paths differently and terrain generation reads the bulk path.

The multi-noise search is reset before each chunk, which makes a chunk
independent of the chunks generated before it. `generate_batch` creates one
independent generator per worker and returns chunks in input order. Workers
are retained by the calling object because a returned chunk borrows its
block state table; the same applies to `generate`, whose chunk must not
outlive the generator. A batch hands each worker runs of consecutive
positions rather than single chunks, so the bounded per-worker start-probe
caches stay warm: each cached column holds the six unbearded heightmap rows
a start reads, or the vertical block column the fossil start scans. Those
columns are pure functions of the seed and data, so a cached probe produces
the same start as a freshly generated one.

`reference_world` reproduces the structure oracle's reference worlds: fill and
structure pieces only, with the surface pass, carvers and features omitted.
The chunk still reaches the configured target status. `reference_carvers` is a
second reference mode for `reference_world` runs: it keeps the real surface
pass and the dimension's carvers, while features stay off. Selecting
`generation_target::terrain` with it also disables structures, so the chunk is
exactly the fill, surface and carving output of the dimension. A production run
(without `reference_world`) always executes the dimension's carvers.

## Custom feature reference worlds

The feature oracle compares a generated chunk against a reference world
whose biome lists curated placed features. To validate a feature that is not
part of that list, point the generator at a small datapack containing the
configured and placed feature and append it to the generated biome:

```bash
python3 tools/worldgen/oracle/generate.py \
    --server-jar /path/to/server.jar --seed 1 \
    --output /tmp/reference --features --features-only-extra \
    --extra-feature-pack /path/to/pack \
    --extra-feature oracle:my_feature
```

`--extra-feature-pack` merges the pack's `data` tree into the reference
world. `--features-only-extra` drops the curated list so the new feature
keeps its own decoration index and random stream. Run the feature oracle
with the same `REFRAIN_VANILLA_FEATURE_*` variables used by the curated
world; `REFRAIN_VANILLA_FEATURE_CHUNK_X`/`_Z` select one chunk and
`REFRAIN_VANILLA_FEATURE_BORDER_BLOCK` skips chunks whose reference output
touches a border, where features from neighbouring chunks write across the
chunk boundary and a single-chunk oracle cannot reproduce them.

### Root systems and vegetation patches

A root-system reference world can be built from two placed features: a
simple-block feature that writes a stone platform at a fixed position and a
root-system feature placed one block below it. The root system climbs
through the platform and delegates to a nested grass feature above it, so
the chunk exercises the ceiling scan, dirt replacement and hanging roots.
The same pattern works for vegetation patches: place a patch at a fixed
surface position and compare a single chunk. Set
`REFRAIN_VANILLA_FEATURE_CHUNK_X`/`_Z` to that chunk and point
`REFRAIN_VANILLA_FEATURE_REGION` and `REFRAIN_VANILLA_FEATURE_DATA` at the
generated world.

## Persisted light

Generation produces blocks, not light. A column is lit when the runtime adopts
it into the loaded set, after feature placement, and the chunk record is saved
with the computed arrays. The record also carries a light-correct flag and the
fingerprint of the light table the arrays were computed with. The flag is set
only when the stored arrays are the complete layer set the column's block
sections require - one layer per block section plus the padding layers below
`yPos` and above the top section, with both nibble arrays at full length - and
the fingerprint matches the runtime table. A record written before the fields
existed, an imported Anvil column, a column whose light a block edit changed and
a column saved under different light content all carry the conservative values.

A load reuses the saved arrays when the flag is set, the layer set verifies and
the fingerprint matches. The initial light pass does not run; the engine only
rebuilds the sky-source index that later edits need. Any other record is relit
from its blocks, which is what a user of an older world or a freshly imported
one sees, and the relit column is marked for saving so the upgraded arrays reach
the store on the next save.

A block edit clears the flag and marks for saving every column the update
writes, including the neighbours a propagation crosses into. Only a drain that
leaves no queued update sets the flag again, so a save taken while a drain is
pending stores the touched columns as not correct and the next load relights
them; a save taken after the drain stores the corrected arrays. Trusting the
flag assumes the save wrote arrays consistent with the neighbours it recorded:
a column saved before a later neighbour edit keeps the border light of its own
save until an edit or a relight recomputes it, exactly like the reference. The
flag and the fingerprint are appended fields of the RWF chunk record, and a
reader that predates them skips the unknown fields and relights the column, so
the chunk field-set version stays 1.

A light table installed at runtime relights the loaded columns and marks them to
be saved, and a record read back under another table fails the fingerprint check
and relights on its first load. A world saved with one content version and
opened with another therefore shows the light the current table produces
instead of arrays from the old one.

## Verification status

The reference-backed suites currently cover:

- terrain fill, biome selection, surface rules, carvers and feature
  decoration, including a whole-chunk feature oracle;
- nether and end terrain fill and surface rules against reference worlds of
  those dimensions, a carved nether reference chunk compared through the
  runtime service, plus deterministic fixed-biome chunks of both dimensions;
- structure starts, payload trees, piece placement and full-stage
  generation for jungle temple, warm ocean ruin, beached shipwreck,
  buried treasure, mineshaft, ocean monument, stronghold, nether fortress,
  bastion remnant, nether fossil, end city, a plains village start, a
  pillager outpost, a taiga village, an ancient city, trial chambers and
  trail ruins, including their feature pool elements;
- `locate_structure` against real seeds and reference heightmaps;
- the storage bridge, RWF round trip and Anvil export/import;
- parallel fill determinism and fixed-biome structure generator
  determinism;
- the full-chunk generator against the reference full-stage worlds of a
  plains village, a woodland mansion, a stronghold, a pillager outpost, a
  taiga village, an ancient city and trial chambers, plus batch equality and
  the terrain/features/full status ladder;
- the production tile path against the single-chunk reference mode: the probe
  chunk of the border-decoration regression, every chunk of a three-by-three
  tile with a fixed biome, the same tile with the dimension's biome source and
  a per-chunk plan, and the chunk rectangle `generate_area_tiled` serves;
- the configured feature, placement modifier, block predicate, state provider
  and tree placer families the standard biomes name, through synthetic unit
  tests and a content-driven test that creates one generator and generates one
  full chunk for every standard overworld, nether and end biome;
- fixed-biome feature reference worlds for cherry grove, plains, desert and
  badlands: the runtime runs the real features with structure starts disabled
  and decorates the target chunk and its eight neighbours, and the compared
  chunk matches the reference block for block and in all four final
  heightmaps. The spawn terrain of those worlds is ocean, so the chunks cover
  the underground families (ores, disks, geodes, lava lakes, springs) and
  their fluids. The reference command and environment variables are recorded
  in `tools/worldgen/oracle/README.md`;
- a fixed-biome warm ocean feature world reproduces its chunk block for block
  and in all four final heightmaps: chunk `(0,-1)` of the seed-1 reference
  world reaches zero with neighbour decoration in force-load order. The chunk
  exercises the weighted tall/short seagrass selection, sea pickles and the
  cross-border writes of both. Heightmap placements read the heightmap of the
  chunk that holds the candidate column, so a candidate the offset modifier
  spreads into a neighbour uses that neighbour's height;
- a fixed-biome lush caves world reaches zero with a two-chunk neighbour
  radius: the wider region lets the vegetation patches read the chunks their
  own generation regions cover, and the default-state report lets a bare
  tall-grass placement emit the upper half of the double plant. The chunk
  exercises the patch edge-column draws, the reference hash-set vegetation
  order, waterlogged patch flooding and the double-plant placement;
- a fixed-biome deep dark world reaches zero with a two-chunk neighbour
  radius and force-load order. Its charge cursor, decay, spread attempts,
  movement, face choice and vein merging reproduce a reference trace exactly
  in isolated scenarios, including three sequential patches over water,
  gravel, tuff and an existing vein field. Each sculk patch moves its charge
  cursors at most twelve blocks from the origin, so the patches of a
  neighbour chunk must see the chunks their own generation regions cover; the
  wider radius provides them, and each decoration writes through its own
  generation region again. The scanned chunks and commands are recorded in
  `tools/worldgen/oracle/README.md`;
- a fixed-biome basalt deltas world reaches zero with the ticket sweep of its
  force-load and the rectangle the reference world generated. The delta,
  small and large basalt column and basalt blob families sample the
  noise-settings generation depth (128 blocks in the nether) instead of the
  level height, which keeps their height-range draws inside the generation
  area. Datapack state keys that spell only default property values fold onto
  the bare block name the reference palette writes, a spring writes the
  legacy block of its configured fluid state rather than the falling flag,
  and a mushroom needs a solid-render substrate. The chunk exercises all four
  behaviours, and the command and environment are recorded in
  `tools/worldgen/oracle/README.md`;
- a fixed-biome end feature world with the `minecraft:the_end` biome kept:
  chunk `(6,0)` of the seed-1 reference world contains the obsidian platform
  the fixed placement at `(100,49,0)` writes, and the runtime reproduces it
  block for block and in all four final heightmaps. The command and the
  environment variables are recorded in `tools/worldgen/oracle/README.md`;
- the remaining reference gaps through synthetic tests: the pine and mega
  pine foliage placers reject a document without their canopy height provider,
  the biome membership predicate expands data-pack biome tags and rejects a
  tag the pack does not define, the structure feature pool element reads the
  generation climate for the snow and freeze features, every positive integer
  provider type drives a capped processor limit while an unknown type is
  rejected with its name, and template entities and end city shulker and
  elytra data markers place their entities with the transformed position and
  rotation;
- benchmarks for fill, parallel fill, structure selection, corridor
  placement, template placement, feature freeze and decoration.

Known gaps: the feature registry decodes every configured feature, placement
modifier, block predicate and state-provider type the 26.3 datapack uses,
including the cherry, poplar, bending and upwards-branching tree placers, the
mangrove root placer and the pale moss and creaking heart decorators. The pine
and mega pine foliage placers require the canopy height provider their codecs
name (`height` and `crown_height`); a document that omits it is rejected at
load time, exactly like the reference codec, and a negative test covers both
names. A biome whose plan names an unknown type still fails
`chunk_generator::create` with an `unsupported` error instead of silently
dropping that feature, and both the content-driven test and a negative
configured-feature test fail when that happens. Predicates whose reference
definition reads collision geometry (sturdy faces and multiface attachment)
derive a full face from the block tags and the state shape: slabs, stairs,
trapdoors, doors, walls, fences, fence gates, panes, bars, carpets, snow
layers, paths, farmland, speleothems, cave vines, multiface layers and sculk
sensors answer per direction; a bare state key takes the properties its
default state carries from the default-state report. States outside those
families still fall back to the heightmap motion-blocking tags. The substrate
checks of a simple-block placement cover the families the 26.3 datapack
places: a mushroom needs a solid-render block below it or a block that
overrides its light requirement. Generation carries no light data; the runtime
lights a column when it is adopted into the world, which is after feature
placement, so the reference's light gate for feature placement is still not
modelled and admits every position in a dimension without skylight. That is
the case the feature worlds exercise.
The unobstructed predicate admits every position because generation has no
entities. The biome membership predicate resolves `#tag` lists through the
data pack's biome tag tree and compares explicit biome ids directly; a tag the
pack does not define is rejected at decode time. Structure feature placement
samples the biome and its temperature from the chunk region the piece writes
into, so snow and freeze feature modifiers run there like they do in the chunk
feature stage; a feature pool element that reaches outside that region sees no
climate and leaves the two weather features alone. Template entities are
placed into the generation chunk that contains their world position, and the
end city shulker and elytra data markers create their entities. The storage
bridge turns every placed entity tag into the column's entity records: the
type, the world position, the body rotation, the block position and the
remaining template data components. The records are sorted by position before
they are written, so the same seed and template always produce the same
record bytes and a reload keeps the entity once. A tag the bridge cannot
interpret (no type id or a malformed position) fails the conversion instead of
being dropped silently, and a chunk that carries no entity omits the record
field entirely. The generated structure starts and references are persisted
with the column as well. The bridge turns the starts the column owns and the
references it carries into the vanilla chunk `structures` compound: a `starts`
compound keyed by structure id that carries the structure id, the chunk
position and one vanilla piece tag per piece, and a `References` compound of
packed chunk positions, with the reference multiplicity kept. A jigsaw pool
element stores its processor list the way the reference does: a named list as
its id string and an inline list as the direct codec's object form
(`{"processors": [...]}`), so a generated piece whose datapack document lists
its processors inline round-trips. The inline document is converted under depth
and value-count bounds, and a value the tag model cannot carry fails the
conversion. A column with no structure data keeps the field empty, and any
other record the compound cannot represent fails the conversion instead of
losing a start silently. An Anvil export of a generated world therefore
declares the structures the reference reader expects. Restoring persisted
starts into generation is a separate capability: a loaded column does not take
part in structure continuation because the stored compound does not carry the
generation step of a structure or the template-derived placement state the
restore direction needs from the caller.
What the client receives from these records is covered in
[architecture](architecture.md#generated-entities). Capped processor lists decode and run their
delegate over the whole piece; every positive integer provider type of 26.3
drives the limit, and an unknown limit type is rejected with its name. Rule
block entity modifiers are decoded for `clear`,
`passthrough`, `append_static` and `append_loot`. A rule that names a loot
table keeps the seed drawn from its own positional stream, while a container
tag carried by the template still receives the piece stream's seed at
placement. Processor rule position predicates decode `always_true`,
`linear_pos` and `axis_aligned_linear_pos`; the two linear forms measure the
world position against the reference position of the structure start (the
centre of its first piece's box at the box floor), so the template-local
position does not participate. A missing predicate is `always_true` and draws
nothing. End gateways and the obsidian end platform are placed by the end
feature stage. The obsidian platform chunk is compared against a reference
world; the gateway frame, exit block entity and rotation are covered by
feature-level tests. The fixed-biome end feature worlds the oracle generates
contain no generated gateway even at several thousand chunks, so no reference
chunk compares a gateway position yet. Structure containers write the target
version's built-in loot table ids and consume the reference draw order, so the
reference-backed container cases compare both the loot table id and the stored
seed: the piece-placement suite covers the buried treasure, jungle temple,
desert pyramid, shipwreck, ocean ruin, igloo basement, nether fortress and
stronghold containers, and generated-chunk cases cover the end city, nether
fortress and stronghold containers. The stronghold piece-placement case
generates the pre-structure terrain through the chunk generator because the
piece selectors read block state before they draw; replaying onto the saved
chunk would read the finished stronghold instead. The bonus chest and monster
room containers have synthetic tests that replay the reference draw counts
before the seed and compare the built-in table id and the seed that order
produces. A container whose loot table id differs from the reference, or that
omits it, fails the comparison.

Ruined portal starts place the template at the reference position and
orientation. A dry reference candidate at chunk (-65, 0) matches block for
block and heightmap for heightmap; a candidate that intersects flowing lava
is not a stable oracle after the reference world settles. Woodland mansion
pieces match the reference list exactly and its block oracle matches the
reference chunk. An ocean monument start plus every room, wing and penthouse
piece matches the reference world in the sixteen chunks the building covers.
A stronghold start plus every fitted corridor and room matches the reference
world in the sixty-three chunks the pieces cover, and the ring placement
reproduces the reference candidate chunks.

The jigsaw framework is validated on the overworld structures the structure
oracle adds: seed 1 places a pillager outpost start at chunk `(5, 20)`, a taiga
village start at chunk `(15, 8)`, an ancient city start at chunk `(9, 11)` and
a trial chambers start at chunk `(18, 14)`. The start piece lists match the
reference children exactly, including list elements, the outpost feature
plates, the ancient city's start anchor and the trial chambers pool aliases.
A full-stage reference chunk of each structure matches block for block and in
all four final heightmaps: the outpost and trial chambers start chunks, a taiga
village chunk one chunk south of the start and an ancient city chunk away from
its fluid pools. A trail ruins chunk exercises both capped archaeology lists:
the reference and runtime chunks agree on every block, heightmap and brushable
block entity, including the loot seed each capped selection stored. A trial
chambers chest chunk agrees on its container loot tables and seeds as well.
The same comparison runs for the procedural structure families: the buried
treasure chest, the basement igloo chest, every jungle temple chest and
dispenser, the four desert pyramid chests, the shipwreck markers and the ocean
ruin chest and brushable block all match a reference chunk in both fields. The
procedural container oracles compare both fields as well, including the nether
fortress and end city containers; the stronghold case builds the pre-structure
terrain with the chunk generator so its piece selectors read the block states
the reference placement saw. The bonus chest and monster room containers are
covered by synthetic draw-order tests.
The clean chunks are chosen deliberately: reference worlds settle flowing
water and lava after saving, so a chunk whose pieces place fluids cannot be
compared block for block, and a feature element that writes into the compared
chunk from a neighbouring chunk depends on the order those chunks were
generated. The commands and environment variables are recorded in
`tools/worldgen/oracle/README.md`.

The nether structure oracle also compares a full-stage bastion remnant chunk
and a full-stage nether fossil chunk block for block and in all four final
heightmaps. The bastion start list and junctions match the reference for the
treasure, bridge, stable and housing/units branches; the fossil start and its
dried ghast placement match a soul-sand-valley reference chunk. A treasure
chunk crossed by the two high-rampart wall pieces matches block for block,
in all four final heightmaps and in its container block entities, which
exercises the axis-aligned linear position predicate. A hoglin stable chunk
matches the same way after wall sides stop connecting to hanging lanterns; the
four-block blackstone wall difference seen there was a final shape rescan
approximation, not a rule or position predicate difference. The dimension
oracle of seed 1 matches nether chunks `(0,0)`, `(1,0)` and `(0,1)` and end
chunks `(0,0)` and `(1,1)` block for block and in all four final heightmaps.
Both reference worlds fix a real biome and strip its carvers and features, so
they contain exactly the fill and surface output of that dimension. The
deterministic dimension test repeats the fixed-biome chunks without a reference
world, using a locally generated 26.3 datapack.

A nether reference world keeps the fixed biome's real carvers and the real
material rule while features and structures stay off. The runtime service
reproduces a chunk with visible cave carving block for block and in all four
final heightmaps. `tools/worldgen/oracle/README.md` records the command and the
environment variables of that case.

The two nether climate noises use the historical nether-biome octave layout:
their generators come from legacy streams seeded with the world seed plus a
per-noise offset, and each octave stack follows the older descending octave
order. The other dimensions and every other noise document keep the standard
layout. A plain nether reference world (no data pack, no fixed biome) compares
the stored 4x4x4 biome grid of every full chunk in the region against the
runtime climate sampler, and a nether world that keeps the real multi-noise
biomes and carvers but strips features compares a carved terrain chunk block
for block and in all four final heightmaps. Both tests take the seed and, for
the terrain chunk, the chunk coordinate from the environment;
`tools/worldgen/oracle/README.md` records the commands.

A second dimension reference world keeps one structure set and uses an empty
material rule, so a compared chunk contains terrain fill plus structure pieces
only. The full-stage test selects the dimension through
`REFRAIN_FULL_STAGE_DIMENSION` and takes the level bounds, noise settings and
sea level from the dimension profile; the reference fortress start and end city
start piece lists are compared separately. The fortress and end city reference
runs are documented in `tools/worldgen/oracle/README.md`.

## See also

- [Running a server](running.md) for the server startup that reads the
  generated datapack, and for the world import and export commands.
- [Testing](testing.md) for the test suite, which skips its data-driven world
  generation cases when the datapack is missing.
