// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/platform/subprocess.hpp>

#if !defined(_WIN32)

#include <fmt/format.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <pthread.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// The spawn call hands the child environment to the new image.
extern char **environ;

namespace refrain::platform {
namespace {

[[nodiscard]] auto io_failure(std::string_view what) -> error {
    const std::error_code code{errno, std::generic_category()};
    return make_error(error_code::io_error,
                      fmt::format("{}: {}", what, code.message()));
}

/// A `posix_spawn` error is the error number itself, not `errno`.
[[nodiscard]] auto spawn_failure(int code, std::string_view what) -> error {
    const std::error_code failure{code, std::generic_category()};
    return make_error(error_code::io_error,
                      fmt::format("{}: {}", what, failure.message()));
}

/// Block `SIGPIPE` for the writing thread for the lifetime of the guard.
///
/// A child that exits before reading its stdin closes the read end; the next
/// parent `write()` then raises `SIGPIPE`, whose default action terminates the
/// whole process instead of returning `EPIPE`. Blocking the signal in this
/// thread turns the write into an ordinary `EPIPE`; the signal left pending is
/// consumed before the previous mask is restored, because restoring it first
/// would deliver the signal and kill the process anyway. The mask is
/// thread-scoped, so a concurrent write on another thread keeps the default
/// disposition.
class sigpipe_guard {
  public:
    sigpipe_guard() noexcept {
        sigemptyset(&blocked_);
        sigaddset(&blocked_, SIGPIPE);
        active_ = ::pthread_sigmask(SIG_BLOCK, &blocked_, &previous_) == 0;
    }

    ~sigpipe_guard() {
        if (!active_) {
            return;
        }
        sigset_t pending{};
        if (::sigpending(&pending) == 0 &&
            ::sigismember(&pending, SIGPIPE) == 1) {
            int received{0};
            static_cast<void>(::sigwait(&blocked_, &received));
        }
        static_cast<void>(::pthread_sigmask(SIG_SETMASK, &previous_, nullptr));
    }

    sigpipe_guard(const sigpipe_guard &) = delete;
    auto operator=(const sigpipe_guard &) -> sigpipe_guard & = delete;

  private:
    sigset_t blocked_{};
    sigset_t previous_{};
    bool active_{false};
};

/// Parent-side pipe ends. The destructor closes whatever is still open, so an
/// early return cannot leak a descriptor.
class input_pipe {
  public:
    input_pipe() = default;
    ~input_pipe() { close(); }

    input_pipe(const input_pipe &) = delete;
    auto operator=(const input_pipe &) -> input_pipe & = delete;

    [[nodiscard]] auto open() -> bool { return ::pipe(fds_) == 0; }

    [[nodiscard]] auto read_fd() const noexcept -> int { return fds_[0]; }
    [[nodiscard]] auto write_fd() const noexcept -> int { return fds_[1]; }

    void close_read() noexcept { close_one(fds_[0]); }
    void close_write() noexcept { close_one(fds_[1]); }

    void close() noexcept {
        close_read();
        close_write();
    }

  private:
    static void close_one(int &fd) noexcept {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }

    int fds_[2]{-1, -1};
};

} // namespace

auto run_process(const subprocess_options &options)
    -> result<subprocess_result> {
    if (options.program.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument, "program is empty")};
    }
    input_pipe input{};
    if (!input.open()) {
        return std::unexpected{io_failure("cannot create the stdin pipe")};
    }

    // Everything the child needs is built here, in the parent: the argument
    // vector and the file actions. The spawn then only performs the calls the
    // child half of a multithreaded process is allowed to make, so no
    // allocation or stdio state is touched after the fork.
    std::vector<char *> argv{};
    argv.reserve(options.arguments.size() + 2);
    argv.push_back(const_cast<char *>(options.program.c_str()));
    for (const auto &argument : options.arguments) {
        argv.push_back(const_cast<char *>(argument.c_str()));
    }
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions{};
    if (const int created{::posix_spawn_file_actions_init(&actions)};
        created != 0) {
        return std::unexpected{
            spawn_failure(created, "cannot initialise the spawn file actions")};
    }
    struct actions_guard {
        posix_spawn_file_actions_t *value;
        ~actions_guard() { ::posix_spawn_file_actions_destroy(value); }
    } release_actions{&actions};

    if (const int wired{::posix_spawn_file_actions_adddup2(
            &actions, input.read_fd(), STDIN_FILENO)};
        wired != 0) {
        return std::unexpected{
            spawn_failure(wired, "cannot wire the stdin pipe")};
    }
    // The child must not inherit either end of the pipe: the dup'd descriptor
    // is the child's stdin, and the write end would keep it open forever.
    if (const int closed{
            ::posix_spawn_file_actions_addclose(&actions, input.read_fd())};
        closed != 0) {
        return std::unexpected{
            spawn_failure(closed, "cannot close the stdin pipe")};
    }
    if (const int closed{
            ::posix_spawn_file_actions_addclose(&actions, input.write_fd())};
        closed != 0) {
        return std::unexpected{
            spawn_failure(closed, "cannot close the stdin pipe")};
    }
    if (!options.working_directory.empty()) {
#if defined(__GLIBC__) || defined(__APPLE__)
        const int changed{::posix_spawn_file_actions_addchdir_np(
            &actions, options.working_directory.c_str())};
        if (changed != 0) {
            return std::unexpected{spawn_failure(
                changed, fmt::format("cannot use working directory '{}'",
                                     options.working_directory))};
        }
#else
        return std::unexpected{make_error(
            error_code::unsupported,
            fmt::format("cannot start '{}' in '{}': the platform has no "
                        "spawn file action to change the working directory",
                        options.program, options.working_directory))};
#endif
    }

    pid_t pid{0};
    const int spawned{::posix_spawnp(&pid, options.program.c_str(), &actions,
                                     nullptr, argv.data(), environ)};
    // The file actions have been consumed: the parent's read end is no longer
    // needed, and the child owns its stdin through the dup.
    input.close_read();
    if (spawned != 0) {
        return std::unexpected{spawn_failure(
            spawned, fmt::format("cannot start '{}'", options.program))};
    }

    // Feed the child's stdin, then close it so a child that reads until EOF
    // terminates. A failure here is not fatal: the child reports what it made
    // of a short input. The write can hit a child that already exited (a
    // command that never reads its stdin); the guard keeps that from becoming
    // a process-wide `SIGPIPE` death.
    std::size_t written{0};
    {
        const sigpipe_guard guard{};
        while (written < options.stdin_text.size()) {
            const auto result{::write(input.write_fd(),
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
    }
    input.close();

    int status{0};
    pid_t waited{-1};
    bool timed_out{false};
    if (options.timeout.count() > 0) {
        // Poll so the wait remains interruptible: a bounded child is killed
        // and reaped rather than left running behind a blocking wait.
        const auto deadline{std::chrono::steady_clock::now() + options.timeout};
        constexpr auto poll_interval{std::chrono::milliseconds{10}};
        for (;;) {
            waited = ::waitpid(pid, &status, WNOHANG);
            if (waited < 0 && errno == EINTR) {
                continue;
            }
            if (waited != 0) {
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                timed_out = true;
                static_cast<void>(::kill(pid, SIGKILL));
                do {
                    waited = ::waitpid(pid, &status, 0);
                } while (waited < 0 && errno == EINTR);
                break;
            }
            std::this_thread::sleep_for(poll_interval);
        }
    } else {
        do {
            waited = ::waitpid(pid, &status, 0);
        } while (waited < 0 && errno == EINTR);
    }
    if (waited < 0) {
        return std::unexpected{io_failure("cannot wait for the child")};
    }
    if (timed_out) {
        return subprocess_result{.exit_code = 128 + SIGKILL, .timed_out = true};
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
