// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace refrain::platform {

// Thread-level primitives: the OS-visible name and the native thread id.
//
// These are the only places allowed to call `pthread_setname_np` /
// `SetThreadDescription` / `gettid`; kernel code depends on this abstraction so
// `refrain::log` and the worker pools stay platform-neutral.
//
// The native id is the same value spdlog prints for `%t` on the supported
// platforms (Linux `gettid`, Windows `GetCurrentThreadId`), which is what makes
// the log thread-name registry keyed by `msg.thread_id` work in synchronous and
// asynchronous modes.

/// Best-effort name for the **calling** thread.
///
/// Linux exposes at most 15 bytes (`TASK_COMM_LEN` minus the terminator); a
/// longer name is truncated by the kernel. Windows truncates nothing and uses
/// UTF-16. Returns false when the platform has no implementation or the call
/// fails; callers must not treat this as fatal (debugger/`top -H` visibility
/// is a diagnostic aid, not a correctness requirement).
[[nodiscard]] auto set_current_thread_name(std::string_view name) noexcept
    -> bool;

/// Native id of the calling thread. Never zero on a running process.
[[nodiscard]] auto current_thread_id() noexcept -> std::uint64_t;

/// Relative scheduling class for background/normal/interactive work.
enum class thread_priority : std::uint8_t {
    background = 0,
    normal = 1,
    high = 2,
};

/// Best-effort priority for the calling thread. Linux maps to per-thread nice
/// values (+10/0/-5; negative nice needs privilege); Windows maps to
/// THREAD_PRIORITY_BELOW_NORMAL/NORMAL/ABOVE_NORMAL. Failure is only a hint.
[[nodiscard]] auto
set_current_thread_priority(thread_priority priority) noexcept -> bool;

/// Pin the calling thread to the given logical CPUs (best effort).
///
/// An empty list, an out-of-range CPU or a platform restriction returns false;
/// callers must treat affinity as an optional scheduling hint, never as a
/// correctness requirement. Linux uses `pthread_setaffinity_np`; Windows uses
/// `SetThreadAffinityMask` (up to 64 CPUs in the process group).
[[nodiscard]] auto
set_current_thread_affinity(std::span<const std::uint32_t> cpus) noexcept
    -> bool;

} // namespace refrain::platform
