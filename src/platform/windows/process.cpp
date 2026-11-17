// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/platform/process.hpp>

#if defined(_WIN32)

#include <refrain/core/error.hpp>

#include <fmt/format.h>

#include <windows.h>

#include <dbghelp.h>

#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

namespace refrain::platform {

namespace {

fault_callback g_callback{nullptr};
void *g_context{nullptr};
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter{nullptr};
std::atomic<bool> g_installed{false};

/// Native minidump fallback used when no Crashpad handler is attached. The
/// directory is prepared by `set_fallback_dump_directory()` (startup context);
/// the fault handler only builds the file name and calls dbghelp, so it never
/// allocates through the C++ runtime.
constexpr std::size_t max_dump_directory{1024};
wchar_t g_dump_directory[max_dump_directory + 1]{};
std::atomic<bool> g_dump_enabled{false};
HMODULE g_dbghelp{nullptr};

using minidump_write_dump_fn = BOOL(WINAPI *)(
    HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, const PMINIDUMP_EXCEPTION_INFORMATION,
    const PMINIDUMP_USER_STREAM_INFORMATION,
    const PMINIDUMP_CALLBACK_INFORMATION);
minidump_write_dump_fn g_minidump_write_dump{nullptr};

[[nodiscard]] auto append_text(wchar_t *out, std::size_t capacity,
                               std::size_t at, const wchar_t *text) noexcept
    -> std::size_t {
    while (*text != L'\0' && at + 1 < capacity) {
        out[at++] = *text++;
    }
    return at;
}

[[nodiscard]] auto append_hex(wchar_t *out, std::size_t capacity,
                              std::size_t at, unsigned long long value) noexcept
    -> std::size_t {
    wchar_t digits[16]{};
    std::size_t count{0};
    do {
        digits[count++] = L"0123456789abcdef"[value & 0xFULL];
        value >>= 4U;
    } while (value != 0 && count < 16);
    while (count > 0 && at + 1 < capacity) {
        out[at++] = digits[--count];
    }
    return at;
}

/// Write one minidump named `refrain-<pid>-<ticks>.dmp` into the configured
/// directory. Runs in the unhandled-exception filter: the process is already
/// terminating, so a failure here is silently ignored and the marker callback
/// still records the fault.
void write_fallback_minidump(EXCEPTION_POINTERS *info) noexcept {
    if (info == nullptr || !g_dump_enabled.load(std::memory_order_acquire) ||
        g_minidump_write_dump == nullptr) {
        return;
    }
    constexpr std::size_t capacity{max_dump_directory + 64};
    wchar_t path[capacity]{};
    std::size_t at{append_text(path, capacity, 0, g_dump_directory)};
    at = append_text(path, capacity, at, L"refrain-");
    at = append_hex(path, capacity, at, ::GetCurrentProcessId());
    at = append_text(path, capacity, at, L"-");
    at = append_hex(path, capacity, at, ::GetTickCount64());
    at = append_text(path, capacity, at, L".dmp");
    path[at] = L'\0';

    const HANDLE file{::CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                    nullptr)};
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION exception{};
    exception.ThreadId = ::GetCurrentThreadId();
    exception.ExceptionPointers = info;
    exception.ClientPointers = FALSE;
    (void)g_minidump_write_dump(
        ::GetCurrentProcess(), ::GetCurrentProcessId(), file,
        static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo),
        &exception, nullptr, nullptr);
    (void)::CloseHandle(file);
}

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
    write_fallback_minidump(info);
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

auto set_fallback_dump_directory(std::string_view utf8_directory) noexcept
    -> bool {
    if (utf8_directory.empty() ||
        utf8_directory.size() >= static_cast<std::size_t>(max_dump_directory)) {
        return false;
    }
    const auto converted{::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8_directory.data(),
        static_cast<int>(utf8_directory.size()), g_dump_directory,
        static_cast<int>(max_dump_directory))};
    if (converted <= 0) {
        return false;
    }
    std::size_t length{static_cast<std::size_t>(converted)};
    if (length > 0 && g_dump_directory[length - 1] != L'\\' &&
        g_dump_directory[length - 1] != L'/') {
        if (length + 1 >= max_dump_directory) {
            return false;
        }
        g_dump_directory[length++] = L'\\';
    }
    g_dump_directory[length] = L'\0';

    if (g_dbghelp == nullptr) {
        g_dbghelp = ::LoadLibraryW(L"dbghelp.dll");
        if (g_dbghelp != nullptr) {
            // GetProcAddress returns FARPROC; a function-pointer cast is
            // rejected by -Wcast-function-type, so reinterpret through memcpy
            // (the Win32 ABI guarantees the same representation).
            const auto symbol{::GetProcAddress(g_dbghelp, "MiniDumpWriteDump")};
            minidump_write_dump_fn resolved{nullptr};
            static_assert(sizeof(resolved) == sizeof(symbol));
            std::memcpy(&resolved, &symbol, sizeof(resolved));
            g_minidump_write_dump = resolved;
        }
    }
    const bool usable{g_minidump_write_dump != nullptr};
    g_dump_enabled.store(usable, std::memory_order_release);
    return usable;
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

auto executable_path() -> result<std::string> {
    // GetModuleFileNameW without a size hint truncates and reports
    // ERROR_INSUFFICIENT_BUFFER, so the buffer doubles until the call fits or
    // the path exceeds the 32767-character Win32 limit.
    std::wstring wide;
    for (std::size_t capacity{512}; capacity <= 32768U; capacity *= 2U) {
        wide.resize(capacity);
        const auto length{::GetModuleFileNameW(
            nullptr, wide.data(), static_cast<DWORD>(wide.size()))};
        if (length == 0U) {
            return std::unexpected{make_error(
                error_code::io_error,
                "failed to read the executable path from the process image")};
        }
        if (length < static_cast<DWORD>(wide.size())) {
            wide.resize(length);
            break;
        }
        if (capacity == 32768U) {
            return std::unexpected{make_error(
                error_code::io_error,
                "the executable path does not fit in 32767 characters")};
        }
    }
    if (wide.empty()) {
        return std::unexpected{
            make_error(error_code::io_error,
                       "failed to read the executable path from the process "
                       "image")};
    }
    const int bytes{::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                          static_cast<int>(wide.size()),
                                          nullptr, 0, nullptr, nullptr)};
    if (bytes <= 0) {
        return std::unexpected{
            make_error(error_code::io_error,
                       "failed to convert the executable path to UTF-8")};
    }
    std::string utf8(static_cast<std::size_t>(bytes), '\0');
    if (::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                              static_cast<int>(wide.size()), utf8.data(), bytes,
                              nullptr, nullptr) != bytes) {
        return std::unexpected{
            make_error(error_code::io_error,
                       "failed to convert the executable path to UTF-8")};
    }
    return utf8;
}

auto process_alive(std::uint64_t pid) noexcept -> bool {
    // PROCESS_QUERY_LIMITED_INFORMATION works for processes of other users on
    // Vista+. A pid that names no process is reported as an invalid parameter;
    // any other failure means a process exists that this caller may not open
    // (another account, the System process, a protected process), and the
    // liveness probe must then count it as alive, matching the POSIX EPERM
    // policy. Never read liveness off a handle that was not opened.
    if (pid == 0U ||
        pid > static_cast<std::uint64_t>(std::numeric_limits<DWORD>::max())) {
        return false;
    }
    HANDLE handle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                  static_cast<DWORD>(pid));
    if (handle == nullptr) {
        return ::GetLastError() != ERROR_INVALID_PARAMETER;
    }
    DWORD exit_code{0};
    const bool alive{::GetExitCodeProcess(handle, &exit_code) != 0 &&
                     exit_code == STILL_ACTIVE};
    ::CloseHandle(handle);
    return alive;
}

void abort_process() noexcept { std::abort(); }

auto set_environment_variable(std::string_view name, std::string_view value)
    -> status {
    const std::string owned_name{name};
    const std::string owned_value{value};
    if (::_putenv_s(owned_name.c_str(), owned_value.c_str()) != 0) {
        return std::unexpected{make_error(
            error_code::io_error, "failed to set an environment variable")};
    }
    return status_ok();
}

auto unset_environment_variable(std::string_view name) -> status {
    const std::string owned_name{name};
    if (::_putenv_s(owned_name.c_str(), "") != 0) {
        return std::unexpected{make_error(
            error_code::io_error, "failed to unset an environment variable")};
    }
    return status_ok();
}

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
