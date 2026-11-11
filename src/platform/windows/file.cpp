// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/platform/file.hpp>

#if defined(_WIN32)

#include <refrain/core/error.hpp>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <string>
#include <system_error>

namespace refrain::platform::file {
namespace {

/// UTF-8 (engine convention) -> UTF-8 std::string for diagnostics.
[[nodiscard]] auto to_utf8(const std::filesystem::path &path) -> std::string {
    const std::u8string wide = path.u8string();
    return std::string{wide.begin(), wide.end()};
}

/// Build the diagnostic from an *already captured* native error: once
/// CloseHandle/DeleteFileW run, GetLastError() no longer describes the failure.
[[nodiscard]] auto io_failure(std::string_view what, std::string_view path,
                              DWORD native_error) -> error {
    std::string message{what};
    if (!path.empty()) {
        message += " '";
        message += path;
        message += "'";
    }
    message += ": ";
    message += std::system_category().message(static_cast<int>(native_error));
    return make_error(error_code::io_error, std::move(message));
}

[[nodiscard]] auto write_temp_file(const std::filesystem::path &path,
                                   std::span<const std::byte> data) -> status {
    const HANDLE handle =
        ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::unexpected{io_failure("failed to create temporary file",
                                          "<temp>", ::GetLastError())};
    }
    std::size_t written{0};
    DWORD last_error{ERROR_SUCCESS};
    bool ok = true;
    while (written < data.size()) {
        DWORD chunk{0};
        const auto *first = data.data() + written;
        // DWORD-sized writes: a plain static_cast truncates data > 4 GiB (and a
        // size that is an exact multiple of 2^32 would loop forever).
        const auto remaining = static_cast<DWORD>(std::min<std::size_t>(
            data.size() - written, std::numeric_limits<DWORD>::max()));
        if (::WriteFile(handle, first, remaining, &chunk, nullptr) == 0) {
            last_error = ::GetLastError();
            ok = false;
            break;
        }
        if (chunk == 0) { // defensive: no progress would hang the loop
            last_error = ERROR_WRITE_FAULT;
            ok = false;
            break;
        }
        written += chunk;
    }
    if (ok && ::FlushFileBuffers(handle) == 0) {
        last_error = ::GetLastError();
        ok = false;
    }
    (void)::CloseHandle(handle);
    if (!ok) {
        return std::unexpected{io_failure(
            "failed to write/flush temporary file", "<temp>", last_error)};
    }
    return status_ok();
}

} // namespace

auto native_path(std::string_view utf8_path) -> std::filesystem::path {
    const auto *first = reinterpret_cast<const char8_t *>(utf8_path.data());
    return std::filesystem::path{std::u8string{first, utf8_path.size()}};
}

auto native_string(std::string_view utf8_path) -> native_path_string {
    return native_path(utf8_path).wstring();
}

auto atomic_replace(std::string_view path, std::span<const std::byte> data)
    -> status {
    if (path.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument, "empty path")};
    }
    const std::filesystem::path target = native_path(path);
    if (target.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument, "invalid UTF-8 path")};
    }
    // Atomic: two threads replacing the same path concurrently must never pick
    // the same temporary name.
    static std::atomic<std::uint64_t> sequence{0};
    const std::filesystem::path temp{
        target.wstring() +
        std::format(L".tmp.{}.{}", ::GetCurrentProcessId(),
                    sequence.fetch_add(1, std::memory_order_relaxed))};

    if (auto written = write_temp_file(temp, data); !written) {
        (void)::DeleteFileW(temp.c_str());
        return written;
    }
    // Indexers, scanners and concurrent replacers can hold the target open for
    // a moment; MoveFileEx then reports a sharing violation. A short bounded
    // retry keeps the atomic-replace contract without masking permanent errors.
    for (int attempt{0}; attempt < 32; ++attempt) {
        if (::MoveFileExW(temp.c_str(), target.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
            0) {
            return status_ok();
        }
        const DWORD native_error{::GetLastError()};
        if (native_error != ERROR_SHARING_VIOLATION &&
            native_error != ERROR_ACCESS_DENIED) {
            const auto failure{
                io_failure("failed to replace file", path, native_error)};
            (void)::DeleteFileW(temp.c_str());
            return std::unexpected{failure};
        }
        // Another writer replacing the same path only holds it for the
        // duration of its own move, so yield the CPU first and back off to a
        // real sleep once yielding stops helping.
        if (attempt < 4) {
            ::SwitchToThread();
        } else {
            ::Sleep(1);
        }
    }
    const auto failure{
        io_failure("failed to replace file", path, ::GetLastError())};
    (void)::DeleteFileW(temp.c_str());
    return std::unexpected{failure};
}

auto temp_directory() -> std::string {
    std::error_code error{};
    const auto directory = std::filesystem::temp_directory_path(error);
    if (error || directory.empty()) {
        return std::string{"."};
    }
    return to_utf8(directory);
}

auto create_private_directory(std::string_view utf8_path) -> status {
    if (utf8_path.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument, "empty directory path")};
    }
    const std::filesystem::path path = native_path(utf8_path);
    std::error_code error{};
    std::filesystem::create_directories(path, error);
    if (error && !std::filesystem::is_directory(path)) {
        return std::unexpected{
            make_error(error_code::io_error,
                       "failed to create private directory '" +
                           std::string{utf8_path} + "': " + error.message())};
    }
    // The crash database holds minidumps. Windows directory DACLs are
    // inherited from the parent; per-user %LOCALAPPDATA% is already private, so
    // this only guarantees the directory exists. Explicit DACL tightening is
    // not implemented yet.
    return status_ok();
}

auto is_owner_private_directory(std::string_view utf8_path) -> bool {
    // ACL inspection is not implemented yet; for now the portable contract is
    // "the (per-user) directory exists".
    if (utf8_path.empty()) {
        return false;
    }
    std::error_code error{};
    return std::filesystem::is_directory(native_path(utf8_path), error) &&
           !error;
}

} // namespace refrain::platform::file

#else

// POSIX implementation: see src/platform/posix/file.cpp.

#endif // _WIN32
