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
#include <optional>
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

/// The column a full queue evicts: the farthest from both centre lists, then
/// the greatest position on a tie, which is the last entry of the dispatch
/// order. `wanted` must not be empty.
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

    worldgen::chunk_generator_config generator{};
    generator.data_root = service->settings_.data_root;
    generator.templates_root = service->settings_.templates_root;
    generator.seed = service->settings_.seed;
    generator.kind = worldgen::dimension_kind::overworld;
    generator.target = worldgen::generation_target::full;
    generator.proto_cache_capacity =
        static_cast<worldgen::jint>(service->settings_.proto_cache_capacity);
    auto scheduler{
        worldgen::chunk_scheduler::create(worldgen::chunk_scheduler::config{
            .generator = std::move(generator),
            .workers = service->settings_.worker_count,
        })};
    if (!scheduler) {
        return std::unexpected{scheduler.error()};
    }
    service->scheduler_ = std::move(*scheduler);

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
        "started chunk generation with {} workers, credit {} and a "
        "{} chunk proto cache per worker",
        service->settings_.worker_count, service->admission_.credits(),
        service->settings_.proto_cache_capacity);
    return service;
}

chunk_generation::~chunk_generation() { stop(); }

auto chunk_generation::request(refrain::world::chunk_pos pos) -> bool {
    const auto key{key_of(pos)};
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
    // The queue is bounded: evict the farthest column rather than refuse the
    // newest one, so stale work can never lock a walking player out of the
    // queue. Both centre lists and the position break ties, which keeps the
    // choice deterministic.
    std::vector<refrain::world::chunk_pos> queued{};
    queued.reserve(wanted_.size());
    for (const auto &entry : wanted_) {
        queued.push_back({.x = entry.first, .z = entry.second});
    }
    const auto victim{
        farthest_wanted_column(queued, centres_, lookahead_centres_)};
    const bool admitted{key_of(victim) != key};
    wanted_.erase(key_of(victim));
    return admitted;
}

void chunk_generation::forget(refrain::world::chunk_pos pos) {
    const std::lock_guard lock{mutex_};
    wanted_.erase(key_of(pos));
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
    const auto headroom_ns{
        static_cast<std::uint64_t>(settings_.admission.budget_ms *
                                   settings_.admission.headroom * 1'000'000.0)};
    if (tick_elapsed_ns > headroom_ns) {
        return 0;
    }
    const std::lock_guard lock{mutex_};
    if (stopping_ || scheduler_ == nullptr) {
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
        rank_columns_by_distance(candidates, centres_, lookahead_centres_)};
    std::size_t admitted{0};
    for (const auto &pos : ordered) {
        if (admitted >= available) {
            break;
        }
        const auto key{key_of(pos)};
        wanted_.erase(key);
        if (!scheduler_->demand(worldgen::chunk_pos{.x = pos.x, .z = pos.z})) {
            // The position is still in the scheduler's working set; leaving it
            // wanted would lose the request, so put it back for a later tick.
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
        // chunk, so it is taken from the chunk positions and never from the
        // order the workers happened to finish.
        std::sort(finished_.begin(), finished_.end(),
                  [](const auto &left, const auto &right) {
                      return key_of(left.first) < key_of(right.first);
                  });
        // Flatten finished columns only while the ready queue has room. A
        // column left unflattened stays in flight, so its admission credit is
        // not released and the caller's slow adoption throttles generation
        // instead of growing this queue without bound.
        while (!finished_.empty() && ready_.size() < max_ready_columns) {
            auto column{std::move(finished_.front())};
            finished_.erase(finished_.begin());
            in_flight_ -= std::min<std::size_t>(in_flight_, 1);
            active_.erase(key_of(column.first));
            // The ready map keeps the world's apply order by position, so the
            // order never depends on which worker finished first, and a
            // limited take leaves the remaining columns sorted for the next
            // tick.
            ready_.insert_or_assign(key_of(column.first),
                                    std::move(column.second));
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
            return key_of(left.position) < key_of(right.position);
        });
    return failures;
}

void chunk_generation::fail_task(refrain::world::chunk_pos pos,
                                 std::string message) {
    const std::lock_guard lock{mutex_};
    // A failure is reported once per admitted column: the scheduler reports
    // the failing status, the later statuses of that column are skipped.
    if (active_.erase(key_of(pos)) == 0) {
        return;
    }
    in_flight_ -= std::min<std::size_t>(in_flight_, 1);
    failed_.push_back(
        generation_failure{.position = pos, .message = std::move(message)});
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

auto chunk_generation::cache_entries() const -> std::size_t {
    const std::lock_guard lock{mutex_};
    return scheduler_ != nullptr ? scheduler_->stats().cache_entries : 0;
}

auto chunk_generation::cache_evictions() const -> std::uint64_t {
    const std::lock_guard lock{mutex_};
    return scheduler_ != nullptr ? scheduler_->stats().cache_evictions : 0;
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
    // Unload every worker cache once no worker can touch it any more.
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
            scheduler_->complete(*job, &message);
            log_generation.debug(
                "failed to generate chunk ({}, {}) at the {} step: {}",
                job->position.x, job->position.z,
                worldgen::pipeline_step_name(job->step), message);
            fail_task(refrain::world::chunk_pos{.x = job->position.x,
                                                .z = job->position.z},
                      message);
            continue;
        }
        if (job->step != worldgen::pipeline_step::served) {
            scheduler_->complete(*job, nullptr);
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
        {
            const std::lock_guard lock{mutex_};
            if (stopping_) {
                return;
            }
            finished_.emplace_back(
                refrain::world::chunk_pos{.x = job->position.x,
                                          .z = job->position.z},
                std::move(*column));
        }
    }
}

} // namespace refrain::server
