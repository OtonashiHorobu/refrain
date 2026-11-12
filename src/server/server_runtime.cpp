// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/protocol/current.hpp>
#include <refrain/server/server_runtime.hpp>

#include <refrain/session/chunk_tracker.hpp>
#include <refrain/session/play_session.hpp>

#include <refrain/app/services.hpp>
#include <refrain/core/io.hpp>
#include <refrain/core/log.hpp>
#include <refrain/core/schedule.hpp>
#include <refrain/core/time.hpp>
#include <refrain/data/content_maintenance.hpp>
#include <refrain/data/vanilla.hpp>
#include <refrain/mod/rfmod.hpp>
#include <refrain/net/registry.hpp>
#include <refrain/net/server.hpp>
#include <refrain/server/chunk_packet.hpp>
#include <refrain/server/chunk_view.hpp>
#include <refrain/server/command_bridge.hpp>
#include <refrain/server/entity_broadcast.hpp>
#include <refrain/server/inventory_menu.hpp>
#include <refrain/server/player_visibility.hpp>
#include <refrain/sim/replay.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/store.hpp>

#include <fmt/format.h>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace refrain::server {

using app::report_error;
namespace {

constexpr std::size_t max_console_commands = 1024;
constexpr std::uint64_t max_console_file_bytes = 1ULL * 1024ULL * 1024ULL;

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
                refrain::log::error("autosave: {}", written.error().message);
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

struct runtime_state {
    mutable std::mutex commands_mutex{};
    std::vector<std::string> commands{};
    std::atomic<bool> stop{false};
    std::atomic<std::int64_t> heartbeat_ns{0};
    refrain::core::config::store *config{nullptr};
    static constexpr std::size_t tick_samples{2048};
    std::array<std::uint32_t, tick_samples> tick_us{};
    std::size_t tick_count{0};
    std::uint64_t tick_total_us{0};
    std::uint64_t tick_max_us{0};
};

void record_tick_work(runtime_state &state, std::uint64_t work_ns) {
    const auto us{static_cast<std::uint32_t>(work_ns / 1'000ULL)};
    state.tick_us[state.tick_count % runtime_state::tick_samples] = us;
    ++state.tick_count;
    state.tick_total_us += us;
    state.tick_max_us =
        std::max(state.tick_max_us, static_cast<std::uint64_t>(us));
}

void push_command(const std::shared_ptr<runtime_state> &state,
                  std::string line) {
    if (line.empty()) {
        return;
    }
    const std::lock_guard lock{state->commands_mutex};
    if (state->commands.size() < max_console_commands) {
        state->commands.push_back(std::move(line));
    }
}

[[nodiscard]] auto drain_commands(const std::shared_ptr<runtime_state> &state)
    -> std::vector<std::string> {
    std::vector<std::string> drained{};
    const std::lock_guard lock{state->commands_mutex};
    drained.swap(state->commands);
    return drained;
}

void push_text_commands(const std::shared_ptr<runtime_state> &state,
                        std::string_view text) {
    std::size_t start{0};
    while (start <= text.size()) {
        const auto newline{text.find('\n', start)};
        const auto end{newline == std::string_view::npos ? text.size()
                                                         : newline};
        auto line{std::string{text.substr(start, end - start)}};
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        push_command(state, std::move(line));
        if (newline == std::string_view::npos) {
            break;
        }
        start = newline + 1;
    }
}

void start_console_thread(const std::shared_ptr<runtime_state> &state) {
    std::thread console{[state] {
        refrain::log::set_thread_name("console");
        std::string line{};
        try {
            while (!state->stop.load(std::memory_order_acquire) &&
                   std::getline(std::cin, line)) {
                push_command(state, line);
            }
        } catch (...) {
            // The console is diagnostics: a failed read must not terminate the
            // server; the tick loop can still be stopped by `--ticks`/signal.
        }
    }};
    // A blocking stdin read cannot be woken portably. The console thread only
    // touches the shared state, so it is safe to detach at shutdown and let
    // process exit reclaim it.
    console.detach();
}

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
                        refrain::log::warn("gc-delete: {}", line);
                    } else {
                        refrain::log::info("gc-delete: {}", line);
                    }
                })};
            if (!report) {
                refrain::log::error("gc-delete: {}", report.error().message);
            } else if (report->deleted != 0U || report->deferred != 0U ||
                       report->failed != 0U || report->failed_stuck != 0U ||
                       report->skipped_referenced != 0U) {
                refrain::log::info(
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
            refrain::log::debug("content-gc: busy");
            return refrain::status_ok();
        }
    }
    const auto index{
        fmt::format("{}/content/blobs.idx", options.content_store)};
    if (!refrain::core::io::exists(index)) {
        refrain::log::debug("content-gc: no index");
        return refrain::status_ok();
    }
    auto receipts{refrain::mod::list_receipt_paths(options.content_store)};
    if (!receipts) {
        refrain::log::error("content-gc: receipt scan failed: {}",
                            receipts.error().message);
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
            refrain::log::error("content-gc: {}: {}", path,
                                receipt.error().message);
            continue;
        }
        for (const auto &file : receipt->files) {
            gc.extra_roots.push_back(file.blake3);
        }
    }
    auto report{refrain::data::collect_garbage(
        options.content_store, gc, [](std::string_view line) {
            refrain::log::info("content-gc: {}", line);
        })};
    if (!report) {
        refrain::log::error("content-gc: {}", report.error().message);
        return std::unexpected{refrain::make_error(
            report.error().code,
            fmt::format("content-gc: {}", report.error().message))};
    }
    if (worker != nullptr) {
        // The enqueue is complete; let the worker pick it up even when this
        // run planned nothing new (an earlier queue may still be pending).
        worker->notify();
        refrain::log::info("content-gc: queued {} blobs {} segments",
                           report->queued_blobs, report->queued_segments);
    } else {
        refrain::log::info("content-gc: dry run");
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
            refrain::log::debug("autosave: {} chunks tick={}", batch_size,
                                context.world->ticks());
        } else {
            refrain::log::warn("autosave: writer busy");
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
            refrain::log::error("store compaction failed: {}",
                                compacted.error().message);
            return std::unexpected{compacted.error()};
        }
        refrain::log::info("store compaction: objects={} blobs={}",
                           compacted->objects.segments,
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
            refrain::log::error("runtime compaction failed: {}",
                                compacted.error().message);
            return std::unexpected{compacted.error()};
        }
        if (*compacted != 0) {
            refrain::log::info("compaction: {} regions", *compacted);
        }
        return refrain::status_ok();
    } catch (...) {
        return maintenance_exception("region-compact", "compact_regions");
    }
}

void print_console_help() {
    refrain::log::info(
        "console: commands: stop, save, tps, hash, pos, move <dx> <dy> <dz>, "
        "walk <dx> <dz>, jump, dig <x> <y> <z>, place <x> <y> <z> <block>, "
        "give <item> <count>, inv, spawn <type> <x> <y> <z>, entities, "
        "kill <entity-id>, palette-gc, schedules, config <namespace> "
        "get|set <key> [value], config save|reload [namespace], "
        "config names");
}

[[nodiscard]] auto split_console_line(std::string_view line)
    -> std::vector<std::string_view> {
    std::vector<std::string_view> tokens{};
    std::size_t position{0};
    while (position < line.size()) {
        while (position < line.size() && line[position] == ' ') {
            ++position;
        }
        if (position == line.size()) {
            break;
        }
        const auto start{position};
        while (position < line.size() && line[position] != ' ') {
            ++position;
        }
        tokens.push_back(line.substr(start, position - start));
    }
    return tokens;
}

template <typename T>
[[nodiscard]] auto parse_number(std::string_view text, T &out) -> bool {
    const auto parsed{
        std::from_chars(text.data(), text.data() + text.size(), out)};
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

[[nodiscard]] auto parse_config_value(std::string_view text)
    -> std::variant<bool, std::int64_t, double, std::string> {
    if (text == "true") {
        return true;
    }
    if (text == "false") {
        return false;
    }
    std::int64_t integer{0};
    const auto int_parsed{
        std::from_chars(text.data(), text.data() + text.size(), integer)};
    if (int_parsed.ec == std::errc{} &&
        int_parsed.ptr == text.data() + text.size()) {
        return integer;
    }
    double real{0.0};
    const auto real_parsed{
        std::from_chars(text.data(), text.data() + text.size(), real)};
    if (real_parsed.ec == std::errc{} &&
        real_parsed.ptr == text.data() + text.size()) {
        return real;
    }
    return std::string{text};
}

void execute_config_command(std::span<const std::string_view> tokens,
                            refrain::core::config::store *config) {
    namespace cfg = refrain::core::config;
    if (config == nullptr) {
        refrain::log::warn("config: no host configuration store");
        return;
    }
    if (tokens.size() >= 2 && tokens[1] == "names") {
        const auto names{config->names()};
        std::string joined{};
        for (const auto &name : names) {
            if (!joined.empty()) {
                joined += ' ';
            }
            joined += name;
        }
        refrain::log::info("config: namespaces={} list={}", names.size(),
                           joined);
        return;
    }
    if (tokens.size() >= 2 && tokens[1] == "save") {
        if (tokens.size() == 2) {
            if (auto saved{config->save_all()}; !saved) {
                refrain::log::error("config: save failed: {}",
                                    saved.error().message);
            } else {
                refrain::log::info("config: saved dirty namespaces");
            }
        } else if (auto saved{config->save(tokens[2])}; !saved) {
            refrain::log::error("config: save '{}' failed: {}", tokens[2],
                                saved.error().message);
        } else {
            refrain::log::info("config: saved '{}'", tokens[2]);
        }
        return;
    }
    if (tokens.size() >= 2 && tokens[1] == "reload") {
        if (tokens.size() == 2) {
            if (auto loaded{config->reload_all()}; !loaded) {
                refrain::log::error("config: reload failed: {}",
                                    loaded.error().message);
            } else {
                refrain::log::info(
                    "config: reloaded; running subsystems keep their "
                    "startup snapshot until restart");
            }
        } else if (auto loaded{config->reload(tokens[2])}; !loaded) {
            refrain::log::error("config: reload '{}' failed: {}", tokens[2],
                                loaded.error().message);
        } else {
            refrain::log::info(
                "config: reloaded '{}'; restart required to apply", tokens[2]);
        }
        return;
    }
    if (tokens.size() < 4) {
        refrain::log::warn(
            "config: usage: config <namespace> get <key> | set <key> <value> "
            "| save [namespace] | reload [namespace] | names");
        return;
    }
    const auto name_space{tokens[1]};
    const auto action{tokens[2]};
    const auto key{tokens[3]};
    auto module{config->module(name_space)};
    if (!module) {
        refrain::log::warn("config: unknown namespace '{}'", name_space);
        return;
    }
    if (action == "get") {
        const auto kind{module->kind(key)};
        if (kind == cfg::value_kind::missing) {
            refrain::log::warn("config: {}.{} is not set", name_space, key);
        } else if (kind == cfg::value_kind::boolean) {
            refrain::log::info("config: {}.{} = {}", name_space, key,
                               module->get_bool(key, false));
        } else if (kind == cfg::value_kind::integer) {
            refrain::log::info("config: {}.{} = {}", name_space, key,
                               module->get_int64(key, 0));
        } else if (kind == cfg::value_kind::floating) {
            refrain::log::info("config: {}.{} = {}", name_space, key,
                               module->get_double(key, 0.0));
        } else if (kind == cfg::value_kind::string) {
            refrain::log::info("config: {}.{} = '{}'", name_space, key,
                               module->get_string(key, ""));
        } else if (kind == cfg::value_kind::array) {
            const auto values{module->get_strings(key)};
            refrain::log::info("config: {}.{} = {} strings", name_space, key,
                               values.size());
        } else {
            refrain::log::info("config: {}.{} = <table>", name_space, key);
        }
        return;
    }
    if (action == "set") {
        if (tokens.size() < 5) {
            refrain::log::warn("config: set expects a value");
            return;
        }
        const auto value{parse_config_value(tokens[4])};
        const auto result{std::visit(
            [&](const auto &typed) -> status {
                using T = std::decay_t<decltype(typed)>;
                if constexpr (std::is_same_v<T, bool>) {
                    return config->set_bool(name_space, key, typed);
                } else if constexpr (std::is_same_v<T, std::int64_t>) {
                    return config->set_int64(name_space, key, typed);
                } else if constexpr (std::is_same_v<T, double>) {
                    return config->set_double(name_space, key, typed);
                } else {
                    return config->set_string(name_space, key, typed);
                }
            },
            value)};
        if (!result) {
            refrain::log::error("config: set failed: {}",
                                result.error().message);
        } else {
            refrain::log::info(
                "config: {}.{} updated in memory; 'config save' persists it "
                "and subsystems pick it up after restart",
                name_space, key);
        }
        return;
    }
    refrain::log::warn("config: unknown action '{}'", action);
}

void execute_console_command(const std::string &line,
                             const std::shared_ptr<runtime_state> &state,
                             refrain::sim::world &store,
                             snapshot_writer &writer,
                             const refrain::core::tick_info &tick,
                             refrain::core::schedule::scheduler *scheduler,
                             refrain::net::server *listener) {
    if (line == "stop") {
        refrain::log::info("console: stop");
        state->stop.store(true, std::memory_order_release);
        return;
    }
    if (line == "palette-gc") {
        // Quiesced maintenance: this runs on the sim thread, so no tick or
        // console command overlaps it. Drain the writer first; the rebuild
        // closes/reopens the store and reloads the previously loaded chunks.
        writer.drain();
        auto rebuilt{
            store.rebuild_palette_in_place(1, store.store().zstd_level())};
        if (!rebuilt) {
            refrain::log::error("console: palette-gc failed: {}",
                                rebuilt.error().message);
            // A failed reopen leaves the store invalid; continuing to tick
            // would operate on a closed world.
            if (!store.store().valid()) {
                state->stop.store(true, std::memory_order_release);
            }
            return;
        }
        refrain::log::info("palette-gc: entries {}->{} bytes {}->{}",
                           rebuilt->entries_before, rebuilt->entries_after,
                           rebuilt->bytes_before, rebuilt->bytes_after);
        return;
    }
    if (line == "save") {
        // Drain the background writer first: a snapshot taken before this
        // command could otherwise be written after flush() and resurrect an
        // older chunk revision.
        writer.drain();
        auto committed{store.flush()};
        if (!committed) {
            refrain::log::error("save: {}", committed.error().message);
        } else {
            refrain::log::info("save: ok");
        }
        return;
    }
    if (line == "net") {
        if (listener == nullptr) {
            refrain::log::warn("console: net is unavailable");
            return;
        }
        const auto stats{listener->stats()};
        refrain::log::info(
            "net: players={} in={} out={} bytes_in={} bytes_out={}",
            stats.connections, stats.packets_in, stats.packets_out,
            stats.bytes_in, stats.bytes_out);
        return;
    }
    if (line == "tps") {
        const auto samples{
            std::min(state->tick_count, runtime_state::tick_samples)};
        std::vector<std::uint32_t> sorted(state->tick_us.begin(),
                                          state->tick_us.begin() + samples);
        std::sort(sorted.begin(), sorted.end());
        const auto percentile = [&](double fraction) {
            if (sorted.empty()) {
                return std::uint64_t{0};
            }
            const auto index{static_cast<std::size_t>(
                static_cast<double>(sorted.size() - 1) * fraction)};
            return static_cast<std::uint64_t>(sorted[index]);
        };
        const auto average{state->tick_count == 0
                               ? 0.0
                               : static_cast<double>(state->tick_total_us) /
                                     static_cast<double>(state->tick_count) /
                                     1000.0};
        refrain::log::info(
            "tick: {} interval={}ms missed={} work_avg={:.2f}ms "
            "work_p99={:.2f}ms work_max={:.2f}ms samples={}",
            tick.index,
            std::chrono::duration_cast<std::chrono::milliseconds>(tick.actual)
                .count(),
            tick.missed, average,
            static_cast<double>(percentile(0.99)) / 1000.0,
            static_cast<double>(state->tick_max_us) / 1000.0, samples);
        return;
    }
    if (line == "hash") {
        refrain::log::info("hash: {}",
                           refrain::data::to_hex(store.state_hash()));
        return;
    }
    if (line == "inv") {
        if (store.player().inventory.empty()) {
            refrain::log::info("inventory: empty");
        } else {
            for (const auto &stack : store.player().inventory) {
                refrain::log::info("inventory: {} x{}", stack.item,
                                   stack.count);
            }
        }
        return;
    }
    if (line == "pos") {
        const auto &player{store.player()};
        refrain::log::info("player: x={:.1f} y={:.1f} z={:.1f}", player.x,
                           player.y, player.z);
        return;
    }
    if (line == "entities") {
        if (store.entities().empty()) {
            refrain::log::info("entities: none");
        } else {
            for (const auto &[id, value] : store.entities()) {
                refrain::log::info("entity {} {} x={:.1f} y={:.1f} z={:.1f}",
                                   id, value.type, value.x, value.y, value.z);
            }
        }
        return;
    }
    const auto tokens{split_console_line(line)};
    if (!tokens.empty() && tokens[0] == "config") {
        execute_config_command(tokens, state->config);
        return;
    }
    if (line == "jump") {
        store.jump();
        refrain::log::info("jump: on_ground={}", store.player().on_ground);
        return;
    }
    if (tokens.size() == 3 && tokens[0] == "give") {
        std::uint32_t count{0};
        if (!parse_number(tokens[2], count) || count == 0) {
            refrain::log::warn("console: give expects a positive count");
            return;
        }
        auto added{store.add_item(tokens[1], count)};
        if (!added) {
            refrain::log::warn("console: give failed: {}",
                               added.error().message);
            return;
        }
        refrain::log::info("give: {} x{}", tokens[1], count);
        return;
    }
    if (tokens.size() == 5 && tokens[0] == "spawn") {
        double x{0.0};
        double y{0.0};
        double z{0.0};
        if (!parse_number(tokens[2], x) || !parse_number(tokens[3], y) ||
            !parse_number(tokens[4], z)) {
            refrain::log::warn("console: spawn expects numeric coordinates");
            return;
        }
        auto spawned{store.spawn_entity(tokens[1], x, y, z)};
        if (!spawned) {
            refrain::log::warn("console: spawn failed: {}",
                               spawned.error().message);
            return;
        }
        refrain::log::info("spawn: {} #{}", tokens[1], *spawned);
        return;
    }
    if (tokens.size() == 2 && tokens[0] == "kill") {
        refrain::sim::entity_id id{0};
        if (!parse_number(tokens[1], id)) {
            refrain::log::warn("console: kill expects an entity id");
            return;
        }
        if (!store.remove_entity(id)) {
            refrain::log::warn("console: no entity {}", id);
            return;
        }
        refrain::log::info("kill: #{}", id);
        return;
    }
    if (tokens.size() == 3 && tokens[0] == "walk") {
        double dx{0.0};
        double dz{0.0};
        if (!parse_number(tokens[1], dx) || !parse_number(tokens[2], dz)) {
            refrain::log::warn("console: walk expects two numbers");
            return;
        }
        const auto moved{store.move_player(dx, 0.0, dz)};
        refrain::log::info("walk: {:.1f},{:.1f},{:.1f} {}", store.player().x,
                           store.player().y, store.player().z,
                           moved ? "moved" : "blocked");
        return;
    }
    if (tokens.size() == 4 && tokens[0] == "move") {
        double dx{0.0};
        double dy{0.0};
        double dz{0.0};
        if (!parse_number(tokens[1], dx) || !parse_number(tokens[2], dy) ||
            !parse_number(tokens[3], dz)) {
            refrain::log::warn("console: move expects three numbers");
            return;
        }
        auto &player{store.player()};
        store.set_player_position(player.x + dx, player.y + dy, player.z + dz);
        refrain::log::info("move: {:.1f},{:.1f},{:.1f}", player.x, player.y,
                           player.z);
        return;
    }
    if ((tokens.size() == 4 && tokens[0] == "dig") ||
        (tokens.size() == 5 && tokens[0] == "place")) {
        std::int32_t x{0};
        std::int32_t y{0};
        std::int32_t z{0};
        if (!parse_number(tokens[1], x) || !parse_number(tokens[2], y) ||
            !parse_number(tokens[3], z)) {
            refrain::log::warn("console: {} expects integer coordinates",
                               tokens[0]);
            return;
        }
        if (y < std::numeric_limits<std::int16_t>::min() ||
            y > std::numeric_limits<std::int16_t>::max()) {
            refrain::log::warn("console: block y={} is out of range", y);
            return;
        }
        auto value{refrain::sim::air_block};
        std::optional<std::string> gained{};
        if (tokens[0] == "dig") {
            const auto existing{store.block_at(
                refrain::sim::block_pos{x, static_cast<std::int16_t>(y), z})};
            if (existing != refrain::sim::air_block) {
                gained = std::string{store.registry().key(existing)};
            }
        } else if (tokens[0] == "place") {
            value = store.registry().intern(std::string{tokens[4]});
        }
        auto applied{store.set_block(
            refrain::sim::block_pos{x, static_cast<std::int16_t>(y), z},
            value)};
        if (!applied) {
            refrain::log::warn("console: {} failed: {}", tokens[0],
                               applied.error().message);
            return;
        }
        if (gained) {
            (void)store.add_item(*gained, 1);
        }
        if (tokens[0] == "place") {
            // The server remains creative-permissive: consume when available,
            // but do not reject a scripted placement without a prior `give`.
            (void)store.remove_item(tokens[4], 1);
        }
        refrain::log::info("{}: {},{},{} -> {}", tokens[0], x, y, z,
                           tokens[0] == "place" ? tokens[4] : "air");
        return;
    }
    if (line == "help") {
        print_console_help();
        return;
    }
    if (line == "schedules") {
        if (scheduler == nullptr) {
            refrain::log::info("schedules: unavailable");
            return;
        }
        const auto tasks{scheduler->tasks()};
        if (tasks.empty()) {
            refrain::log::info("schedules: none");
            return;
        }
        for (const auto &task : tasks) {
            refrain::log::info(
                "schedule: {}.{} phase={} interval={} enabled={} runs={} "
                "failures={} coalesced={} last_tick={}",
                task.service, task.method,
                refrain::core::schedule::phase_name(task.phase_of),
                task.interval_ticks, task.enabled ? "yes" : "no",
                task.stats.runs, task.stats.failures, task.stats.coalesced,
                task.stats.last_tick);
            if (!task.stats.last_error.empty()) {
                refrain::log::info("schedule: {}.{} last_error={}",
                                   task.service, task.method,
                                   task.stats.last_error);
            }
        }
        return;
    }
    refrain::log::warn("console: unknown '{}'", line);
}

} // namespace

namespace {

constexpr std::uint8_t player_info_add{1U};
constexpr std::uint8_t player_info_game_mode{4U};
constexpr std::uint8_t player_info_listed{8U};
constexpr std::uint8_t player_info_latency{16U};
// TODO: the protocol entity type id is hard-coded for the player; non-player
// entities need a table built from the entity_type registry.
constexpr std::uint32_t player_entity_type{159U};

template <typename Packet, typename Encoder>
[[nodiscard]] auto encode_play_body(std::uint16_t packet_id,
                                    const Packet &packet, Encoder encode)
    -> std::vector<std::byte> {
    std::vector<std::byte> body;
    refrain::net::writer writer{body};
    writer.write_varint(packet_id);
    if (auto encoded{encode(writer, packet)}; !encoded) {
        body.clear();
    }
    return body;
}

[[nodiscard]] auto session_entry(const refrain::session::player &value)
    -> refrain::net::player_info_entry {
    return refrain::net::player_info_entry{
        .id = value.id,
        .name = value.name,
        .game_mode = value.game_mode,
        .listed = true,
        .latency = 0U,
    };
}

void send_player_info(refrain::net::server &listener, std::uint64_t connection,
                      std::uint8_t actions,
                      const std::vector<refrain::session::player> &players) {
    refrain::net::player_info_update payload{};
    payload.actions = actions;
    payload.entries.reserve(players.size());
    for (const auto &player : players) {
        payload.entries.push_back(session_entry(player));
    }
    refrain::protocol::current::play::clientbound::player_info_update packet{};
    packet.payload = std::move(payload);
    auto body{encode_play_body(refrain::protocol::current::play::clientbound::
                                   player_info_update::packet_id,
                               packet,
                               refrain::protocol::current::play::clientbound::
                                   encode_player_info_update)};
    if (!body.empty()) {
        listener.send_raw(connection, body);
    }
}

[[nodiscard]] auto encode_add_entity_body(const refrain::session::player &value)
    -> std::vector<std::byte> {
    refrain::protocol::current::play::clientbound::add_entity packet{
        .id = static_cast<std::int32_t>(value.entity_id),
        .uuid = value.id,
        .type_ = player_entity_type,
        .x = value.x,
        .y = value.y,
        .z = value.z,
        .movement = {},
        .x_rot = 0,
        .y_rot = 0,
        .y_head_rot = 0,
        .data = 0,
    };
    return encode_play_body(
        refrain::protocol::current::play::clientbound::add_entity::packet_id,
        packet,
        refrain::protocol::current::play::clientbound::encode_add_entity);
}

void send_remove_entity(refrain::net::server &listener,
                        std::uint64_t connection, std::uint64_t entity_id);

[[nodiscard]] auto encode_remove_entity_body(std::uint64_t entity_id)
    -> std::vector<std::byte> {
    refrain::protocol::current::play::clientbound::remove_entities packet{};
    packet.entity_ids.push_back(static_cast<std::uint32_t>(entity_id));
    return encode_play_body(
        refrain::protocol::current::play::clientbound::remove_entities::
            packet_id,
        packet,
        refrain::protocol::current::play::clientbound::encode_remove_entities);
}

void send_remove_entity(refrain::net::server &listener,
                        std::uint64_t connection, std::uint64_t entity_id) {
    auto body{encode_remove_entity_body(entity_id)};
    if (!body.empty()) {
        listener.send_raw(connection, body);
    }
}

void announce_session_join(refrain::net::server &listener,
                           const refrain::session::play_session &players,
                           std::uint64_t connection) {
    const auto snapshot{players.all()};
    send_player_info(listener, connection, player_info_add, snapshot);
    send_player_info(listener, connection,
                     static_cast<std::uint8_t>(player_info_game_mode |
                                               player_info_listed |
                                               player_info_latency),
                     snapshot);
    for (const auto &other : snapshot) {
        if (other.connection == connection) {
            continue;
        }
        std::vector<refrain::session::player> joining{};
        for (const auto &candidate : snapshot) {
            if (candidate.connection == connection) {
                joining.push_back(candidate);
                break;
            }
        }
        send_player_info(
            listener, other.connection,
            static_cast<std::uint8_t>(player_info_add | player_info_game_mode |
                                      player_info_listed | player_info_latency),
            joining);
    }
}

void announce_session_leave(refrain::net::server &listener,
                            const refrain::session::player &left,
                            const refrain::session::play_session &players) {
    std::vector<std::uint64_t> targets;
    for (const auto &player : players.all()) {
        targets.push_back(player.connection);
    }
    refrain::protocol::current::play::clientbound::player_info_remove remove{};
    remove.profile_ids.push_back(left.id);
    auto info_body{
        encode_play_body(refrain::protocol::current::play::clientbound::
                             player_info_remove::packet_id,
                         remove,
                         refrain::protocol::current::play::clientbound::
                             encode_player_info_remove)};
    for (const auto target : targets) {
        if (!info_body.empty()) {
            listener.send_raw(target, info_body);
        }
    }
}

void send_session_health(refrain::net::server &listener,
                         std::uint64_t connection, float health) {
    refrain::protocol::current::play::clientbound::set_health packet{
        .health = health, .food = 20, .saturation = 5.0F};
    auto body{encode_play_body(
        refrain::protocol::current::play::clientbound::set_health::packet_id,
        packet,
        refrain::protocol::current::play::clientbound::encode_set_health)};
    if (!body.empty()) {
        listener.send_raw(connection, body);
    }
}

void send_session_experience(refrain::net::server &listener,
                             std::uint64_t connection) {
    refrain::protocol::current::play::clientbound::set_experience packet{
        .experience_progress = 0.0F,
        .experience_level = 0,
        .total_experience = 0,
    };
    auto body{encode_play_body(
        refrain::protocol::current::play::clientbound::set_experience::
            packet_id,
        packet,
        refrain::protocol::current::play::clientbound::encode_set_experience)};
    if (!body.empty()) {
        listener.send_raw(connection, body);
    }
}

void send_session_animation(refrain::net::server &listener,
                            const refrain::session::play_session &players,
                            std::uint64_t source, std::uint64_t target,
                            std::int8_t action) {
    const auto mover{players.player_of(source)};
    if (mover == nullptr) {
        return;
    }
    refrain::protocol::current::play::clientbound::animate packet{
        .id = static_cast<std::int32_t>(mover->entity_id),
        .action = action,
    };
    auto body{encode_play_body(
        refrain::protocol::current::play::clientbound::animate::packet_id,
        packet, refrain::protocol::current::play::clientbound::encode_animate)};
    if (!body.empty()) {
        listener.send_raw(target, body);
    }
}

void send_session_hurt(refrain::net::server &listener,
                       const refrain::session::player &target) {
    refrain::protocol::current::play::clientbound::hurt_animation packet{
        .id = static_cast<std::int32_t>(target.entity_id),
        .yaw = 0.0F,
    };
    auto body{encode_play_body(
        refrain::protocol::current::play::clientbound::hurt_animation::
            packet_id,
        packet,
        refrain::protocol::current::play::clientbound::encode_hurt_animation)};
    if (!body.empty()) {
        listener.send_raw(target.connection, body);
    }
}

void handle_session_attack(refrain::net::server &listener,
                           refrain::session::play_session &players,
                           const refrain::net::attack_event &event) {
    const auto target{players.player_by_entity(
        static_cast<std::uint32_t>(event.target_entity))};
    if (target == nullptr || target->connection == event.connection) {
        return;
    }
    const auto player_id{target->connection};
    const auto health{std::max(0.0F, target->health - 1.0F)};
    static_cast<void>(players.set_health(player_id, health));
    send_session_health(listener, player_id, health);
    send_session_hurt(listener, *target);
    for (const auto &other : players.all()) {
        if (other.connection != event.connection) {
            send_session_animation(listener, players, event.connection,
                                   other.connection, 0);
        }
    }
}

void broadcast_session_animation(refrain::net::server &listener,
                                 const refrain::session::play_session &players,
                                 std::uint64_t source, std::int8_t action) {
    for (const auto &other : players.all()) {
        if (other.connection != source) {
            send_session_animation(listener, players, source, other.connection,
                                   action);
        }
    }
}

void sync_player_visibility(
    refrain::net::server &listener,
    const refrain::session::play_session &players,
    std::unordered_map<std::uint64_t, refrain::net::movement_event> &last,
    std::unordered_map<std::uint64_t, std::unordered_set<std::uint64_t>>
        &visible,
    std::int32_t tracking_chunks) {
    const auto angle = [](float degrees) {
        return static_cast<std::int8_t>(
            std::lround(static_cast<double>(degrees) * (256.0 / 360.0)));
    };
    const auto snapshot{players.all()};
    // One buffer per target per tick: the transport then copies and posts once
    // per target instead of once per entity update.
    std::unordered_map<std::uint64_t, std::vector<std::byte>> outbox;
    const auto append = [&outbox](std::uint64_t target,
                                  const std::vector<std::byte> &body) {
        auto &buffer{outbox[target]};
        buffer.insert(buffer.end(), body.begin(), body.end());
    };

    // Visibility transitions first: a viewer that has just come into range
    // needs the spawn packet, one that left needs the removal.
    for (const auto &viewer : snapshot) {
        auto &seen{visible[viewer.connection]};
        for (const auto &other : snapshot) {
            if (other.connection == viewer.connection) {
                continue;
            }
            const refrain::server::visibility_query query{
                .world = viewer.world,
                .x = viewer.x,
                .z = viewer.z,
                .view_distance = viewer.view_distance,
            };
            const bool now{refrain::server::player_visible(
                query, other.world, other.x, other.z, tracking_chunks)};
            const bool was{seen.contains(other.connection)};
            if (now && !was) {
                if (auto body{encode_add_entity_body(other)}; !body.empty()) {
                    append(viewer.connection, body);
                }
                seen.insert(other.connection);
            } else if (!now && was) {
                if (auto body{encode_remove_entity_body(other.entity_id)};
                    !body.empty()) {
                    append(viewer.connection, body);
                }
                seen.erase(other.connection);
            }
        }
    }

    // Movement reaches only viewers that already track the mover.
    for (const auto &mover : snapshot) {
        const refrain::net::movement_event current{
            .connection = mover.connection,
            .x = mover.x,
            .y = mover.y,
            .z = mover.z,
            .yaw = mover.yaw,
            .pitch = mover.pitch,
            .on_ground = mover.on_ground,
        };
        const auto previous{last.find(mover.connection)};
        if (previous != last.end() && previous->second.x == current.x &&
            previous->second.y == current.y &&
            previous->second.z == current.z &&
            previous->second.yaw == current.yaw &&
            previous->second.pitch == current.pitch) {
            continue;
        }
        std::vector<std::byte> body;
        if (previous == last.end()) {
            refrain::protocol::current::play::clientbound::teleport_entity
                packet{};
            packet.id = static_cast<std::int32_t>(mover.entity_id);
            packet.change.position = {.x = mover.x, .y = mover.y, .z = mover.z};
            packet.change.delta_movement = {};
            packet.change.y_rot = mover.yaw;
            packet.change.x_rot = mover.pitch;
            packet.relatives = 0;
            packet.on_ground = mover.on_ground;
            body = encode_play_body(refrain::protocol::current::play::
                                        clientbound::teleport_entity::packet_id,
                                    packet,
                                    refrain::protocol::current::play::
                                        clientbound::encode_teleport_entity);
        } else {
            const auto move{
                refrain::server::choose_entity_move(previous->second, current)};
            switch (move.kind) {
            case refrain::server::entity_move_kind::teleport: {
                refrain::protocol::current::play::clientbound::teleport_entity
                    packet{};
                packet.id = static_cast<std::int32_t>(mover.entity_id);
                packet.change.position = {
                    .x = mover.x, .y = mover.y, .z = mover.z};
                packet.change.delta_movement = {};
                packet.change.y_rot = mover.yaw;
                packet.change.x_rot = mover.pitch;
                packet.relatives = 0;
                packet.on_ground = mover.on_ground;
                body = encode_play_body(
                    refrain::protocol::current::play::clientbound::
                        teleport_entity::packet_id,
                    packet,
                    refrain::protocol::current::play::clientbound::
                        encode_teleport_entity);
                break;
            }
            case refrain::server::entity_move_kind::position: {
                refrain::protocol::current::play::clientbound::move_entity_pos
                    packet{};
                packet.entity_id = static_cast<std::uint32_t>(mover.entity_id);
                packet.delta = {.on_ground = current.on_ground,
                                .x = move.dx,
                                .y = move.dy,
                                .z = move.dz,
                                .steps = {}};
                body = encode_play_body(
                    refrain::protocol::current::play::clientbound::
                        move_entity_pos::packet_id,
                    packet,
                    refrain::protocol::current::play::clientbound::
                        encode_move_entity_pos);
                break;
            }
            case refrain::server::entity_move_kind::position_rotation: {
                refrain::protocol::current::play::clientbound::
                    move_entity_pos_rot packet{};
                packet.entity_id = static_cast<std::uint32_t>(mover.entity_id);
                packet.delta = {.on_ground = current.on_ground,
                                .x = move.dx,
                                .y = move.dy,
                                .z = move.dz,
                                .steps = {}};
                packet.y_rot = angle(current.yaw);
                packet.x_rot = angle(current.pitch);
                body = encode_play_body(
                    refrain::protocol::current::play::clientbound::
                        move_entity_pos_rot::packet_id,
                    packet,
                    refrain::protocol::current::play::clientbound::
                        encode_move_entity_pos_rot);
                break;
            }
            case refrain::server::entity_move_kind::rotation: {
                refrain::protocol::current::play::clientbound::move_entity_rot
                    packet{};
                packet.entity_id = static_cast<std::uint32_t>(mover.entity_id);
                packet.on_ground = current.on_ground;
                packet.y_rot = angle(current.yaw);
                packet.x_rot = angle(current.pitch);
                body = encode_play_body(
                    refrain::protocol::current::play::clientbound::
                        move_entity_rot::packet_id,
                    packet,
                    refrain::protocol::current::play::clientbound::
                        encode_move_entity_rot);
                break;
            }
            case refrain::server::entity_move_kind::none:
                break;
            }
        }
        last[mover.connection] = current;
        if (body.empty()) {
            continue;
        }
        for (const auto &target : snapshot) {
            if (target.connection == mover.connection) {
                continue;
            }
            const auto found{visible.find(target.connection)};
            if (found == visible.end() ||
                !found->second.contains(mover.connection)) {
                continue;
            }
            append(target.connection, body);
        }
    }

    for (const auto &[target, buffer] : outbox) {
        if (!buffer.empty()) {
            listener.send_raw(target, buffer);
        }
    }
}

} // namespace

auto run_server(const server_run_options &options) -> int {
    if (options.world_path.empty()) {
        return report_error(refrain::make_error(
            refrain::error_code::invalid_argument,
            "refrain-server: --world is required to start the tick loop"));
    }

    const auto level_name{options.world.get_string("level_name", "world")};
    const auto view_distance{static_cast<std::int32_t>(
        options.network.get_int64("view_distance", 10))};
    const auto simulation_distance{static_cast<std::int32_t>(
        options.network.get_int64("simulation_distance", 10))};
    const auto max_players{options.network.get_int64("status.max_players", 20)};
    const auto online_mode{options.network.get_bool("online_mode", false)};
    refrain::log::info(
        "config: level={} view={} sim={} max_players={} online_mode={}",
        level_name, view_distance, simulation_distance, max_players,
        online_mode);
    const auto zstd_level{static_cast<int>(
        options.storage.get_int64("zstd_level", options.world_zstd_level))};
    const auto zstd_near_level{static_cast<int>(
        options.storage.get_int64("zstd_near_level", options.zstd_near_level))};
    const auto zstd_mid_level{static_cast<int>(
        options.storage.get_int64("zstd_mid_level", options.zstd_mid_level))};
    const auto zstd_far_level{static_cast<int>(
        options.storage.get_int64("zstd_far_level", options.zstd_far_level))};
    const auto zstd_near_radius{
        static_cast<std::int32_t>(options.storage.get_int64(
            "zstd_near_radius", options.zstd_near_radius))};
    const auto zstd_mid_radius{static_cast<std::int32_t>(
        options.storage.get_int64("zstd_mid_radius", options.zstd_mid_radius))};
    const auto autosave_interval{
        static_cast<std::uint64_t>(options.storage.get_int64(
            "autosave_interval_ticks",
            static_cast<std::int64_t>(options.autosave_interval_ticks)))};
    const auto compact_interval{
        static_cast<std::uint64_t>(options.storage.get_int64(
            "compact_interval_ticks",
            static_cast<std::int64_t>(options.compact_interval_ticks)))};
    const auto store_compact_interval{
        static_cast<std::uint64_t>(options.storage.get_int64(
            "store_compact_interval_ticks",
            static_cast<std::int64_t>(options.store_compact_interval_ticks)))};
    const auto compact_min_bytes{
        static_cast<std::uint64_t>(options.storage.get_int64(
            "compact_min_bytes",
            static_cast<std::int64_t>(options.compact_min_bytes)))};
    const auto compact_garbage_percent{
        static_cast<std::uint32_t>(options.storage.get_int64(
            "compact_garbage_percent", options.compact_garbage_percent))};
    const auto watchdog_timeout{
        static_cast<std::uint32_t>(options.runtime.get_int64(
            "watchdog_timeout_ms", options.watchdog_timeout_ms))};
    refrain::log::info("config: zstd={}, watchdog={}ms, autosave={}",
                       zstd_level, watchdog_timeout, autosave_interval);

    // Keep the maintenance worker's option view consistent with the resolved
    // configuration values.
    server_run_options effective{options};
    effective.world_zstd_level = zstd_level;
    effective.zstd_near_level = zstd_near_level;
    effective.zstd_mid_level = zstd_mid_level;
    effective.zstd_far_level = zstd_far_level;
    effective.zstd_near_radius = zstd_near_radius;
    effective.zstd_mid_radius = zstd_mid_radius;
    effective.watchdog_timeout_ms = watchdog_timeout;
    effective.autosave_interval_ticks = autosave_interval;
    effective.compact_interval_ticks = compact_interval;
    effective.store_compact_interval_ticks = store_compact_interval;
    effective.compact_min_bytes = compact_min_bytes;
    effective.compact_garbage_percent = compact_garbage_percent;

    auto store{refrain::sim::world::open(options.world_path)};
    if (!store) {
        return report_error(store.error());
    }
    if (auto configured{store->store().set_zstd_level(zstd_level)};
        !configured) {
        return report_error(configured.error());
    }
    if (auto configured{store->set_zstd_policy(
            refrain::sim::zstd_policy{.near_level = zstd_near_level,
                                      .mid_level = zstd_mid_level,
                                      .far_level = zstd_far_level,
                                      .near_radius = zstd_near_radius,
                                      .mid_radius = zstd_mid_radius})};
        !configured) {
        return report_error(configured.error());
    }
    auto level{store->store().level()};
    if (!level) {
        return report_error(level.error());
    }
    refrain::log::info("world: {} dataversion={} seed={}", options.world_path,
                       level->data_version, level->seed);
    store->seed(static_cast<std::uint64_t>(level->seed));
    store->set_player_position(static_cast<double>(level->spawn_x) + 0.5,
                               static_cast<double>(level->spawn_y),
                               static_cast<double>(level->spawn_z) + 0.5);

    // Ticket seeding: the spawn column plus its 3x3 neighbourhood. The
    // tick loop keeps them loaded; a missing chunk in a sparse imported world
    // is a warning, not a fatal startup error.
    const auto spawn_chunk{refrain::sim::chunk_pos{
        level->spawn_x >= 0 ? level->spawn_x / 16 : (level->spawn_x - 15) / 16,
        level->spawn_z >= 0 ? level->spawn_z / 16
                            : (level->spawn_z - 15) / 16}};
    for (std::int32_t dx{-1}; dx <= 1; ++dx) {
        for (std::int32_t dz{-1}; dz <= 1; ++dz) {
            const refrain::sim::chunk_pos pos{spawn_chunk.x + dx,
                                              spawn_chunk.z + dz};
            auto ticketed{
                store->add_ticket(pos, refrain::sim::ticket_kind::spawn)};
            if (!ticketed) {
                refrain::log::warn("spawn ticket: {},{}: {}", pos.x, pos.z,
                                   ticketed.error().message);
            }
        }
    }
    refrain::log::info("spawn: {} chunks {} tickets", store->loaded_chunks(),
                       store->ticket_count(spawn_chunk));

    std::optional<refrain::data::vanilla_tables> tables{};
    auto table_bytes{
        refrain::core::io::read_file("content/vanilla/26.3/tables.rvtb")};
    if (!table_bytes) {
        refrain::log::warn("network: vanilla tables unavailable: {}",
                           table_bytes.error().message);
    } else {
        auto opened{refrain::data::vanilla_tables::open(*table_bytes)};
        if (!opened) {
            refrain::log::warn("network: vanilla tables invalid: {}",
                               opened.error().message);
        } else {
            tables = std::move(*opened);
        }
    }
    std::optional<refrain::net::registry_catalog> registries{};
    auto loaded_registries{refrain::net::registry_catalog::load(
        "content/vanilla/26.3/protocol_registry.json")};
    if (!loaded_registries) {
        refrain::log::warn("network: registry catalog unavailable: {}",
                           loaded_registries.error().message);
    } else {
        registries = std::move(*loaded_registries);
    }
    std::vector<std::vector<std::byte>> spawn_chunks{};
    if (tables && registries) {
        refrain::server::chunk_packet_source source{
            .world = &*store,
            .tables = &*tables,
            .registries = &*registries,
        };
        auto built{refrain::server::build_spawn_chunks(source, spawn_chunk, 3)};
        if (!built) {
            refrain::log::error("network: spawn chunks failed: {}",
                                built.error().message);
        } else {
            spawn_chunks = std::move(*built);
            refrain::log::info("network: spawn_chunks={}", spawn_chunks.size());
        }
    }
    auto command_environment{
        std::make_shared<refrain::server::command_environment>()};
    command_environment->world = &store->store();
    const auto initial_mode{
        level->game_mode >= 0 && level->game_mode <= 3
            ? static_cast<refrain::net::game_mode>(level->game_mode)
            : refrain::net::game_mode::creative};
    command_environment->default_mode = initial_mode;
    refrain::session::play_session players{};
    refrain::session::chunk_tracker views{};
    std::unordered_map<std::uint64_t, refrain::net::movement_event>
        last_moves{};
    std::unordered_map<std::uint64_t, std::unordered_set<std::uint64_t>>
        visible_players{};
    const auto tracking_chunks{static_cast<std::int32_t>(std::max<std::int64_t>(
        1, options.network.get_int64("entity_tracking_range", 512) / 16))};
    refrain::net::server *active_listener{nullptr};
    const auto external_join{options.player_join};
    const auto external_leave{options.player_leave};
    refrain::net::server_inputs network_inputs{};
    network_inputs.commands =
        refrain::server::make_command_bridge(command_environment);
    network_inputs.policy.hardcore = level->hardcore;
    network_inputs.policy.difficulty =
        level->difficulty >= 0 && level->difficulty <= 3
            ? static_cast<std::uint32_t>(level->difficulty)
            : 2U;
    network_inputs.policy.difficulty_locked = level->difficulty_locked;
    network_inputs.join_state =
        [command_environment](const refrain::net::player_identity &identity) {
            return command_environment->join_state(identity.connection);
        };
    network_inputs.fallback_bind_host =
        options.network.get_string("bind_host", "0.0.0.0");
    network_inputs.fallback_port =
        static_cast<std::uint16_t>(options.network.get_int64("port", 25565));
    network_inputs.fallback_compression = static_cast<std::int32_t>(
        options.network.get_int64("compression_threshold", 256));
    network_inputs.fallback_view_distance = static_cast<std::int32_t>(
        options.network.get_int64("view_distance", 10));
    network_inputs.fallback_simulation_distance = static_cast<std::int32_t>(
        options.network.get_int64("simulation_distance", 10));
    network_inputs.fallback_max_players = static_cast<std::int32_t>(
        options.network.get_int64("status.max_players", 20));
    network_inputs.fallback_online_mode =
        options.network.get_bool("online_mode", true);
    network_inputs.fallback_secure_chat =
        options.network.get_bool("enforce_secure_profile", true);
    network_inputs.fallback_motd =
        options.network.get_string("status.motd", "A Refrain Server");
    network_inputs.make_transport = options.make_transport;
    network_inputs.protocol = options.protocol;
    network_inputs.status = options.status;
    network_inputs.tab = options.tab;
    network_inputs.encode_tab = options.encode_tab;
    const auto spawn_chunk_x{static_cast<std::int32_t>(level->spawn_x / 16)};
    const auto spawn_chunk_z{static_cast<std::int32_t>(level->spawn_z / 16)};
    network_inputs.player_join =
        [&players, &views, &active_listener, external_join, spawn_chunk_x,
         spawn_chunk_z](const refrain::net::player_identity &identity) {
            const auto added{players.add_player(
                identity.connection, identity.id, std::string{identity.name},
                std::string{identity.world}, 0.0, 64.0, 0.0, 1)};
            if (added) {
                static_cast<void>(views.update(
                    identity.connection, spawn_chunk_x, spawn_chunk_z, 10));
            }
            if (added && active_listener != nullptr) {
                announce_session_join(*active_listener, players,
                                      identity.connection);
            }
            if (external_join) {
                external_join(identity);
            }
        };
    network_inputs.player_leave =
        [&players, &views, &last_moves, &visible_players, &active_listener,
         external_leave](const refrain::net::player_identity &identity) {
            const auto leaving{players.player_of(identity.connection)};
            const auto copy{
                leaving == nullptr
                    ? std::nullopt
                    : std::optional<refrain::session::player>{*leaving}};
            static_cast<void>(players.remove_player(identity.connection));
            static_cast<void>(views.remove(identity.connection));
            last_moves.erase(identity.connection);
            visible_players.erase(identity.connection);
            if (copy && active_listener != nullptr) {
                for (auto &[viewer, seen] : visible_players) {
                    if (seen.erase(identity.connection) > 0) {
                        send_remove_entity(*active_listener, viewer,
                                           copy->entity_id);
                    }
                }
            }
            if (copy && active_listener != nullptr) {
                announce_session_leave(*active_listener, *copy, players);
            }
            if (external_leave) {
                external_leave(identity);
            }
        };
    network_inputs.movement =
        [&players, &views](const refrain::net::movement_event &event) {
            static_cast<void>(players.update_position(event.connection, event.x,
                                                      event.y, event.z,
                                                      event.yaw, event.pitch));
            const auto player{players.player_of(event.connection)};
            if (player != nullptr) {
                static_cast<void>(views.update(
                    event.connection,
                    static_cast<std::int32_t>(std::floor(player->x / 16.0)),
                    static_cast<std::int32_t>(std::floor(player->z / 16.0)),
                    player->view_distance));
            }
        };
    network_inputs.view_distance =
        [&players, &views](const refrain::net::view_distance_event &event) {
            static_cast<void>(
                players.set_view_distance(event.connection, event.distance));
            const auto player{players.player_of(event.connection)};
            if (player != nullptr) {
                static_cast<void>(views.update(
                    event.connection,
                    static_cast<std::int32_t>(std::floor(player->x / 16.0)),
                    static_cast<std::int32_t>(std::floor(player->z / 16.0)),
                    event.distance));
            }
        };
    network_inputs.attack =
        [&players, &active_listener](const refrain::net::attack_event &event) {
            if (active_listener != nullptr) {
                handle_session_attack(*active_listener, players, event);
            }
        };
    network_inputs.animation = [&players,
                                &active_listener](std::uint64_t connection,
                                                  std::int8_t action) {
        if (active_listener != nullptr) {
            broadcast_session_animation(*active_listener, players, connection,
                                        action);
        }
    };
    network_inputs.play_ready =
        [&players,
         &active_listener](const refrain::net::player_identity &identity) {
            if (active_listener == nullptr) {
                return;
            }
            const auto value{players.player_of(identity.connection)};
            send_session_health(*active_listener, identity.connection,
                                value == nullptr ? 20.0F : value->health);
            send_session_experience(*active_listener, identity.connection);
        };
    network_inputs.respawn = [&players,
                              &active_listener](std::uint64_t connection) {
        static_cast<void>(players.set_health(connection, 20.0F));
        if (active_listener != nullptr) {
            send_session_health(*active_listener, connection, 20.0F);
        }
    };
    network_inputs.verify_session = options.verify_session;
    network_inputs.seed = level->seed;
    network_inputs.spawn_x = static_cast<double>(level->spawn_x) + 0.5;
    network_inputs.spawn_y = static_cast<double>(level->spawn_y);
    network_inputs.spawn_z = static_cast<double>(level->spawn_z) + 0.5;
    network_inputs.registry_catalog_path =
        "content/vanilla/26.3/protocol_registry.json";
    network_inputs.spawn_chunks =
        std::make_shared<const std::vector<std::vector<std::byte>>>(
            std::move(spawn_chunks));
    // TODO: breaking ignores block hardness, the held tool, drops and
    // neighbour updates; every block becomes air immediately.
    network_inputs.break_block =
        [world = &*store](std::int32_t x, std::int32_t y, std::int32_t z) {
            if (y < std::numeric_limits<std::int16_t>::min() ||
                y > std::numeric_limits<std::int16_t>::max()) {
                return false;
            }
            return world
                ->set_block(
                    refrain::sim::block_pos{x, static_cast<std::int16_t>(y), z},
                    refrain::sim::air_block)
                .has_value();
        };
    // TODO: placing ignores the held item, placement rules and orientation;
    // every placement writes minecraft:stone.
    network_inputs.place_block =
        [world = &*store](std::int32_t x, std::int32_t y, std::int32_t z) {
            if (y < std::numeric_limits<std::int16_t>::min() ||
                y > std::numeric_limits<std::int16_t>::max()) {
                return false;
            }
            const auto stone{
                world->registry().intern(std::string{"minecraft:stone"})};
            return world
                ->set_block(
                    refrain::sim::block_pos{x, static_cast<std::int16_t>(y), z},
                    stone)
                .has_value();
        };
    auto network{refrain::net::server::start(std::move(network_inputs),
                                             options.network)};
    std::unique_ptr<refrain::net::server> listener{};
    if (!network) {
        refrain::log::error("network: listener failed: {}",
                            network.error().message);
    } else {
        listener = std::move(*network);
        active_listener = listener.get();
        command_environment->listener = listener.get();
        refrain::log::info("network: listening on {}:{}",
                           options.network.get_string("bind_host", "0.0.0.0"),
                           listener->local_port());
    }

    // Chunk streaming: the network layer reports viewport changes, the
    // simulation thread encodes the missing chunks in bounded batches.
    struct chunk_stream_state {
        std::int32_t center_x{0};
        std::int32_t center_z{0};
        std::int32_t radius{0};
        bool center_dirty{false};
        bool seeded{false};
        bool reported{false};
        std::size_t sent{0};
        std::set<std::pair<std::int32_t, std::int32_t>> known{};
        std::deque<std::pair<std::int32_t, std::int32_t>> pending{};
    };
    std::unordered_map<std::uint64_t, chunk_stream_state> chunk_streams{};
    // Encoded chunks are shared by every connection and survive a tick, so a
    // second player with the same viewport does not encode the same chunk
    // again. The bound keeps the resident bytes proportional to the cache.
    std::map<std::pair<std::int32_t, std::int32_t>, std::vector<std::byte>>
        encoded_chunks{};
    constexpr std::size_t max_encoded_chunks{256};
    refrain::server::chunk_packet_source stream_source{};
    bool stream_chunks{false};
    if (tables && registries) {
        stream_source = refrain::server::chunk_packet_source{
            .world = &*store,
            .tables = &*tables,
            .registries = &*registries,
        };
        stream_chunks = true;
    }
    const auto process_chunk_streams = [&]() {
        if (!listener || !stream_chunks) {
            return;
        }
        const auto active_ids{listener->active_connections()};
        const std::set<std::uint64_t> active{active_ids.begin(),
                                             active_ids.end()};
        for (auto iterator = chunk_streams.begin();
             iterator != chunk_streams.end();) {
            if (!active.contains(iterator->first)) {
                iterator = chunk_streams.erase(iterator);
            } else {
                ++iterator;
            }
        }
        for (const auto &request : views.take_requests()) {
            auto &stream{chunk_streams[request.connection]};
            const auto center_changed{stream.radius != request.radius ||
                                      stream.center_x != request.chunk_x ||
                                      stream.center_z != request.chunk_z};
            stream.center_x = request.chunk_x;
            stream.center_z = request.chunk_z;
            stream.radius = request.radius;
            if (request.seed_radius > 0 && !stream.seeded) {
                for (std::int32_t dz{-request.seed_radius};
                     dz <= request.seed_radius; ++dz) {
                    for (std::int32_t dx{-request.seed_radius};
                         dx <= request.seed_radius; ++dx) {
                        stream.known.emplace(stream.center_x + dx,
                                             stream.center_z + dz);
                    }
                }
                stream.seeded = true;
            }
            if (center_changed) {
                // Chunks that leave the viewport are dropped from the client
                // cache, so a walking player keeps a bounded chunk set.
                std::vector<refrain::server::chunk_coordinate> tracked{};
                tracked.reserve(stream.known.size());
                for (const auto &[x, z] : stream.known) {
                    tracked.push_back({x, z});
                }
                for (const auto &position :
                     refrain::server::chunks_outside_view(
                         tracked, {stream.center_x, stream.center_z},
                         stream.radius)) {
                    listener->send_forget_chunk(request.connection, position.x,
                                                position.z);
                    stream.known.erase({position.x, position.z});
                }
            }
            if (center_changed || stream.pending.empty()) {
                stream.pending.clear();
                for (std::int32_t distance{0}; distance <= stream.radius;
                     ++distance) {
                    for (std::int32_t dz{-distance}; dz <= distance; ++dz) {
                        for (std::int32_t dx{-distance}; dx <= distance; ++dx) {
                            if (std::max(std::abs(dx), std::abs(dz)) !=
                                distance) {
                                continue;
                            }
                            const auto x{stream.center_x + dx};
                            const auto z{stream.center_z + dz};
                            if (!stream.known.contains({x, z})) {
                                stream.pending.emplace_back(x, z);
                            }
                        }
                    }
                }
            }
            stream.center_dirty = stream.center_dirty || center_changed;
            stream.reported = false;
        }
        // Bound the bytes queued for clients that cannot drain them. A slow
        // client keeps its pending list and resumes once the socket catches
        // up; the whole stream pauses when the aggregate backlog is large.
        constexpr std::size_t max_connection_backlog{1U * 1024U * 1024U};
        constexpr std::size_t max_total_backlog{64U * 1024U * 1024U};
        std::size_t total_backlog{0};
        for (const auto id : active_ids) {
            total_backlog += listener->backlog(id);
        }
        if (total_backlog > max_total_backlog) {
            return;
        }
        std::size_t encode_budget{1};
        for (const auto id : active_ids) {
            auto match{chunk_streams.find(id)};
            if (match == chunk_streams.end()) {
                continue;
            }
            if (listener->backlog(id) > max_connection_backlog) {
                continue;
            }
            auto &stream{match->second};
            std::vector<std::vector<std::byte>> packets;
            constexpr std::size_t batch_budget{8};
            packets.reserve(batch_budget);
            while (!stream.pending.empty() && packets.size() < batch_budget) {
                const auto [x, z]{stream.pending.front()};
                if (stream.known.contains({x, z})) {
                    stream.pending.pop_front();
                    continue;
                }
                const auto cache_key{std::pair{x, z}};
                auto cached{encoded_chunks.find(cache_key)};
                if (cached == encoded_chunks.end()) {
                    if (encode_budget == 0) {
                        // Leave the coordinate pending: the next tick picks it
                        // up again instead of dropping it from the viewport.
                        break;
                    }
                    --encode_budget;
                    auto encoded{refrain::server::encode_full_chunk(
                        stream_source, refrain::sim::chunk_pos{x, z})};
                    if (!encoded) {
                        // The world may not have this chunk generated;
                        // remember it so the same coordinate is not retried
                        // every tick.
                        refrain::log::debug(
                            "network: chunk ({}, {}) encode failed: {}", x, z,
                            encoded.error().message);
                        stream.known.emplace(x, z);
                        stream.pending.pop_front();
                        continue;
                    }
                    cached =
                        encoded_chunks.emplace(cache_key, std::move(*encoded))
                            .first;
                    if (encoded_chunks.size() > max_encoded_chunks) {
                        encoded_chunks.erase(encoded_chunks.begin());
                    }
                }
                stream.pending.pop_front();
                packets.push_back(cached->second);
                stream.known.emplace(x, z);
                ++stream.sent;
            }
            const auto update_center{stream.center_dirty};
            if (update_center || !packets.empty()) {
                listener->send_chunk_batch(id, stream.center_x, stream.center_z,
                                           packets, update_center);
                stream.center_dirty = false;
            }
            if (stream.pending.empty() && !stream.reported) {
                refrain::log::info(
                    "network: chunk view ready for connection {} ({} "
                    "chunk(s) streamed)",
                    id, stream.sent);
                stream.reported = true;
            }
        }
    };

    // Player inventory: map runtime item keys to protocol item ids once, then
    // mirror the simulation inventory into the player inventory container.
    std::unordered_map<std::string, std::uint32_t> item_ids{};
    std::unordered_map<std::uint32_t, std::string> item_names{};
    if (tables) {
        if (const auto registry{tables->find_registry("minecraft:item")}) {
            const auto count{tables->registry_entry_count(*registry)};
            item_ids.reserve(count);
            for (std::uint32_t index{0}; index < count; ++index) {
                const auto entry{tables->registry_entry(*registry, index)};
                if (entry.protocol_id >= 0) {
                    const auto id{
                        static_cast<std::uint32_t>(entry.protocol_id)};
                    item_ids.emplace(std::string{entry.name}, id);
                    item_names.emplace(id, std::string{entry.name});
                }
            }
        }
    }
    if (auto stone{item_ids.find("minecraft:stone")}; stone != item_ids.end()) {
        refrain::log::info("inventory: minecraft:stone -> {}", stone->second);
    }
    std::set<std::uint64_t> inventory_forced{};
    std::unordered_map<std::uint64_t, refrain::sim::item_stack> carried_items{};
    std::unordered_map<std::uint64_t, std::vector<refrain::sim::item_stack>>
        inventory_snapshots{};
    std::unordered_map<std::uint64_t, refrain::net::item_stack>
        carried_snapshots{};
    // The client echoes the last menu state id it received in every click.
    // Ids wrap at 15 bits like the client counter; any other echoed value
    // means the client missed an update and gets the full menu state.
    std::unordered_map<std::uint64_t, std::int32_t> inventory_states{};
    // Runtime stack to protocol stack; unknown items and empty stacks encode
    // as an empty stack.
    const auto protocol_stack = [&](const refrain::sim::item_stack &stack) {
        if (stack.count == 0 || stack.item.empty()) {
            return refrain::net::item_stack{};
        }
        const auto item{item_ids.find(stack.item)};
        if (item == item_ids.end()) {
            return refrain::net::item_stack{};
        }
        return refrain::net::item_stack{
            .count = stack.count,
            .item_id = item->second,
        };
    };
    const auto next_state = [&](std::uint64_t id) {
        auto &state{inventory_states[id]};
        state = (state + 1) & 32767;
        return state;
    };
    const auto process_container_clicks = [&]() {
        if (!listener) {
            return;
        }
        for (const auto &slot : listener->take_creative_slots()) {
            auto *inventory = players.inventory_of(slot.connection);
            const auto index{static_cast<std::size_t>(slot.slot)};
            if (inventory == nullptr || slot.slot < 0 ||
                index >= inventory->size()) {
                continue;
            }
            auto &target{(*inventory)[index]};
            if (slot.item_id == 0 || slot.count == 0) {
                target = sim::item_stack{};
            } else {
                const auto name{item_names.find(slot.item_id)};
                if (name == item_names.end()) {
                    continue;
                }
                target = sim::item_stack{
                    .item = name->second,
                    .count = slot.count,
                };
            }
            inventory_forced.insert(slot.connection);
        }
        for (const auto &close : listener->take_container_closes()) {
            if (close.container_id != 0) {
                continue;
            }
            if (auto *inventory = players.inventory_of(close.connection)) {
                const auto carried{carried_items.find(close.connection)};
                if (carried != carried_items.end()) {
                    const auto remainder{refrain::server::place_back(
                        *inventory, carried->second)};
                    if (remainder.count == 0) {
                        carried_items.erase(close.connection);
                    } else {
                        carried->second = remainder;
                    }
                }
            }
            inventory_forced.insert(close.connection);
        }
        for (const auto &click : listener->take_container_clicks()) {
            if (click.container_id != 0) {
                continue;
            }
            auto *inventory = players.inventory_of(click.connection);
            if (inventory == nullptr) {
                continue;
            }
            const auto known{inventory_states.find(click.connection)};
            const auto stale{known == inventory_states.end() ||
                             known->second != click.state_id};
            if (click.mode != 0) {
                // Shift, drag and number-key clicks are not modeled; resend
                // the authoritative state so the client prediction reverts.
                inventory_forced.insert(click.connection);
                continue;
            }
            auto &carried{carried_items[click.connection]};
            const auto changed{refrain::server::apply_player_click(
                *inventory, carried, click.slot, click.button)};
            refrain::log::debug(
                "inventory: click conn={} slot={} button={} mode={} "
                "state={} stale={} -> carried='{}' x{} changed={}",
                click.connection, click.slot, click.button, click.mode,
                click.state_id, stale, carried.item, carried.count, changed);
            if (stale) {
                inventory_forced.insert(click.connection);
            }
        }
    };

    const auto process_inventory = [&]() {
        if (!listener) {
            return;
        }
        const auto active_ids{listener->active_connections()};
        const std::set<std::uint64_t> active{active_ids.begin(),
                                             active_ids.end()};
        for (auto iterator = inventory_snapshots.begin();
             iterator != inventory_snapshots.end();) {
            if (!active.contains(iterator->first)) {
                iterator = inventory_snapshots.erase(iterator);
            } else {
                ++iterator;
            }
        }
        for (auto iterator = carried_snapshots.begin();
             iterator != carried_snapshots.end();) {
            if (!active.contains(iterator->first)) {
                iterator = carried_snapshots.erase(iterator);
            } else {
                ++iterator;
            }
        }
        for (auto iterator = inventory_states.begin();
             iterator != inventory_states.end();) {
            if (!active.contains(iterator->first)) {
                iterator = inventory_states.erase(iterator);
            } else {
                ++iterator;
            }
        }
        for (const auto id : active_ids) {
            const auto *record{players.player_of(id)};
            if (record == nullptr) {
                continue;
            }
            const auto &inventory{record->inventory};
            const auto previous{inventory_snapshots.find(id)};
            const auto forced{inventory_forced.contains(id)};
            if (previous == inventory_snapshots.end() || forced) {
                std::vector<refrain::net::item_stack> slots(46);
                for (std::size_t index{0}; index < inventory.size(); ++index) {
                    const auto slot{refrain::server::menu_slot_of(index)};
                    if (slot < 0) {
                        continue;
                    }
                    slots[static_cast<std::size_t>(slot)] =
                        protocol_stack(inventory[index]);
                }
                auto carried{refrain::net::item_stack{}};
                if (const auto stack{carried_items.find(id)};
                    stack != carried_items.end()) {
                    carried = protocol_stack(stack->second);
                }
                listener->send_container_content(id, 0, next_state(id), slots,
                                                 carried);
                carried_snapshots[id] = carried;
            } else {
                for (const auto slot : refrain::server::changed_menu_slots(
                         previous->second, inventory)) {
                    const auto index{refrain::server::inventory_index_of(slot)};
                    const auto stack{index < inventory.size()
                                         ? protocol_stack(inventory[index])
                                         : refrain::net::item_stack{}};
                    listener->send_container_set_slot(id, 0, next_state(id),
                                                      slot, stack);
                }
                auto carried{refrain::net::item_stack{}};
                if (const auto stack{carried_items.find(id)};
                    stack != carried_items.end()) {
                    carried = protocol_stack(stack->second);
                }
                const auto sent{carried_snapshots.find(id)};
                if (sent == carried_snapshots.end() ||
                    sent->second != carried) {
                    listener->send_cursor_item(id, carried);
                    carried_snapshots[id] = carried;
                }
            }
            inventory_snapshots[id] = inventory;
        }
        inventory_forced.clear();
        for (auto iterator = carried_items.begin();
             iterator != carried_items.end();) {
            if (!active.contains(iterator->first)) {
                iterator = carried_items.erase(iterator);
            } else {
                ++iterator;
            }
        }
    };

    snapshot_writer writer{*store};
    // One background deleter, only for an applying GC policy. `--content-gc
    // off|dry-run` must never delete: no worker is started and a queue left by
    // an earlier apply run is left untouched (clearing it requires an explicit
    // maintenance action, not an implicit startup sweep).
    std::optional<gc_delete_worker> gc_worker{};
    if (!options.content_store.empty() && options.content_gc_apply) {
        gc_worker.emplace(options.content_store, options.content_gc_budget_ms);
    }
    const auto state{std::make_shared<runtime_state>()};
    state->config = options.config;
    state->heartbeat_ns.store(refrain::core::now_ns(),
                              std::memory_order_relaxed);

    // Periodic maintenance is scheduler-driven: the host owns the
    // tasks, the tick loop only calls `run()` at the two tick boundaries.
    // Registration order is fixed: autosave, content-gc, store-compact,
    // region-compact.
    refrain::core::schedule::scheduler scheduler{};
    maintenance_context maintenance{.world = &*store,
                                    .writer = &writer,
                                    .options = &effective,
                                    .gc_worker = &gc_worker};
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
            refrain::log::warn("schedule: {} interval {}->{}", method, interval,
                               clamped);
        }
        tasks[task_count++] = refrain::core::schedule::task_descriptor{
            .service = "runtime",
            .method = method,
            .interval_ticks = static_cast<std::int64_t>(clamped),
            .phase_of = refrain::core::schedule::phase::post_tick,
            .invoke = invoke,
        };
    };
    if (autosave_interval > 0) {
        add_task("autosave", autosave_interval, &maintenance_autosave);
    }
    if (options.content_gc_interval_ticks > 0 &&
        !options.content_store.empty()) {
        add_task("content-gc", options.content_gc_interval_ticks,
                 &maintenance_content_gc);
    }
    if (store_compact_interval > 0) {
        add_task("store-compact", store_compact_interval,
                 &maintenance_store_compact);
    }
    if (compact_interval > 0) {
        add_task("region-compact", compact_interval,
                 &maintenance_region_compact);
    }
    if (task_count > 0) {
        if (auto added{scheduler.add(std::span{tasks.data(), task_count},
                                     &maintenance, nullptr)};
            !added) {
            return report_error(added.error());
        }
    }

    if (!options.command_file.empty() && !options.script_file.empty()) {
        return report_error(refrain::make_error(
            refrain::error_code::invalid_argument,
            "refrain-server: --cmd-file and --script are mutually exclusive"));
    }
    std::vector<refrain::sim::replay_step> replay{};
    std::size_t replay_index{0};
    if (!options.script_file.empty()) {
        auto text{refrain::core::io::read_text(
            options.script_file,
            {.max_bytes = max_console_file_bytes, .allow_symlink = false})};
        if (!text) {
            return report_error(text.error());
        }
        auto parsed{refrain::sim::parse_replay_script(*text)};
        if (!parsed) {
            return report_error(parsed.error());
        }
        replay = std::move(*parsed);
    }
    if (!options.command_file.empty()) {
        auto text{refrain::core::io::read_text(
            options.command_file,
            {.max_bytes = max_console_file_bytes, .allow_symlink = false})};
        if (!text) {
            return report_error(text.error());
        }
        push_text_commands(state, *text);
    } else if (options.script_file.empty()) {
        start_console_thread(state);
    }

    std::thread watchdog{};
    if (watchdog_timeout > 0) {
        watchdog = std::thread{[state, timeout = watchdog_timeout] {
            refrain::log::set_thread_name("watchdog");
            const auto timeout_ns{static_cast<std::int64_t>(timeout) *
                                  1'000'000LL};
            bool warned{false};
            while (!state->stop.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds{250});
                const auto heartbeat{
                    state->heartbeat_ns.load(std::memory_order_relaxed)};
                const auto stalled{heartbeat != 0 &&
                                   refrain::core::now_ns() - heartbeat >
                                       timeout_ns};
                if (stalled && !warned) {
                    refrain::log::warn("watchdog: stalled {}ms", timeout);
                    warned = true;
                } else if (!stalled && warned) {
                    refrain::log::info("watchdog: recovered");
                    warned = false;
                }
            }
        }};
    }

    refrain::core::tick_clock clock{};
    clock.start();
    std::int64_t ticks_run{0};
    refrain::log::info("tick loop: {} tps", refrain::core::ticks_per_second);

    while (!state->stop.load(std::memory_order_acquire)) {
        auto tick{clock.wait_next()};
        if (!tick) {
            refrain::log::error("tick loop: {}", tick.error().message);
            break;
        }
        const auto tick_start_ns{refrain::core::now_ns()};
        state->heartbeat_ns.store(refrain::core::now_ns(),
                                  std::memory_order_relaxed);
        for (const auto &batch : writer.drain_completed()) {
            store->mark_clean(batch);
        }
        for (const auto &line : drain_commands(state)) {
            execute_console_command(line, state, *store, writer, *tick,
                                    &scheduler, listener.get());
        }
        while (replay_index < replay.size() &&
               replay[replay_index].tick == ticks_run) {
            execute_console_command(replay[replay_index].command, state, *store,
                                    writer, *tick, &scheduler, listener.get());
            if (state->stop.load(std::memory_order_acquire)) {
                break;
            }
            ++replay_index;
        }
        scheduler.run(refrain::core::schedule::phase::pre_tick, ticks_run);
        if (listener) {
            listener->tick();
            sync_player_visibility(*listener, players, last_moves,
                                   visible_players, tracking_chunks);
        }
        process_chunk_streams();
        process_container_clicks();
        process_inventory();
        store->tick();
        ++ticks_run;
        scheduler.run(refrain::core::schedule::phase::post_tick, ticks_run);
        record_tick_work(*state, static_cast<std::uint64_t>(
                                     refrain::core::now_ns() - tick_start_ns));
        if (options.on_tick) {
            options.on_tick(static_cast<std::uint64_t>(ticks_run));
        }
        if (options.hash_every > 0 &&
            (ticks_run % static_cast<std::int64_t>(options.hash_every)) == 0) {
            refrain::log::info("replay: tick={} hash={}", ticks_run,
                               refrain::data::to_hex(store->state_hash()));
        }
        if (options.ticks > 0 && ticks_run >= options.ticks) {
            refrain::log::info("tick limit: {}", ticks_run);
            state->stop.store(true, std::memory_order_release);
            break;
        }
        if ((ticks_run % 20) == 0) {
            refrain::log::debug(
                "tick {}: {}ms missed={}", tick->index,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    tick->actual)
                    .count(),
                tick->missed);
        }
    }

    state->stop.store(true, std::memory_order_release);
    if (watchdog.joinable()) {
        watchdog.join();
    }
    // Stop accepting connections before the world teardown so no handler can
    // observe a half-destroyed store.
    if (listener) {
        listener->stop();
        listener.reset();
    }
    // Tasks borrow the store/writer/GC worker; drop them before teardown.
    scheduler.clear();
    writer.stop();
    for (const auto &batch : writer.drain_completed()) {
        store->mark_clean(batch);
    }
    // Shutdown order: the world writer first, then the content queue worker
    // (so no unlink overlaps store teardown), and only then the final flush.
    if (gc_worker) {
        gc_worker->stop();
    }
    refrain::log::info("replay: final tick={} hash={}", store->ticks(),
                       refrain::data::to_hex(store->state_hash()));
    auto committed{store->flush()};
    if (!committed) {
        refrain::log::error("shutdown: commit failed: {}",
                            committed.error().message);
        return 1;
    }
    if (options.config != nullptr) {
        bool dirty{false};
        for (const auto &name : options.config->names()) {
            dirty = dirty || options.config->dirty(name);
        }
        if (dirty) {
            if (auto saved{options.config->save_all()}; !saved) {
                refrain::log::error("config: shutdown save failed: {}",
                                    saved.error().message);
            } else {
                refrain::log::info("config: saved");
            }
        }
    }
    refrain::log::info("server stopped: {} ticks", ticks_run);
    return 0;
}

} // namespace refrain::server
