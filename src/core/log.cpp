// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/log.hpp>

#include <refrain/platform/thread.hpp>

#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/details/fmt_helper.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace refrain::log {
namespace {

// Vanilla-shaped console line (`[13:10:52] [worker-0/info]: message`). The
// level is lowercase because spdlog's `%l` is lowercase; `%N` is the project
// thread-name flag (falls back to `t<tid>` for unnamed threads).
constexpr auto console_pattern = "[%H:%M:%S] [%N/%l]: %v";
// Files keep the same fields but add an ISO-8601 date and UTC offset so logs
// from different days/hosts remain sortable and unambiguous.
constexpr auto file_pattern = "[%Y-%m-%dT%H:%M:%S.%e%z] [%N/%l]: %v";
constexpr std::size_t async_queue_size = 8192;
constexpr std::size_t file_size_limit = 16U * 1024U * 1024U;
constexpr std::size_t file_count = 4;

std::mutex g_mutex{}; // init()/shutdown()/set_level() only (cold path)
/// The logger pointer is hot-path state. `write()` loads it atomically and
/// logs *outside* any user mutex (spdlog loggers/sinks are internally
/// thread-safe); the global mutex is only needed to serialize init/shutdown.
/// An atomic shared_ptr keeps the logger (and therefore its sinks) alive for
/// the duration of a concurrent write even when shutdown() swaps it out.
std::atomic<std::shared_ptr<spdlog::logger>> g_logger{};
std::shared_ptr<spdlog::sinks::sink> g_console_sink{};
std::atomic<bool> g_initialized{false};
std::atomic<level> g_console_level{level::info};
std::atomic<level> g_file_level{level::off};
std::atomic<bool> g_has_console_sink{false};
std::atomic<bool> g_has_file_sink{false};

/// TID -> readable thread name. Registration is cold path (thread start);
/// formatting reads under a shared lock. Keyed by the native id spdlog prints
/// for `%t`, so async messages resolve the producer, not the backend thread.
std::shared_mutex g_thread_names_mutex{};
std::unordered_map<std::uint64_t, std::string> g_thread_names{};

/// `%N` formatter: readable name, or `t<tid>` when the thread was not named.
class thread_name_formatter final : public spdlog::custom_flag_formatter {
  public:
    void format(const spdlog::details::log_msg &msg, const std::tm &,
                spdlog::memory_buf_t &dest) override {
        std::string_view name{};
        {
            const std::shared_lock lock{g_thread_names_mutex};
            const auto found{
                g_thread_names.find(static_cast<std::uint64_t>(msg.thread_id))};
            if (found != g_thread_names.end()) {
                name = found->second;
            }
        }
        if (name.empty()) {
            dest.push_back('t');
            spdlog::details::fmt_helper::append_int(msg.thread_id, dest);
            return;
        }
        spdlog::details::fmt_helper::append_string_view(
            spdlog::string_view_t{name.data(), name.size()}, dest);
    }

    [[nodiscard]] auto clone() const
        -> std::unique_ptr<spdlog::custom_flag_formatter> override {
        return std::make_unique<thread_name_formatter>();
    }
};

/// Install a pattern with `%N` registered. `pattern_formatter` refuses unknown
/// flags at construction, so the custom flag must be added before
/// `set_pattern()` compiles the string.
void apply_pattern(const spdlog::sink_ptr &sink, std::string_view pattern) {
    auto formatter{std::make_unique<spdlog::pattern_formatter>()};
    formatter->add_flag<thread_name_formatter>('N');
    formatter->set_pattern(std::string{pattern});
    sink->set_formatter(std::move(formatter));
}

[[nodiscard]] auto to_spdlog(level lvl) noexcept -> spdlog::level::level_enum {
    switch (lvl) {
    case level::trace:
        return spdlog::level::trace;
    case level::debug:
        return spdlog::level::debug;
    case level::info:
        return spdlog::level::info;
    case level::warn:
        return spdlog::level::warn;
    case level::error:
        return spdlog::level::err;
    case level::critical:
        return spdlog::level::critical;
    case level::off:
        return spdlog::level::off;
    }
    return spdlog::level::info;
}

/// Requires `g_mutex`. Re-applies `lvl` as the console threshold (no-op as far
/// as sinks go when the console sink is disabled).
void apply_console_level_locked(level lvl) noexcept {
    g_console_level.store(lvl, std::memory_order_relaxed);
    if (g_console_sink != nullptr) {
        try {
            g_console_sink->set_level(to_spdlog(lvl));
        } catch (...) {
            // Level changes must never propagate.
        }
    }
}

/// Roll back every sink created by a partially failed init() so a later valid
/// call starts clean. Requires g_mutex to be held.
void rollback_failed_init_locked() noexcept {
    try {
        g_logger.store(std::shared_ptr<spdlog::logger>{},
                       std::memory_order_release);
        g_console_sink.reset();
        g_has_console_sink.store(false, std::memory_order_relaxed);
        g_has_file_sink.store(false, std::memory_order_relaxed);
        g_file_level.store(level::off, std::memory_order_relaxed);
        g_initialized.store(false, std::memory_order_release);
        spdlog::shutdown();
    } catch (...) {
        // best effort: init() must never throw
    }
}

/// Same as above for the exception handlers, which run after the scope holding
/// g_mutex has unwound.
void rollback_failed_init() noexcept {
    try {
        const std::lock_guard lock{g_mutex};
        rollback_failed_init_locked();
    } catch (...) {
    }
}

} // namespace

auto to_string(level lvl) noexcept -> std::string_view {
    switch (lvl) {
    case level::trace:
        return "trace";
    case level::debug:
        return "debug";
    case level::info:
        return "info";
    case level::warn:
        return "warn";
    case level::error:
        return "error";
    case level::critical:
        return "critical";
    case level::off:
        return "off";
    }
    return "info";
}

auto level_from_string(std::string_view name) noexcept -> level {
    if (name == "trace") {
        return level::trace;
    }
    if (name == "debug") {
        return level::debug;
    }
    if (name == "info") {
        return level::info;
    }
    if (name == "warn" || name == "warning") {
        return level::warn;
    }
    if (name == "error" || name == "err") {
        return level::error;
    }
    if (name == "critical" || name == "fatal") {
        return level::critical;
    }
    if (name == "off" || name == "none") {
        return level::off;
    }
    return level::info;
}

auto init(const config &cfg) noexcept -> status {
    try {
        const std::lock_guard lock{g_mutex};
        if (g_initialized.load(std::memory_order_acquire)) {
            // Documented contract: only the first successful init creates
            // sinks; later calls are no-ops except for the console level.
            apply_console_level_locked(cfg.console_level);
            return status_ok();
        }

        g_console_level.store(cfg.console_level, std::memory_order_relaxed);
        g_file_level.store(cfg.file_level, std::memory_order_relaxed);
        g_has_console_sink.store(false, std::memory_order_relaxed);
        g_has_file_sink.store(false, std::memory_order_relaxed);

        std::vector<spdlog::sink_ptr> sinks{};

        if (cfg.console_level != level::off) {
            // Console diagnostics go to **stderr**: stdout belongs to the
            // command's data output (`dump`/JSON/`where` lines are grepped by
            // CI and parsed by scripts), so a log line on stdout corrupts the
            // machine-readable stream. `stdout_color_sinks.h`/`stdout_sinks.h`
            // are the spdlog headers that also declare the `stderr_*` aliases.
            if (cfg.enable_colors) {
                auto sink =
                    std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
                apply_pattern(sink, console_pattern);
                sink->set_level(to_spdlog(cfg.console_level));
                sinks.push_back(sink);
                g_console_sink = sink;
            } else {
                auto sink = std::make_shared<spdlog::sinks::stderr_sink_mt>();
                apply_pattern(sink, console_pattern);
                sink->set_level(to_spdlog(cfg.console_level));
                sinks.push_back(sink);
                g_console_sink = sink;
            }
            g_has_console_sink.store(true, std::memory_order_relaxed);
        }

        if (!cfg.file_path.empty()) {
            const std::filesystem::path path{cfg.file_path};
            if (path.has_parent_path()) {
                std::error_code ec{};
                std::filesystem::create_directories(path.parent_path(), ec);
                if (ec) {
                    rollback_failed_init_locked();
                    return std::unexpected(make_error(
                        error_code::io_error,
                        "cannot create log directory: " + ec.message()));
                }
            }
            auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                cfg.file_path, file_size_limit, file_count);
            apply_pattern(sink, file_pattern);
            sink->set_level(to_spdlog(cfg.file_level));
            sinks.push_back(sink);
            g_file_level.store(cfg.file_level, std::memory_order_relaxed);
            g_has_file_sink.store(true, std::memory_order_relaxed);
        }

        if (sinks.empty()) {
            g_initialized.store(true, std::memory_order_release);
            return status_ok();
        }

        std::shared_ptr<spdlog::logger> logger{};
        if (cfg.async) {
            spdlog::init_thread_pool(async_queue_size, 1);
            logger = std::make_shared<spdlog::async_logger>(
                "refrain", sinks.begin(), sinks.end(), spdlog::thread_pool(),
                spdlog::async_overflow_policy::block);
            // spdlog::flush_every() installs a periodic `flush_all()`, which
            // only walks the loggers held by the spdlog registry. A logger
            // built directly with make_shared is not registered automatically,
            // so without this line the periodic flush - and therefore
            // `flush_interval_ms` - was a no-op for the `refrain` logger
            // (messages only reached the file on shutdown()/`warn`). A
            // duplicate name raises spdlog_ex, which the catch handlers below
            // roll back like any other init failure.
            spdlog::register_logger(logger);
            if (cfg.flush_interval_ms > 0) {
                // `flush_interval_ms` is a live option for async logging,
                // not dead configuration.
                spdlog::flush_every(
                    std::chrono::milliseconds{cfg.flush_interval_ms});
            }
        } else {
            logger = std::make_shared<spdlog::logger>("refrain", sinks.begin(),
                                                      sinks.end());
        }
        logger->set_level(spdlog::level::trace);
        logger->flush_on(spdlog::level::warn);

        // Publish the logger before flipping `g_initialized`: a concurrent
        // write() that observes initialized == true must also see the logger.
        g_logger.store(std::move(logger), std::memory_order_release);
        g_initialized.store(true, std::memory_order_release);
        return status_ok();
    } catch (const std::bad_alloc &) {
        rollback_failed_init();
        // SSO-sized literal: the OOM report must not allocate again (a second
        // `bad_alloc` escaping this `noexcept` function would terminate).
        return std::unexpected{
            make_error(error_code::out_of_memory, "out of memory")};
    } catch (const std::exception &ex) {
        rollback_failed_init();
        // The message is diagnostics; building it allocates, so a second
        // failure falls back to the SSO literal instead of terminating inside
        // this `noexcept` function.
        try {
            return std::unexpected{
                make_error(error_code::unknown,
                           fmt::format("log init failed: {}", ex.what()))};
        } catch (...) {
            return std::unexpected{
                make_error(error_code::unknown, "log init failed")};
        }
    } catch (...) {
        rollback_failed_init();
        return std::unexpected(
            make_error(error_code::unknown, "log init failed"));
    }
}

void shutdown() noexcept {
    try {
        std::shared_ptr<spdlog::logger> logger{};
        {
            const std::lock_guard lock{g_mutex};
            // The exchange is atomic; a concurrent write() either observed the
            // logger before this point (and keeps it alive through its own
            // shared_ptr until the call returns) or sees nullptr afterwards.
            logger = g_logger.exchange(std::shared_ptr<spdlog::logger>{},
                                       std::memory_order_acq_rel);
            g_console_sink.reset();
            g_has_console_sink.store(false, std::memory_order_relaxed);
            g_has_file_sink.store(false, std::memory_order_relaxed);
            g_file_level.store(level::off, std::memory_order_relaxed);
            g_initialized.store(false, std::memory_order_release);
        }
        if (logger != nullptr) {
            logger->flush();
        }
        spdlog::shutdown();
        // Clear names only after the async backend has drained and formatted
        // every queued message; clearing before the flush made async lines
        // fall back to `t<tid>` even though their producer had a name.
        const std::unique_lock names_lock{g_thread_names_mutex};
        g_thread_names.clear();
    } catch (...) {
        // shutdown must never throw
    }
}

auto initialized() noexcept -> bool {
    return g_initialized.load(std::memory_order_acquire);
}

auto enabled(level lvl) noexcept -> bool {
    if (lvl == level::off || !g_initialized.load(std::memory_order_acquire)) {
        return false;
    }
    const auto wanted = static_cast<std::int8_t>(lvl);
    if (g_has_console_sink.load(std::memory_order_relaxed) &&
        wanted >= static_cast<std::int8_t>(
                      g_console_level.load(std::memory_order_relaxed))) {
        return true;
    }
    if (g_has_file_sink.load(std::memory_order_relaxed) &&
        wanted >= static_cast<std::int8_t>(
                      g_file_level.load(std::memory_order_relaxed))) {
        return true;
    }
    return false;
}

void set_level(level lvl) noexcept {
    try {
        const std::lock_guard lock{g_mutex};
        apply_console_level_locked(lvl);
    } catch (...) {
    }
}

auto current_level() noexcept -> level {
    return g_console_level.load(std::memory_order_relaxed);
}

void set_thread_name(std::string_view name) noexcept {
    try {
        const auto id{platform::current_thread_id()};
        {
            const std::unique_lock lock{g_thread_names_mutex};
            if (name.empty()) {
                g_thread_names.erase(id);
            } else {
                g_thread_names.insert_or_assign(id, std::string{name});
            }
        }
        // On Linux the main thread's name is the process name, so the "main"
        // label must stay a log label instead of renaming the server.
        if (!name.empty() && name != "main") {
            // Diagnostics only: a platform without thread naming (old Windows,
            // restricted sandbox) still gets the log-side name.
            (void)platform::set_current_thread_name(name);
        }
    } catch (...) {
        // Naming must never propagate (it runs on thread startup paths).
    }
}

void write(level lvl, std::string_view message) noexcept {
    if (lvl == level::off || !g_initialized.load(std::memory_order_acquire)) {
        return;
    }
    try {
        // No global mutex on the logging hot path. The atomic load keeps
        // the logger alive until this call returns; spdlog loggers and _mt
        // sinks are internally synchronized, so concurrent writes are safe and
        // shutdown() can run concurrently (the worst case is a message written
        // to a logger that is being flushed).
        const auto logger{g_logger.load(std::memory_order_acquire)};
        if (logger == nullptr) {
            return;
        }
        logger->log(to_spdlog(lvl), "{}", message);
    } catch (...) {
        // logging failures must never propagate
    }
}

} // namespace refrain::log
