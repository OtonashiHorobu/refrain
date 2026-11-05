// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <string>
#include <string_view>

namespace refrain::kernel {

enum class crash_upload : unsigned char {
    disabled = 0,
    enabled = 1,
};

/// Crashpad client configuration.
struct crash_config {
    /// Product name embedded in the minidump (never the player/mojang name).
    std::string product{"Refrain"};
    std::string version{};
    /// Path to the `crashpad_handler` executable spawned as a child process.
    std::string handler_path{};
    /// Directory where minidumps are written (always local and opt-in uploads
    /// copy from here; never contains world data).
    std::string database_path{};
    std::string metrics_path{};
    crash_upload upload{crash_upload::disabled};
    std::string upload_url{};
    /// When Crashpad is unavailable, install a minimal fallback handler that
    /// writes a crash marker file instead of a minidump.
    bool fallback_marker{true};
    /// Attach the crash handler to the current process; false only for tests.
    bool start_handler{true};
};

/// Global crash reporter facade. Initialized once during host startup, before
/// kernel/log initialization (so a crash during boot is still attributed).
class crash_reporter {
  public:
    crash_reporter() = delete;

    /// Initialize the crash reporter. When Crashpad cannot be attached the
    /// configured fallback marker is installed; failures preserve the platform
    /// layer's error code/message (e.g. `unsupported` when no fault-handler
    /// implementation exists for this platform) with added context instead of a
    /// generic `mod_error`.
    ///
    /// Never throws: a hard OOM or formatting failure while
    /// building the diagnostics is reported as `out_of_memory`/`unknown` and a
    /// partially attached client/handler is rolled back, so the caller can log
    /// and keep booting.
    [[nodiscard]] static auto initialize(const crash_config &cfg) noexcept
        -> status;

    /// Record a Crashpad annotation (thread-safe, cheap, never throws).
    ///
    /// Annotations are snapshotted by the handler when it is started
    /// (`initialize()`), so values set before `initialize()` (or between
    /// `shutdown()` and the next `initialize()`) reach the handler. This
    /// Crashpad fork exposes no runtime annotation-update API: calls made while
    /// a handler is already attached are still validated/kept for diagnostics
    /// and for the next handler start, but they do NOT update the running
    /// handler. Annotations are cleared by `shutdown()`.
    ///
    /// A hard OOM while copying the key/value drops this annotation (the
    /// contract is "never throws", not "always stored").
    static void set_annotation(std::string_view key,
                               std::string_view value) noexcept;

    /// Flush pending minidumps and detach the handler (called last on
    /// shutdown). Never throws: teardown runs before the diagnostics, so a
    /// formatting failure cannot leave a handler attached.
    static void shutdown() noexcept;

    [[nodiscard]] static auto initialized() noexcept -> bool;

    /// True when a real Crashpad client is attached (false = fallback/stub).
    [[nodiscard]] static auto enabled() noexcept -> bool;

    /// Copy of the configured crash database path. The value is owned, not a
    /// view into mutex-guarded state; the last value is kept after `shutdown()`
    /// for diagnostics and survives a concurrent re-initialize.
    ///
    /// Returns an empty string when the copy cannot be allocated (hard OOM);
    /// the value is diagnostic only, so this degrades instead of terminating.
    [[nodiscard]] static auto database_path() noexcept -> std::string;

    /// Crash on purpose (NULL dereference) to validate the handler in CI and
    /// local smoke tests. Returns only if no handler is attached.
    static void trigger_for_testing() noexcept;
};

} // namespace refrain::kernel
