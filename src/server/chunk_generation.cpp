// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/chunk_generation.hpp>

#include <refrain/core/log.hpp>
#include <refrain/platform/thread.hpp>
#include <refrain/server/content_reload_inputs.hpp>
#include <refrain/world/proto_bridge.hpp>
#include <refrain/worldgen/chunk_generator.hpp>
#include <refrain/worldgen/dimension_generator.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>

namespace refrain::server {

namespace {

constexpr refrain::log::logger log_generation{"chunk-gen"};

/// The generator configuration a tile worker starts from. A non-null
/// `generation` replaces the boot roots with the merged generation tree and
/// binds the registry mapping; null keeps the boot content.
[[nodiscard]] auto tile_generator_config(
    const chunk_generation::config &settings,
    std::shared_ptr<const worldgen::generation_data> generation)
    -> worldgen::chunk_generator_config {
    worldgen::chunk_generator_config generator{};
    generator.data_root = settings.data_root;
    generator.templates_root = settings.templates_root;
    generator.seed = settings.seed;
    generator.kind = worldgen::dimension_kind::overworld;
    generator.target = worldgen::generation_target::full;
    generator.tile_size = settings.tile_size;
    generator.generation = std::move(generation);
    return generator;
}

/// Compose the published content view into a chunk-generation snapshot. The
/// installed data root stays the base layer; the view is the overlay, so a
/// data-only reload costs no copy and produces the same chunks.
[[nodiscard]] auto build_generation(const chunk_generation::config &settings,
                                    const app::content_view_snapshot &snapshot)
    -> result<std::shared_ptr<const worldgen::generation_data>> {
    if (!snapshot.view) {
        return std::unexpected{
            make_error(error_code::unknown,
                       "the published content view names no view to bind chunk "
                       "generation to")};
    }
    return worldgen::generation_data::build(
        snapshot.view, snapshot.registries, snapshot.generation,
        snapshot.registry_generation, settings.data_root,
        settings.templates_root);
}

/// Monotonic store for the applied-epoch counters: several workers report the
/// epoch they applied and a late report of an older epoch never lowers it.
void store_max(std::atomic<std::uint64_t> &target,
               std::uint64_t value) noexcept {
    auto current{target.load()};
    while (current < value && !target.compare_exchange_weak(current, value)) {
    }
}

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

/// Nearest distance to any of `centres`, or a sentinel that ranks a tile
/// behind every tile a centre list names when the list is empty. A primary
/// list is always non-empty in the dispatch path; the look-ahead list may be
/// empty, and then its key must not change the primary order.
[[nodiscard]] auto
secondary_distance(std::span<const refrain::world::chunk_pos> centres,
                   refrain::world::chunk_pos tile, std::int32_t tile_size)
    -> std::int64_t {
    if (centres.empty()) {
        return 0;
    }
    return tile_distance(centres, tile, tile_size);
}

/// The tile a full queue evicts: the farthest from the accepted centres, then
/// the farthest from the look-ahead centres, greatest corner on a tie.
/// `wanted` must not be empty.
[[nodiscard]] auto
farthest_tile(std::span<const refrain::world::chunk_pos> wanted,
              std::span<const refrain::world::chunk_pos> centres,
              std::span<const refrain::world::chunk_pos> lookahead,
              std::int32_t tile_size) -> refrain::world::chunk_pos {
    auto victim{wanted.front()};
    auto victim_distance{tile_distance(centres, victim, tile_size)};
    auto victim_ahead{secondary_distance(lookahead, victim, tile_size)};
    for (const auto &pos : wanted.subspan(1)) {
        const auto distance{tile_distance(centres, pos, tile_size)};
        const auto ahead{secondary_distance(lookahead, pos, tile_size)};
        if (distance > victim_distance ||
            (distance == victim_distance && ahead > victim_ahead) ||
            (distance == victim_distance && ahead == victim_ahead &&
             key_of(pos) > key_of(victim))) {
            victim = pos;
            victim_distance = distance;
            victim_ahead = ahead;
        }
    }
    return victim;
}

[[nodiscard]] auto
centre_distance(std::span<const refrain::world::chunk_pos> centres,
                refrain::world::chunk_pos pos) -> std::int64_t;

/// The column a full experimental queue evicts: the farthest from both centre
/// lists, then the greatest position on a tie. `wanted` must not be empty.
[[nodiscard]] auto
farthest_wanted_column(std::span<const refrain::world::chunk_pos> wanted,
                       std::span<const refrain::world::chunk_pos> accepted,
                       std::span<const refrain::world::chunk_pos> lookahead)
    -> refrain::world::chunk_pos {
    auto victim{wanted.front()};
    auto victim_distance{centre_distance(accepted, victim)};
    auto victim_ahead{centre_distance(lookahead, victim)};
    for (const auto &pos : wanted.subspan(1)) {
        const auto distance{centre_distance(accepted, pos)};
        const auto ahead{centre_distance(lookahead, pos)};
        if (distance > victim_distance ||
            (distance == victim_distance && ahead > victim_ahead) ||
            (distance == victim_distance && ahead == victim_ahead &&
             key_of(pos) > key_of(victim))) {
            victim = pos;
            victim_distance = distance;
            victim_ahead = ahead;
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
    settings.proto_cache_capacity =
        std::clamp<std::size_t>(settings.proto_cache_capacity, 25, 4096);
    // Credits can never exceed the workers that can spend them, and the lower
    // bound is clamped first so a pinned level stays a valid range.
    settings.admission.min_credits = std::clamp<std::size_t>(
        settings.admission.min_credits, 1, settings.worker_count);
    settings.admission.max_credits =
        std::clamp(settings.admission.max_credits,
                   settings.admission.min_credits, settings.worker_count);
    auto service{std::unique_ptr<chunk_generation>{
        new chunk_generation(std::move(settings))}};
    if (service->settings_.experimental_chunk_pipeline) {
        // The experimental path drives the per-chunk status scheduler. It is
        // created before the workers start and never selected by the server
        // composition, which leaves `experimental_chunk_pipeline` false.
        worldgen::chunk_generator_config generator{};
        generator.data_root = service->settings_.data_root;
        generator.templates_root = service->settings_.templates_root;
        generator.seed = service->settings_.seed;
        generator.kind = worldgen::dimension_kind::overworld;
        generator.target = worldgen::generation_target::full;
        generator.proto_cache_capacity = static_cast<worldgen::jint>(
            service->settings_.proto_cache_capacity);
        auto scheduler{
            worldgen::chunk_scheduler::create(worldgen::chunk_scheduler::config{
                .generator = std::move(generator),
                .workers = service->settings_.worker_count,
            })};
        if (!scheduler) {
            service->stop();
            return std::unexpected{scheduler.error()};
        }
        service->scheduler_ = std::move(*scheduler);
    }
    service->workers_.reserve(service->settings_.worker_count);
    try {
        for (std::size_t index{0}; index < service->settings_.worker_count;
             ++index) {
            service->workers_.emplace_back(
                [pointer = service.get(), index] { pointer->run(index); });
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
    log_generation.info(
        "started chunk generation with {} workers, credit {} and the {} path",
        service->settings_.worker_count, service->admission_.credits(),
        service->settings_.experimental_chunk_pipeline ? "experimental chunk"
                                                       : "tile");
    return service;
}

chunk_generation::~chunk_generation() { stop(); }

auto chunk_generation::request(refrain::world::chunk_pos pos) -> bool {
    const bool pipeline{settings_.experimental_chunk_pipeline};
    const auto unit{pipeline ? pos : tile_of(pos, settings_.tile_size)};
    const auto key{key_of(unit)};
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
    // The queue is bounded: evict the farthest unit rather than refuse the
    // newest one, so stale work can never lock a walking player out of the
    // queue. The position breaks a distance tie, which keeps the choice
    // deterministic.
    std::vector<refrain::world::chunk_pos> queued{};
    queued.reserve(wanted_.size());
    for (const auto &entry : wanted_) {
        queued.push_back({.x = entry.first, .z = entry.second});
    }
    const auto victim{
        pipeline ? farthest_wanted_column(queued, centres_, lookahead_centres_)
                 : farthest_tile(queued, centres_, lookahead_centres_,
                                 settings_.tile_size)};
    const bool admitted{key_of(victim) != key};
    wanted_.erase(key_of(victim));
    return admitted;
}

void chunk_generation::forget(refrain::world::chunk_pos pos) {
    const std::lock_guard lock{mutex_};
    if (settings_.experimental_chunk_pipeline) {
        wanted_.erase(key_of(pos));
        return;
    }
    wanted_.erase(key_of(tile_of(pos, settings_.tile_size)));
}

void chunk_generation::set_priority_centres(
    std::span<const refrain::world::chunk_pos> centres) {
    const std::lock_guard lock{mutex_};
    centres_.assign(centres.begin(), centres.end());
    lookahead_centres_.clear();
}

void chunk_generation::set_priority_centres(
    std::span<const refrain::world::chunk_pos> centres,
    std::span<const refrain::world::chunk_pos> lookahead) {
    const std::lock_guard lock{mutex_};
    centres_.assign(centres.begin(), centres.end());
    lookahead_centres_.assign(lookahead.begin(), lookahead.end());
}

auto rank_tiles_by_distance(
    std::span<const refrain::world::chunk_pos> wanted,
    std::span<const refrain::world::chunk_pos> accepted,
    std::span<const refrain::world::chunk_pos> lookahead,
    std::int32_t tile_size) -> std::vector<refrain::world::chunk_pos> {
    struct ranked {
        std::int64_t distance{0};
        std::int64_t ahead{0};
        std::int32_t x{0};
        std::int32_t z{0};
    };
    std::vector<ranked> entries{};
    entries.reserve(wanted.size());
    for (const auto &pos : wanted) {
        entries.push_back(ranked{
            .distance = tile_distance(accepted, pos, tile_size),
            .ahead = secondary_distance(lookahead, pos, tile_size),
            .x = pos.x,
            .z = pos.z,
        });
    }
    std::sort(entries.begin(), entries.end(),
              [](const ranked &left, const ranked &right) {
                  return std::tie(left.distance, left.ahead, left.x, left.z) <
                         std::tie(right.distance, right.ahead, right.x,
                                  right.z);
              });
    std::vector<refrain::world::chunk_pos> ordered{};
    ordered.reserve(entries.size());
    for (const auto &entry : entries) {
        ordered.push_back({.x = entry.x, .z = entry.z});
    }
    return ordered;
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

auto rank_columns_by_distance(
    std::span<const refrain::world::chunk_pos> wanted,
    std::span<const refrain::world::chunk_pos> accepted,
    std::span<const refrain::world::chunk_pos> lookahead)
    -> std::vector<refrain::world::chunk_pos> {
    struct ranked {
        std::int64_t distance{0};
        std::int64_t ahead{0};
        std::int32_t x{0};
        std::int32_t z{0};
    };
    std::vector<ranked> entries{};
    entries.reserve(wanted.size());
    for (const auto &pos : wanted) {
        entries.push_back(ranked{
            .distance = centre_distance(accepted, pos),
            .ahead = centre_distance(lookahead, pos),
            .x = pos.x,
            .z = pos.z,
        });
    }
    std::sort(entries.begin(), entries.end(),
              [](const ranked &left, const ranked &right) {
                  return std::tie(left.distance, left.ahead, left.x, left.z) <
                         std::tie(right.distance, right.ahead, right.x,
                                  right.z);
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
        log_generation.info(
            "chunk generation credit is now {} after the {} signal",
            change->credits, to_string(change->reason));
    }
}

auto chunk_generation::dispatch(std::uint64_t tick_elapsed_ns) -> std::size_t {
    if (scheduler_ != nullptr) {
        // The experimental scheduler runs one frozen shared block-state table
        // over the boot content. A published content generation cannot rebuild
        // that table without invalidating the ids its worker generators already
        // resolved, so the scheduler keeps the boot content and the reload
        // reaches this path only after a restart.
        const auto snapshot{server::published_content_view()};
        if (!scheduler_baseline_recorded_) {
            scheduler_baseline_recorded_ = true;
            scheduler_observed_generation_ = snapshot.generation;
        } else if (snapshot.generation != scheduler_observed_generation_) {
            scheduler_observed_generation_ = snapshot.generation;
            if (!scheduler_reload_notice_.exchange(true)) {
                log_generation.warn(
                    "the experimental chunk scheduler keeps its boot content "
                    "after content view generation {}, so a reload needs a "
                    "restart to change this path",
                    snapshot.generation);
            }
        }
    }
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
    if (settings_.experimental_chunk_pipeline) {
        if (scheduler_ == nullptr) {
            return 0;
        }
        const auto ordered{
            rank_columns_by_distance(candidates, centres_, lookahead_centres_)};
        std::size_t admitted{0};
        for (const auto &pos : ordered) {
            if (admitted >= available) {
                break;
            }
            const auto key{key_of(pos)};
            wanted_.erase(key);
            if (!scheduler_->demand(
                    worldgen::chunk_pos{.x = pos.x, .z = pos.z})) {
                wanted_.insert(key);
                continue;
            }
            active_.insert(key);
            ++in_flight_;
            ++admitted;
        }
        if (admitted > 0) {
            wake_.notify_all();
        }
        return admitted;
    }
    const auto ordered{rank_tiles_by_distance(
        candidates, centres_, lookahead_centres_, settings_.tile_size)};
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
    return take_results(std::numeric_limits<std::size_t>::max());
}

auto chunk_generation::take_results(std::size_t max_columns) -> std::vector<
    std::pair<refrain::world::chunk_pos, refrain::world::chunk>> {
    std::vector<std::pair<refrain::world::chunk_pos, refrain::world::chunk>>
        results{};
    {
        const std::lock_guard lock{mutex_};
        // The flattening order decides which columns reach the world's apply
        // queue first when the adoption budget cannot take every finished
        // tile, so it is taken from the tile positions and never from the
        // order the workers happened to finish.
        std::sort(finished_.begin(), finished_.end(),
                  [](const tile_result &left, const tile_result &right) {
                      return key_of(left.origin) < key_of(right.origin);
                  });
        // Flatten finished tiles only while the ready queue has room. A tile
        // left unflattened stays in flight, so its admission credit is not
        // released and the caller's slow adoption throttles generation
        // instead of growing this queue without bound.
        while (!finished_.empty() && ready_.size() < max_ready_columns) {
            auto tile{std::move(finished_.front())};
            finished_.erase(finished_.begin());
            in_flight_ -= std::min<std::size_t>(in_flight_, 1);
            active_.erase(key_of(tile.origin));
            for (auto &column : tile.columns) {
                // The ready map keeps the world's apply order by position, so
                // the order never depends on which worker finished first, and
                // a limited take leaves the remaining columns sorted for the
                // next tick.
                ready_.insert_or_assign(key_of(column.first),
                                        std::move(column.second));
            }
        }
        const auto count{std::min(max_columns, ready_.size())};
        results.reserve(count);
        for (std::size_t index{0}; index < count; ++index) {
            auto first{ready_.begin()};
            results.emplace_back(
                refrain::world::chunk_pos{.x = first->first.first,
                                          .z = first->first.second},
                std::move(first->second));
            ready_.erase(first);
        }
    }
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

auto chunk_generation::bind_generation(
    const app::content_view_snapshot &snapshot)
    -> result<std::shared_ptr<const worldgen::generation_data>> {
    std::unique_lock lock{generation_mutex_};
    for (;;) {
        if (generation_cache_ != nullptr &&
            generation_cache_epoch_ == snapshot.generation) {
            return generation_cache_;
        }
        if (generation_failed_epoch_ == snapshot.generation) {
            return std::unexpected{
                make_error(error_code::unknown, generation_failure_reason_)};
        }
        if (generation_in_flight_epoch_ != snapshot.generation) {
            break;
        }
        generation_cv_.wait(lock, [this, &snapshot] {
            return (generation_cache_ != nullptr &&
                    generation_cache_epoch_ == snapshot.generation) ||
                   generation_failed_epoch_ == snapshot.generation ||
                   generation_in_flight_epoch_ != snapshot.generation;
        });
    }
    generation_in_flight_epoch_ = snapshot.generation;
    ++generation_compose_attempts_;
    lock.unlock();
    auto built{build_generation(settings_, snapshot)};
    lock.lock();
    generation_in_flight_epoch_ = 0;
    if (built) {
        // A late composition of an older epoch must not overwrite the newer
        // cached one, so the store is epoch-ordered.
        if (snapshot.generation >= generation_cache_epoch_) {
            generation_cache_ = *built;
            generation_cache_epoch_ = snapshot.generation;
        }
    } else {
        generation_failed_epoch_ = snapshot.generation;
        generation_failure_reason_ = built.error().message;
    }
    generation_cv_.notify_all();
    return built;
}

auto chunk_generation::bound_generation() const -> std::uint64_t {
    return bound_generation_.load();
}

auto chunk_generation::bound_registry_generation() const -> std::uint64_t {
    return bound_registry_generation_.load();
}

auto chunk_generation::generation_rebuilds() const -> std::uint64_t {
    return generation_rebuilds_.load();
}

auto chunk_generation::generation_rebuild_failures() const -> std::uint64_t {
    return generation_rebuild_failures_.load();
}

auto chunk_generation::generation_compose_attempts() const -> std::uint64_t {
    return generation_compose_attempts_.load();
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
    if (scheduler_ != nullptr) {
        scheduler_->clear();
    }
}

void chunk_generation::run(std::size_t worker) {
    static_cast<void>(refrain::platform::set_current_thread_name("chunk-gen"));
    if (!settings_.worker_cpus.empty()) {
        static_cast<void>(refrain::platform::set_current_thread_affinity(
            settings_.worker_cpus));
    }
    if (scheduler_ != nullptr) {
        run_pipeline(worker);
        return;
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
    // The published epochs this worker has observed and the applied ones it
    // reported. A failed bind observes the epoch but does not apply it, so the
    // service-level applied counters never claim content the worker is not
    // serving.
    std::uint64_t observed_view_generation{0};
    std::uint64_t observed_registry_generation{0};
    bool bound_overlay{false};
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
        const auto snapshot{server::published_content_view()};
        if (!generator) {
            if (generator_error) {
                fail_task(origin, *generator_error);
                continue;
            }
            // The first generator keeps the boot content unless the published
            // view carries documents chunk generation reads or a registry
            // mapping, in which case it binds the newest generation right
            // away. A view that changes nothing generation reads (the ordinary
            // recipe/loot reload) leaves the boot roots in place, so no data is
            // parsed twice and the first chunk is produced exactly as before.
            std::shared_ptr<const worldgen::generation_data> generation{};
            bool bind_failed{false};
            if (snapshot.view && snapshot.generation != 0) {
                auto built{bind_generation(snapshot)};
                if (!built) {
                    bind_failed = true;
                    ++generation_rebuild_failures_;
                    log_generation.error(
                        "the chunk generator cannot bind content view "
                        "generation {}: {}",
                        snapshot.generation, built.error().message);
                } else if ((*built)->overlays_generation_content() ||
                           !(*built)->registries().empty()) {
                    bound_overlay = (*built)->overlays_generation_content();
                    generation = std::move(*built);
                }
            }
            auto created{worldgen::chunk_generator::create(
                tile_generator_config(settings_, std::move(generation)))};
            if (!created) {
                if (!generator_failure_reported_.exchange(true)) {
                    log_generation.error(
                        "the chunk generator is unavailable: {}",
                        created.error().message);
                }
                generator_error = created.error().message;
                fail_task(origin, *generator_error);
                continue;
            }
            generator = std::move(*created);
            observed_view_generation = snapshot.generation;
            observed_registry_generation = snapshot.registry_generation;
            if (!bind_failed) {
                store_max(bound_generation_, snapshot.generation);
                store_max(bound_registry_generation_,
                          snapshot.registry_generation);
            }
        } else if (snapshot.generation != observed_view_generation ||
                   snapshot.registry_generation !=
                       observed_registry_generation) {
            // A tick-boundary publish advanced the live generation: rebuild
            // this worker's generator for the newest snapshot before it starts
            // the next chunk. The old generator finished the previous chunk,
            // and queued tiles stay queued; nothing here blocks the tick that
            // published.
            result<std::shared_ptr<const worldgen::generation_data>> built{
                std::unexpected{make_error(error_code::unknown,
                                           "the published snapshot names no "
                                           "content view")}};
            if (snapshot.view && snapshot.generation != 0) {
                built = bind_generation(snapshot);
            }
            bool applied{false};
            if (built) {
                const bool overlays{(*built)->overlays_generation_content()};
                const bool needs_rebind{overlays ||
                                        (*built)->registry_generation() !=
                                            observed_registry_generation ||
                                        bound_overlay};
                if (!needs_rebind) {
                    // Data-only reload: chunk generation reads none of it, so
                    // the generator stays and the chunks stay identical.
                    applied = true;
                } else if (auto created{worldgen::chunk_generator::create(
                               tile_generator_config(settings_,
                                                     std::move(*built)))};
                           created) {
                    generator = std::move(*created);
                    bound_overlay = overlays;
                    ++generation_rebuilds_;
                    log_generation.debug(
                        "rebuilt the chunk generator for content view "
                        "generation {}",
                        snapshot.generation);
                    applied = true;
                }
            }
            if (!applied) {
                ++generation_rebuild_failures_;
                auto reported{rebind_failure_reported_.load()};
                if (reported < snapshot.generation &&
                    rebind_failure_reported_.compare_exchange_strong(
                        reported, snapshot.generation)) {
                    log_generation.error(
                        "failed to bind chunk generation to content view "
                        "generation {}, so the previous content keeps serving: "
                        "{}",
                        snapshot.generation,
                        built ? "the generator could not load the new content"
                              : built.error().message);
                }
            }
            observed_view_generation = snapshot.generation;
            observed_registry_generation = snapshot.registry_generation;
            if (applied) {
                store_max(bound_generation_, snapshot.generation);
                store_max(bound_registry_generation_,
                          snapshot.registry_generation);
            }
        }
        auto &engine{*generator};
        auto protos{engine.generate_tile(
            worldgen::chunk_pos{.x = origin.x, .z = origin.z})};
        if (!protos) {
            log_generation.debug("failed to generate tile ({}, {}): {}",
                                 origin.x, origin.z, protos.error().message);
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
                log_generation.debug("failed to convert chunk ({}, {}): {}",
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

void chunk_generation::run_pipeline(std::size_t worker) {
    for (;;) {
        std::optional<worldgen::pipeline_job> job{};
        {
            std::unique_lock lock{mutex_};
            wake_.wait(lock, [this, worker] {
                return stopping_ ||
                       (scheduler_ != nullptr && scheduler_->has_job(worker));
            });
            if (stopping_) {
                return;
            }
            job = scheduler_->next_job(worker);
        }
        if (!job.has_value()) {
            continue;
        }
        auto generated{scheduler_->run(*job)};
        if (!generated) {
            const auto message{generated.error().message};
            // A terrain job names the position it fills, but the chain that
            // failed is the demanded column its dependency window belongs to.
            const auto failed_position{job->demand};
            scheduler_->complete(*job, &message);
            log_generation.debug(
                "failed to generate chunk ({}, {}) at the {} step: {}",
                failed_position.x, failed_position.z,
                worldgen::pipeline_step_name(job->step), message);
            fail_task(refrain::world::chunk_pos{.x = failed_position.x,
                                                .z = failed_position.z},
                      message);
            wake_.notify_all();
            continue;
        }
        if (job->step != worldgen::pipeline_step::served) {
            scheduler_->complete(*job, nullptr);
            // A completed terrain or decoration step can make the ordered
            // lane or a queued terrain job ready on another worker; those
            // workers are asleep on the wake predicate.
            wake_.notify_all();
            continue;
        }
        auto column{refrain::world::from_proto_chunk(
            **generated, scheduler_->biome_names())};
        if (!column) {
            const auto message{fmt::format(
                "column ({}, {}) conversion failed: {}", job->position.x,
                job->position.z, column.error().message)};
            scheduler_->complete(*job, &message);
            log_generation.debug("failed to convert chunk ({}, {}): {}",
                                 job->position.x, job->position.z,
                                 column.error().message);
            fail_task(refrain::world::chunk_pos{.x = job->position.x,
                                                .z = job->position.z},
                      message);
            continue;
        }
        scheduler_->complete(*job, nullptr);
        wake_.notify_all();
        tile_result result{.origin =
                               refrain::world::chunk_pos{.x = job->position.x,
                                                         .z = job->position.z},
                           .columns = {}};
        result.columns.emplace_back(
            refrain::world::chunk_pos{.x = job->position.x,
                                      .z = job->position.z},
            std::move(*column));
        {
            const std::lock_guard lock{mutex_};
            if (stopping_) {
                return;
            }
            finished_.push_back(std::move(result));
        }
    }
}

auto chunk_generation::cache_entries() const -> std::size_t {
    const std::lock_guard lock{mutex_};
    return scheduler_ != nullptr ? scheduler_->stats().cache_entries : 0;
}

auto chunk_generation::cache_evictions() const -> std::uint64_t {
    const std::lock_guard lock{mutex_};
    return scheduler_ != nullptr ? scheduler_->stats().cache_evictions : 0;
}

} // namespace refrain::server
