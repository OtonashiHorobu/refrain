// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <fmt/format.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace refrain::log {

enum class level : std::int8_t {
    trace = 0,
    debug,
    info,
    warn,
    error,
    critical,
    off,
};

[[nodiscard]] auto to_string(level lvl) noexcept -> std::string_view;

[[nodiscard]] auto level_from_string(std::string_view name) noexcept -> level;

struct config {
    /// Console sink level. `off` disables the console sink entirely.
    ///
    /// The console sink writes to **stderr**, never stdout: stdout is the
    /// machine-readable data channel of the tools (`refrain-tools ... dump`,
    /// `data check`, `content where`, ...) and CI greps its byte stream, so a
    /// log line interleaved there would corrupt the output (`refrain-tools
    /// gamepack dump > out.txt` must stay a clean dump).
    level console_level{level::info};
    /// Optional file sink (rotating by size). Empty path = disabled.
    std::string file_path{};
    level file_level{level::debug};
    bool enable_colors{true};
    /// Opt-in asynchronous logger (dedicated queue + background thread). The
    /// server and tools keep the synchronous default; tests disable it. The
    /// synchronous logger flushes on `warn` and above plus at shutdown().
    bool async{false};
    /// Periodic flush interval for the asynchronous logger; 0 keeps the
    /// `warn`-and-above + shutdown-only flushing. Ignored when `async` is
    /// false.
    std::uint32_t flush_interval_ms{1000};
};

/// Initialize the global logger. Only the first successful init creates sinks;
/// later calls are no-ops except for re-applying `console_level` (an init that
/// enables no sinks just records the level and drops everything). A partial
/// failure rolls back the sinks it already created, so a later valid init
/// starts from a clean state. A call after shutdown re-initializes. Never
/// throws.
[[nodiscard]] auto init(const config &cfg) noexcept -> status;

/// Flush and tear down all sinks.
void shutdown() noexcept;

[[nodiscard]] auto initialized() noexcept -> bool;

/// Cheap runtime level filter for the console sink.
void set_level(level lvl) noexcept;

[[nodiscard]] auto current_level() noexcept -> level;

/// Assign a readable name to the calling thread.
///
/// The name is used by the `%N` pattern flag (the default console/file
/// patterns print `[worker-0/info]`; unnamed threads fall back to `t<tid>`)
/// and is also published as the best-effort OS thread name, so `top -H`,
/// `ps -T` and debuggers show the same label.
///
/// Works before `init()` (the registry is independent of the sinks) and is
/// idempotent; an empty name clears the entry. The registry is keyed by the
/// native thread id, which is why the async logger still attributes messages
/// to their producer: spdlog copies `thread_id` into the queued message.
///
/// Names should be short and ASCII (`main`, `worker-0`, `io-12`, `console`);
/// Linux truncates the OS-visible name to 15 bytes.
void set_thread_name(std::string_view name) noexcept;

/// Cheap check: true when `lvl` reaches at least one configured sink. The
/// formatting wrappers short-circuit on this, so a disabled level costs one
/// atomic load instead of a `fmt::format` + lock + dispatch. Note that the
/// arguments of a logging call are still evaluated by the caller (use the
/// short-circuit deliberately for expensive payloads).
[[nodiscard]] auto enabled(level lvl) noexcept -> bool;

/// Raw write entry point (std::string_view payload, already formatted).
///
/// Thread-safe and lock-free with respect to the init/shutdown mutex: the
/// logger pointer is published atomically, so logging never serializes all
/// threads behind one global lock. A concurrent `shutdown()` is safe; a message
/// racing with it may be dropped or still be flushed, but never crashes.
void write(level lvl, std::string_view message) noexcept;

// ---------------------------------------------------------------------------
// Formatting wrappers. Logging is cold path; formatting happens only when the
// level is enabled to keep call sites cheap.
// ---------------------------------------------------------------------------

template <typename... Args>
void log(level lvl, fmt::format_string<Args...> format_str, Args &&...args) {
    if (!enabled(lvl)) {
        return;
    }
    write(lvl, fmt::format(format_str, std::forward<Args>(args)...));
}

template <typename... Args>
void trace(fmt::format_string<Args...> format_str, Args &&...args) {
    log(level::trace, format_str, std::forward<Args>(args)...);
}

template <typename... Args>
void debug(fmt::format_string<Args...> format_str, Args &&...args) {
    log(level::debug, format_str, std::forward<Args>(args)...);
}

template <typename... Args>
void info(fmt::format_string<Args...> format_str, Args &&...args) {
    log(level::info, format_str, std::forward<Args>(args)...);
}

template <typename... Args>
void warn(fmt::format_string<Args...> format_str, Args &&...args) {
    log(level::warn, format_str, std::forward<Args>(args)...);
}

template <typename... Args>
void error(fmt::format_string<Args...> format_str, Args &&...args) {
    log(level::error, format_str, std::forward<Args>(args)...);
}

template <typename... Args>
void critical(fmt::format_string<Args...> format_str, Args &&...args) {
    log(level::critical, format_str, std::forward<Args>(args)...);
}

} // namespace refrain::log
