// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/kernel/crash.hpp>

#include <refrain/core/log.hpp>
#include <refrain/platform/file.hpp>
#include <refrain/platform/process.hpp>

#if REFRAIN_WITH_CRASHPAD
#include <base/files/file_path.h>

#include <client/crash_report_database.h>
#include <client/crashpad_client.h>
#include <client/settings.h>
#endif

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr refrain::log::logger log_crash_reporter{"crash-reporter"};
constexpr refrain::log::logger log_kernel{"kernel"};

} // namespace

// The CMake option always defines REFRAIN_WITH_CRASHPAD (0 or 1); the default
// keeps this translation unit buildable outside of the project build system.
#if !defined(REFRAIN_WITH_CRASHPAD)
#define REFRAIN_WITH_CRASHPAD 0
#endif

namespace refrain::kernel {

namespace {

/// Global crash reporter state. All fields are guarded by `mutex`; the signal
/// handler path below never touches this struct (only the signal-safe globals
/// declared in the POSIX fallback section).
struct reporter_state {
    std::mutex mutex;
    /// Configured database path. Kept after shutdown for diagnostics;
    /// `database_path()` returns an owned copy, so this member may be
    /// reassigned by a later initialize() without dangling a caller's view.
    std::string database_path{};
    std::string metrics_path{};
    /// Diagnostics copy of annotations. Crashpad annotations are only read by
    /// the handler at StartHandler() time; this fork has no runtime update API,
    /// so the map is kept for future handler restarts and for debugging.
    std::map<std::string, std::string> annotations{};
    /// Published without the mutex: `initialized()`/`enabled()` are `noexcept`
    /// and must not lock. Written under `mutex`, read with acquire.
    std::atomic<bool> initialized{false};
    /// True only when a real Crashpad client is attached (fallback marker or
    /// test stub keep this false).
    std::atomic<bool> enabled{false};
    bool fallback_installed{false};
#if REFRAIN_WITH_CRASHPAD
    std::unique_ptr<crashpad::CrashpadClient> client{};
#endif
};

/// Function-local static: thread-safe initialization since C++11.
auto state() noexcept -> reporter_state & {
    static reporter_state instance{};
    return instance;
}

/// Error for a hard OOM caught at a `noexcept` boundary. The message is a
/// short static literal: on the supported standard libraries it fits the
/// small-string buffer, so reporting the failure does not allocate again (a
/// second `bad_alloc` inside a `noexcept` handler would call
/// `std::terminate`).
[[nodiscard]] auto out_of_memory_error() noexcept -> error {
    return error{error_code::out_of_memory, std::string{"out of memory"}};
}

/// Same for a non-OOM exception escaping a `noexcept` boundary (formatting,
/// mutex, filesystem). Message kept SSO-sized for the same reason.
[[nodiscard]] auto unknown_error() noexcept -> error {
    return error{error_code::unknown, std::string{"unknown error"}};
}

/// Exception barriers for the public `noexcept` facade.
///
/// The crash reporter is documented as never throwing, but initialization,
/// annotation storage and diagnostics allocate (`std::string`, `std::map`,
/// `fmt`) and take the state mutex. Only the public entry points catch;
/// internal helpers stay plain functions (never mark allocating/locking
/// code `noexcept`) so exceptions reach these barriers instead of turning into
/// `std::terminate`.
template <typename Fn> void guarded_void(Fn &&fn) noexcept {
    try {
        std::forward<Fn>(fn)();
    } catch (...) {
        // Best effort: the entry point is documented as never throwing.
    }
}

template <typename Fn>
[[nodiscard]] auto guarded_string(Fn &&fn) noexcept -> std::string {
    try {
        return std::forward<Fn>(fn)();
    } catch (...) {
        return {};
    }
}

/// Create the crash database/metrics directory as owner-private (POSIX mode
/// 0700). The platform layer owns the permission tightening; its error is
/// propagated with the path context instead of a generic message.
///
/// Not `noexcept`: the contextual message allocates. The only caller is
/// `initialize_impl()`, whose public `initialize()` entry point is the
/// exception barrier.
[[nodiscard]] auto ensure_private_directory(std::string_view path) -> status {
    if (path.empty()) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "empty crash directory path")};
    }
    auto created = platform::file::create_private_directory(path);
    if (!created) {
        return std::unexpected{
            make_error(created.error().code,
                       "cannot create crash directory '" + std::string{path} +
                           "': " + std::string{created.error().message})};
    }
    return status_ok();
}

/// Not `noexcept`: `log::error` formats (allocates) and `make_error` copies the
/// message. Reached only through the barrier in `initialize_impl()`.
[[nodiscard]] auto fail(error_code code, std::string message) -> status {
    log_crash_reporter.error("failed to initialize the crash reporter: {}",
                             message);
    return std::unexpected{make_error(code, std::move(message))};
}

// ---------------------------------------------------------------------------
// Fault-handler fallback: async-signal-safe crash marker via the platform layer
// ---------------------------------------------------------------------------

constexpr std::size_t k_max_marker_path{1024};
constexpr std::string_view k_marker_file_name{"crash_marker.txt"};

/// Static, null-terminated marker path captured at initialize() time. The
/// platform handler only reads it; it never allocates.
char g_marker_path[k_max_marker_path]{};

/// Runs inside the platform fault handler (signal context): the platform layer
/// owns all signal/fd mechanics; this callback only forwards the path.
void fallback_fault_callback(const platform::fault_info &info,
                             void * /*context*/) noexcept {
    (void)platform::write_fault_marker(g_marker_path, info);
}

/// Build the async-signal-safe marker path.
///
/// The path is joined with `std::filesystem` (UTF-8 -> native -> UTF-8) instead
/// of string concatenation, so both `/` and Windows `\` separators and trailing
/// separators are normalized uniformly. Oversized paths produce a specific
/// `invalid_argument` error naming the limit instead of a generic
/// "handler could not be installed" failure.
///
/// Not `noexcept`: `std::filesystem::path`/`u8string()` and the contextual
/// error message allocate. That is safe here because this runs at
/// `initialize()` time, not in signal context: the handler only reads the
/// static buffer this function fills, and `initialize()` catches the failure.
[[nodiscard]] auto build_marker_path(std::string_view database_path) -> status {
    if (database_path.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "crash reporter: database path must not be empty")};
    }
    std::filesystem::path marker = platform::file::native_path(database_path);
    marker /= std::string{k_marker_file_name};
    const auto utf8 = marker.u8string();
    if (utf8.size() + 1U > k_max_marker_path) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "crash reporter: crash marker path for '" +
                           std::string{database_path} + "' needs " +
                           std::to_string(utf8.size()) + " bytes, above the " +
                           std::to_string(k_max_marker_path) +
                           "-byte static-buffer limit (shorten crash_config."
                           "database_path)")};
    }
    std::size_t index{0};
    for (const char8_t character : utf8) {
        g_marker_path[index++] = static_cast<char>(character);
    }
    g_marker_path[index] = '\0';
    return status_ok();
}

/// Not `noexcept`: both error paths allocate the contextual message.
[[nodiscard]] auto install_fallback_handlers(std::string_view database_path)
    -> status {
    if (auto built = build_marker_path(database_path); !built) {
        return built; // specific reason: empty path or >1 KiB marker path
    }
    auto installed =
        platform::install_fault_handler(&fallback_fault_callback, nullptr);
    if (!installed) {
        // Keep the platform error code (e.g. `unsupported` where no
        // fallback handler exists) and add the crash-context instead of
        // collapsing it into a generic mod_error.
        return std::unexpected{
            make_error(installed.error().code,
                       "fallback crash handler could not be installed for '" +
                           std::string{database_path} +
                           "': " + std::string{installed.error().message})};
    }
    return status_ok();
}

void restore_fallback_handlers() noexcept {
    platform::restore_fault_handlers();
}

// ---------------------------------------------------------------------------
// Crashpad client (real implementation)
// ---------------------------------------------------------------------------

#if REFRAIN_WITH_CRASHPAD

/// Mini-chromium's FilePath takes a narrow string on POSIX and a wide string on
/// Windows; the platform layer owns that conversion (`native_string` matches
/// `base::FilePath::StringType`), so there is no platform branch here.
auto to_file_path(std::string_view utf8_path) -> base::FilePath {
    return base::FilePath{platform::file::native_string(utf8_path)};
}

/// Starts the out-of-process Crashpad handler. Returns true on success; on
/// failure logs the reason and lets initialize() fall back to the marker.
///
/// Not `noexcept`: Crashpad's client/database path allocates and the
/// diagnostics format; `initialize_impl()`'s caller is the barrier.
[[nodiscard]] auto start_crashpad(reporter_state &current,
                                  const crash_config &cfg) -> bool {
    if (cfg.handler_path.empty()) {
        log_kernel.error("Crashpad is enabled but no handler path is "
                         "configured");
        return false;
    }
    auto client = std::make_unique<crashpad::CrashpadClient>();
    const auto database = to_file_path(cfg.database_path);
    const auto metrics = to_file_path(current.metrics_path);

    // Uploads are controlled through the on-disk Settings of the database.
    // CrashpadClient has no SetUploadsEnabled in this fork, so record the
    // opt-in before the handler starts.
    if (auto database_instance =
            crashpad::CrashReportDatabase::Initialize(database);
        static_cast<bool>(database_instance)) {
        if (auto *settings = database_instance->GetSettings(); settings) {
            if (!settings->SetUploadsEnabled(cfg.upload ==
                                             crash_upload::enabled)) {
                log_crash_reporter.warn(
                    "failed to persist the upload preference");
            }
        }
    } else {
        log_crash_reporter.warn("failed to open the crash database at '{}'",
                                cfg.database_path);
    }

    std::map<std::string, std::string> annotations{{"product", cfg.product}};
    if (!cfg.version.empty()) {
        annotations.emplace("version", cfg.version);
    }
    for (const auto &[key, value] : current.annotations) {
        annotations.insert_or_assign(key, value);
    }

    // Empty URL means "record dumps locally, never upload".
    const std::string upload_url =
        (cfg.upload == crash_upload::enabled) ? cfg.upload_url : std::string{};
    const std::vector<std::string> arguments{};
    const bool started = client->StartHandler(
        to_file_path(cfg.handler_path), database, metrics, upload_url,
        std::string{}, annotations, arguments,
        /*restartable=*/false, /*asynchronous_start=*/false);
    if (!started) {
        log_crash_reporter.error("failed to start the Crashpad handler at '{}'",
                                 cfg.handler_path);
        return false;
    }
    current.client = std::move(client);
    return true;
}

#endif // REFRAIN_WITH_CRASHPAD

/// Body of the public `initialize()`. Deliberately not `noexcept`: it
/// allocates paths/messages and formats diagnostics, so failures reach the
/// barrier in `crash_reporter::initialize()` instead of terminating the
/// process mid-boot.
[[nodiscard]] auto initialize_impl(const crash_config &cfg) -> status {
    auto &current = state();
    const std::lock_guard lock{current.mutex};
    if (current.initialized.load(std::memory_order_acquire)) {
        return std::unexpected{
            make_error(error_code::already_exists,
                       std::string{"crash reporter already initialized"})};
    }
    if (cfg.database_path.empty()) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            std::string{"crash_config.database_path must not be empty"})};
    }
    if (auto database_directory = ensure_private_directory(cfg.database_path);
        !database_directory) {
        return database_directory;
    }
    current.database_path = cfg.database_path;
    current.metrics_path =
        cfg.metrics_path.empty() ? cfg.database_path : cfg.metrics_path;
    if (current.metrics_path != cfg.database_path) {
        if (auto metrics_directory =
                ensure_private_directory(current.metrics_path);
            !metrics_directory) {
            return metrics_directory;
        }
    }

    if (!cfg.start_handler) {
        // Test-only stub: no handler is attached and no marker is installed.
        current.initialized.store(true, std::memory_order_release);
        log_kernel.warn("started the crash reporter in stub mode, the handler "
                        "stays disabled");
        return status_ok();
    }

#if REFRAIN_WITH_CRASHPAD
    if (start_crashpad(current, cfg)) {
        current.enabled.store(true, std::memory_order_release);
        current.initialized.store(true, std::memory_order_release);
        log_kernel.info("started Crashpad with database '{}' and uploads {}",
                        current.database_path,
                        cfg.upload == crash_upload::enabled ? "enabled"
                                                            : "disabled");
        return status_ok();
    }
#else
    log_kernel.warn("this build has no Crashpad support");
#endif

    if (cfg.fallback_marker) {
        // Propagate the platform/step error (specific code + context)
        // instead of collapsing it into a generic mod_error.
        auto installed = install_fallback_handlers(current.database_path);
        if (!installed) {
            return installed;
        }
        current.fallback_installed = true;
        current.initialized.store(true, std::memory_order_release);
        log_kernel.warn(
            "Crashpad is unavailable, the fallback crash marker is enabled "
            "at '{}'",
            std::string_view{g_marker_path});
        return status_ok();
    }
    return fail(error_code::mod_error,
                "Crashpad is unavailable and crash_config.fallback_marker is "
                "disabled");
}

/// Undo a partially applied initialize() after the barrier caught an exception
/// (typically `std::bad_alloc` while building a diagnostic). Best effort: a
/// failure here is swallowed because the public entry point is `noexcept`.
void rollback_failed_initialize() noexcept {
    try {
        auto &current = state();
        const std::lock_guard lock{current.mutex};
#if REFRAIN_WITH_CRASHPAD
        current.client.reset();
#endif
        if (current.fallback_installed) {
            restore_fallback_handlers();
            current.fallback_installed = false;
        }
        current.enabled.store(false, std::memory_order_release);
        current.initialized.store(false, std::memory_order_release);
    } catch (...) {
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public facade
// ---------------------------------------------------------------------------

auto crash_reporter::initialize(const crash_config &cfg) noexcept -> status {
    // Exception barrier: `initialize_impl()` allocates and formats; the public
    // API is documented as never throwing, so report the failure as a value
    // and leave no half-initialized handler behind.
    try {
        return initialize_impl(cfg);
    } catch (const std::bad_alloc &) {
        rollback_failed_initialize();
        return std::unexpected{out_of_memory_error()};
    } catch (...) {
        rollback_failed_initialize();
        return std::unexpected{unknown_error()};
    }
}

void crash_reporter::set_annotation(std::string_view key,
                                    std::string_view value) noexcept {
    // Annotations are snapshotted by Crashpad when the handler starts.
    // Always record the value (so it is attached by the next initialize() after
    // a shutdown() and visible to diagnostics), but do not pretend a running
    // handler is updated: this fork has no runtime annotation API.
    //
    // Exception barrier: the map/string insert allocates. On hard OOM the
    // annotation is dropped rather than terminating the caller (the documented
    // contract is "cheap, never throws", not "always stored").
    guarded_void([key, value] {
        auto &current = state();
        const std::lock_guard lock{current.mutex};
        current.annotations.insert_or_assign(std::string{key},
                                             std::string{value});
        if (current.initialized.load(std::memory_order_acquire) &&
            current.enabled.load(std::memory_order_acquire)) {
            log_crash_reporter.debug(
                "recorded annotation '{}' after the handler started, it is "
                "attached on the next handler start only",
                key);
        }
    });
}

void crash_reporter::shutdown() noexcept {
    guarded_void([] {
        auto &current = state();
        bool detached{false};
        bool removed{false};
        {
            const std::lock_guard lock{current.mutex};
            if (!current.initialized.load(std::memory_order_acquire)) {
                return;
            }
#if REFRAIN_WITH_CRASHPAD
            if (current.client) {
                // This fork has no StopHandler()/flush API: releasing the
                // client closes the client socket and the handler process
                // exits when the client goes away. Local minidumps are already
                // written by the handler on crash.
                current.client.reset();
                detached = true;
            }
#endif
            if (current.fallback_installed) {
                restore_fallback_handlers();
                current.fallback_installed = false;
                removed = true;
            }
            current.enabled.store(false, std::memory_order_release);
            current.initialized.store(false, std::memory_order_release);
            current.annotations.clear();
        }
        // Diagnostics run after the state is consistent: a formatting failure
        // must not skip the teardown above (the barrier swallows it).
        if (detached) {
            log_kernel.info("detached the Crashpad client");
        }
        if (removed) {
            log_kernel.info("removed the fallback crash handler");
        }
    });
}

auto crash_reporter::initialized() noexcept -> bool {
    // Atomic read, no lock/allocation: the getter is genuinely `noexcept`
    // instead of relying on `std::mutex::lock` never throwing.
    return state().initialized.load(std::memory_order_acquire);
}

auto crash_reporter::enabled() noexcept -> bool {
    return state().enabled.load(std::memory_order_acquire);
}

auto crash_reporter::database_path() noexcept -> std::string {
    // Return an owned copy. The string is only reassigned by a successful
    // initialize(), which may run on another thread while a caller holds the
    // value; a string_view into mutex-guarded state could dangle there.
    //
    // Copying allocates, so the barrier reports a hard OOM as an empty
    // path instead of terminating (documented in crash.hpp; the value is
    // diagnostic only).
    return guarded_string([] {
        auto &current = state();
        const std::lock_guard lock{current.mutex};
        return current.database_path;
    });
}

void crash_reporter::trigger_for_testing() noexcept {
    bool armed{false};
    guarded_void([&armed] {
        auto &current = state();
        const std::lock_guard lock{current.mutex};
        armed = current.initialized.load(std::memory_order_acquire) &&
                (current.enabled.load(std::memory_order_acquire) ||
                 current.fallback_installed);
    });
    if (!armed) {
        // Matches the documented contract: returns when no handler is attached.
        guarded_void([] {
            log_kernel.warn(
                "ignored the test crash: no crash handler is installed");
        });
        return;
    }
    // The deliberate fault stays outside the logging barrier: diagnostics must
    // not be able to swallow the crash this function exists to trigger.
    guarded_void(
        [] { log_kernel.error("triggered a deliberate crash for testing"); });
    // Deliberate fault, implemented inside the platform layer: exercises the
    // attached handler in CI smoke tests. UBSan builds are expected to
    // flag it on purpose.
    platform::trigger_segmentation_fault();
}

} // namespace refrain::kernel
