// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/world/model.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace refrain::app {
struct content_view_snapshot;
} // namespace refrain::app

namespace refrain::worldgen {
class chunk_scheduler;
class generation_data;
} // namespace refrain::worldgen

namespace refrain::server {

/// The three inputs the generation credit controller reads once per second.
struct admission_sample {
    /// Columns the viewport wants that have no credit yet.
    std::size_t queue_depth{0};
    /// Simulation tick work p99 over the last second, in milliseconds.
    double tick_p99_ms{0.0};
    /// Resident set of the process, or zero when the host cannot report it.
    std::uint64_t resident_bytes{0};
};

/// Why the controller moved the credit level.
enum class admission_reason : std::uint8_t {
    /// A tick exceeded its budget: cut now.
    tick_p99 = 0,
    /// Resident memory crossed its bound: cut now.
    memory = 1,
    /// Demand outgrew the credits while the tick stayed inside its headroom:
    /// raise after the hysteresis window.
    queue_depth = 2,
};

[[nodiscard]] auto to_string(admission_reason reason) -> std::string_view;

/// One move of the credit level.
struct admission_change {
    std::size_t credits{0};
    admission_reason reason{admission_reason::queue_depth};
};

/// Orders `wanted` columns by Chebyshev distance to the nearest centre, then
/// by position. An empty centre list ranks every column equally, so the
/// position tie-break alone decides. Admission hands columns to the workers in
/// this order, which is what makes a walking player's viewport fill first.
[[nodiscard]] auto
rank_columns_by_distance(std::span<const refrain::world::chunk_pos> wanted,
                         std::span<const refrain::world::chunk_pos> centres)
    -> std::vector<refrain::world::chunk_pos>;

/// Same, with the look-ahead centres as the secondary priority: the accepted
/// viewport ranks lexicographically before the prediction, so a look-ahead
/// column can never starve a column the client is standing in. Used by the
/// experimental per-chunk queue, which ranks columns rather than tiles.
[[nodiscard]] auto
rank_columns_by_distance(std::span<const refrain::world::chunk_pos> wanted,
                         std::span<const refrain::world::chunk_pos> accepted,
                         std::span<const refrain::world::chunk_pos> lookahead)
    -> std::vector<refrain::world::chunk_pos>;

/// The column a full queue evicts: the farthest from the centres, greatest
/// position on a tie. `wanted` must not be empty. Keeping the nearest columns
/// is what lets a walking player join a queue that stale work already filled.
[[nodiscard]] auto
farthest_column(std::span<const refrain::world::chunk_pos> wanted,
                std::span<const refrain::world::chunk_pos> centres)
    -> refrain::world::chunk_pos;

/// Orders `wanted` tiles by how soon a player at one of `accepted` centres
/// needs them, then by how soon a player heading for one of `lookahead`
/// centres needs them, then by the tile corner.
///
/// The two centre lists are ranked lexicographically, not merged: a tile the
/// accepted viewport already needs beats every tile only the look-ahead needs,
/// so a prediction can never starve the viewport the client is standing in.
/// The look-ahead list is a secondary key only, which is what makes movement
/// prediction a priority hint instead of a second viewport.
[[nodiscard]] auto
rank_tiles_by_distance(std::span<const refrain::world::chunk_pos> wanted,
                       std::span<const refrain::world::chunk_pos> accepted,
                       std::span<const refrain::world::chunk_pos> lookahead,
                       std::int32_t tile_size)
    -> std::vector<refrain::world::chunk_pos>;

/// Tuning for the credit controller.
struct admission_limits {
    std::size_t min_credits{1};
    std::size_t max_credits{8};
    std::size_t initial_credits{2};
    /// Tick period the sampled p99 is compared against.
    double budget_ms{50.0};
    /// A raise needs the p99 at or below this fraction of the budget, so a
    /// server that sits close to the limit does not ratchet itself into it.
    double headroom{0.8};
    /// Resident memory above this bound is a miss; zero disables the input.
    std::uint64_t memory_bound_bytes{2ULL * 1024ULL * 1024ULL * 1024ULL};
    /// Consecutive good samples needed to add one credit.
    std::uint32_t raise_after_samples{3};
};

/// Once-per-second credit controller for generation admission.
///
/// It is deliberately a plain value type with an explicit `update(sample)`
/// seam: the tick never calls it, and a unit test feeds it a synthetic
/// sequence without a server. A miss (tick p99 over the budget, or resident
/// memory over its bound) cuts the level on that sample; a raise needs
/// `raise_after_samples` consecutive good samples with more demand than
/// credits, which keeps a single noisy second from oscillating the level.
class admission_controller {
  public:
    explicit admission_controller(admission_limits limits = {});

    /// Feed one second of samples. Returns the new level when it moved.
    [[nodiscard]] auto update(const admission_sample &sample)
        -> std::optional<admission_change>;

    [[nodiscard]] auto credits() const noexcept -> std::size_t {
        return credits_;
    }
    [[nodiscard]] auto limits() const noexcept -> const admission_limits & {
        return limits_;
    }

  private:
    [[nodiscard]] auto cut(admission_reason reason)
        -> std::optional<admission_change>;

    admission_limits limits_{};
    std::size_t credits_{0};
    std::uint32_t good_samples_{0};
};

/// One generation task that did not produce its columns.
///
/// The task is the admission and accounting unit: a tile on the production
/// path and one column on the experimental per-chunk path. `tile_size()`
/// returns the edge the failed task covers, which is one on the per-chunk
/// path. The message says why the task failed and is meant for the operator
/// log.
struct generation_failure {
    refrain::world::chunk_pos tile{};
    std::string message{};
};

/// Generates world columns off the simulation thread.
///
/// A fixed pool of workers runs at startup; each worker owns its own generator
/// (a generator instance is not thread-safe), so the number of threads never
/// changes with the load. What adapts is admission: the simulation thread
/// requests positions, the once-per-second controller sets a credit level, and
/// `dispatch()` hands the nearest wanted columns to the workers only while
/// credits, tick headroom and the client backlog allow. The workers never
/// touch the world, so the only shared state is the wanted set, the work
/// queue, the counters and the result queue under one mutex.
///
/// The production path generates aligned tiles: a worker produces a whole
/// `tile_size` square and the columns of one tile are adopted together. The
/// experimental `experimental_chunk_pipeline` flag replaces the tile jobs with
/// the per-chunk status scheduler, which serves a column as soon as its own
/// three-by-three is decorated, at the cost of content that depends on how
/// many workers share the sequence. The flag is off by default and the server
/// composition never sets it.
class chunk_generation {
  public:
    struct config {
        /// Installed datapack `data` root. It stays the boot content root; a
        /// published content view that changes documents this generator reads
        /// is composed on top of it by the bound generation.
        std::string data_root{};
        std::string templates_root{};
        std::int64_t seed{0};
        /// Generation worker threads; zero derives the count from the host.
        std::size_t worker_count{0};
        /// Threads kept for the simulation and IO lanes when deriving
        /// `worker_count`, so generation does not oversubscribe the machine.
        std::uint32_t sim_threads{1};
        std::uint32_t io_threads{2};
        /// Upper bound on wanted tiles; further requests are refused so a
        /// burst of viewport changes cannot grow the queue without bound.
        std::size_t max_pending{512};
        /// Edge in chunks of the square one generation task produces. A tile
        /// is filled and decorated with one ring around it before any of its
        /// chunks is served, so cross-border feature writes land in the
        /// chunks a client receives. The tile is also the unit the priority
        /// queue and the admission credits count.
        ///
        /// A smaller tile shortens the wait for the first chunks of a join
        /// but costs fill and decoration work per served chunk: the fill and
        /// decoration ring is `(edge + 2) ^ 2 / edge ^ 2` times the served
        /// area, and the structure-start scan reaches eight chunks past it.
        /// Measured per served chunk on the development host, edge eight costs
        /// about 0.53 of the edge-four cost and 0.24 of the edge-two cost.
        ///
        /// The production default stays eight for a second reason: with the
        /// dimension's own biomes the served content is not edge neutral,
        /// because a ring chunk's feature placement reads the terrain outside
        /// the tile and a smaller edge brings a smaller decorated ring. Only a
        /// fixed biome makes both edges produce the same chunk. Changing the
        /// default therefore needs a generator change that decorates the ring
        /// of the served square rather than of the tile, not only a latency
        /// measurement.
        std::int32_t tile_size{8};
        /// EXPERIMENTAL: serve columns through the per-chunk status scheduler
        /// instead of the production tile path. Off by default and not wired
        /// to any server option, so a production server never enables it.
        ///
        /// The scheduler runs one shared deterministic pipeline state: a frozen
        /// content block-state table, one proto chunk cache and one ordered
        /// decoration lane. The served content is the serial per-chunk
        /// pipeline's content and does not depend on the worker count, which
        /// the scheduler benchmark hashes at one, four and eight workers. The
        /// flag stays off because the production tile path is still the only
        /// path with the full reference-oracle, spawn and viewport coverage; a
        /// separate change switches the default once those are in place.
        bool experimental_chunk_pipeline{false};
        /// Proto chunks each worker's generator keeps when
        /// `experimental_chunk_pipeline` is on. Ignored by the tile path.
        std::size_t proto_cache_capacity{256};
        /// Logical CPUs the generation workers pin to; empty leaves the OS
        /// default. Pinning away from the simulation and IO cores is a hint,
        /// never a requirement.
        std::vector<std::uint32_t> worker_cpus{};
        admission_limits admission{};
    };

    [[nodiscard]] static auto start(config settings)
        -> result<std::unique_ptr<chunk_generation>>;
    ~chunk_generation();

    chunk_generation(const chunk_generation &) = delete;
    auto operator=(const chunk_generation &) -> chunk_generation & = delete;

    /// The worker count `start` derives from the host when the config leaves
    /// it at zero: `clamp(hardware_concurrency - sim_threads - io_threads, 1,
    /// 8)`.
    [[nodiscard]] static auto default_worker_count(std::uint32_t sim_threads,
                                                   std::uint32_t io_threads)
        -> std::size_t;

    /// Queue one column: through its tile on the production path, or as its
    /// own scheduler chain on the experimental path. Returns false when the
    /// unit is already wanted or in flight, the queue is full, or generation
    /// is stopping; the column is still served when its unit completes.
    auto request(refrain::world::chunk_pos pos) -> bool;

    /// Drop the wanted unit that contains one column (the caller no longer
    /// needs it).
    void forget(refrain::world::chunk_pos pos);

    /// Centres the dispatch ranks wanted tiles by, nearest first. The
    /// simulation thread refreshes them from the player positions each tick.
    ///
    /// The second list is the look-ahead centre of every moving player: it is
    /// a lower priority than the accepted positions, so a prediction can
    /// never starve the viewport the client is in. A call with one list
    /// clears the look-ahead centres.
    void
    set_priority_centres(std::span<const refrain::world::chunk_pos> centres);

    /// Same, with the look-ahead centres as the secondary priority.
    void
    set_priority_centres(std::span<const refrain::world::chunk_pos> centres,
                         std::span<const refrain::world::chunk_pos> lookahead);

    /// Run the credit controller for one second of samples. Logs one info line
    /// per change and nothing otherwise.
    void update_admission(const admission_sample &sample);

    /// Admit wanted work to the workers while credits and tick-budget
    /// headroom allow, nearest first. `tick_elapsed_ns` is the work the
    /// current tick already did: once it passes the headroom the tick stops
    /// admitting. Returns the number of tiles or columns admitted.
    auto dispatch(std::uint64_t tick_elapsed_ns) -> std::size_t;

    /// Take every column of every finished tile, ordered by position so the
    /// order the world applies them never depends on which worker finished
    /// first.
    [[nodiscard]] auto take_results() -> std::vector<
        std::pair<refrain::world::chunk_pos, refrain::world::chunk>>;

    /// Same, but at most `max_columns` columns; the rest stay queued, ordered
    /// by position, for the next call. A limit of zero takes nothing.
    ///
    /// The caller uses this to spread one finished tile over several ticks: a
    /// tile delivers up to `tile_size * tile_size` columns at once and every
    /// adoption runs the column decode, the entity spawn and the initial
    /// light, which is the tick spike a joining client pays for. The queue is
    /// bounded by the task credits plus the columns already finished, so a
    /// slow adopter applies backpressure to admission instead of growing
    /// memory.
    [[nodiscard]] auto take_results(std::size_t max_columns) -> std::vector<
        std::pair<refrain::world::chunk_pos, refrain::world::chunk>>;

    /// Take every task that failed since the last call, ordered by its minimum
    /// corner. A failure produces no columns; the caller forgets the columns
    /// the task covers so the streamer can request the position again instead
    /// of waiting for an answer that will never arrive.
    [[nodiscard]] auto take_failures() -> std::vector<generation_failure>;

    /// Columns of finished tiles the service may have queued for adoption
    /// before admission stops flattening more. The service adopts a few per
    /// tick, so without this bound a fast generator and a slow adopter could
    /// hold every column a whole credit window produced in memory. At the
    /// bound the finished tiles stay in flight: their admission credit is not
    /// released, so generation pauses instead of growing the queue.
    static constexpr std::size_t max_ready_columns{8 * 64};

    /// Edge in chunks of the square one task covers. Failures name the
    /// minimum corner of such a square, so a caller expands it by this edge.
    /// The experimental per-chunk path reports an edge of one because every
    /// task serves one column.
    [[nodiscard]] auto tile_size() const noexcept -> std::int32_t {
        return settings_.experimental_chunk_pipeline ? 1 : settings_.tile_size;
    }

    /// Whether the experimental per-chunk scheduler is driving generation.
    [[nodiscard]] auto uses_chunk_pipeline() const noexcept -> bool {
        return settings_.experimental_chunk_pipeline;
    }

    /// Cached proto chunks across the scheduler workers; zero on the tile
    /// path because that path has no proto chunk cache.
    [[nodiscard]] auto cache_entries() const -> std::size_t;

    /// Scheduler proto cache evictions; zero on the tile path.
    [[nodiscard]] auto cache_evictions() const -> std::uint64_t;

    [[nodiscard]] auto credits() const -> std::size_t;
    [[nodiscard]] auto queue_depth() const -> std::size_t;
    [[nodiscard]] auto in_flight() const -> std::size_t;
    [[nodiscard]] auto worker_count() const noexcept -> std::size_t;

    /// Published content-view epoch the tile workers are bound to. It is
    /// recorded when a worker creates its first generator and advances when a
    /// worker rebuilds that generator for a newer published generation.
    [[nodiscard]] auto bound_generation() const -> std::uint64_t;

    /// Registry epoch of the content the tile workers are bound to.
    [[nodiscard]] auto bound_registry_generation() const -> std::uint64_t;

    /// Generators rebuilt for a newer published content generation since
    /// start. Zero while the server runs without a reload.
    [[nodiscard]] auto generation_rebuilds() const -> std::uint64_t;

    /// Rebuild attempts that failed and kept the previous generator serving.
    [[nodiscard]] auto generation_rebuild_failures() const -> std::uint64_t;

    /// Stop every worker and join them.
    void stop();

  private:
    explicit chunk_generation(config settings);

    /// One worker's run loop. The index selects the scheduler worker queue on
    /// the experimental path and is unused by the tile path.
    void run(std::size_t worker);

    /// The experimental per-chunk scheduler loop: wait for a status job of
    /// this worker, run it, convert a served column and report the outcome.
    void run_pipeline(std::size_t worker);

    /// Clear the in-flight markers of `origin` and report the failure so the
    /// caller can ask for the unit again. Runs on a worker thread.
    void fail_task(refrain::world::chunk_pos origin, std::string message);

    config settings_{};
    mutable std::mutex mutex_{};
    std::condition_variable wake_{};
    /// The columns one finished tile produced. The tile is the admission and
    /// accounting unit; the columns are what the world applies.
    struct tile_result {
        refrain::world::chunk_pos origin{};
        std::vector<std::pair<refrain::world::chunk_pos, refrain::world::chunk>>
            columns{};
    };

    /// Wanted tile origins, keyed by the minimum corner for deduplication.
    std::set<std::pair<std::int32_t, std::int32_t>> wanted_{};
    /// Admitted tiles the simulation has not applied yet, keyed by the
    /// minimum corner; a tile is never admitted twice.
    std::set<std::pair<std::int32_t, std::int32_t>> active_{};
    std::vector<refrain::world::chunk_pos> centres_{};
    std::vector<refrain::world::chunk_pos> lookahead_centres_{};
    std::deque<refrain::world::chunk_pos> work_{};
    std::vector<tile_result> finished_{};
    /// Columns of finished tiles, ordered by position, that `take_results`
    /// has not handed out yet.
    std::map<std::pair<std::int32_t, std::int32_t>, refrain::world::chunk>
        ready_{};
    /// Tiles whose task failed since the last `take_failures()`.
    std::vector<generation_failure> failed_{};
    /// Experimental per-chunk status scheduler; null on the production path.
    std::unique_ptr<worldgen::chunk_scheduler> scheduler_{};
    std::size_t in_flight_{0};
    admission_controller admission_{};
    bool stopping_{false};
    std::vector<std::thread> workers_{};
    /// Only the first worker that cannot create its generator reports it;
    /// the rest fail the same way and would only repeat the line.
    std::atomic<bool> generator_failure_reported_{false};

    /// Bind a published snapshot to the installed roots, reusing the compose
    /// result when another worker already built the same published generation.
    /// The compose copies the installed datapack once per generation instead of
    /// once per worker.
    [[nodiscard]] auto
    bind_generation(const app::content_view_snapshot &snapshot)
        -> result<std::shared_ptr<const worldgen::generation_data>>;

    /// Content generation binding. Every tile worker records the published
    /// epoch its generator was built from and rebuilds it on its next task
    /// after a tick-boundary publish advanced the epoch; the swap happens on
    /// the worker thread between tasks, so a tick never waits for a rebuild,
    /// the old generator finishes the chunk it is producing and queued tiles
    /// wait in the work queue until a worker picks them up with the new
    /// generation.
    std::atomic<std::uint64_t> bound_generation_{0};
    std::atomic<std::uint64_t> bound_registry_generation_{0};
    std::atomic<std::uint64_t> generation_rebuilds_{0};
    std::atomic<std::uint64_t> generation_rebuild_failures_{0};

    /// Set once when the experimental scheduler observes a published content
    /// generation: it keeps its boot content, so a reload only reaches it
    /// after a restart.
    std::atomic<bool> scheduler_reload_notice_{false};
    bool scheduler_baseline_recorded_{false};
    std::uint64_t scheduler_observed_generation_{0};

    /// Published view epoch of the last generation rebuild that failed and was
    /// reported; later failures of the same epoch are not repeated by every
    /// worker.
    std::atomic<std::uint64_t> rebind_failure_reported_{0};

    /// The compose result of the newest published generation. Worker
    /// generators capture the shared pointer; an older generation stays alive
    /// through the generator that still uses it.
    mutable std::mutex generation_mutex_{};
    std::uint64_t generation_cache_epoch_{0};
    std::shared_ptr<const worldgen::generation_data> generation_cache_{};
};

} // namespace refrain::server
