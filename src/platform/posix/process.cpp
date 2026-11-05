// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/platform/process.hpp>

#if !defined(_WIN32)

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

namespace refrain::platform {
namespace {

constexpr std::array<int, 6> fallback_signals{SIGSEGV, SIGABRT, SIGFPE,
                                              SIGILL,  SIGBUS,  SIGTRAP};

/// One crash-marker line, formatted at install time (normal execution context)
/// so the signal handler itself performs no formatting at all — it only does a
/// raw `write()` of immutable bytes. This keeps the handler async-signal-safe
/// while still using std::format instead of a hand-rolled integer formatter.
struct marker_line {
    int native_code{0};
    std::size_t size{0};
    std::array<char, 192> bytes{};
};

struct handler_state {
    fault_callback callback{nullptr};
    void *context{nullptr};
    std::array<struct sigaction, fallback_signals.size()> previous{};
    /// PID is captured at install time; it cannot change while the handler is
    /// armed, so the current process id is baked into the preformatted lines.
    std::array<marker_line, fallback_signals.size()> marker_lines{};
    std::size_t installed{0};
};

handler_state &state() noexcept {
    static handler_state instance{};
    return instance;
}

[[nodiscard]] auto to_kind(int signal_number) noexcept -> fault_kind {
    switch (signal_number) {
    case SIGSEGV:
        return fault_kind::segmentation;
    case SIGABRT:
        return fault_kind::abort;
    case SIGFPE:
        return fault_kind::floating_point;
    case SIGILL:
        return fault_kind::illegal_instruction;
    case SIGBUS:
        return fault_kind::bus;
    case SIGTRAP:
        return fault_kind::trap;
    default:
        return fault_kind::unknown;
    }
}

/// Format all marker lines up-front with std::format. Allocation and formatting
/// happen here, never inside the signal handler. OOM (or any format failure)
/// degrades to "marker unavailable" instead of calling std::terminate from a
/// noexcept install path.
void build_marker_lines(handler_state &current) noexcept {
    const auto pid = static_cast<unsigned long>(::getpid());
    for (std::size_t index = 0; index < fallback_signals.size(); ++index) {
        auto &line = current.marker_lines[index];
        line.native_code = fallback_signals[index];
        line.size = 0;
        try {
            const std::string text =
                std::format("refrain crash pid={} signal={}\n", pid,
                            fallback_signals[index]);
            line.size = text.size() < line.bytes.size() ? text.size()
                                                        : line.bytes.size();
            std::copy_n(text.begin(), static_cast<std::ptrdiff_t>(line.size),
                        line.bytes.begin());
        } catch (...) {
            line.size = 0; // marker disabled, crash handling continues
        }
    }
}

extern "C" void fault_trampoline(int signal_number, siginfo_t * /*info*/,
                                 void * /*context*/) {
    auto &current = state();
    const fault_info info{to_kind(signal_number), signal_number};
    if (current.callback != nullptr) {
        current.callback(info, current.context);
    }
    // SA_RESETHAND already restored the default disposition before this handler
    // ran. Returning (instead of raise()+_exit(), which exited normally while
    // the signal was still blocked) lets the faulting instruction re-execute
    // under the default disposition, so the process dies *by signal* and the
    // platform still produces a core dump / minidump.
    //
    // For asynchronous signals the re-raise is unnecessary but harmless: the
    // default disposition is in force again, so a subsequent occurrence of the
    // same fault terminates the process.
}

} // namespace

namespace {

/// Body of `install_fault_handler()`. Deliberately not `noexcept`: the
/// `sigaction` failure path builds a contextual message (`std::string` +
/// `std::error_code::message()`), which allocates. The public entry point is
/// the exception barrier that preserves the platform layer's "returns status,
/// never throws" contract.
[[nodiscard]] auto install_fault_handler_impl(fault_callback callback,
                                              void *context) -> status {
    if (callback == nullptr) {
        return std::unexpected{
            make_error(error_code::invalid_argument, "null fault callback")};
    }
    auto &current = state();
    if (current.installed != 0) {
        current.callback = callback;
        current.context = context;
        return status_ok();
    }

    build_marker_lines(current);

    struct sigaction action{};
    action.sa_sigaction = &fault_trampoline;
    action.sa_flags = SA_SIGINFO | SA_RESETHAND;
    (void)::sigemptyset(&action.sa_mask);

    std::size_t index{0};
    for (; index < fallback_signals.size(); ++index) {
        if (::sigaction(fallback_signals[index], &action,
                        &current.previous[index]) != 0) {
            for (std::size_t rollback = 0; rollback < index; ++rollback) {
                (void)::sigaction(fallback_signals[rollback],
                                  &current.previous[rollback], nullptr);
            }
            return std::unexpected{make_error(
                error_code::mod_error,
                std::string{"sigaction failed: "} +
                    std::error_code{errno, std::generic_category()}.message())};
        }
    }
    current.installed = index;
    current.callback = callback;
    current.context = context;
    return status_ok();
}

} // namespace

auto install_fault_handler(fault_callback callback, void *context) noexcept
    -> status {
    // Exception barrier: keep the platform contract ("no exception escapes")
    // literal even when the OOM hits the error message itself.
    try {
        return install_fault_handler_impl(callback, context);
    } catch (const std::bad_alloc &) {
        return std::unexpected{
            make_error(error_code::out_of_memory, "out of memory")};
    } catch (...) {
        return std::unexpected{
            make_error(error_code::unknown, "unknown error")};
    }
}

void restore_fault_handlers() noexcept {
    auto &current = state();
    for (std::size_t index = 0; index < current.installed; ++index) {
        (void)::sigaction(fallback_signals[index], &current.previous[index],
                          nullptr);
    }
    current.installed = 0;
    current.callback = nullptr;
    current.context = nullptr;
}

auto write_fault_marker(const char *path, const fault_info &info) noexcept
    -> bool {
    if (path == nullptr || *path == '\0') {
        return false;
    }
    const auto &current = state();
    const marker_line *selected = nullptr;
    for (const auto &line : current.marker_lines) {
        if (line.native_code == info.native_code && line.size > 0) {
            selected = &line;
            break;
        }
    }
    if (selected == nullptr) {
        return false; // not one of the armed fatal faults
    }

    int descriptor{-1};
    while ((descriptor = ::open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                                0600)) < 0) {
        if (errno != EINTR) {
            return false; // retry EINTR like the write loop below
        }
    }
    std::size_t written{0};
    while (written < selected->size) {
        const auto chunk = ::write(descriptor, selected->bytes.data() + written,
                                   selected->size - written);
        if (chunk <= 0) {
            if (chunk < 0 && errno == EINTR) {
                continue;
            }
            (void)::close(descriptor);
            return false;
        }
        written += static_cast<std::size_t>(chunk);
    }
    (void)::close(descriptor);
    return true;
}

auto native_signal_for(fault_kind kind) noexcept -> int {
    switch (kind) {
    case fault_kind::segmentation:
        return SIGSEGV;
    case fault_kind::abort:
        return SIGABRT;
    case fault_kind::floating_point:
        return SIGFPE;
    case fault_kind::illegal_instruction:
        return SIGILL;
    case fault_kind::bus:
        return SIGBUS;
    case fault_kind::trap:
        return SIGTRAP;
    case fault_kind::unknown:
        return 0;
    }
    return 0;
}

auto process_id() noexcept -> std::uint64_t {
    return static_cast<std::uint64_t>(::getpid());
}

auto process_alive(std::uint64_t pid) noexcept -> bool {
    // kill(pid, 0) performs the permission/existence check without delivering
    // a signal. ESRCH means "no such process"; EPERM means it exists but is
    // owned by another user, which still counts as alive for a stale lock.
    if (pid == 0U ||
        pid > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) {
        return false;
    }
    if (::kill(static_cast<pid_t>(pid), 0) == 0) {
        return true;
    }
    return errno == EPERM;
}

void abort_process() noexcept { std::abort(); }

void trigger_segmentation_fault() noexcept {
    // Genuine null dereference inside the platform layer: the compiler is not
    // allowed to optimize it away, and any installed fault handler sees a real
    // SIGSEGV (which Crashpad needs for a faithful minidump).
    volatile int *invalid_pointer = nullptr;
    *invalid_pointer = 0x5a; // NOLINT
    std::abort();            // defensive: unreachable unless SIGSEGV is blocked
}

} // namespace refrain::platform

#else

// Windows implementation: see src/platform/windows/process.cpp.

#endif // !_WIN32
