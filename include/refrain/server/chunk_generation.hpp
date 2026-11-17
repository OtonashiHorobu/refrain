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

namespace refrain::worldgen {
class chunk_scheduler;
}

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

/// The column a full queue evicts: the farthest from the centres, greatest
/// position on a tie. `wanted` must not be empty. Keeping the nearest columns
/// is what lets a walking player join a queue that stale work already filled.
[[nodiscard]] auto
farthest_column(std::span<const refrain::world::chunk_pos> wanted,
                std::span<const refrain::world::chunk_pos> centres)
    -> refrain::world::chunk_pos;

/// Orders `wanted` columns by Chebyshev distance to the nearest accepted
/// centre, then to the nearest look-ahead centre, then by position. An empty
/// accepted-centre list ranks every column equally, so the look-ahead list and
/// the position tie-break decide. The two lists are ranked lexicographically,
/// not merged: a column the accepted viewport already needs beats every column
/// only the look-ahead needs, so movement prediction can never starve the
/// viewport the client is standing in.
[[nodiscard]] auto
rank_columns_by_distance(std::span<const refrain::world::chunk_pos> wanted,
                         std::span<const refrain::world::chunk_pos> accepted,
                         std::span<const refrain::world::chunk_pos> lookahead)
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

/// One requested column whose per-chunk pipeline failed.
///
/// The column is the admission and accounting unit, so a failure covers
/// exactly one chunk: no column was produced for it. The message says why the
/// chunk failed and is meant for the operator log.
struct generation_failure {
    refrain::world::chunk_pos position{};
    std::string message{};
};

/// Generates world columns off the simulation thread.
///
/// A fixed pool of workers runs at startup; each worker drives its own
/// generator through the per-chunk status scheduler, so the number of threads
/// never changes with the load. What adapts is admission: the simulation
/// thread requests columns, the once-per-second controller sets a credit
/// level, and `dispatch()` hands the nearest wanted columns to the scheduler
/// only while credits, tick headroom and the client backlog allow.
///
/// A requested column becomes three status jobs on one worker: terrain fills
/// its dependency ring, decorated places the structures and runs the feature
/// step of its three-by-three, and served copies the chunk out. The worker's
/// cache keeps the chunks around the served one, so neighbours share their
/// terrain and decoration across requests, and a column is served as soon as
/// its own status dependencies are decorated instead of after a whole square
/// of columns. The workers never touch the world, so the only shared state is
/// the wanted set, the scheduler queues, the counters and the result queue
/// under one mutex.
class chunk_generation {
  public:
    struct config {
        std::string data_root{};
        std::string templates_root{};
        std::int64_t seed{0};
        /// Generation worker threads; zero derives the count from the host.
        std::size_t worker_count{0};
        /// Threads kept for the simulation and IO lanes when deriving
        /// `worker_count`, so generation does not oversubscribe the machine.
        std::uint32_t sim_threads{1};
        std::uint32_t io_threads{2};
        /// Upper bound on wanted columns; further requests are refused so a
        /// burst of viewport changes cannot grow the queue without bound.
        std::size_t max_pending{512};
        /// Proto chunks each worker's generator keeps in its pipeline cache.
        /// The cache holds the terrain and feature writes of the chunks around
        /// the served one; an entry is about one terrain chunk (roughly 200 KiB
        /// at full height), so the default is a few tens of megabytes per
        /// worker. Entries over the bound are evicted in insertion order and an
        /// evicted chunk is regenerated from the seed on a later request.
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

    /// Queue one column. Returns false when the column is already wanted or in
    /// flight, the queue is full, or generation is stopping; a column that is
    /// already wanted is still served when it completes.
    auto request(refrain::world::chunk_pos pos) -> bool;

    /// Drop one wanted column (the caller no longer needs it). A column that
    /// was already admitted still completes.
    void forget(refrain::world::chunk_pos pos);

    /// Centres the dispatch ranks wanted columns by, nearest first. The
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

    /// Admit wanted columns to the scheduler while credits and tick-budget
    /// headroom allow, nearest first. `tick_elapsed_ns` is the work the
    /// current tick already did: once it passes the headroom the tick stops
    /// admitting. Returns the number of columns admitted.
    auto dispatch(std::uint64_t tick_elapsed_ns) -> std::size_t;

    /// Take every finished column, ordered by position so the order the world
    /// applies them never depends on which worker finished first.
    [[nodiscard]] auto take_results() -> std::vector<
        std::pair<refrain::world::chunk_pos, refrain::world::chunk>>;

    /// Same, but at most `max_columns` columns; the rest stay queued, ordered
    /// by position, for the next call. A limit of zero takes nothing.
    ///
    /// The caller uses this to spread finished columns over several ticks:
    /// every adoption runs the column decode, the entity spawn and the initial
    /// light, which is the tick spike a joining client pays for. The queue is
    /// bounded by the task credits plus the columns already finished, so a
    /// slow adopter applies backpressure to admission instead of growing
    /// memory.
    [[nodiscard]] auto take_results(std::size_t max_columns) -> std::vector<
        std::pair<refrain::world::chunk_pos, refrain::world::chunk>>;

    /// Take every column whose pipeline failed since the last call, ordered
    /// by position. A failure produces no column; the caller tells the
    /// streamer so it can request the position again instead of waiting for an
    /// answer that will never arrive.
    [[nodiscard]] auto take_failures() -> std::vector<generation_failure>;

    /// Columns of finished chunks the service may have queued for adoption
    /// before admission stops flattening more. The service adopts a few per
    /// tick, so without this bound a fast generator and a slow adopter could
    /// hold every column a whole credit window produced in memory. At the
    /// bound the finished chunks stay in flight: their admission credit is not
    /// released, so generation pauses instead of growing the queue.
    static constexpr std::size_t max_ready_columns{8 * 64};

    /// Cached proto chunks across every worker. Bounded by the configured
    /// per-worker proto cache capacity.
    [[nodiscard]] auto cache_entries() const -> std::size_t;
    /// Proto chunks evicted over the per-worker cache bounds since the start.
    [[nodiscard]] auto cache_evictions() const -> std::uint64_t;

    [[nodiscard]] auto credits() const -> std::size_t;
    [[nodiscard]] auto queue_depth() const -> std::size_t;
    [[nodiscard]] auto in_flight() const -> std::size_t;
    [[nodiscard]] auto worker_count() const noexcept -> std::size_t;

    /// Stop every worker and join them.
    void stop();

  private:
    explicit chunk_generation(config settings);

    /// One worker's run loop: wait for a scheduler job of this worker, run
    /// it, convert a served chunk to a column and report the outcome.
    void run(std::size_t worker);

    /// Clear the in-flight markers of `pos` and report the failure so the
    /// caller can ask for the column again. Runs on a worker thread.
    void fail_task(refrain::world::chunk_pos pos, std::string message);

    config settings_{};
    mutable std::mutex mutex_{};
    std::condition_variable wake_{};
    /// Wanted column positions for deduplication.
    std::set<std::pair<std::int32_t, std::int32_t>> wanted_{};
    /// Admitted columns the simulation has not applied yet; a column is never
    /// admitted twice.
    std::set<std::pair<std::int32_t, std::int32_t>> active_{};
    std::vector<refrain::world::chunk_pos> centres_{};
    std::vector<refrain::world::chunk_pos> lookahead_centres_{};
    /// Per-chunk status scheduler and the workers that drive it.
    std::unique_ptr<worldgen::chunk_scheduler> scheduler_{};
    /// Columns that finished since the last `take_results`, ordered by
    /// position when they are flattened.
    std::vector<std::pair<refrain::world::chunk_pos, refrain::world::chunk>>
        finished_{};
    /// Columns of finished chunks, ordered by position, that `take_results`
    /// has not handed out yet.
    std::map<std::pair<std::int32_t, std::int32_t>, refrain::world::chunk>
        ready_{};
    /// Columns whose pipeline failed since the last `take_failures()`.
    std::vector<generation_failure> failed_{};
    std::size_t in_flight_{0};
    admission_controller admission_{};
    bool stopping_{false};
    std::vector<std::thread> workers_{};
    /// Only the first worker that cannot create its generator reports it;
    /// the rest fail the same way and would only repeat the line.
    std::atomic<bool> generator_failure_reported_{false};
};

} // namespace refrain::server
