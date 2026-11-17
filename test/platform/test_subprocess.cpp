// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Child-process execution: the platform layer must start a real process from a
// multithreaded caller, wire its stdin, honour its working directory and
// report how it exited. A caller on the server path runs while other threads
// allocate, which is the case the spawn implementation has to stay safe for.
//
// The test picks its command interpreter at runtime instead of branching on a
// platform macro, so it stays inside the platform abstraction guard: the two
// backends are told apart by the executable suffix.

#include <refrain/platform/process.hpp>
#include <refrain/platform/subprocess.hpp>

#include "support/scratch_directory.hpp"

#include <boost/ut.hpp>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using boost::ut::expect;
using namespace boost::ut::literals;
using boost::ut::suite;

namespace platform = refrain::platform;
namespace test_support = refrain::test_support;

/// The platform layer has no portable compile-time OS query, and this file may
/// not branch on a platform macro. The executable suffix distinguishes the
/// Windows backend from the POSIX one.
[[nodiscard]] auto on_windows() -> bool {
    const auto executable{platform::executable_path()};
    if (!executable) {
        return false;
    }
    const auto &path{*executable};
    return path.size() >= 4 &&
           (path.ends_with(".exe") || path.ends_with(".EXE"));
}

struct invocation {
    std::string program{};
    std::vector<std::string> arguments{};
};

/// A command interpreter invocation for `script`.
[[nodiscard]] auto shell(std::string_view script) -> invocation {
    if (on_windows()) {
        return invocation{.program = "cmd.exe",
                          .arguments = {"/c", std::string{script}}};
    }
    return invocation{.program = "/bin/sh",
                      .arguments = {"-c", std::string{script}}};
}

/// Script asserting that `marker.txt` exists in the child's directory.
[[nodiscard]] auto marker_script() -> std::string_view {
    return on_windows() ? "if exist marker.txt (exit 0) else (exit 1)"
                        : "test -f marker.txt";
}

[[nodiscard]] auto run(const invocation &command)
    -> refrain::result<platform::subprocess_result> {
    return platform::run_process(platform::subprocess_options{
        .program = command.program,
        .arguments = command.arguments,
    });
}

} // namespace

suite<"platform/subprocess"> subprocess_tests = [] {
    "platform/subprocess: the child exit code is reported"_test = [] {
        const auto result{run(shell("exit 7"))};
        expect(static_cast<bool>(result))
            << (result ? "" : result.error().message);
        if (result) {
            expect(result->exit_code == 7);
        }
    };

    "platform/subprocess: a signal-terminated child reports 128 + signal"_test =
        [] {
            if (on_windows()) {
                // A Windows child reports a terminated process through its
                // exit code, not through POSIX signal status.
                return;
            }
            const auto result{run(shell("kill -TERM $$"))};
            expect(static_cast<bool>(result));
            if (result) {
                expect(result->exit_code == 128 + 15);
            }
        };

    "platform/subprocess: stdin text reaches the child"_test = [] {
        if (on_windows()) {
            // The POSIX backend is the one whose stdio wiring this pins; the
            // Windows backend hands the read end to the child directly.
            return;
        }
        const auto result{platform::run_process(platform::subprocess_options{
            .program = "/bin/sh",
            .arguments = {"-c", R"(read line; [ "$line" = hello ])"},
            .stdin_text = "hello\n",
        })};
        expect(static_cast<bool>(result));
        if (result) {
            expect(result->exit_code == 0);
        }
    };

    "platform/subprocess: a child that exits before reading stdin does not "
    "kill "
    "the parent"_test = [] {
        if (on_windows()) {
            // Windows reports a broken stdin pipe as a write error; the
            // SIGPIPE failure mode this pins is POSIX-only.
            return;
        }
        // More input than a pipe buffer holds: the parent blocks in `write`
        // until the child is gone, so the next write hits the closed read end.
        // Without a SIGPIPE guard that write terminates the test process
        // instead of returning a child status.
        const auto result{platform::run_process(platform::subprocess_options{
            .program = "/bin/sh",
            .arguments = {"-c", "exit 0"},
            .stdin_text = std::string(4U * 1024U * 1024U, 'x'),
        })};
        expect(static_cast<bool>(result))
            << (result ? "" : result.error().message);
        if (result) {
            expect(result->exit_code == 0);
        }
    };

    "platform/subprocess: the working directory reaches the child"_test = [] {
        const test_support::scratch_directory directory{};
        {
            std::ofstream marker{directory.path() / "marker.txt"};
            marker << "present\n";
            expect(static_cast<bool>(marker));
        }
        const auto marker{shell(marker_script())};
        const auto result{platform::run_process(platform::subprocess_options{
            .program = marker.program,
            .arguments = marker.arguments,
            .working_directory = directory.string(),
        })};
        expect(static_cast<bool>(result))
            << (result ? "" : result.error().message);
        if (result) {
            expect(result->exit_code == 0);
        }
    };

    "platform/subprocess: a program name is resolved through PATH"_test = [] {
        const auto result{platform::run_process(platform::subprocess_options{
            .program = on_windows() ? "cmd.exe" : "sh",
            .arguments = {on_windows() ? "/c" : "-c", "exit 0"},
        })};
        expect(static_cast<bool>(result))
            << (result ? "" : result.error().message);
        if (result) {
            expect(result->exit_code == 0);
        }
    };

    "platform/subprocess: a missing program is an error, not a silent child"_test =
        [] {
            const auto result{
                platform::run_process(platform::subprocess_options{
                    .program = "refrain-test-no-such-program",
                })};
            expect(!result);
            if (!result) {
                expect(result.error().message.find(
                           "refrain-test-no-such-program") !=
                       std::string::npos);
            }
        };

    "platform/subprocess: invocation from a multithreaded caller"_test = [] {
        // Other threads allocate while the process is spawned: the spawn path
        // must not depend on any allocator or stdio state it cannot touch
        // after the fork.
        std::atomic<bool> stop{false};
        std::vector<std::thread> allocators{};
        allocators.reserve(4);
        for (int index{0}; index < 4; ++index) {
            allocators.emplace_back([&stop, index] {
                while (!stop.load(std::memory_order_relaxed)) {
                    std::vector<std::string> churn{};
                    churn.reserve(64);
                    for (int item{0}; item < 64; ++item) {
                        churn.emplace_back(
                            std::string(32, static_cast<char>('a' + index)));
                    }
                }
            });
        }
        for (int attempt{0}; attempt < 24; ++attempt) {
            const auto result{run(shell("exit 0"))};
            expect(static_cast<bool>(result))
                << (result ? "" : result.error().message);
            if (result) {
                expect(result->exit_code == 0);
            }
        }
        stop.store(true, std::memory_order_relaxed);
        for (auto &thread : allocators) {
            thread.join();
        }
    };
};
