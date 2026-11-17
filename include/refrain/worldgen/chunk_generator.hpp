// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The production full-chunk generation service. One object owns the terrain
// engine, the feature registry, the structure template manager and the mutable
// search state of a dimension, and shares the loaded immutable worldgen data
// with the other workers of a batch. Generating a chunk runs the stages in
// dependency order and returns the mutable chunk the caller can turn into a
// level chunk.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/chunk/proto_cache.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/dimension_generator.hpp>
#include <refrain/worldgen/feature/feature_registry.hpp>
#include <refrain/worldgen/structure/stage.hpp>
#include <refrain/worldgen/structure/template.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace refrain::worldgen {

class stage_profile;

/// How far a generated chunk advances along the generation chain.
enum class generation_target : std::uint8_t {
    /// Biomes, fill, surface and carvers. The chunk holds no structures and
    /// no features.
    terrain = 0,
    /// Terrain plus structure starts, references, placed pieces and features.
    features = 1,
    /// The features target plus the final status.
    full = 2,
};

/// An inclusive rectangle in chunk coordinates. An empty rectangle has a
/// minimum beyond its maximum.
struct chunk_rectangle {
    jint min_x{0};
    jint min_z{0};
    jint max_x{-1};
    jint max_z{-1};

    [[nodiscard]] auto empty() const noexcept -> bool {
        return min_x > max_x || min_z > max_z;
    }
};

struct chunk_generator_config {
    /// The datapack `data` directory (`<version>/data`).
    std::string data_root{};
    /// Extracted structure templates, laid out as
    /// `<root>/<namespace>/structure/**.nbt`. An empty root still generates
    /// procedural pieces and features; template-backed pieces resolve to air.
    std::string templates_root{};
    jlong seed{0};
    dimension_kind kind{dimension_kind::overworld};
    /// Fixed biome key. Empty selects the dimension's biome source.
    std::string fixed_biome{};
    generation_target target{generation_target::full};
    /// Structure set ids that may generate. Empty enables every set; naming
    /// no existing set turns structure starts off.
    std::vector<std::string> structure_sets{};
    /// Reproduces the reference worlds of the structure oracle: terrain fill
    /// plus structure pieces only, with no surface pass, carvers or features.
    /// The chunk still reaches the configured target status.
    bool reference_world{false};
    /// Second reference mode for a `reference_world` run: keep the real
    /// surface pass and the dimension's carvers instead of stripping them.
    /// Features stay off because `reference_world` already disables them. Use
    /// `generation_target::terrain` when the reference world disables
    /// structures as well. A production run always executes the dimension's
    /// carvers.
    bool reference_carvers{false};
    /// Feature reference mode: decorate the target chunk and then the eight
    /// neighbour chunks through the same region. A reference world saves every
    /// chunk of a generated area, so its chunks contain the feature writes of
    /// their neighbours, which reach a few blocks across a border. The
    /// tile reference path performs the same decoration for a whole tile.
    bool decorate_neighbours{false};
    /// With `decorate_neighbours`, decorate the eight neighbours before the
    /// target chunk. The target's own writes then win where two chunks touch;
    /// the default decorates the target first, so neighbour writes win.
    bool neighbours_before_target{false};
    /// With `decorate_neighbours`, order the decorated chunks the way a
    /// force-load ticket queue does: ascending x, then ascending z. Every
    /// chunk that precedes the target in that order decorates first, then the
    /// target, then the chunks that follow. A saved reference world whose area
    /// was force-loaded contains exactly this interleaving, so a feature that
    /// writes across a border resolves contacts the same way.
    bool neighbours_in_generation_order{false};
    /// With `decorate_neighbours` and a `neighbour_box`, decorate the chunks
    /// in the order a force-load ticket queue runs them instead of ascending
    /// x and z. A ticket keeps its own two rings at the full status, so the
    /// task of every force-loaded chunk first runs the feature layer of the
    /// three-by-three around it; the propagated tickets two rings out then
    /// run one more layer each. `force_load_decoration_order` returns that
    /// sequence. A reference world built by force-loading a rectangle saves
    /// the whole area the tickets reached, and a feature that writes across a
    /// border resolves contacts in this order, so a single-chunk comparison
    /// of such a world needs it. Requires a non-empty `neighbour_box`.
    bool neighbours_ticket_sweep{false};
    /// With `decorate_neighbours`, how many chunks around the target are
    /// decorated. Zero decorates only the target, one the eight surrounding
    /// chunks (the default). Two adds the next ring, which a force-loaded
    /// reference area also generated: a feature of an outer chunk can read a
    /// chunk outside the target's three-by-three region, and a patch spread
    /// from there consumes a different number of random draws when that chunk
    /// reads as air. The runtime always generates one more terrain ring than
    /// it decorates, so every decorated chunk can read the chunks its own
    /// generation region covers, and it attaches no terrain outside the
    /// symmetric square. The value is limited to eight chunks.
    jint neighbour_radius{1};
    /// With `decorate_neighbours`, the reference world's generated chunk
    /// rectangle in absolute chunk coordinates. The whole rectangle decorates
    /// in force-load order, which reproduces features whose writes cross more
    /// than one border, such as a spreading sculk patch. Chunks outside the
    /// rectangle are attached as terrain only, because the reference world
    /// generated them as terrain dependencies without features. An empty
    /// rectangle keeps the symmetric `neighbour_radius` behaviour. The target
    /// chunk must lie inside the rectangle.
    chunk_rectangle neighbour_box{};
    /// Edge in chunks of the square `generate_tile` generates and serves as
    /// one unit. A tile is filled together with one ring of terrain around it
    /// and decorated together with that ring, so a served chunk contains the
    /// feature writes its neighbours make across its border, exactly as a
    /// saved world does. The single-chunk entry points ignore it.
    jint tile_size{8};
    /// Chunks one generator's per-chunk pipeline cache keeps. Each cached
    /// chunk costs about one terrain chunk, roughly 200 KiB at full height, so
    /// the default bound is a few tens of megabytes per worker. Entries over
    /// the bound are evicted in insertion order, and an evicted chunk loses
    /// the feature writes its neighbours already made into it.
    jint proto_cache_capacity{256};
};

/// The chunks a force-load of `box` runs features for, in the order the
/// ticket queue runs them. Every chunk of the rectangle is force-loaded in
/// ascending x and then z; each ticket keeps its own two rings at the full
/// status, so its generation task runs the feature layer of the three-by-three
/// around it, and the tickets propagated two rings out run one more layer
/// each. The result covers the rectangle expanded by three chunks.
[[nodiscard]] auto force_load_decoration_order(const chunk_rectangle &box)
    -> std::vector<chunk_pos>;

/// A reusable full-chunk generator. The object is not thread-safe: a single
/// instance must not generate concurrently. The batch entry points create one
/// independent instance per worker, so no random stream, block-state registry,
/// biome search or template cache is shared between workers. The loaded,
/// immutable datapack data is shared by reference.
class chunk_generator {
  public:
    [[nodiscard]] static auto create(chunk_generator_config config)
        -> result<std::unique_ptr<chunk_generator>>;

    /// Runs the configured stages for one chunk. Calling it again with the
    /// same position produces the same chunk: every multi-noise search is
    /// reset before the chunk's first biome is selected. The returned chunk
    /// borrows this object's block state table, so it must not outlive the
    /// generator.
    [[nodiscard]] auto generate(const chunk_pos &pos)
        -> result<std::unique_ptr<proto_chunk>>;

    /// Generates `positions` in parallel and returns the chunks in input
    /// order. `workers` is clamped to the position count; zero selects the
    /// hardware concurrency. Every worker owns its mutable state and shares
    /// only immutable data, so the result is identical to serial generation.
    /// Worker instances are kept for later batches, so returned chunks stay
    /// valid as long as this object does.
    [[nodiscard]] auto generate_batch(std::span<const chunk_pos> positions,
                                      std::size_t workers = 0)
        -> result<std::vector<std::unique_ptr<proto_chunk>>>;

    /// Generates a `width` by `depth` rectangle of chunks starting at
    /// `origin`, x outer to the inner z loop, and returns them in that order.
    [[nodiscard]] auto generate_area(chunk_pos origin, jint width, jint depth,
                                     std::size_t workers = 0)
        -> result<std::vector<std::unique_ptr<proto_chunk>>>;

    /// Generates one reference tile: the `tile_size` by `tile_size` square of
    /// chunks whose minimum corner is `origin`. The tile and one ring around
    /// it are filled and decorated in force-load order, then only the inner
    /// tile is returned. Every returned chunk therefore holds the feature
    /// writes its eight neighbours made across its border. This path
    /// reproduces a saved reference world whose area was force-loaded tile by
    /// tile; production generation goes through `generate_chunk` and the
    /// per-chunk scheduler, so a tile edge never selects served content.
    [[nodiscard]] auto generate_tile(const chunk_pos &origin)
        -> result<std::vector<std::unique_ptr<proto_chunk>>>;

    /// Generates every tile that covers a `width` by `depth` chunk rectangle
    /// starting at `origin` and returns the chunks of that rectangle in
    /// x-major order. Tiles are aligned to the tile grid, so the rectangle is
    /// generated through whole tiles and chunks outside it are discarded.
    [[nodiscard]] auto generate_area_tiled(chunk_pos origin, jint width,
                                           jint depth, std::size_t workers = 0)
        -> result<std::vector<std::unique_ptr<proto_chunk>>>;

    /// Generates one chunk through the per-chunk status pipeline. The chunk,
    /// the eight neighbours whose features cross into it and one more terrain
    /// ring are materialised in this generator's cache, the neighbours'
    /// features are applied in ascending chunk order, and the returned chunk is
    /// a copy at the configured target status. A repeated request returns the
    /// same blocks without re-running terrain or decoration. Additive: the
    /// tile and single-chunk entry points above keep their own path.
    [[nodiscard]] auto generate_chunk(const chunk_pos &pos)
        -> result<std::unique_ptr<proto_chunk>>;

    /// Runs only the terrain step of the pipeline for `pos`: fills the
    /// five-by-five terrain ring and the structure starts of the reference
    /// window. The scheduler calls this as its first status job; it is a no-op
    /// for a chunk whose dependencies are already cached. A terrain target has
    /// no pipeline and reports an invalid request.
    [[nodiscard]] auto advance_terrain(const chunk_pos &pos) -> result<void>;

    /// Runs only the decorated step of the pipeline for `pos`: places the
    /// structure pieces of the three-by-three, repairs the shape of `pos` and
    /// runs the feature step of the three-by-three in ascending order. It
    /// first ensures the terrain step, so the scheduler may call it directly.
    [[nodiscard]] auto advance_decorated(const chunk_pos &pos) -> result<void>;

    /// Copies the decorated chunk out at the configured target status and
    /// seals it. This is the served step of the pipeline; the scheduler calls
    /// it once the two earlier steps completed.
    [[nodiscard]] auto take_served(const chunk_pos &pos)
        -> result<std::unique_ptr<proto_chunk>>;

    /// Serves a `width` by `depth` chunk rectangle through the per-chunk
    /// pipeline in ascending x then z order and returns the chunks in x-major
    /// order. Workers replay the same call sequence on their own caches, so
    /// the chunks do not depend on the worker count. The scheduler that
    /// replaces the tile jobs is a separate step; this entry point only proves
    /// the content, not the throughput.
    [[nodiscard]] auto generate_chunk_area(chunk_pos origin, jint width,
                                           jint depth, std::size_t workers = 0)
        -> result<std::vector<std::unique_ptr<proto_chunk>>>;

    /// Counters of this generator's per-chunk cache.
    [[nodiscard]] auto cache_stats() const noexcept -> proto_cache_stats {
        return proto_cache_.stats();
    }
    /// Drops every cached chunk and structure start, so the next request
    /// generates its neighbourhood from scratch.
    void clear_proto_cache() noexcept;

    [[nodiscard]] auto config() const noexcept
        -> const chunk_generator_config & {
        return config_;
    }
    [[nodiscard]] auto profile() const noexcept -> const dimension_profile & {
        return dimension_->profile();
    }
    /// Biome keys indexed by the ids the generated chunks carry. The storage
    /// bridge maps those ids to names.
    [[nodiscard]] auto biome_names() const -> const std::vector<std::string> & {
        return dimension_->biome_source().biomes();
    }

    [[nodiscard]] auto states() noexcept -> block_state_registry & {
        return dimension_->states();
    }
    [[nodiscard]] auto target() const noexcept -> generation_target {
        return config_.target;
    }
    /// Edge in chunks of the square the reference tile path generates.
    [[nodiscard]] auto tile_size() const noexcept -> jint {
        return config_.tile_size;
    }

    /// Records the wall-clock time of every stage of the chunks generated
    /// afterwards into `profile`. Null disables recording. The profile never
    /// changes the generated chunk. A profile is not thread-safe, so each
    /// worker needs its own instance.
    void set_stage_profile(stage_profile *profile) noexcept {
        profile_ = profile;
    }

  private:
    friend class chunk_scheduler;

    /// Loaded data every worker of one configuration shares. It is immutable
    /// after `create`, so workers reference it instead of parsing the datapack
    /// again. Per-worker state (block-state registry, biome search, template
    /// cache, start-probe caches) stays in the generator instance.
    struct shared_generator_data;

    chunk_generator(
        chunk_generator_config config,
        std::shared_ptr<const shared_generator_data> shared,
        std::unique_ptr<dimension_generator> dimension,
        std::unique_ptr<feature::feature_registry> features,
        std::unique_ptr<structure::structure_template_manager> templates);

    [[nodiscard]] static auto
    create_with_shared(chunk_generator_config config,
                       std::shared_ptr<const shared_generator_data> shared)
        -> result<std::unique_ptr<chunk_generator>>;

    [[nodiscard]] auto generate_impl(const chunk_pos &pos)
        -> result<std::unique_ptr<proto_chunk>>;

    /// Generates one tile. Implemented in the `.cpp` so the tile staging can
    /// be written as one sequence: fill the tile and its rings, place the
    /// structures once, repair shapes, decorate every chunk of the tile and
    /// its ring in force-load order, then serve the inner tile.
    [[nodiscard]] auto generate_tile_impl(const chunk_pos &origin)
        -> result<std::vector<std::unique_ptr<proto_chunk>>>;

    /// Decoration and terrain radii of the per-chunk pipeline, in chunks. A
    /// served chunk runs its own features step, so it needs the decoration of
    /// its eight neighbours (the write radius) and one further terrain ring
    /// those decorations read. Structure starts keep the reference search
    /// radius.
    static constexpr jint pipeline_decoration_radius = 1;
    static constexpr jint pipeline_terrain_radius = 2;
    static constexpr jint pipeline_structure_radius = 8;

    /// The structure starts of one chunk rectangle, copied into one list in
    /// ascending chunk order because the beard and reference collectors take
    /// a contiguous vector.
    struct cached_start_window {
        std::vector<structure::structure_start> starts{};
    };

    /// Starts owned by `pos`, generated on first request and cached. The
    /// result is a pure function of the seed and the position: the multi-noise
    /// search is reset before the scan, so a cached entry never depends on
    /// which chunk ran before it.
    [[nodiscard]] auto cached_start_at(const chunk_pos &pos)
        -> result<const std::vector<structure::structure_start> *>;

    /// Copies the starts of every chunk of `box` into one list, ascending x
    /// then z. Doubles as the bound check for the pipeline.
    [[nodiscard]] auto start_window(const chunk_rectangle &box)
        -> result<cached_start_window>;

    /// The start window one served chunk needs, with the one-entry cache that
    /// lets the three status jobs of the same position share the copy. The
    /// pointer stays valid until the next position is requested or the proto
    /// cache is cleared.
    [[nodiscard]] auto pipeline_window(const chunk_pos &pos)
        -> result<const cached_start_window *>;

    /// Materialises the chunks of `box` at `terrain` status, reading the
    /// beardifier inputs of `starts`.
    [[nodiscard]] auto
    ensure_terrain(const chunk_rectangle &box,
                   const std::vector<structure::structure_start> &starts)
        -> result<void>;

    /// Places the structure pieces of every chunk of `positions` before any
    /// feature runs, like the tile staging. `box` is the terrain the pieces
    /// read.
    [[nodiscard]] auto ensure_structure_pieces(
        std::span<const chunk_pos> positions, const chunk_rectangle &box,
        const std::vector<structure::structure_start> &starts) -> result<void>;

    /// Runs the features step of every chunk of `positions` that has not run
    /// it yet, ascending. `box` is the terrain and decorated neighbourhood
    /// the pass reads and writes.
    [[nodiscard]] auto ensure_decorated(std::span<const chunk_pos> positions,
                                        const chunk_rectangle &box)
        -> result<void>;

    /// Copies a served chunk out of the cache: its blocks, heightmaps,
    /// structure records (owned starts plus the references its start window
    /// produces) and the configured status.
    [[nodiscard]] auto
    serve_cached(const chunk_pos &pos,
                 const std::vector<structure::structure_start> &starts)
        -> result<std::unique_ptr<proto_chunk>>;

    /// Attaches every cached chunk of `box` to `region`. A served chunk is
    /// read-only: the neighbours still read it and drop their writes into it.
    void attach_cached(chunk_region &region, const chunk_rectangle &box);

    /// The structure-start context of this generator. A lazy height or block
    /// probe that fails stores its error in `error` and keeps returning a
    /// sentinel; the caller checks it after the starts stage.
    [[nodiscard]] auto make_start_context(std::optional<error> &error)
        -> structure::structure_start_context;

    /// Fills the terrain stages of one chunk with `beard` as its beardifier
    /// input; null means no beardifier. Resets the multi-noise search first, so
    /// the biome grid does not depend on the chunks filled before it.
    [[nodiscard]] auto fill_terrain(const chunk_pos &pos,
                                    const structure::structure_beard *beard)
        -> result<std::unique_ptr<proto_chunk>>;

    /// The candidate biome plan of one decorated chunk: the fixed biome when
    /// the generator pins one, otherwise the distinct biomes its own
    /// three-by-three palette holds. The chunks must already be attached to
    /// `region`.
    [[nodiscard]] auto candidate_biomes_at(const chunk_region &region,
                                           const chunk_pos &position) const
        -> std::vector<std::string>;

    /// One chunk a decoration pass decorates, with the candidate biomes its
    /// own three-by-three biome window contributes. The plan is the generator
    /// wide sorted feature plan; the candidates select which of its features
    /// this decoration considers.
    struct decoration_target {
        chunk_pos position{};
        proto_chunk *chunk{nullptr};
        const std::vector<std::string> *candidate_biomes{nullptr};
    };

    /// Decorates `targets` in order through `region`. Writes are confined to
    /// the one-chunk write radius around each target, like the reference
    /// generation region. Builds the place and temperature state once, so the
    /// biome search hint never depends on how many targets the pass has.
    [[nodiscard]] auto
    decorate_chunks(chunk_region &region,
                    std::span<const decoration_target> targets) -> result<void>;

    /// The structure placement context of one pass and the feature placers
    /// its pieces resolve. Owned through a unique_ptr because the context
    /// callback points back at the placers.
    struct structure_placement;

    [[nodiscard]] auto make_structure_placement(
        chunk_region &region,
        std::span<const structure::structure_start *const> starts)
        -> result<std::unique_ptr<structure_placement>>;

    /// The generation biome and climate a structure feature pool element reads
    /// at a world position. The position's chunk must be attached to `region`;
    /// outside it the probes report no biome and no temperature and the
    /// weather features leave the position alone.
    [[nodiscard]] auto structure_biome_at(const chunk_region &region,
                                          const block_pos &block) const
        -> std::string_view;
    [[nodiscard]] auto structure_temperature_at(const chunk_region &region,
                                                const block_pos &block) const
        -> std::optional<float>;

    /// One cached unbearded start column: the six heightmap rows the starts
    /// stage reads. The column is filled at `biomes` status, so the four final
    /// heightmaps stay at their empty value, exactly as a freshly generated
    /// column reports them.
    struct start_height_column {
        std::array<std::array<jint, 16 * 16>, heightmap_kind_count> heights{};
    };

    /// One vertical block column a structure start reads. The fossil start
    /// scans a single world column, so the cache stores that column instead of
    /// the generated chunk.
    struct block_column_key {
        chunk_pos chunk{};
        jint x{0};
        jint z{0};

        friend constexpr auto operator==(const block_column_key &,
                                         const block_column_key &)
            -> bool = default;
    };

    struct block_column_hash {
        [[nodiscard]] auto
        operator()(const block_column_key &key) const noexcept -> std::size_t {
            const auto base = chunk_pos_hash{}(key.chunk);
            return base ^ (static_cast<std::size_t>(
                               static_cast<std::uint32_t>(key.x)) *
                               0x9E3779B97F4A7C15ULL +
                           static_cast<std::size_t>(
                               static_cast<std::uint32_t>(key.z)));
        }
    };

    /// Returns the cached column, generating it on the first request. The
    /// cache is per generator instance, so its size covers the sliding start
    /// window of a contiguous batch without growing with the batch length.
    /// Worker instances for a batch: this generator plus the extra ones the
    /// previous batches created, created up to `worker_count` if needed.
    [[nodiscard]] auto worker_instances(std::size_t worker_count)
        -> result<std::vector<chunk_generator *>>;
    [[nodiscard]] auto start_height_at(const chunk_pos &column)
        -> result<const start_height_column *>;
    /// Unbearded block state of one position. Reads a cached vertical column
    /// and only generates the height column when that block column is new.
    [[nodiscard]] auto start_block_at(const chunk_pos &column, jint x, jint y,
                                      jint z) -> result<block_state_id>;
    auto store_height_column(const chunk_pos &column, start_height_column value)
        -> const start_height_column *;
    auto store_block_column(block_column_key key,
                            std::vector<block_state_id> states)
        -> const std::vector<block_state_id> *;

    /// Pristine terrain of one neighbour, or null when it is not cached. The
    /// returned chunk is shared and must be cloned before a stage writes to
    /// it. `beard` is the beardifier input the caller is about to fill with;
    /// null means no beardifier. A cached chunk only matches the exact beard
    /// it was generated with, because the beardifier changes the density the
    /// fill samples.
    [[nodiscard]] auto
    cached_terrain(const chunk_pos &pos,
                   const structure::structure_beard *beard) const
        -> const proto_chunk *;
    /// Keeps a pristine terrain chunk for the neighbour fills of later
    /// targets. `beard` is the beardifier input the chunk was filled with.
    void store_terrain(const chunk_pos &pos,
                       const structure::structure_beard *beard,
                       std::unique_ptr<proto_chunk> chunk);
    /// One cached pristine chunk and the beardifier input it was filled with.
    struct cached_terrain_entry {
        bool bearded{false};
        structure::structure_beard beard{};
        std::unique_ptr<proto_chunk> chunk{};
    };
    /// Number of pristine chunks a worker keeps. A target fills its eight
    /// neighbours, so the sliding window of a batch is at most three columns
    /// of chunks; the bound covers that window without growing with the batch.
    static constexpr std::size_t terrain_cache_capacity = 64;
    std::unordered_map<chunk_pos, cached_terrain_entry, chunk_pos_hash>
        terrain_cache_{};
    std::deque<chunk_pos> terrain_cache_order_{};

    chunk_generator_config config_{};
    std::shared_ptr<const shared_generator_data> shared_{};
    std::unique_ptr<dimension_generator> dimension_{};
    std::unique_ptr<feature::feature_registry> features_{};
    std::unique_ptr<structure::structure_template_manager> templates_{};

    /// Working set of the per-chunk pipeline: terrain, the neighbour feature
    /// writes and the served chunks of this worker. Separate from the
    /// single-chunk neighbour cache above so neither path changes the other.
    proto_chunk_cache proto_cache_{
        static_cast<std::size_t>(proto_chunk_cache::default_capacity)};
    /// Structure starts of the per-chunk pipeline, keyed by the chunk that
    /// owns them. Bounded like the chunk cache; a missed chunk is regenerated
    /// from the seed and the position.
    std::unordered_map<chunk_pos, std::vector<structure::structure_start>,
                       chunk_pos_hash>
        start_cache_{};
    std::deque<chunk_pos> start_cache_order_{};
    static constexpr std::size_t start_cache_capacity = 4096;
    /// The start window of the pipeline position currently being advanced.
    /// The three status jobs of one chunk reuse it, so a chunk does not copy
    /// the radius-eight start window three times.
    std::optional<std::pair<chunk_pos, cached_start_window>>
        pipeline_window_cache_{};
    /// Extra generators created by the batch entry points. They share
    /// `shared_` and keep their registries alive for the chunks they returned.
    std::vector<std::unique_ptr<chunk_generator>> workers_{};
    /// Bounded per-worker caches for the structure-start probes. Height
    /// columns are pure functions of the seed and data, so reusing them does
    /// not change the generated chunk.
    std::unordered_map<chunk_pos, start_height_column, chunk_pos_hash>
        height_columns_{};
    std::deque<chunk_pos> height_column_order_{};
    std::unordered_map<block_column_key, std::vector<block_state_id>,
                       block_column_hash>
        block_columns_{};
    std::deque<block_column_key> block_column_order_{};
    /// Optional per-stage accounting owned by the caller. Null in production.
    stage_profile *profile_{nullptr};
    bool has_structures_{false};
    bool has_features_{false};
};

/// One step of the per-chunk status pipeline. The steps run in this order for
/// one chunk: terrain fills its dependency ring, decorated places the
/// structures and runs the feature step of its three-by-three, and served
/// copies the finished chunk out.
enum class pipeline_step : std::uint8_t {
    terrain = 0,
    decorated = 1,
    served = 2,
};

[[nodiscard]] auto pipeline_step_name(pipeline_step step) -> std::string_view;

/// One unit of pipeline work. A job belongs to the worker that owns the
/// generator cache it must run on: a generator's block state table and proto
/// cache are thread-private, so a chunk never moves between workers.
struct pipeline_job {
    pipeline_step step{pipeline_step::terrain};
    chunk_pos position{};
    std::size_t worker{0};
};

/// One chunk the scheduler could not complete.
struct pipeline_failure {
    chunk_pos position{};
    std::string message{};
};

/// Counters a caller reads to confirm the scheduler and its caches are doing
/// the work.
struct chunk_scheduler_stats {
    /// Chunks demanded since construction.
    std::size_t demanded{0};
    /// Chunks served since construction.
    std::size_t served{0};
    /// Chunks whose pipeline failed since construction.
    std::size_t failed{0};
    /// Demanded chunks whose pipeline has not completed yet.
    std::size_t pending{0};
    /// Cached proto chunks across every worker.
    std::size_t cache_entries{0};
    /// Proto cache entries evicted over the worker cache bounds.
    std::uint64_t cache_evictions{0};
};

/// Runs per-chunk status jobs on a fixed set of worker generators.
///
/// A demanded chunk queues its three status jobs on one worker, in pipeline
/// order, and the worker runs them on its own generator and proto cache. The
/// worker keeps the cache between jobs, so the terrain and decoration of the
/// chunks around the served one are shared across requests instead of being
/// regenerated, and a chunk is served as soon as its own dependencies are
/// decorated rather than after a whole square of chunks.
///
/// Every scheduling decision is taken from the demand order and the job
/// position, never from which worker finished first: the worker of a chunk is
/// fixed when it is demanded (round-robin in demand order), its jobs stay in
/// that worker's queue, and completions only move a chunk from pending to
/// served or failed. A caller that demands the same positions in the same order
/// therefore runs the same jobs in the same order on the same workers.
///
/// The workers of one scheduler are independent: each owns its generator,
/// block state table and proto cache. This is why a scheduler can serve chunks
/// in parallel without a lock around the generator, and why the served content
/// is only guaranteed to be identical for the same worker count and demand
/// sequence.
class chunk_scheduler {
  public:
    struct config {
        chunk_generator_config generator{};
        /// Worker generators; each one also bounds one proto cache.
        std::size_t workers{1};
    };

    [[nodiscard]] static auto create(config settings)
        -> result<std::unique_ptr<chunk_scheduler>>;
    chunk_scheduler(const chunk_scheduler &) = delete;
    auto operator=(const chunk_scheduler &) -> chunk_scheduler & = delete;
    chunk_scheduler(chunk_scheduler &&) = delete;
    auto operator=(chunk_scheduler &&) -> chunk_scheduler & = delete;
    ~chunk_scheduler() = default;

    /// Queue the three status jobs of `pos` on one worker. Returns false when
    /// the position is already demanded and has not completed yet.
    auto demand(const chunk_pos &pos) -> bool;

    /// Whether `worker` has a job to run. Failed-position jobs are still
    /// queued and are skipped by `next_job`, so a non-empty queue is enough
    /// for the caller's wait predicate.
    [[nodiscard]] auto has_job(std::size_t worker) const -> bool;

    /// Pop the next job of `worker`. Jobs of a position that already failed
    /// are skipped. Returns nothing when the worker has no queued job.
    [[nodiscard]] auto next_job(std::size_t worker)
        -> std::optional<pipeline_job>;

    /// Run one job on its worker's generator. The served step returns the
    /// finished chunk; the two earlier steps return a null pointer. A worker
    /// generator is created on its first job, so a scheduler costs nothing
    /// until it is used and a missing datapack fails the jobs instead of the
    /// scheduler.
    [[nodiscard]] auto run(const pipeline_job &job)
        -> result<std::unique_ptr<proto_chunk>>;

    /// Record the outcome of a finished job. A failure is reported once for
    /// the chunk and the chunk's remaining jobs are skipped.
    void complete(const pipeline_job &job, const std::string *error_message);

    /// Failures since the previous call, ascending position.
    [[nodiscard]] auto take_failures() -> std::vector<pipeline_failure>;

    /// Biome keys of the scheduler's dimension. Every worker generator loads
    /// the same data, so one copy names the biome ids of every served chunk.
    /// Empty until a worker generator was created.
    [[nodiscard]] auto biome_names() const -> const std::vector<std::string> &;

    [[nodiscard]] auto pending() const -> std::size_t;

    [[nodiscard]] auto stats() const -> chunk_scheduler_stats;

    /// Drops every cached chunk of every worker. The scheduler keeps running
    /// after it; the next demand rebuilds its neighbourhood from the seed.
    void clear();

  private:
    explicit chunk_scheduler(config settings);

    /// The generator of `worker`, created on the first job of that worker.
    /// Only the worker's own thread calls this, so a generator is never used
    /// by two threads; creation is serialised because every later worker
    /// reuses the first worker's immutable data.
    [[nodiscard]] auto worker_generator(std::size_t worker)
        -> result<chunk_generator *>;

    struct chain_state {
        std::uint8_t remaining{3};
    };

    config settings_{};
    mutable std::mutex mutex_{};
    mutable std::mutex create_mutex_{};
    std::vector<std::unique_ptr<chunk_generator>> workers_{};
    std::vector<std::deque<pipeline_job>> queues_{};
    std::unordered_map<chunk_pos, chain_state, chunk_pos_hash> chains_{};
    std::unordered_set<chunk_pos, chunk_pos_hash> failed_{};
    std::vector<pipeline_failure> failures_{};
    std::vector<std::string> biome_names_{};
    std::optional<std::string> generator_error_{};
    /// Per-worker cache counters, captured on the worker's own thread after a
    /// job so a reader never inspects a generator the worker is using.
    std::vector<std::size_t> worker_entries_{};
    std::vector<std::uint64_t> worker_evictions_{};
    std::size_t next_worker_{0};
    std::size_t demanded_{0};
    std::size_t served_{0};
    std::size_t failed_count_{0};
};

} // namespace refrain::worldgen
