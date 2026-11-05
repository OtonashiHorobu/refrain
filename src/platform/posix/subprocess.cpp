// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/platform/subprocess.hpp>

#if !defined(_WIN32)

#include <fmt/format.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace refrain::platform {
namespace {

[[nodiscard]] auto io_failure(std::string_view what) -> error {
    const std::error_code code{errno, std::generic_category()};
    return make_error(error_code::io_error,
                      fmt::format("{}: {}", what, code.message()));
}

} // namespace

auto run_process(const subprocess_options &options)
    -> result<subprocess_result> {
    if (options.program.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument, "program is empty")};
    }
    int input_pipe[2]{-1, -1};
    if (::pipe(input_pipe) != 0) {
        return std::unexpected{io_failure("cannot create the stdin pipe")};
    }
    const pid_t pid{::fork()};
    if (pid < 0) {
        ::close(input_pipe[0]);
        ::close(input_pipe[1]);
        return std::unexpected{io_failure("cannot fork")};
    }
    if (pid == 0) {
        // Child: wire stdin, change directory, exec. Only async-signal-safe
        // calls from here on; exec failure is reported as exit code 127.
        if (::dup2(input_pipe[0], STDIN_FILENO) < 0) {
            ::_exit(127);
        }
        ::close(input_pipe[0]);
        ::close(input_pipe[1]);
        if (!options.working_directory.empty() &&
            ::chdir(options.working_directory.c_str()) != 0) {
            ::_exit(127);
        }
        std::vector<char *> argv;
        argv.reserve(options.arguments.size() + 2);
        argv.push_back(const_cast<char *>(options.program.c_str()));
        for (const auto &argument : options.arguments) {
            argv.push_back(const_cast<char *>(argument.c_str()));
        }
        argv.push_back(nullptr);
        ::execvp(options.program.c_str(), argv.data());
        ::_exit(127);
    }
    ::close(input_pipe[0]);
    std::size_t written{0};
    while (written < options.stdin_text.size()) {
        const auto result{::write(input_pipe[1],
                                  options.stdin_text.data() + written,
                                  options.stdin_text.size() - written)};
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        written += static_cast<std::size_t>(result);
    }
    ::close(input_pipe[1]);
    int status{0};
    pid_t waited{-1};
    do {
        waited = ::waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        return std::unexpected{io_failure("cannot wait for the child")};
    }
    if (WIFEXITED(status)) {
        return subprocess_result{.exit_code = WEXITSTATUS(status)};
    }
    if (WIFSIGNALED(status)) {
        return subprocess_result{.exit_code = 128 + WTERMSIG(status)};
    }
    return subprocess_result{.exit_code = 1};
}

} // namespace refrain::platform

#endif // !defined(_WIN32)
