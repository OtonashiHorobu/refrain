// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/platform/thread.hpp>

#if !defined(_WIN32)

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/types.h>

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace refrain::platform {
namespace {

/// `TASK_COMM_LEN` is 16 on Linux, including the NUL terminator. Other POSIX
/// platforms use longer limits, but the project only ships Linux/Windows; a
/// conservative 16-byte buffer keeps names identical across debuggers.
constexpr std::size_t max_thread_name_bytes = 15;

} // namespace

auto set_current_thread_name(std::string_view name) noexcept -> bool {
    if (name.empty()) {
        return false;
    }
    std::array<char, max_thread_name_bytes + 1> buffer{};
    const auto length{std::min(name.size(), max_thread_name_bytes)};
    std::copy_n(name.data(), length, buffer.data());
#if defined(__APPLE__)
    return ::pthread_setname_np(buffer.data()) == 0;
#else
    return ::pthread_setname_np(::pthread_self(), buffer.data()) == 0;
#endif
}

auto set_current_thread_priority(thread_priority priority) noexcept -> bool {
    int nice{0};
    switch (priority) {
    case thread_priority::background:
        nice = 10;
        break;
    case thread_priority::normal:
        nice = 0;
        break;
    case thread_priority::high:
        nice = -5;
        break;
    }
#if defined(__linux__)
    const auto tid{static_cast<id_t>(::syscall(SYS_gettid))};
#else
    const auto tid{static_cast<id_t>(::getpid())};
#endif
    return ::setpriority(PRIO_PROCESS, tid, nice) == 0;
}

auto set_current_thread_affinity(std::span<const std::uint32_t> cpus) noexcept
    -> bool {
    if (cpus.empty()) {
        return false;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    for (const auto cpu : cpus) {
        if (cpu >= CPU_SETSIZE) {
            return false;
        }
        CPU_SET(static_cast<int>(cpu), &set);
    }
    return ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set) == 0;
}

auto current_thread_id() noexcept -> std::uint64_t {
#if defined(__linux__)
    // Mirrors spdlog's `%t` on Linux: the kernel TID (LWP), not `pthread_t`.
    return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#else
    // On macOS spdlog uses `pthread_mach_thread_np`; this fallback is only for
    // POSIX builds that neither the project nor the log registry target, and
    // deliberately keeps the value non-zero for diagnostics.
    return static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(::pthread_self()));
#endif
}

} // namespace refrain::platform

#endif // !defined(_WIN32)
