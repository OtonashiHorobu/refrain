// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/platform/random_access_file.hpp>

#if defined(_WIN32)

#include <refrain/platform/file.hpp>

#include <fmt/format.h>

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace refrain::platform::file {
namespace {

[[nodiscard]] auto failure_code(DWORD native_error) noexcept -> error_code {
    switch (native_error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
        return error_code::not_found;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return error_code::already_exists;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
        return error_code::permission_denied;
    default:
        return error_code::io_error;
    }
}

[[nodiscard]] auto native_failure(error_code fallback, std::string_view what,
                                  std::string_view path, DWORD native_error)
    -> error {
    const std::error_code code{static_cast<int>(native_error),
                               std::system_category()};
    return make_error(fallback,
                      fmt::format("{} '{}': {}", what, path, code.message()));
}

[[nodiscard]] auto simple_error(error_code code, std::string message)
    -> std::unexpected<error> {
    return std::unexpected{make_error(code, std::move(message))};
}

} // namespace

struct random_access_file::impl {
    HANDLE handle{INVALID_HANDLE_VALUE};
    std::string path;
    ~impl() {
        if (handle != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle);
        }
    }
};

random_access_file::random_access_file() noexcept = default;
random_access_file::~random_access_file() = default;
random_access_file::random_access_file(random_access_file &&other) noexcept =
    default;
auto random_access_file::operator=(random_access_file &&other) noexcept
    -> random_access_file & = default;

random_access_file::random_access_file(std::unique_ptr<impl> state) noexcept
    : impl_{std::move(state)} {}

auto random_access_file::open(std::string_view path, open_mode mode,
                              open_options options)
    -> result<random_access_file> {
    if (path.empty()) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "cannot open an empty path")};
    }
    auto access{mode == open_mode::read_only ? GENERIC_READ
                                             : (GENERIC_READ | GENERIC_WRITE)};
    const auto native{native_path(path)};
    if (options.no_follow) {
        const DWORD attributes{::GetFileAttributesW(native.c_str())};
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return std::unexpected{make_error(
                error_code::permission_denied,
                fmt::format("refusing to follow symlink '{}'", path))};
        }
    }
    DWORD disposition{OPEN_EXISTING};
    if (options.exclusive) {
        disposition = CREATE_NEW;
    } else if (options.truncate) {
        disposition = TRUNCATE_EXISTING;
    } else if (options.create) {
        disposition = OPEN_ALWAYS;
    }
    const HANDLE handle{::CreateFileW(native.c_str(), access, FILE_SHARE_READ,
                                      nullptr, disposition,
                                      FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD native_error{::GetLastError()};
        return std::unexpected{native_failure(failure_code(native_error),
                                              "cannot open file", path,
                                              native_error)};
    }
    auto state{std::make_unique<impl>()};
    state->handle = handle;
    state->path = std::string{path};
    return random_access_file{std::move(state)};
}

auto random_access_file::valid() const noexcept -> bool {
    return impl_ != nullptr && impl_->handle != INVALID_HANDLE_VALUE;
}

auto random_access_file::read_at(std::uint64_t offset, std::span<std::byte> out)
    -> status {
    if (!valid()) {
        return std::unexpected{
            make_error(error_code::not_initialized, "file handle is closed")};
    }
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (::SetFilePointerEx(impl_->handle, position, nullptr, FILE_BEGIN) == 0) {
        const DWORD native_error{::GetLastError()};
        return std::unexpected{native_failure(failure_code(native_error),
                                              "cannot seek file", impl_->path,
                                              native_error)};
    }
    std::size_t done{0};
    while (done < out.size()) {
        const auto chunk{static_cast<DWORD>(
            std::min<std::size_t>(out.size() - done, 1U << 30U))};
        DWORD read{0};
        if (::ReadFile(impl_->handle, out.data() + done, chunk, &read,
                       nullptr) == 0) {
            const DWORD native_error{::GetLastError()};
            return std::unexpected{native_failure(failure_code(native_error),
                                                  "cannot read file",
                                                  impl_->path, native_error)};
        }
        if (read == 0) {
            return simple_error(
                error_code::io_error,
                fmt::format("unexpected end of file at offset {} in '{}'",
                            offset + done, impl_->path));
        }
        done += static_cast<std::size_t>(read);
    }
    return status_ok();
}

auto random_access_file::write_at(std::uint64_t offset,
                                  std::span<const std::byte> data) -> status {
    if (!valid()) {
        return std::unexpected{
            make_error(error_code::not_initialized, "file handle is closed")};
    }
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (::SetFilePointerEx(impl_->handle, position, nullptr, FILE_BEGIN) == 0) {
        const DWORD native_error{::GetLastError()};
        return std::unexpected{native_failure(failure_code(native_error),
                                              "cannot seek file", impl_->path,
                                              native_error)};
    }
    std::size_t done{0};
    while (done < data.size()) {
        const auto chunk{static_cast<DWORD>(
            std::min<std::size_t>(data.size() - done, 1U << 30U))};
        DWORD written{0};
        if (::WriteFile(impl_->handle, data.data() + done, chunk, &written,
                        nullptr) == 0) {
            const DWORD native_error{::GetLastError()};
            return std::unexpected{native_failure(failure_code(native_error),
                                                  "cannot write file",
                                                  impl_->path, native_error)};
        }
        if (written == 0) {
            return simple_error(error_code::io_error,
                                fmt::format("short write at offset {} in '{}'",
                                            offset + done, impl_->path));
        }
        done += static_cast<std::size_t>(written);
    }
    return status_ok();
}

auto random_access_file::sync() -> status {
    if (!valid()) {
        return std::unexpected{
            make_error(error_code::not_initialized, "file handle is closed")};
    }
    if (::FlushFileBuffers(impl_->handle) == 0) {
        const DWORD native_error{::GetLastError()};
        return std::unexpected{native_failure(failure_code(native_error),
                                              "cannot sync file", impl_->path,
                                              native_error)};
    }
    return status_ok();
}

auto random_access_file::truncate(std::uint64_t size) -> status {
    if (!valid()) {
        return std::unexpected{
            make_error(error_code::not_initialized, "file handle is closed")};
    }
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(size);
    if (::SetFilePointerEx(impl_->handle, position, nullptr, FILE_BEGIN) == 0 ||
        ::SetEndOfFile(impl_->handle) == 0) {
        const DWORD native_error{::GetLastError()};
        return std::unexpected{native_failure(failure_code(native_error),
                                              "cannot truncate file",
                                              impl_->path, native_error)};
    }
    return status_ok();
}

auto random_access_file::size() -> result<std::uint64_t> {
    if (!valid()) {
        return std::unexpected{
            make_error(error_code::not_initialized, "file handle is closed")};
    }
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(impl_->handle, &size) == 0) {
        const DWORD native_error{::GetLastError()};
        return std::unexpected{native_failure(failure_code(native_error),
                                              "cannot stat file", impl_->path,
                                              native_error)};
    }
    return static_cast<std::uint64_t>(size.QuadPart);
}

auto random_access_file::path() const -> std::string_view {
    return impl_ != nullptr ? std::string_view{impl_->path}
                            : std::string_view{};
}

auto sync_directory(std::string_view utf8_path) -> status {
    // NTFS journals metadata updates; FlushFileBuffers on a directory handle is
    // not supported by every filesystem, so the documented contract is POSIX
    // durability and Windows returns success.
    static_cast<void>(utf8_path);
    return status_ok();
}

auto rename_replace(std::string_view from, std::string_view to) -> status {
    const auto native_from{native_path(from)};
    const auto native_to{native_path(to)};
    if (::MoveFileExW(native_from.c_str(), native_to.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ==
        0) {
        const DWORD native_error{::GetLastError()};
        return std::unexpected{native_failure(failure_code(native_error),
                                              "cannot replace file", to,
                                              native_error)};
    }
    return status_ok();
}

} // namespace refrain::platform::file

#endif // defined(_WIN32)
