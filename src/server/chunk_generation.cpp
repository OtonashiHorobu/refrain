// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/chunk_generation.hpp>

#include <refrain/core/log.hpp>
#include <refrain/platform/thread.hpp>
#include <refrain/world/proto_bridge.hpp>
#include <refrain/worldgen/chunk_generator.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>

namespace refrain::server {

namespace {

constexpr refrain::log::logger log_generation{"chunk-gen"};

/// The pool never grows past this, whatever the host reports: the generator is
/// memory bound well before it is CPU bound.
constexpr std::size_t max_workers{8};

[[nodiscard]] auto key_of(refrain::world::chunk_pos pos)
    -> std::pair<std::int32_t, std::int32_t> {
    return {pos.x, pos.z};
}

/// Floor division, so a negative chunk coordinate lands in the tile that
/// geometrically contains it.
[[nodiscard]] constexpr auto floor_div(std::int32_t value, std::int32_t divisor)
    -> std::int32_t {
    const auto quotient{value / divisor};
    return value % divisor != 0 && value < 0 ? quotient - 1 : quotient;
}

/// Minimum corner of the tile that contains `pos`.
[[nodiscard]] constexpr auto tile_of(refrain::world::chunk_pos pos,
                                     std::int32_t tile_size)
    -> refrain::world::chunk_pos {
    return {.x = floor_div(pos.x, tile_size) * tile_size,
            .z = floor_div(pos.z, tile_size) * tile_size};
}

/// Chebyshev distance from a chunk to the nearest chunk of the tile whose
/// minimum corner is `tile`. The distance to a rectangle is the larger of the
/// two axis distances, each zero when the coordinate lies inside the tile.
[[nodiscard]] auto
tile_distance(std::span<const refrain::world::chunk_pos> centres,
              refrain::world::chunk_pos tile, std::int32_t tile_size)
    -> std::int64_t {
    if (centres.empty()) {
        return 0;
    }
    auto best{std::numeric_limits<std::int64_t>::max()};
    for (const auto &centre : centres) {
        const auto axis = [&](std::int32_t coordinate,
                              std::int32_t minimum) -> std::int64_t {
            const auto maximum{minimum + tile_size - 1};
            if (coordinate < minimum) {
                return static_cast<std::int64_t>(minimum) - coordinate;
            }
            if (coordinate > maximum) {
                return static_cast<std::int64_t>(coordinate) - maximum;
            }
            return 0;
        };
        const auto dx{axis(centre.x, tile.x)};
        const auto dz{axis(centre.z, tile.z)};
        best = std::min(best, std::max(dx, dz));
    }
    return best;
}

/// Orders `wanted` tiles by the Chebyshev distance of their nearest chunk to
/// the nearest centre, then by the tile corner. An empty centre list ranks
/// every tile equally, so the corner tie-break alone decides.
[[nodiscard]] auto
rank_tiles_by_distance(std::span<const refrain::world::chunk_pos> wanted,
                       std::span<const refrain::world::chunk_pos> centres,
                       std::int32_t tile_size)
    -> std::vector<refrain::world::chunk_pos> {
    struct ranked {
        std::int64_t distance{0};
        std::int32_t x{0};
        std::int32_t z{0};
    };
    std::vector<ranked> entries{};
    entries.reserve(wanted.size());
    for (const auto &pos : wanted) {
        entries.push_back(
            ranked{.distance = tile_distance(centres, pos, tile_size),
                   .x = pos.x,
                   .z = pos.z});
    }
    std::sort(entries.begin(), entries.end(),
              [](const ranked &left, const ranked &right) {
                  return std::tie(left.distance, left.x, left.z) <
                         std::tie(right.distance, right.x, right.z);
              });
    std::vector<refrain::world::chunk_pos> ordered{};
    ordered.reserve(entries.size());
    for (const auto &entry : entries) {
        ordered.push_back({.x = entry.x, .z = entry.z});
    }
    return ordered;
}

/// The tile a full queue evicts: the farthest from the centres, greatest
/// corner on a tie. `wanted` must not be empty.
[[nodiscard]] auto
farthest_tile(std::span<const refrain::world::chunk_pos> wanted,
              std::span<const refrain::world::chunk_pos> centres,
              std::int32_t tile_size) -> refrain::world::chunk_pos {
    auto victim{wanted.front()};
    auto victim_distance{tile_distance(centres, victim, tile_size)};
    for (const auto &pos : wanted.subspan(1)) {
        const auto distance{tile_distance(centres, pos, tile_size)};
        if (distance > victim_distance ||
            (distance == victim_distance && key_of(pos) > key_of(victim))) {
            victim = pos;
            victim_distance = distance;
        }
    }
    return victim;
}

/// Chebyshev distance to the nearest centre; zero when there is no centre, so
/// the position tie-break alone orders the queue.
[[nodiscard]] auto
centre_distance(std::span<const refrain::world::chunk_pos> centres,
                refrain::world::chunk_pos pos) -> std::int64_t {
    if (centres.empty()) {
        return 0;
    }
    auto best{std::numeric_limits<std::int64_t>::max()};
    for (const auto &centre : centres) {
        const auto dx{std::abs(static_cast<std::int64_t>(pos.x) -
                               static_cast<std::int64_t>(centre.x))};
        const auto dz{std::abs(static_cast<std::int64_t>(pos.z) -
                               static_cast<std::int64_t>(centre.z))};
        best = std::min(best, std::max(dx, dz));
    }
    return best;
}

} // namespace

auto to_string(admission_reason reason) -> std::string_view {
    switch (reason) {
    case admission_reason::tick_p99:
        return "tick-p99";
    case admission_reason::memory:
        return "memory";
    case admission_reason::queue_depth:
        return "queue-depth";
    }
    return "queue-depth";
}

admission_controller::admission_controller(admission_limits limits)
    : limits_{std::move(limits)} {
    limits_.min_credits = std::max<std::size_t>(limits_.min_credits, 1);
    limits_.max_credits = std::max(limits_.max_credits, limits_.min_credits);
    limits_.raise_after_samples =
        std::max<std::uint32_t>(limits_.raise_after_samples, 1);
    credits_ = std::clamp(limits_.initial_credits, limits_.min_credits,
                          limits_.max_credits);
}

auto admission_controller::cut(admission_reason reason)
    -> std::optional<admission_change> {
    good_samples_ = 0;
    if (credits_ <= limits_.min_credits) {
        return std::nullopt;
    }
    // A miss cuts at once and by half, so an overshoot is corrected before
    // the next raise window instead of one credit at a time.
    const auto step{std::max<std::size_t>(credits_ / 2, 1)};
    credits_ = std::max(limits_.min_credits, credits_ - step);
    return admission_change{credits_, reason};
}

auto admission_controller::update(const admission_sample &sample)
    -> std::optional<admission_change> {
    if (sample.tick_p99_ms > limits_.budget_ms) {
        return cut(admission_reason::tick_p99);
    }
    if (limits_.memory_bound_bytes != 0 &&
        sample.resident_bytes > limits_.memory_bound_bytes) {
        return cut(admission_reason::memory);
    }
    // Demand is the raise signal: with no backlog more credits buy nothing,
    // and a level that already covers the demand must not creep upwards.
    if (credits_ >= limits_.max_credits || sample.queue_depth <= credits_) {
        good_samples_ = 0;
        return std::nullopt;
    }
    if (sample.tick_p99_ms > limits_.budget_ms * limits_.headroom) {
        good_samples_ = 0;
        return std::nullopt;
    }
    ++good_samples_;
    if (good_samples_ < limits_.raise_after_samples) {
        return std::nullopt;
    }
    good_samples_ = 0;
    ++credits_;
    return admission_change{credits_, admission_reason::queue_depth};
}

chunk_generation::chunk_generation(config settings)
    : settings_{std::move(settings)}, admission_{settings_.admission} {}

auto chunk_generation::default_worker_count(std::uint32_t sim_threads,
                                            std::uint32_t io_threads)
    -> std::size_t {
    const auto hardware{std::thread::hardware_concurrency()};
    const auto reserved{static_cast<std::size_t>(sim_threads) +
                        static_cast<std::size_t>(io_threads)};
    if (hardware <= reserved) {
        return 1;
    }
    return std::clamp<std::size_t>(hardware - reserved, 1, max_workers);
}

auto chunk_generation::start(config settings)
    -> result<std::unique_ptr<chunk_generation>> {
    if (settings.data_root.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "chunk generation needs the datapack data root")};
    }
    if (settings.worker_count == 0) {
        settings.worker_count =
            default_worker_count(settings.sim_threads, settings.io_threads);
    }
    settings.worker_count =
        std::clamp<std::size_t>(settings.worker_count, 1, max_workers);
    settings.tile_size = std::clamp<std::int32_t>(settings.tile_size, 1, 32);
    // Credits can never exceed the workers that can spend them, and the lower
    // bound is clamped first so a pinned level stays a valid range.
    settings.admission.min_credits = std::clamp<std::size_t>(
        settings.admission.min_credits, 1, settings.worker_count);
    settings.admission.max_credits =
        std::clamp(settings.admission.max_credits,
                   settings.admission.min_credits, settings.worker_count);
    auto service{std::unique_ptr<chunk_generation>{
        new chunk_generation(std::move(settings))}};
    service->workers_.reserve(service->settings_.worker_count);
    try {
        for (std::size_t index{0}; index < service->settings_.worker_count;
             ++index) {
            service->workers_.emplace_back(
                [pointer = service.get()] { pointer->run(); });
        }
    } catch (const std::system_error &error) {
        // Starting a worker is the one seam that can still fail after the
        // service was built; join what already started and report the count.
        service->stop();
        return std::unexpected{make_error(
            error_code::unknown,
            fmt::format("chunk generation could not start its workers: {}",
                        error.what()))};
    }
    log_generation.info("workers={} credit={}", service->settings_.worker_count,
                        service->admission_.credits());
    return service;
}

chunk_generation::~chunk_generation() { stop(); }

auto chunk_generation::request(refrain::world::chunk_pos pos) -> bool {
    const auto tile{tile_of(pos, settings_.tile_size)};
    const auto key{key_of(tile)};
    const std::lock_guard lock{mutex_};
    if (stopping_ || active_.contains(key)) {
        return false;
    }
    if (!wanted_.insert(key).second) {
        return false;
    }
    if (wanted_.size() <= settings_.max_pending) {
        return true;
    }
    // The queue is bounded: evict the farthest tile rather than refuse the
    // newest one, so stale work can never lock a walking player out of the
    // queue. The position breaks a distance tie, which keeps the choice
    // deterministic.
    std::vector<refrain::world::chunk_pos> queued{};
    queued.reserve(wanted_.size());
    for (const auto &entry : wanted_) {
        queued.push_back({.x = entry.first, .z = entry.second});
    }
    const auto victim{farthest_tile(queued, centres_, settings_.tile_size)};
    const bool admitted{key_of(victim) != key};
    wanted_.erase(key_of(victim));
    return admitted;
}

void chunk_generation::forget(refrain::world::chunk_pos pos) {
    const std::lock_guard lock{mutex_};
    wanted_.erase(key_of(tile_of(pos, settings_.tile_size)));
}

void chunk_generation::set_priority_centres(
    std::span<const refrain::world::chunk_pos> centres) {
    const std::lock_guard lock{mutex_};
    centres_.assign(centres.begin(), centres.end());
}

auto rank_columns_by_distance(
    std::span<const refrain::world::chunk_pos> wanted,
    std::span<const refrain::world::chunk_pos> centres)
    -> std::vector<refrain::world::chunk_pos> {
    struct ranked {
        std::int64_t distance{0};
        std::int32_t x{0};
        std::int32_t z{0};
    };
    std::vector<ranked> entries{};
    entries.reserve(wanted.size());
    for (const auto &pos : wanted) {
        entries.push_back(ranked{
            .distance = centre_distance(centres, pos), .x = pos.x, .z = pos.z});
    }
    std::sort(entries.begin(), entries.end(),
              [](const ranked &left, const ranked &right) {
                  return std::tie(left.distance, left.x, left.z) <
                         std::tie(right.distance, right.x, right.z);
              });
    std::vector<refrain::world::chunk_pos> ordered{};
    ordered.reserve(entries.size());
    for (const auto &entry : entries) {
        ordered.push_back({.x = entry.x, .z = entry.z});
    }
    return ordered;
}

auto farthest_column(std::span<const refrain::world::chunk_pos> wanted,
                     std::span<const refrain::world::chunk_pos> centres)
    -> refrain::world::chunk_pos {
    auto victim{wanted.front()};
    auto victim_distance{centre_distance(centres, victim)};
    for (const auto &pos : wanted.subspan(1)) {
        const auto distance{centre_distance(centres, pos)};
        if (distance > victim_distance ||
            (distance == victim_distance && key_of(pos) > key_of(victim))) {
            victim = pos;
            victim_distance = distance;
        }
    }
    return victim;
}

void chunk_generation::update_admission(const admission_sample &sample) {
    std::optional<admission_change> change{};
    {
        const std::lock_guard lock{mutex_};
        change = admission_.update(sample);
    }
    if (change) {
        log_generation.info("credit={} reason={}", change->credits,
                            to_string(change->reason));
    }
}

auto chunk_generation::dispatch(std::uint64_t tick_elapsed_ns) -> std::size_t {
    const auto headroom_ns{
        static_cast<std::uint64_t>(settings_.admission.budget_ms *
                                   settings_.admission.headroom * 1'000'000.0)};
    if (tick_elapsed_ns > headroom_ns) {
        return 0;
    }
    const std::lock_guard lock{mutex_};
    if (stopping_) {
        return 0;
    }
    const auto available{admission_.credits() > in_flight_
                             ? admission_.credits() - in_flight_
                             : std::size_t{0}};
    if (available == 0 || wanted_.empty()) {
        return 0;
    }
    std::vector<refrain::world::chunk_pos> candidates{};
    candidates.reserve(wanted_.size());
    for (const auto &key : wanted_) {
        candidates.push_back({.x = key.first, .z = key.second});
    }
    const auto ordered{
        rank_tiles_by_distance(candidates, centres_, settings_.tile_size)};
    std::size_t admitted{0};
    for (const auto &pos : ordered) {
        if (admitted >= available) {
            break;
        }
        const auto key{key_of(pos)};
        wanted_.erase(key);
        active_.insert(key);
        work_.push_back(pos);
        ++in_flight_;
        ++admitted;
    }
    if (admitted > 0) {
        wake_.notify_all();
    }
    return admitted;
}

auto chunk_generation::take_results() -> std::vector<
    std::pair<refrain::world::chunk_pos, refrain::world::chunk>> {
    std::vector<std::pair<refrain::world::chunk_pos, refrain::world::chunk>>
        results{};
    {
        const std::lock_guard lock{mutex_};
        auto tiles{std::exchange(finished_, {})};
        in_flight_ -= std::min(in_flight_, tiles.size());
        for (auto &tile : tiles) {
            active_.erase(key_of(tile.origin));
            for (auto &column : tile.columns) {
                results.push_back(std::move(column));
            }
        }
    }
    // The world is applied in ascending position order, never in completion
    // order, so admission has no influence on the resulting state.
    std::sort(results.begin(), results.end(),
              [](const auto &left, const auto &right) {
                  return key_of(left.first) < key_of(right.first);
              });
    return results;
}

auto chunk_generation::take_failures() -> std::vector<generation_failure> {
    std::vector<generation_failure> failures{};
    {
        const std::lock_guard lock{mutex_};
        failures = std::exchange(failed_, {});
    }
    std::sort(
        failures.begin(), failures.end(),
        [](const generation_failure &left, const generation_failure &right) {
            return key_of(left.tile) < key_of(right.tile);
        });
    return failures;
}

void chunk_generation::fail_task(refrain::world::chunk_pos origin,
                                 std::string message) {
    const std::lock_guard lock{mutex_};
    active_.erase(key_of(origin));
    in_flight_ -= std::min<std::size_t>(in_flight_, 1);
    failed_.push_back(
        generation_failure{.tile = origin, .message = std::move(message)});
}

auto chunk_generation::credits() const -> std::size_t {
    const std::lock_guard lock{mutex_};
    return admission_.credits();
}

auto chunk_generation::queue_depth() const -> std::size_t {
    const std::lock_guard lock{mutex_};
    return wanted_.size();
}

auto chunk_generation::in_flight() const -> std::size_t {
    const std::lock_guard lock{mutex_};
    return in_flight_;
}

auto chunk_generation::worker_count() const noexcept -> std::size_t {
    return settings_.worker_count;
}

void chunk_generation::stop() {
    {
        const std::lock_guard lock{mutex_};
        stopping_ = true;
    }
    wake_.notify_all();
    for (auto &worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

void chunk_generation::run() {
    static_cast<void>(refrain::platform::set_current_thread_name("chunk-gen"));
    if (!settings_.worker_cpus.empty()) {
        static_cast<void>(refrain::platform::set_current_thread_affinity(
            settings_.worker_cpus));
    }
    // The generator is created on the worker's first task, not at startup:
    // a quiet server pays for the generator instances it actually uses, and
    // the resident set the controller samples grows with the credit level
    // instead of being fixed before the controller has a reading.
    //
    // A worker whose generator cannot be created keeps serving its queue by
    // failing every task with the same message. Exiting here would leave the
    // queued tiles in flight for good, and the caller would never learn which
    // columns went unanswered.
    std::unique_ptr<worldgen::chunk_generator> generator{};
    std::optional<std::string> generator_error{};
    for (;;) {
        refrain::world::chunk_pos origin{};
        {
            std::unique_lock lock{mutex_};
            wake_.wait(lock, [this] { return stopping_ || !work_.empty(); });
            if (stopping_) {
                return;
            }
            origin = work_.front();
            work_.pop_front();
        }
        if (!generator) {
            if (generator_error) {
                fail_task(origin, *generator_error);
                continue;
            }
            auto created{worldgen::chunk_generator::create(
                worldgen::chunk_generator_config{
                    .data_root = settings_.data_root,
                    .templates_root = settings_.templates_root,
                    .seed = settings_.seed,
                    .kind = worldgen::dimension_kind::overworld,
                    .target = worldgen::generation_target::full,
                    .tile_size = settings_.tile_size,
                })};
            if (!created) {
                if (!generator_failure_reported_.exchange(true)) {
                    log_generation.error("generator unavailable: {}",
                                         created.error().message);
                }
                generator_error = created.error().message;
                fail_task(origin, *generator_error);
                continue;
            }
            generator = std::move(*created);
        }
        auto &engine{*generator};
        auto protos{engine.generate_tile(
            worldgen::chunk_pos{.x = origin.x, .z = origin.z})};
        if (!protos) {
            log_generation.debug("tile ({}, {}) failed: {}", origin.x, origin.z,
                                 protos.error().message);
            fail_task(origin, protos.error().message);
            continue;
        }
        tile_result result{.origin = origin, .columns = {}};
        result.columns.reserve(protos->size());
        bool failed{false};
        std::string failure_message{};
        for (auto &proto : *protos) {
            if (proto == nullptr) {
                failed = true;
                failure_message =
                    fmt::format("tile ({}, {}) produced an empty column",
                                origin.x, origin.z);
                break;
            }
            const auto position{
                refrain::world::chunk_pos{proto->chunk_x(), proto->chunk_z()}};
            auto column{
                refrain::world::from_proto_chunk(*proto, engine.biome_names())};
            if (!column) {
                log_generation.debug("chunk ({}, {}) conversion failed: {}",
                                     position.x, position.z,
                                     column.error().message);
                failed = true;
                failure_message =
                    fmt::format("column ({}, {}) conversion failed: {}",
                                position.x, position.z, column.error().message);
                break;
            }
            result.columns.emplace_back(position, std::move(*column));
        }
        if (failed) {
            fail_task(origin, std::move(failure_message));
            continue;
        }
        {
            const std::lock_guard lock{mutex_};
            if (stopping_) {
                return;
            }
            finished_.push_back(std::move(result));
        }
    }
}

} // namespace refrain::server
