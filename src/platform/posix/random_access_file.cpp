// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/platform/random_access_file.hpp>

#if !defined(_WIN32)

#include <fmt/format.h>

#include <cerrno>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace refrain::platform::file {
namespace {

[[nodiscard]] auto native_error(error_code fallback, std::string_view what,
                                std::string_view path) -> error {
    const std::error_code code{errno, std::generic_category()};
    std::string message{what};
    if (!path.empty()) {
        message += " '";
        message += path;
        message += "'";
    }
    if (code) {
        message += ": ";
        message += code.message();
    }
    return make_error(fallback, std::move(message));
}

[[nodiscard]] auto failure_code() noexcept -> error_code {
    switch (errno) {
    case ENOENT:
        return error_code::not_found;
    case EEXIST:
        return error_code::already_exists;
    case EACCES:
    case EPERM:
        return error_code::permission_denied;
    default:
        return error_code::io_error;
    }
}

[[nodiscard]] auto simple_error(error_code code, std::string message)
    -> std::unexpected<error> {
    return std::unexpected{make_error(code, std::move(message))};
}

} // namespace

struct random_access_file::impl {
    int descriptor{-1};
    std::string path;
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
    auto flags{mode == open_mode::read_only ? O_RDONLY : O_RDWR};
    if (options.create) {
        flags |= O_CREAT;
    }
    if (options.exclusive) {
        flags |= O_EXCL;
    }
    if (options.truncate) {
        flags |= O_TRUNC;
    }
    if (options.no_follow) {
        flags |= O_NOFOLLOW;
    }
    int descriptor{-1};
    for (;;) {
        descriptor = ::open(std::string{path}.c_str(), flags, 0644);
        if (descriptor >= 0 || errno != EINTR) {
            break;
        }
    }
    if (descriptor < 0) {
        return std::unexpected{
            native_error(failure_code(), "cannot open file", path)};
    }
    auto state{std::make_unique<impl>()};
    state->descriptor = descriptor;
    state->path = std::string{path};
    return random_access_file{std::move(state)};
}

auto random_access_file::valid() const noexcept -> bool {
    return impl_ != nullptr && impl_->descriptor >= 0;
}

auto random_access_file::read_at(std::uint64_t offset, std::span<std::byte> out)
    -> status {
    if (!valid()) {
        return std::unexpected{
            make_error(error_code::not_initialized, "file handle is closed")};
    }
    std::size_t done{0};
    while (done < out.size()) {
        const auto position{static_cast<off_t>(offset + done)};
        const auto result{::pread(impl_->descriptor, out.data() + done,
                                  out.size() - done, position)};
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected{
                native_error(failure_code(), "cannot read file", impl_->path)};
        }
        if (result == 0) {
            return simple_error(
                error_code::io_error,
                fmt::format("unexpected end of file at offset {} in '{}'",
                            offset + done, impl_->path));
        }
        done += static_cast<std::size_t>(result);
    }
    return status_ok();
}

auto random_access_file::write_at(std::uint64_t offset,
                                  std::span<const std::byte> data) -> status {
    if (!valid()) {
        return std::unexpected{
            make_error(error_code::not_initialized, "file handle is closed")};
    }
    std::size_t done{0};
    while (done < data.size()) {
        const auto position{static_cast<off_t>(offset + done)};
        const auto result{::pwrite(impl_->descriptor, data.data() + done,
                                   data.size() - done, position)};
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected{
                native_error(failure_code(), "cannot write file", impl_->path)};
        }
        if (result == 0) {
            return simple_error(error_code::io_error,
                                fmt::format("short write at offset {} in '{}'",
                                            offset + done, impl_->path));
        }
        done += static_cast<std::size_t>(result);
    }
    return status_ok();
}

auto random_access_file::sync() -> status {
    if (!valid()) {
        return std::unexpected{
            make_error(error_code::not_initialized, "file handle is closed")};
    }
    int result{-1};
    for (;;) {
        result = ::fdatasync(impl_->descriptor);
        if (result == 0 || errno != EINTR) {
            break;
        }
    }
    if (result != 0) {
        return std::unexpected{native_error(error_code::io_error,
                                            "cannot sync file", impl_->path)};
    }
    return status_ok();
}

auto random_access_file::truncate(std::uint64_t size) -> status {
    if (!valid()) {
        return std::unexpected{
            make_error(error_code::not_initialized, "file handle is closed")};
    }
    int result{-1};
    for (;;) {
        result = ::ftruncate(impl_->descriptor, static_cast<off_t>(size));
        if (result == 0 || errno != EINTR) {
            break;
        }
    }
    if (result != 0) {
        return std::unexpected{native_error(
            error_code::io_error, "cannot truncate file", impl_->path)};
    }
    return status_ok();
}

auto random_access_file::size() -> result<std::uint64_t> {
    if (!valid()) {
        return std::unexpected{
            make_error(error_code::not_initialized, "file handle is closed")};
    }
    struct stat info{};
    if (::fstat(impl_->descriptor, &info) != 0) {
        return std::unexpected{native_error(error_code::io_error,
                                            "cannot stat file", impl_->path)};
    }
    return static_cast<std::uint64_t>(info.st_size);
}

auto random_access_file::path() const -> std::string_view {
    return impl_ != nullptr ? std::string_view{impl_->path}
                            : std::string_view{};
}

auto sync_directory(std::string_view utf8_path) -> status {
    int descriptor{-1};
    for (;;) {
        descriptor =
            ::open(std::string{utf8_path}.c_str(), O_RDONLY | O_DIRECTORY);
        if (descriptor >= 0 || errno != EINTR) {
            break;
        }
    }
    if (descriptor < 0) {
        return std::unexpected{
            native_error(failure_code(), "cannot open directory", utf8_path)};
    }
    int result{-1};
    for (;;) {
        result = ::fsync(descriptor);
        if (result == 0 || errno != EINTR) {
            break;
        }
    }
    const int saved{errno};
    ::close(descriptor);
    if (result != 0 && saved != EINVAL && saved != ENOTSUP) {
        errno = saved;
        return std::unexpected{native_error(
            error_code::io_error, "cannot sync directory", utf8_path)};
    }
    return status_ok();
}

auto rename_replace(std::string_view from, std::string_view to) -> status {
    if (::rename(std::string{from}.c_str(), std::string{to}.c_str()) != 0) {
        return std::unexpected{
            native_error(failure_code(), "cannot replace file", to)};
    }
    return status_ok();
}

} // namespace refrain::platform::file

#endif // !defined(_WIN32)
