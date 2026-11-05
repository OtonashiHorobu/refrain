// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/platform/process.hpp>

#if defined(_WIN32)

#include <refrain/core/error.hpp>

#include <windows.h>

#include <atomic>
#include <charconv>
#include <cstdlib>
#include <limits>
#include <string>

namespace refrain::platform {

namespace {

fault_callback g_callback{nullptr};
void *g_context{nullptr};
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter{nullptr};
std::atomic<bool> g_installed{false};

[[nodiscard]] auto map_exception(DWORD code) noexcept -> fault_kind {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_GUARD_PAGE:
    case EXCEPTION_STACK_OVERFLOW:
        return fault_kind::segmentation;
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_INVALID_OPERATION:
    case EXCEPTION_FLT_OVERFLOW:
    case EXCEPTION_FLT_UNDERFLOW:
    case EXCEPTION_FLT_STACK_CHECK:
    case EXCEPTION_FLT_DENORMAL_OPERAND:
        return fault_kind::floating_point;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
        return fault_kind::illegal_instruction;
    case EXCEPTION_BREAKPOINT:
    case EXCEPTION_SINGLE_STEP:
        return fault_kind::trap;
    default:
        return fault_kind::unknown;
    }
}

LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS *info) noexcept {
    if (g_callback != nullptr && info != nullptr &&
        info->ExceptionRecord != nullptr) {
        const fault_info failure{
            .kind = map_exception(info->ExceptionRecord->ExceptionCode),
            .native_code =
                static_cast<int>(info->ExceptionRecord->ExceptionCode)};
        g_callback(failure, g_context);
    }
    // The fallback must not swallow the fault: return to the previous filter
    // (Crashpad's, when installed) or let Windows terminate the process. This
    // is the Windows equivalent of the POSIX SA_RESETHAND re-raise.
    if (g_previous_filter != nullptr) {
        return g_previous_filter(info);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

auto install_fault_handler(fault_callback callback, void *context) noexcept
    -> status {
    if (callback == nullptr) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "fault callback must not be null")};
    }
    g_callback = callback;
    g_context = context;
    if (!g_installed.exchange(true, std::memory_order_acq_rel)) {
        g_previous_filter =
            ::SetUnhandledExceptionFilter(&unhandled_exception_filter);
    }
    return status_ok();
}

void restore_fault_handlers() noexcept {
    if (g_installed.exchange(false, std::memory_order_acq_rel)) {
        ::SetUnhandledExceptionFilter(g_previous_filter);
    }
    g_previous_filter = nullptr;
    g_callback = nullptr;
    g_context = nullptr;
}

auto write_fault_marker(const char *path, const fault_info &info) noexcept
    -> bool {
    // The contract is async-signal-safe: no allocation, no CRT formatting. The
    // fixed-size stack buffer is written with CreateFileA/WriteFile, which are
    // safe enough in the Windows unhandled-exception context (there are no
    // POSIX signal restrictions; the process is already terminating).
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    char buffer[128]{};
    auto *cursor{buffer};
    const auto append = [&cursor](std::string_view text) {
        for (const char character : text) {
            *cursor++ = character;
        }
    };
    append("refrain crash pid=");
    auto pid_result{std::to_chars(cursor, buffer + sizeof(buffer) - 1,
                                  ::GetCurrentProcessId())};
    if (pid_result.ec != std::errc{}) {
        return false;
    }
    cursor = pid_result.ptr;
    append(" signal=");
    auto signal_result{
        std::to_chars(cursor, buffer + sizeof(buffer) - 1, info.native_code)};
    if (signal_result.ec != std::errc{}) {
        return false;
    }
    cursor = signal_result.ptr;
    append("\n");
    const auto length{static_cast<DWORD>(cursor - buffer)};

    HANDLE file{::CreateFileA(path, FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written{0};
    const bool ok{::WriteFile(file, buffer, length, &written, nullptr) != 0 &&
                  written == length};
    ::CloseHandle(file);
    return ok;
}

auto native_signal_for(fault_kind kind) noexcept -> int {
    switch (kind) {
    case fault_kind::segmentation:
        return static_cast<int>(EXCEPTION_ACCESS_VIOLATION);
    case fault_kind::abort:
        return static_cast<int>(EXCEPTION_BREAKPOINT);
    case fault_kind::floating_point:
        return static_cast<int>(EXCEPTION_FLT_DIVIDE_BY_ZERO);
    case fault_kind::illegal_instruction:
        return static_cast<int>(EXCEPTION_ILLEGAL_INSTRUCTION);
    case fault_kind::bus:
        return static_cast<int>(EXCEPTION_IN_PAGE_ERROR);
    case fault_kind::trap:
        return static_cast<int>(EXCEPTION_SINGLE_STEP);
    case fault_kind::unknown:
        return 0;
    }
    return 0;
}

auto process_id() noexcept -> std::uint64_t {
    return static_cast<std::uint64_t>(::GetCurrentProcessId());
}

auto process_alive(std::uint64_t pid) noexcept -> bool {
    // PROCESS_QUERY_LIMITED_INFORMATION works for processes of other users on
    // Vista+; a null handle means "no such process" (or no access, in which
    // case the caller's mtime fallback still applies).
    if (pid == 0U ||
        pid > static_cast<std::uint64_t>(std::numeric_limits<DWORD>::max())) {
        return false;
    }
    HANDLE handle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                  static_cast<DWORD>(pid));
    if (handle == nullptr) {
        return false;
    }
    DWORD exit_code{0};
    const bool alive{::GetExitCodeProcess(handle, &exit_code) != 0 &&
                     exit_code == STILL_ACTIVE};
    ::CloseHandle(handle);
    return alive;
}

void abort_process() noexcept { std::abort(); }

void trigger_segmentation_fault() noexcept {
    // Raise a real access violation so the unhandled-exception filter (or
    // Crashpad) sees the same path as a genuine fault. Non-continuable: the
    // process must terminate if no filter handles it.
    ::RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0,
                     nullptr);
    std::abort();
}

} // namespace refrain::platform

#else

// POSIX implementation: see src/platform/posix/process.cpp.

#endif // _WIN32
