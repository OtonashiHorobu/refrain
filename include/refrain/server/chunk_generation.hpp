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
        /// Upper bound on wanted tiles; further requests are refused so a
        /// burst of viewport changes cannot grow the queue without bound.
        std::size_t max_pending{512};
        /// Edge in chunks of the square one generation task produces. A tile
        /// is filled and decorated with one ring around it before any of its
        /// chunks is served, so cross-border feature writes land in the
        /// chunks a client receives. The tile is also the unit the priority
        /// queue and the admission credits count.
        std::int32_t tile_size{8};
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

    /// Queue the tile that contains one column. Returns false when the tile is
    /// already wanted or in flight, the queue is full, or generation is
    /// stopping; the column is still served when its tile completes.
    auto request(refrain::world::chunk_pos pos) -> bool;

    /// Drop the tile that contains one column (the caller no longer needs it).
    void forget(refrain::world::chunk_pos pos);

    /// Centres the dispatch ranks wanted tiles by, nearest first. The
    /// simulation thread refreshes them from the player positions each tick.
    void
    set_priority_centres(std::span<const refrain::world::chunk_pos> centres);

    /// Run the credit controller for one second of samples. Logs one info line
    /// per change and nothing otherwise.
    void update_admission(const admission_sample &sample);

    /// Admit wanted tiles to the workers while credits and tick-budget
    /// headroom allow, nearest first. `tick_elapsed_ns` is the work the
    /// current tick already did: once it passes the headroom the tick stops
    /// admitting. Returns the number of tiles admitted.
    auto dispatch(std::uint64_t tick_elapsed_ns) -> std::size_t;

    /// Take every column of every finished tile, ordered by position so the
    /// order the world applies them never depends on which worker finished
    /// first.
    [[nodiscard]] auto take_results() -> std::vector<
        std::pair<refrain::world::chunk_pos, refrain::world::chunk>>;

    [[nodiscard]] auto credits() const -> std::size_t;
    [[nodiscard]] auto queue_depth() const -> std::size_t;
    [[nodiscard]] auto in_flight() const -> std::size_t;
    [[nodiscard]] auto worker_count() const noexcept -> std::size_t;

    /// Stop every worker and join them.
    void stop();

  private:
    explicit chunk_generation(config settings);

    void run();

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
    std::deque<refrain::world::chunk_pos> work_{};
    std::vector<tile_result> finished_{};
    std::size_t in_flight_{0};
    admission_controller admission_{};
    bool stopping_{false};
    std::vector<std::thread> workers_{};
    /// Only the first worker that cannot create its generator reports it;
    /// the rest fail the same way and would only repeat the line.
    std::atomic<bool> generator_failure_reported_{false};
};

} // namespace refrain::server
