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

/// Directory for the platform-native dump written by the fallback crash
/// handler when no out-of-process reporter is attached.
///
/// Windows: the unhandled-exception filter writes one `.dmp` minidump per fatal
/// fault into `utf8_directory` through dbghelp (loaded at configuration time),
/// in addition to whatever the marker callback writes. The directory must
/// already exist; a path that is empty or does not fit the fixed static buffer
/// disables the minidump fallback and returns false.
///
/// POSIX: no native minidump writer exists, so the function records nothing and
/// returns false; the async-signal-safe marker callback remains the fallback.
///
/// The call is made during startup, never from the fault handler itself, so
/// it may allocate internally; the fault handler only reads the prepared state.
[[nodiscard]] auto
set_fallback_dump_directory(std::string_view utf8_directory) noexcept -> bool;

/// Native signal/exception code for a portable fault kind (diagnostics/tests;
/// 0 when the platform has no mapping).
[[nodiscard]] auto native_signal_for(fault_kind kind) noexcept -> int;

[[nodiscard]] auto process_id() noexcept -> std::uint64_t;

/// Set and clear an environment variable for the current process.
///
/// The platform layer owns the environment API so callers never branch on the
/// operating system. Names and values are UTF-8; an empty value sets an empty
/// variable (use `unset_environment_variable` to remove it). A failure returns
/// `io_error` with the operating-system message.
[[nodiscard]] auto set_environment_variable(std::string_view name,
                                            std::string_view value) -> status;
[[nodiscard]] auto unset_environment_variable(std::string_view name) -> status;

/// Absolute path of the running executable, as a UTF-8 string.
///
/// The path is resolved by the operating system, not from `argv[0]`: a
/// launcher may pass a bare name, a relative path or a symlink. First-run
/// discovery uses it to find the composition and the installed game data next
/// to the binary, so a server started from an arbitrary working directory keeps
/// working. Fails with `io_error` when the OS cannot report the path and with
/// `unsupported` on a POSIX host without `/proc`.
[[nodiscard]] auto executable_path() -> result<std::string>;

/// Best-effort liveness probe for another process id (stale-lock detection).
///
/// POSIX: `kill(pid, 0)` — false when the process does not exist (ESRCH), true
/// when it exists or exists but is owned by another user (EPERM). Windows:
/// `OpenProcess` + `GetExitCodeProcess` — false only when the pid names no
/// process; a process the caller may not open, such as one owned by another
/// account or a protected process, counts as alive. The result is advisory
/// only and never a security decision. A caller that has no usable pid at all
/// falls back to the age of the owner record instead of this probe.
[[nodiscard]] auto process_alive(std::uint64_t pid) noexcept -> bool;

[[noreturn]] void abort_process() noexcept;

/// Cause a genuine segmentation fault to exercise the installed handler
/// (CI crash smoke). Deliberately undefined behaviour, kept inside the platform
/// layer; UBSan builds are expected to flag it.
[[noreturn]] void trigger_segmentation_fault() noexcept;

} // namespace refrain::platform
