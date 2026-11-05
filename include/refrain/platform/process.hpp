// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <cstdint>
#include <string_view>

namespace refrain::platform {

// Process-level primitives: identity, fatal-fault capture and crash markers.
//
// This is the only layer allowed to call signal/exception/fd APIs
// (`sigaction`, `raise`, `SetUnhandledExceptionFilter`, `open`/`write`, ...).
// Kernel code (e.g. the crash reporter) depends on these abstractions only.

enum class fault_kind : unsigned char {
    segmentation = 0,
    abort,
    floating_point,
    illegal_instruction,
    bus,
    trap,
    unknown,
};

struct fault_info {
    fault_kind kind{fault_kind::unknown};
    /// Raw native signal/exception code (POSIX signal number; Windows
    /// NTSTATUS). Diagnostics only: never branch on it portably.
    int native_code{0};
};

/// Fatal-fault callback. **Must be async-signal-safe**: no allocation, no
/// locks, no logging, only plain writes/atomics. After the callback returns the
/// platform re-executes the fault under the restored default disposition (POSIX
/// `SA_RESETHAND`), so the process terminates *by signal* and still produces a
/// core dump / Crashpad minidump.
using fault_callback = void (*)(const fault_info &info, void *context) noexcept;

/// Install the callback for fatal faults (SIGSEGV/SIGABRT/SIGFPE/SIGILL/SIGBUS/
/// SIGTRAP on POSIX; unhandled SEH exceptions on Windows). On Windows this is
/// the fallback when Crashpad is not installed and chains to the previous
/// top-level filter, so Crashpad still receives the fault.
[[nodiscard]] auto install_fault_handler(fault_callback callback,
                                         void *context) noexcept -> status;

/// Restore the previous fault disposition (idempotent).
void restore_fault_handlers() noexcept;

/// Async-signal-safe: append one human-readable crash line to `path`
/// (`refrain crash pid=<n> signal=<n>`). `path` must be null-terminated and
/// stable for the lifetime of the handler (callers keep a static buffer).
/// Returns false when the marker is unavailable for this platform or the fault
/// is not one of the armed ones. Windows writes the same line through a
/// stack buffer + `WriteFile` in the unhandled-exception filter.
[[nodiscard]] auto write_fault_marker(const char *path,
                                      const fault_info &info) noexcept -> bool;

/// Native signal/exception code for a portable fault kind (diagnostics/tests;
/// 0 when the platform has no mapping).
[[nodiscard]] auto native_signal_for(fault_kind kind) noexcept -> int;

[[nodiscard]] auto process_id() noexcept -> std::uint64_t;

/// Best-effort liveness probe for another process id (stale-lock detection).
///
/// POSIX: `kill(pid, 0)` — false when the process does not exist (ESRCH), true
/// when it exists or exists but is owned by another user (EPERM). Windows:
/// `OpenProcess` + `GetExitCodeProcess`. The result is advisory only: it is
/// never a security decision, and a caller that cannot tell treats the lock as
/// live (the owner mtime window is the portable fallback).
[[nodiscard]] auto process_alive(std::uint64_t pid) noexcept -> bool;

[[noreturn]] void abort_process() noexcept;

/// Cause a genuine segmentation fault to exercise the installed handler
/// (CI crash smoke). Deliberately undefined behaviour, kept inside the platform
/// layer; UBSan builds are expected to flag it.
[[noreturn]] void trigger_segmentation_fault() noexcept;

} // namespace refrain::platform
