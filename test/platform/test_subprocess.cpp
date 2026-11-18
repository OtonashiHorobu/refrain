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
// backends are told apart by the executable suffix. The one exception is the
// Windows-only quoting round-trip at the end, which reads the private quoting
// header and calls `CommandLineToArgvW` directly; the platform guard allow-list
// names this file for that arm.

#include <refrain/platform/process.hpp>
#include <refrain/platform/subprocess.hpp>

#include "support/scratch_directory.hpp"

#include <boost/ut.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
// White-box test of the Win32 command-line quoting (see the allow-list entry in
// tools/platform_check.py): prove that the command line `run_process` builds
// means the caller's arguments to the child, so the test reads the private
// quoting header and runs its output back through `CommandLineToArgvW`, the
// parser the child's runtime uses.
#include <windows.h>

#include <shellapi.h>

#include "../../src/platform/windows/argument_quoting.hpp"

#endif

namespace {

using boost::ut::expect;
using namespace boost::ut::literals;
using boost::ut::suite;

namespace platform = refrain::platform;
namespace test_support = refrain::test_support;
namespace fs = std::filesystem;

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

#if defined(_WIN32)
/// UTF-8 -> UTF-16 for the command-line parser.
[[nodiscard]] auto to_wide(std::string_view text) -> std::wstring {
    if (text.empty()) {
        return {};
    }
    const auto size{::MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0)};
    if (size <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                              static_cast<int>(text.size()), wide.data(),
                              size) != size) {
        return {};
    }
    return wide;
}

/// UTF-16 -> UTF-8 for comparison with the input argument.
[[nodiscard]] auto to_utf8(std::wstring_view text) -> std::string {
    if (text.empty()) {
        return {};
    }
    const auto size{::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                          static_cast<int>(text.size()),
                                          nullptr, 0, nullptr, nullptr)};
    if (size <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(size), '\0');
    if (::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                              static_cast<int>(text.size()), utf8.data(), size,
                              nullptr, nullptr) != size) {
        return {};
    }
    return utf8;
}

/// Split `command_line` the way the child's C runtime does.
[[nodiscard]] auto split_command_line(std::string_view command_line)
    -> std::vector<std::string> {
    const auto wide{to_wide(command_line)};
    int count{0};
    wchar_t **arguments{::CommandLineToArgvW(wide.c_str(), &count)};
    if (arguments == nullptr) {
        return {};
    }
    std::vector<std::string> split{};
    split.reserve(static_cast<std::size_t>(count));
    for (auto index{0}; index < count; ++index) {
        split.push_back(to_utf8(arguments[index]));
    }
    ::LocalFree(arguments);
    return split;
}

/// Command line with a dummy program name, so the argument under test is
/// parsed with the ordinary argument rules rather than the argv[0] special
/// case.
[[nodiscard]] auto probe_line(std::string_view argument) -> std::string {
    return "probe " + platform::windows_detail::quote_argument(argument);
}
#endif

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

    "platform/subprocess: a child that exceeds its timeout is killed"_test =
        [] {
            // The command sleeps for several seconds; the timeout must return
            // control long before that and report the kill instead of blocking.
            const auto sleeper{
                shell(on_windows() ? "ping -n 6 127.0.0.1 >NUL" : "sleep 5")};
            const auto started{std::chrono::steady_clock::now()};
            const auto result{
                platform::run_process(platform::subprocess_options{
                    .program = sleeper.program,
                    .arguments = sleeper.arguments,
                    .timeout = std::chrono::milliseconds{300},
                })};
            const auto elapsed{std::chrono::steady_clock::now() - started};
            expect(static_cast<bool>(result))
                << (result ? "" : result.error().message);
            if (result) {
                expect(result->timed_out);
                expect(result->exit_code == 128 + 9);
            }
            expect(elapsed < std::chrono::seconds{4});
        };

    "platform/subprocess: a timeout kills the whole process group"_test = [] {
        if (on_windows()) {
            // The POSIX backend is the one that starts the child in its own
            // process group and drains it; Win32 terminates only the child.
            return;
        }
        const test_support::scratch_directory directory{};
        const auto script{directory.path() / "wrapper.sh"};
        const auto late_file{directory.path() / "late.txt"};
        const auto pid_file{directory.path() / "grandchild.pid"};
        {
            // The grandchild writes after the timeout would have expired: if
            // the group was not killed it keeps running and the write appears.
            std::ofstream wrapper{script};
            wrapper << "#!/bin/sh\n"
                    << "( sleep 1; printf late > \"$1\" ) &\n"
                    << "echo $! > \"$2\"\n"
                    << "wait\n";
            expect(static_cast<bool>(wrapper));
        }
        std::error_code error;
        fs::permissions(script,
                        fs::perms::owner_read | fs::perms::owner_write |
                            fs::perms::owner_exec,
                        fs::perm_options::add, error);
        expect(!error);
        const auto started{std::chrono::steady_clock::now()};
        const auto result{platform::run_process(platform::subprocess_options{
            .program = script.string(),
            .arguments = {late_file.string(), pid_file.string()},
            .timeout = std::chrono::milliseconds{300},
        })};
        const auto elapsed{std::chrono::steady_clock::now() - started};
        expect(static_cast<bool>(result))
            << (result ? "" : result.error().message);
        if (!result) {
            return;
        }
        expect(result->timed_out);
        unsigned long long grandchild{0};
        {
            std::ifstream input{pid_file};
            input >> grandchild;
        }
        expect(grandchild > 0);
        // Past the grandchild's write deadline: the group kill must have
        // stopped it before it could touch the file.
        std::this_thread::sleep_for(std::chrono::milliseconds{1500});
        expect(!fs::exists(late_file))
            << "the grandchild outlived the timeout and wrote after it";
        // The host may keep the killed grandchild as an unreaped zombie until
        // PID 1 reaps it; both a missing process and a zombie mean it is no
        // longer running.
        const auto is_zombie = [](unsigned long long pid) {
            std::ifstream stat{"/proc/" + std::to_string(pid) + "/stat"};
            std::string line;
            std::getline(stat, line);
            if (!stat) {
                return false;
            }
            const auto close{line.rfind(')')};
            if (close == std::string::npos || close + 2 >= line.size()) {
                return false;
            }
            return line[close + 2] == 'Z';
        };
        bool stopped{false};
        for (int attempt{0}; attempt < 500 && !stopped; ++attempt) {
            stopped =
                !platform::process_alive(grandchild) || is_zombie(grandchild);
            if (!stopped) {
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            }
        }
        expect(stopped) << "the grandchild outlived the timeout";
        expect(elapsed < std::chrono::seconds{4});
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

#if defined(_WIN32)
    "platform/subprocess: a quoted argument round-trips through "
    "CommandLineToArgvW"_test = [] {
        // The cases the escape-only quoting got wrong: a trailing backslash
        // swallows the closing quote, a quote after backslashes needs the
        // odd/even rule, and empty or whitespace arguments need delimiters.
        const std::array<std::string, 18> arguments{{
            "",
            "plain",
            "with space",
            "with\ttab",
            "trailing\\",
            "C:\\out dir\\",
            "C:\\out dir\\\\",
            "\\",
            "\\\\",
            "\"",
            "embedded\"quote",
            "a\\\"b",
            "two \"quoted\" words",
            "back\\slash\\",
            "\\\\server\\share\\dir\\",
            "trailing space ",
            " leading space",
            "quote\\\"at end",
        }};
        for (const auto &argument : arguments) {
            const auto line{probe_line(argument)};
            const auto split{split_command_line(line)};
            expect(split.size() == 2U)
                << ("not one argument after the program: " + line);
            if (split.size() == 2U) {
                expect(split[1] == argument) << line;
            }
        }
        // Argument boundaries survive: the child receives three arguments
        // rather than a merged tail.
        std::string line{"probe"};
        const std::array<std::string, 3> many{{
            "C:\\out dir\\",
            "a\"b",
            "",
        }};
        for (const auto &argument : many) {
            line += ' ';
            line += platform::windows_detail::quote_argument(argument);
        }
        const auto split{split_command_line(line)};
        expect(split.size() == many.size() + 1);
        if (split.size() == many.size() + 1) {
            for (std::size_t index{0}; index < many.size(); ++index) {
                expect(split[index + 1] == many[index]);
            }
        }
    };
#endif
};
