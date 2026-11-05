// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/platform/file.hpp>
#include <refrain/platform/process.hpp>

#include <boost/ut.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#if !defined(_WIN32)
// White-box POSIX test of the platform layer itself (see the allow-list entry
// in tools/platform_check.py): observing "died by signal" requires spawning a
// real child process and inspecting waitpid()'s status.
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
using namespace boost::ut;

namespace fs = std::filesystem;

auto scratch_directory() -> fs::path {
    std::error_code error;
    auto path = fs::path{refrain::platform::file::temp_directory()};
    path /= "refrain-platform-process-tests";
    fs::remove_all(path, error);
    fs::create_directories(path, error);
    return path;
}

auto read_text(const fs::path &path) -> std::string {
    std::ifstream in{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{in},
                       std::istreambuf_iterator<char>{}};
}

suite<"platform/process"> process_tests = [] {
    "platform/process: process_id is non-zero"_test = [] {
        expect(refrain::platform::process_id() > 0U);
    };

    "platform/process: null callback is invalid_argument"_test = [] {
        const auto installed =
            refrain::platform::install_fault_handler(nullptr, nullptr);
        expect(!installed.has_value());
        if (!installed.has_value()) {
            expect(static_cast<int>(installed.error().code) ==
                   static_cast<int>(refrain::error_code::invalid_argument));
        }
    };

    "platform/process: install/restore fault handler is idempotent"_test = [] {
        auto callback = [](const refrain::platform::fault_info &,
                           void *) noexcept {};
        const auto installed =
            refrain::platform::install_fault_handler(callback, nullptr);
        // Windows: unsupported (Crashpad is the supported path there).
        if (!installed.has_value()) {
            expect(static_cast<int>(installed.error().code) ==
                   static_cast<int>(refrain::error_code::unsupported));
            expect(true) << "SKIPPED: platform fault fallback unsupported "
                            "(Windows uses Crashpad)";
            return;
        }
        // Re-install replaces the callback without failing.
        expect(static_cast<bool>(
            refrain::platform::install_fault_handler(callback, nullptr)));
        refrain::platform::restore_fault_handlers();
        refrain::platform::restore_fault_handlers(); // idempotent
        expect(true); // sanity: both restore calls returned without crashing
    };

    "platform/process: fault marker is async-safe preformatted text"_test = [] {
        auto callback = [](const refrain::platform::fault_info &,
                           void *) noexcept {};
        const auto installed =
            refrain::platform::install_fault_handler(callback, nullptr);
        if (!installed.has_value()) {
            expect(true) << "SKIPPED: platform fault fallback unsupported "
                            "(Windows uses Crashpad)";
            return;
        }

        const fs::path directory = scratch_directory();
        const fs::path marker = directory / "crash_marker.txt";
        for (const auto kind : {refrain::platform::fault_kind::segmentation,
                                refrain::platform::fault_kind::abort}) {
            const auto native = refrain::platform::native_signal_for(kind);
            if (native == 0) {
                continue;
            }
            const bool written = refrain::platform::write_fault_marker(
                marker.string().c_str(), {kind, native});
            expect(written);
        }
        refrain::platform::restore_fault_handlers();

        const std::string text = read_text(marker);
        expect(!text.empty());
        expect(text.find("refrain crash pid=") != std::string::npos);

        std::error_code error;
        fs::remove_all(directory, error);
    };

#if !defined(_WIN32) && !defined(REFRAIN_SANITIZE_BUILD)
    // The fallback fault handler must die by signal instead of calling _exit
    // while the fault signal is still blocked: _exit(128 + sig) makes the child
    // exit *normally* and never produce a core dump. Dying by signal keeps the
    // OS core-dump and Crashpad paths working.
    // (Skipped under ASan/UBSan/TSan: the sanitizer runtimes intercept the
    // deliberately undefined null dereference.)
    "platform/process: fallback fault handler dies by signal, not _exit"_test =
        [] {
            auto callback = [](const refrain::platform::fault_info &,
                               void *) noexcept {};
            const auto installed =
                refrain::platform::install_fault_handler(callback, nullptr);
            expect(static_cast<bool>(installed));
            if (!installed) {
                return;
            }

            const pid_t child = ::fork();
            expect(child >= 0);
            if (child == 0) {
                // Child: suppress core-file litter but keep the default
                // disposition so death-by-signal is observable via waitpid().
                const struct rlimit no_core{0, 0};
                (void)::setrlimit(RLIMIT_CORE, &no_core);
                refrain::platform::trigger_segmentation_fault();
                ::_exit(0); // unreachable when the handler is correct
            }

            int status{0};
            expect(::waitpid(child, &status, 0) == child);
            expect(WIFSIGNALED(status));
            if (WIFSIGNALED(status)) {
                expect(WTERMSIG(status) == SIGSEGV);
            }
            refrain::platform::restore_fault_handlers();
        };
#endif
};

} // namespace
