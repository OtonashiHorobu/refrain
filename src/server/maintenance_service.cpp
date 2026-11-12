// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/maintenance_service.hpp>

#include <refrain/core/io.hpp>
#include <refrain/core/log.hpp>
#include <refrain/core/time.hpp>
#include <refrain/data/content_maintenance.hpp>
#include <refrain/mod/rfmod.hpp>

#include <fmt/format.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr refrain::log::logger log_content{"content"};
constexpr refrain::log::logger log_schedule{"schedule"};
constexpr refrain::log::logger log_server{"server"};
constexpr refrain::log::logger log_storage{"storage"};

} // namespace

namespace refrain::server {

namespace {

/// Background writer for autosave snapshots. The sim thread only deep-copies
/// dirty chunks (`snapshot_pending()`); TLV encoding, store writes and group
/// commit run here through `sim::world::write_pending()`.
class snapshot_writer {
  public:
    explicit snapshot_writer(refrain::sim::world &world)
        : world_{world}, thread_{[this] { run(); }} {}

    snapshot_writer(const snapshot_writer &) = delete;
    auto operator=(const snapshot_writer &) -> snapshot_writer & = delete;

    ~snapshot_writer() { stop(); }

    [[nodiscard]] auto drain_completed()
        -> std::vector<std::vector<refrain::sim::snapshot_entry>> {
        const std::lock_guard lock{completed_mutex_};
        std::vector<std::vector<refrain::sim::snapshot_entry>> completed{};
        completed.swap(completed_);
        return completed;
    }

    [[nodiscard]] auto submit(std::vector<refrain::sim::pending_snapshot> batch)
        -> bool {
        {
            const std::lock_guard lock{mutex_};
            if (pending_.size() >= 2) {
                return false;
            }
            pending_.push_back(std::move(batch));
        }
        condition_.notify_one();
        return true;
    }

    /// Wait until every submitted batch has been written and marked complete.
    /// The console `save` path calls this before a synchronous `flush()`, so a
    /// stale in-flight snapshot cannot overwrite the newer flushed state.
    void drain() {
        std::unique_lock lock{mutex_};
        idle_condition_.wait(lock,
                             [this] { return pending_.empty() && !writing_; });
    }

    void stop() noexcept {
        {
            const std::lock_guard lock{mutex_};
            if (stopping_) {
                return;
            }
            stopping_ = true;
        }
        condition_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

  private:
    void run() {
        refrain::log::set_thread_name("io-writer");
        for (;;) {
            std::vector<refrain::sim::pending_snapshot> batch{};
            {
                std::unique_lock lock{mutex_};
                condition_.wait(
                    lock, [this] { return stopping_ || !pending_.empty(); });
                if (pending_.empty()) {
                    if (stopping_) {
                        return;
                    }
                    continue;
                }
                batch = std::move(pending_.front());
                pending_.pop_front();
                writing_ = true;
            }
            if (auto written{world_.write_pending(batch)}; !written) {
                log_storage.error("{}", written.error().message);
            } else {
                const std::lock_guard lock{completed_mutex_};
                completed_.push_back(std::move(*written));
            }
            {
                const std::lock_guard lock{mutex_};
                writing_ = false;
            }
            idle_condition_.notify_all();
        }
    }

    refrain::sim::world &world_;
    std::mutex mutex_{};
    std::condition_variable condition_{};
    std::condition_variable idle_condition_{};
    std::deque<std::vector<refrain::sim::pending_snapshot>> pending_{};
    std::mutex completed_mutex_{};
    std::vector<std::vector<refrain::sim::snapshot_entry>> completed_{};
    bool stopping_{false};
    bool writing_{false};
    std::thread thread_;
};

/// Delay between two bounded drain passes when the queue still holds
/// retryable entries (failures below the retry limit or budget deferrals).
/// Stuck entries (retry limit reached) are not retried until a new enqueue
/// notifies the worker, so a permanently broken store does not spin.
constexpr auto gc_delete_retry_delay{std::chrono::seconds{30}};

/// Background owner of every content-GC unlink.
///
/// The tick thread only plans and enqueues; this worker drains
/// `<store>/content/gc/pending.json` with a bounded per-pass budget. The two
/// share `queue_mutex()`: the tick path uses `try_lock` and skips its interval
/// when a pass is in flight, so a slow `remove` can never stall a tick. One
/// `unlink` may still block (no per-unlink timeout by decision), which is why
/// it is confined to this thread.
class gc_delete_worker {
  public:
    gc_delete_worker(std::string store_root, std::uint64_t budget_ms)
        : store_root_{std::move(store_root)}, budget_ms_{budget_ms},
          thread_{[this] { run(); }} {}

    gc_delete_worker(const gc_delete_worker &) = delete;
    auto operator=(const gc_delete_worker &) -> gc_delete_worker & = delete;

    ~gc_delete_worker() { stop(); }

    [[nodiscard]] auto queue_mutex() -> std::mutex & { return mutex_; }

    /// Wake the worker after an enqueue. Takes no lock, so it is safe while
    /// the caller holds `queue_mutex()`.
    void notify() noexcept {
        wake_requested_.store(true, std::memory_order_release);
        condition_.notify_one();
    }

    void stop() noexcept {
        if (stopping_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        condition_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

  private:
    void run() {
        refrain::log::set_thread_name("gc-delete");
        std::unique_lock lock{mutex_};
        for (;;) {
            if (stopping_.load(std::memory_order_acquire)) {
                return;
            }
            if (!wake_requested_.load(std::memory_order_acquire)) {
                condition_.wait(lock, [this] {
                    return stopping_.load(std::memory_order_acquire) ||
                           wake_requested_.load(std::memory_order_acquire);
                });
            }
            if (stopping_.load(std::memory_order_acquire)) {
                return;
            }
            wake_requested_.store(false, std::memory_order_release);

            // Hold `mutex_` for the whole pass: the planner's try_lock then
            // fails instead of waiting for a blocking unlink.
            auto report{refrain::data::drain_pending_deletes(
                store_root_, budget_ms_, [](std::string_view line) {
                    if (line.starts_with("warning:")) {
                        log_server.warn("gc-delete: {}", line);
                    } else {
                        log_server.info("gc-delete: {}", line);
                    }
                })};
            if (!report) {
                log_server.error("gc-delete: {}", report.error().message);
            } else if (report->deleted != 0U || report->deferred != 0U ||
                       report->failed != 0U || report->failed_stuck != 0U ||
                       report->skipped_referenced != 0U) {
                log_server.info(
                    "gc-delete: deleted={} blobs={} segments={} deferred={} "
                    "failed={} stuck={} remaining={}",
                    report->deleted, report->deleted_blobs,
                    report->deleted_segments, report->deferred, report->failed,
                    report->failed_stuck, report->remaining);
            }

            // A pass can end early on the budget or on a retryable failure.
            // Other stuck entries are left for manual repair; only a new
            // enqueue wakes the worker again.
            const bool retryable{!report ||
                                 report->remaining > report->failed_stuck};
            if (retryable &&
                condition_.wait_for(lock, gc_delete_retry_delay, [this] {
                    return stopping_.load(std::memory_order_acquire) ||
                           wake_requested_.load(std::memory_order_acquire);
                })) {
                // Woken by notify()/stop(); the loop re-checks both.
            }
        }
    }

    std::string store_root_;
    std::uint64_t budget_ms_{0};
    /// Serialises the tick planner (enqueue) and the worker (drain). Held for
    /// the whole worker pass by design.
    std::mutex mutex_;
    std::condition_variable condition_;
    std::atomic<bool> wake_requested_{true};
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

auto run_content_gc(const server_run_options &options, gc_delete_worker *worker)
    -> refrain::status {
    // The pending-delete queue has exactly one writer at a time. Apply mode
    // owns a worker: if it is in a drain pass, skip this interval instead of
    // blocking the tick thread on a possibly blocking unlink. Dry-run has no
    // worker and never touches the queue.
    std::optional<std::unique_lock<std::mutex>> queue_lock{};
    if (worker != nullptr) {
        queue_lock.emplace(worker->queue_mutex(), std::try_to_lock);
        if (!queue_lock->owns_lock()) {
            log_content.debug("busy");
            return refrain::status_ok();
        }
    }
    const auto index{
        fmt::format("{}/content/blobs.idx", options.content_store)};
    if (!refrain::core::io::exists(index)) {
        log_content.debug("no index");
        return refrain::status_ok();
    }
    auto receipts{refrain::mod::list_receipt_paths(options.content_store)};
    if (!receipts) {
        log_content.error("receipt scan failed: {}", receipts.error().message);
        return std::unexpected{refrain::make_error(
            refrain::error_code::io_error,
            fmt::format("content-gc: receipt scan failed: {}",
                        receipts.error().message))};
    }
    refrain::data::gc_options gc{};
    gc.apply = options.content_gc_apply;
    gc.compact = false;
    gc.keep_days = options.content_gc_keep_days;
    gc.budget_ms = options.content_gc_budget_ms;
    // apply: the planner only persists the plan and the `gc-delete` worker
    // drains it; the tick thread never unlinks. dry-run: apply stays false, so
    // nothing is enqueued or deleted (defer_deletes is irrelevant).
    gc.defer_deletes = options.content_gc_apply;
    for (const auto &path : *receipts) {
        auto receipt{refrain::mod::read_receipt(path)};
        if (!receipt) {
            log_content.error("{}: {}", path, receipt.error().message);
            continue;
        }
        for (const auto &file : receipt->files) {
            gc.extra_roots.push_back(file.blake3);
        }
    }
    auto report{refrain::data::collect_garbage(
        options.content_store, gc,
        [](std::string_view line) { log_content.info("{}", line); })};
    if (!report) {
        log_content.error("{}", report.error().message);
        return std::unexpected{refrain::make_error(
            report.error().code,
            fmt::format("content-gc: {}", report.error().message))};
    }
    if (worker != nullptr) {
        // The enqueue is complete; let the worker pick it up even when this
        // run planned nothing new (an earlier queue may still be pending).
        worker->notify();
        log_content.info("queued {} blobs {} segments", report->queued_blobs,
                         report->queued_segments);
    } else {
        log_content.info("dry run");
    }
    return refrain::status_ok();
}

// --- periodic maintenance through the scheduler ----------------------------
//
// Each maintenance action is a schedule task instead of an inline
// `ticks_run % interval` block, so ordering, coalescing and failure accounting
// live in one place (`core::schedule`). The log lines are part of the CLI/log
// contract.
struct maintenance_context {
    refrain::sim::world *world{nullptr};
    snapshot_writer *writer{nullptr};
    const server_run_options *options{nullptr};
    std::optional<gc_delete_worker> *gc_worker{nullptr};
};

[[nodiscard]] auto maintenance_exception(std::string_view task,
                                         const char *what) noexcept
    -> refrain::status {
    return std::unexpected{refrain::make_error(
        refrain::error_code::unknown,
        fmt::format("{}: unexpected exception ({})", task, what))};
}

auto maintenance_autosave(void *self, const void *, std::int64_t) noexcept
    -> refrain::status {
    auto &context{*static_cast<maintenance_context *>(self)};
    try {
        auto snapshot{context.world->snapshot_pending()};
        if (snapshot.empty()) {
            return refrain::status_ok();
        }
        const auto batch_size{snapshot.size()};
        if (context.writer->submit(std::move(snapshot))) {
            log_storage.debug("{} chunks tick={}", batch_size,
                              context.world->ticks());
        } else {
            log_storage.warn("writer busy");
            return std::unexpected{
                refrain::make_error(refrain::error_code::io_error,
                                    "autosave: writer busy, snapshot skipped")};
        }
        return refrain::status_ok();
    } catch (...) {
        return maintenance_exception("autosave", "snapshot/submit");
    }
}

auto maintenance_content_gc(void *self, const void *, std::int64_t) noexcept
    -> refrain::status {
    auto &context{*static_cast<maintenance_context *>(self)};
    try {
        const auto worker{context.gc_worker != nullptr &&
                                  context.gc_worker->has_value()
                              ? &**context.gc_worker
                              : nullptr};
        return run_content_gc(*context.options, worker);
    } catch (...) {
        return maintenance_exception("content-gc", "collect_garbage");
    }
}

auto maintenance_store_compact(void *self, const void *, std::int64_t) noexcept
    -> refrain::status {
    auto &context{*static_cast<maintenance_context *>(self)};
    try {
        auto compacted{context.world->compact_stores()};
        if (!compacted) {
            log_server.error("store compaction failed: {}",
                             compacted.error().message);
            return std::unexpected{compacted.error()};
        }
        log_storage.info("objects={} blobs={}", compacted->objects.segments,
                         compacted->blobs.segments);
        return refrain::status_ok();
    } catch (...) {
        return maintenance_exception("store-compact", "compact_stores");
    }
}

auto maintenance_region_compact(void *self, const void *, std::int64_t) noexcept
    -> refrain::status {
    auto &context{*static_cast<maintenance_context *>(self)};
    try {
        auto compacted{context.world->compact_regions(
            context.options->compact_min_bytes,
            context.options->compact_garbage_percent)};
        if (!compacted) {
            log_server.error("runtime compaction failed: {}",
                             compacted.error().message);
            return std::unexpected{compacted.error()};
        }
        if (*compacted != 0) {
            log_storage.info("{} regions", *compacted);
        }
        return refrain::status_ok();
    } catch (...) {
        return maintenance_exception("region-compact", "compact_regions");
    }
}

} // namespace

struct maintenance_service::impl {
    refrain::sim::world *world{nullptr};
    server_run_options options{};
    std::optional<snapshot_writer> writer{};
    std::optional<gc_delete_worker> gc_worker{};
    maintenance_context task_context{};
    refrain::core::schedule::scheduler scheduler{};
    bool stopped{false};
};

maintenance_service::maintenance_service() : impl_{std::make_unique<impl>()} {}

maintenance_service::~maintenance_service() { stop(); }

auto maintenance_service::start(refrain::sim::world &world,
                                const server_run_options &options) -> status {
    auto &state{*impl_};
    state.world = &world;
    state.options = options;
    state.writer.emplace(world);
    // One background deleter, only for an applying GC policy. `--content-gc
    // off|dry-run` must never delete: no worker is started and a queue left by
    // an earlier apply run is left untouched (clearing it requires an explicit
    // maintenance action, not an implicit startup sweep).
    if (!state.options.content_store.empty() &&
        state.options.content_gc_apply) {
        state.gc_worker.emplace(state.options.content_store,
                                state.options.content_gc_budget_ms);
    }
    state.task_context = maintenance_context{
        .world = &world,
        .writer = &*state.writer,
        .options = &state.options,
        .gc_worker = &state.gc_worker,
    };

    // Registration order is fixed: autosave, content-gc, store-compact,
    // region-compact.
    std::array<refrain::core::schedule::task_descriptor, 4> tasks{};
    std::size_t task_count{0};
    const auto clamp_interval = [](std::uint64_t value) {
        const auto limit{static_cast<std::uint64_t>(
            refrain::core::schedule::max_interval_ticks)};
        return value > limit ? limit : value;
    };
    const auto add_task = [&](std::string_view method, std::uint64_t interval,
                              refrain::core::schedule::invoker invoke) {
        const auto clamped{clamp_interval(interval)};
        if (clamped != interval) {
            log_schedule.warn("{} interval {}->{}", method, interval, clamped);
        }
        tasks[task_count++] = refrain::core::schedule::task_descriptor{
            .service = "runtime",
            .method = method,
            .interval_ticks = static_cast<std::int64_t>(clamped),
            .phase_of = refrain::core::schedule::phase::post_tick,
            .invoke = invoke,
        };
    };
    if (state.options.autosave_interval_ticks > 0) {
        add_task("autosave", state.options.autosave_interval_ticks,
                 &maintenance_autosave);
    }
    if (state.options.content_gc_interval_ticks > 0 &&
        !state.options.content_store.empty()) {
        add_task("content-gc", state.options.content_gc_interval_ticks,
                 &maintenance_content_gc);
    }
    if (state.options.store_compact_interval_ticks > 0) {
        add_task("store-compact", state.options.store_compact_interval_ticks,
                 &maintenance_store_compact);
    }
    if (state.options.compact_interval_ticks > 0) {
        add_task("region-compact", state.options.compact_interval_ticks,
                 &maintenance_region_compact);
    }
    if (task_count > 0) {
        if (auto added{state.scheduler.add(std::span{tasks.data(), task_count},
                                           &state.task_context, nullptr)};
            !added) {
            return std::unexpected{added.error()};
        }
    }
    return status_ok();
}

void maintenance_service::collect_completed() {
    auto &state{*impl_};
    if (!state.writer || state.world == nullptr) {
        return;
    }
    for (const auto &batch : state.writer->drain_completed()) {
        state.world->mark_clean(batch);
    }
}

void maintenance_service::pre_tick(std::int64_t tick) {
    impl_->scheduler.run(refrain::core::schedule::phase::pre_tick, tick);
}

void maintenance_service::post_tick(std::int64_t tick) {
    impl_->scheduler.run(refrain::core::schedule::phase::post_tick, tick);
}

void maintenance_service::drain() {
    if (impl_->writer) {
        impl_->writer->drain();
    }
}

void maintenance_service::stop() noexcept {
    if (!impl_ || impl_->stopped) {
        return;
    }
    impl_->stopped = true;
    // Tasks borrow the store, the writer and the GC worker; drop them before
    // the workers are torn down.
    impl_->scheduler.clear();
    if (impl_->writer) {
        impl_->writer->stop();
    }
    collect_completed();
    if (impl_->gc_worker) {
        impl_->gc_worker->stop();
    }
}

auto maintenance_service::tasks() const
    -> std::vector<refrain::core::schedule::task_info> {
    return impl_->scheduler.tasks();
}

} // namespace refrain::server
