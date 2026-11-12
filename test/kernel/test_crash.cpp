// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/error.hpp>
#include <refrain/core/log.hpp>
#include <refrain/kernel/crash.hpp>
#include <refrain/platform/file.hpp>

#include "support/scratch_directory.hpp"

#include <boost/ut.hpp>

#include <filesystem>
#include <string>
#include <system_error>

namespace {
using namespace boost::ut;

/// The unit tests never attach a real Crashpad handler: handler startup is
/// exercised by the CI crash smoke test (real minidump assertion), which owns
/// the crashpad_handler executable. Here we only cover facade semantics:
/// argument validation, degraded fallback, stub mode and shutdown/restore.
void ensure_silent_log() {
    if (!refrain::log::initialized()) {
        refrain::log::config config{};
        config.console_level = refrain::log::level::off;
        (void)refrain::log::init(config);
    }
}

using scratch_directory = refrain::test_support::scratch_directory;

/// Tests share a process-wide singleton: every test starts from a clean facade.
void reset_reporter() { refrain::kernel::crash_reporter::shutdown(); }
} // namespace

namespace {
using namespace boost::ut;

// NOTE: Boost.UT 2.3.1 does not accept the namespace-scope `"name"_test = ...`
// form with GCC 16 (a user-defined literal is not a valid declarator there), so
// each file registers its tests from a `suite` lambda; the literal form works
// inside that function scope.
[[maybe_unused]] const suite crash_tests = [] {
    "crash/initialize rejects empty database path"_test = [] {
        ensure_silent_log();
        reset_reporter();

        refrain::kernel::crash_config config{};
        config.start_handler = false;
        const auto result = refrain::kernel::crash_reporter::initialize(config);

        expect(!result);
        if (!result) {
            expect(result.error().code ==
                   refrain::error_code::invalid_argument);
        }
        expect(!refrain::kernel::crash_reporter::initialized());
        expect(!refrain::kernel::crash_reporter::enabled());
    };

    "crash/initialize falls back to the crash marker when the handler is missing"_test =
        [] {
            ensure_silent_log();
            reset_reporter();
            const scratch_directory directory;

            refrain::kernel::crash_config config{};
            config.database_path = directory.string();
            config.handler_path = {}; // no crashpad_handler available
            config.fallback_marker = true;
            const auto result =
                refrain::kernel::crash_reporter::initialize(config);

            expect(static_cast<bool>(result));
            if (!result) {
                return;
            }
            expect(refrain::kernel::crash_reporter::initialized());
            // Fallback never sets `enabled`; only a real Crashpad client does.
            expect(!refrain::kernel::crash_reporter::enabled());
            const auto expected_path = directory.string();
            expect(refrain::kernel::crash_reporter::database_path() ==
                   expected_path);

            // Annotation bookkeeping must not crash in fallback mode.
            refrain::kernel::crash_reporter::set_annotation("test", "fallback");

            reset_reporter();
            expect(!refrain::kernel::crash_reporter::initialized());
            expect(!refrain::kernel::crash_reporter::enabled());
            // database_path() intentionally keeps the last value (as an owned
            // copy) for diagnostics.
            expect(refrain::kernel::crash_reporter::database_path() ==
                   expected_path);
        };

    // The crash database directory is owner-private (0700); the default
    // create_directories mode would leave it at umask (usually 0755) and
    // expose minidumps to other local users.
    "crash/database directory is created owner-private"_test = [] {
        ensure_silent_log();
        reset_reporter();
        const scratch_directory directory;

        refrain::kernel::crash_config config{};
        config.database_path = directory.string();
        config.handler_path = {};
        config.fallback_marker = true;
        const auto result = refrain::kernel::crash_reporter::initialize(config);
        expect(static_cast<bool>(result));

        // The platform layer owns the permission check (POSIX 0700); the test
        // has no platform branch of its own.
        expect(refrain::platform::file::is_owner_private_directory(
            directory.string()));

        reset_reporter();
    };

    // database_path() returns an owned copy: a stored string_view would point
    // into mutex-guarded state that a later initialize() (after shutdown)
    // could reassign on another thread.
    "crash/database_path returns an owned copy"_test = [] {
        ensure_silent_log();
        reset_reporter();
        const scratch_directory base;
        const auto first_path = (base.path() / "first").string();
        const auto second_path = (base.path() / "second").string();

        refrain::kernel::crash_config first{};
        first.database_path = first_path;
        first.start_handler = false;
        expect(static_cast<bool>(
            refrain::kernel::crash_reporter::initialize(first)));

        // The owned copy is captured while the reporter is initialised.
        const auto held = refrain::kernel::crash_reporter::database_path();
        expect(held == first_path);

        reset_reporter();
        refrain::kernel::crash_config second{};
        second.database_path = second_path;
        second.start_handler = false;
        expect(static_cast<bool>(
            refrain::kernel::crash_reporter::initialize(second)));

        // Re-initialisation replaced the stored path; the previously returned
        // value is unaffected (it owns its storage).
        expect(held == first_path);
        expect(refrain::kernel::crash_reporter::database_path() == second_path);

        reset_reporter();
        expect(refrain::kernel::crash_reporter::database_path() == second_path);
    };

    // An over-long crash database path must fail with a specific diagnostic
    // (marker path > 1024-byte static buffer) instead of a generic "handler
    // could not be installed"; the error keeps the invalid_argument code and
    // mentions the limit.
    "crash/oversized crash database path reports a specific error"_test = [] {
        ensure_silent_log();
        reset_reporter();
        const scratch_directory base;

        // Build a creatable path longer than the marker buffer: every component
        // is < 255 bytes, the total is still below PATH_MAX.
        std::filesystem::path deep = base.path();
        const std::string component(200, 'x');
        while (deep.string().size() <= 1100) {
            deep /= component;
        }
        // The marker-limit branch is only reachable when the database
        // directory itself can be created. Hosts with a shorter path limit
        // (Windows MAX_PATH without long-path opt-in) cannot represent the
        // fixture, and crash_reporter rejects the path earlier.
        std::error_code probe_error;
        const bool creatable{
            std::filesystem::create_directories(deep, probe_error)};
        if (!creatable) {
            return;
        }
        std::filesystem::remove_all(deep, probe_error);

        refrain::kernel::crash_config config{};
        config.database_path = deep.string();
        config.handler_path = {};
        config.fallback_marker = true;
        const auto result = refrain::kernel::crash_reporter::initialize(config);
        expect(!result);
        if (!result) {
            expect(result.error().code ==
                   refrain::error_code::invalid_argument);
            expect(result.error().message.find("marker") != std::string::npos);
            expect(result.error().message.find("1024") != std::string::npos);
        }
        expect(!refrain::kernel::crash_reporter::initialized());
        reset_reporter();
    };

    // Annotations may be recorded before initialize() (they are attached by
    // the next handler start) and while a fallback/stub reporter is
    // initialized; neither path may crash or reject the call.
    "crash/annotations are collected before and after initialize"_test = [] {
        ensure_silent_log();
        reset_reporter();
        const scratch_directory directory;

        // Before initialize(): collected for the handler that may start later.
        refrain::kernel::crash_reporter::set_annotation("refrain.commit",
                                                        "da94989");

        refrain::kernel::crash_config config{};
        config.database_path = directory.string();
        config.start_handler = false;
        expect(static_cast<bool>(
            refrain::kernel::crash_reporter::initialize(config)));

        // After initialize() (stub mode): tolerated, diagnostic-only.
        refrain::kernel::crash_reporter::set_annotation("refrain.branch",
                                                        "main");
        expect(refrain::kernel::crash_reporter::initialized());

        reset_reporter();
        // Cleared by shutdown: a post-shutdown call is accepted again.
        refrain::kernel::crash_reporter::set_annotation("refrain.branch",
                                                        "main");
    };

    "crash/initialize fails when Crashpad and the fallback are unavailable"_test =
        [] {
            ensure_silent_log();
            reset_reporter();
            const scratch_directory directory;

            refrain::kernel::crash_config config{};
            config.database_path = directory.string();
            config.handler_path = {};
            config.fallback_marker = false;
            const auto result =
                refrain::kernel::crash_reporter::initialize(config);

            expect(!result);
            if (!result) {
                expect(result.error().code == refrain::error_code::mod_error);
            }
            expect(!refrain::kernel::crash_reporter::initialized());
        };

    "crash/start_handler=false is a test stub"_test = [] {
        ensure_silent_log();
        reset_reporter();
        const scratch_directory directory;

        refrain::kernel::crash_config config{};
        config.database_path = directory.string();
        config.handler_path = {};
        config.fallback_marker = false;
        config.start_handler = false;
        const auto result = refrain::kernel::crash_reporter::initialize(config);

        expect(static_cast<bool>(result));
        expect(refrain::kernel::crash_reporter::initialized());
        expect(!refrain::kernel::crash_reporter::enabled());
        const auto expected_path = directory.string();
        expect(refrain::kernel::crash_reporter::database_path() ==
               expected_path);
        reset_reporter();
    };

    "crash/double initialize is rejected"_test = [] {
        ensure_silent_log();
        reset_reporter();
        const scratch_directory directory;

        refrain::kernel::crash_config config{};
        config.database_path = directory.string();
        config.start_handler = false;
        expect(static_cast<bool>(
            refrain::kernel::crash_reporter::initialize(config)));

        const auto second = refrain::kernel::crash_reporter::initialize(config);
        expect(!second);
        if (!second) {
            expect(second.error().code == refrain::error_code::already_exists);
        }

        reset_reporter();
        expect(!refrain::kernel::crash_reporter::initialized());
    };

    "crash/shutdown is idempotent and trigger is inert when unarmed"_test = [] {
        ensure_silent_log();
        reset_reporter();
        refrain::kernel::crash_reporter::shutdown(); // no-op
        expect(!refrain::kernel::crash_reporter::initialized());

        // Contract: trigger_for_testing() returns when no handler is attached,
        // so this must not crash the test binary. The CI crash-smoke step
        // covers a real crash.
        refrain::kernel::crash_reporter::trigger_for_testing();
        expect(true) << "trigger_for_testing() is inert without a handler "
                        "(real crash is the CI crash-smoke step)";
    };
};
} // namespace
