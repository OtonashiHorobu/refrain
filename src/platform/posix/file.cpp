// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/platform/file.hpp>

#if !defined(_WIN32)

#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace refrain::platform::file {
namespace {

/// Open with EINTR retry: a signal in the middle of open() must not turn into a
/// spurious io_error.
[[nodiscard]] auto open_retry(const char *path, int flags, mode_t mode) noexcept
    -> int {
    for (;;) {
        const int descriptor = ::open(path, flags, mode);
        if (descriptor >= 0 || errno != EINTR) {
            return descriptor;
        }
    }
}

[[nodiscard]] auto io_failure(std::string_view what, std::string_view path)
    -> error {
    std::string message{what};
    if (!path.empty()) {
        message += " '";
        message += path;
        message += "'";
    }
    const std::error_code code{errno, std::generic_category()};
    if (code) {
        message += ": ";
        message += code.message();
    }
    return make_error(error_code::io_error, std::move(message));
}

/// Unique sibling name. The sequence is atomic because `atomic_replace` is
/// documented as concurrency-safe: a duplicated
/// name would let two threads write the same temporary file and destroy the
/// atomicity of the rename.
///
/// Built with `std::to_chars` into a fixed buffer instead of `std::format`,
/// so no formatting exceptions are involved. Not `noexcept`: the returned
/// name is ~30 characters, i.e. heap-allocated, so a hard OOM propagates to
/// `atomic_replace()` (documented hard-OOM boundary) instead of terminating.
[[nodiscard]] auto temp_suffix() -> std::string {
    static std::atomic<std::uint64_t> sequence{0};
    const auto stamp = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto index = sequence.fetch_add(1, std::memory_order_relaxed);

    std::array<char, 80> buffer{};
    constexpr std::string_view prefix{".tmp."};
    auto offset{std::size_t{0}};
    for (const char character : prefix) {
        buffer[offset++] = character;
    }
    const auto append_number{[&buffer, &offset](std::uint64_t value) noexcept {
        const auto result = std::to_chars(buffer.data() + offset,
                                          buffer.data() + buffer.size(), value);
        offset = static_cast<std::size_t>(result.ptr - buffer.data());
    }};
    append_number(static_cast<std::uint64_t>(::getpid()));
    buffer[offset++] = '.';
    append_number(stamp);
    buffer[offset++] = '.';
    append_number(index);
    return std::string{buffer.data(), offset};
}

/// Write all bytes through a raw descriptor (handles EINTR/short writes). No
/// portable standard-library equivalent exists for descriptor-level writes.
[[nodiscard]] auto write_all(int descriptor, std::span<const std::byte> data)
    -> bool {
    std::size_t written{0};
    while (written < data.size()) {
        const auto *first = data.data() + written;
        const auto chunk = ::write(descriptor, first, data.size() - written);
        if (chunk < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        written += static_cast<std::size_t>(chunk);
    }
    return true;
}

/// Mode for the replacement file: keep the permissions of the file being
/// replaced (a 0644 temp file would silently widen a 0600 config/token file).
/// A missing target has no mode to preserve: the temporary file is created with
/// 0666 and the process umask decides the final permissions (nullopt).
[[nodiscard]] auto replacement_mode(const std::string &target) noexcept
    -> std::optional<mode_t> {
    struct stat status{};
    if (::stat(target.c_str(), &status) == 0) {
        return status.st_mode & 0777;
    }
    return std::nullopt;
}

/// Create the temporary file and write `data` into it. For an existing target,
/// `mode` is the mode to preserve and is forced with fchmod() (the caller owns
/// the exact permissions of the file being replaced). For a new target `mode`
/// is nullopt: the file is opened with 0666 and *not* fchmod()ed, so the normal
/// create semantics (0666 & ~umask) apply - fchmod(0666) would silently defeat
/// a restrictive umask such as 077.
[[nodiscard]] auto write_temp_file(const std::string &path,
                                   std::span<const std::byte> data,
                                   std::optional<mode_t> mode) -> status {
    constexpr mode_t default_mode{0666};
    const int descriptor =
        open_retry(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                   mode.value_or(default_mode));
    if (descriptor < 0) {
        return std::unexpected{
            io_failure("failed to open temporary file", path)};
    }
    if (mode.has_value() && ::fchmod(descriptor, *mode) != 0) {
        const auto failure =
            io_failure("failed to set temporary file mode", path);
        (void)::close(descriptor);
        return std::unexpected{failure};
    }
    if (!write_all(descriptor, data)) {
        const auto failure = io_failure("failed to write temporary file", path);
        (void)::close(descriptor);
        return std::unexpected{failure};
    }
    if (::fsync(descriptor) != 0) {
        const auto failure = io_failure("failed to flush temporary file", path);
        (void)::close(descriptor);
        return std::unexpected{failure};
    }
    if (::close(descriptor) != 0) {
        return std::unexpected{
            io_failure("failed to close temporary file", path)};
    }
    return status_ok();
}

[[nodiscard]] auto rename_over(const std::string &from, const std::string &to)
    -> status {
    while (::rename(from.c_str(), to.c_str()) != 0) {
        if (errno == EINTR) {
            continue;
        }
        return std::unexpected{io_failure("failed to replace file", to)};
    }
    return status_ok();
}

/// Best-effort: a rename is only durable after the parent directory entry has
/// been flushed. Failure here cannot un-write the file, so it is ignored.
/// Not `noexcept`: `std::filesystem::path`/`string()` allocate; the
/// exception reaches `atomic_replace()`'s documented hard-OOM boundary.
void sync_parent_directory(const std::string &path) {
    const std::filesystem::path parent =
        std::filesystem::path{path}.parent_path();
    const std::string directory = parent.empty() ? "." : parent.string();
    const int descriptor =
        open_retry(directory.c_str(), O_RDONLY | O_CLOEXEC, 0);
    if (descriptor >= 0) {
        (void)::fsync(descriptor);
        (void)::close(descriptor);
    }
}

} // namespace

auto native_path(std::string_view utf8_path) -> std::filesystem::path {
    return std::filesystem::path{std::string{utf8_path}};
}

auto native_string(std::string_view utf8_path) -> native_path_string {
    return std::string{utf8_path};
}

auto atomic_replace(std::string_view path, std::span<const std::byte> data)
    -> status {
    if (path.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument, "empty path")};
    }
    const std::string target{path};
    const std::string temp = target + temp_suffix();
    const auto mode = replacement_mode(target);

    if (auto written = write_temp_file(temp, data, mode); !written) {
        (void)::remove(temp.c_str());
        return written;
    }
    if (auto renamed = rename_over(temp, target); !renamed) {
        (void)::remove(temp.c_str());
        return renamed;
    }
    sync_parent_directory(target);
    return status_ok();
}

auto temp_directory() -> std::string {
    std::error_code error{};
    const auto directory = std::filesystem::temp_directory_path(error);
    if (error || directory.empty()) {
        return std::string{"/tmp"};
    }
    return directory.string();
}

auto create_private_directory(std::string_view utf8_path) -> status {
    if (utf8_path.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument, "empty directory path")};
    }
    const std::string path{utf8_path};
    std::error_code error{};
    std::filesystem::create_directories(path, error);
    if (error && !std::filesystem::is_directory(path)) {
        return std::unexpected{make_error(
            error_code::io_error, "failed to create private directory '" +
                                      path + "': " + error.message())};
    }
    // Crash databases hold minidumps; force owner-only permissions even
    // when the directory already existed (e.g. bootstrapped by an older build
    // with the default 0755) and regardless of the process umask.
    if (::chmod(path.c_str(), 0700) != 0) {
        const auto saved{errno};
        return std::unexpected{
            make_error(error_code::io_error,
                       "failed to restrict private directory '" + path +
                           "': " + std::generic_category().message(saved))};
    }
    return status_ok();
}

auto is_owner_private_directory(std::string_view utf8_path) -> bool {
    if (utf8_path.empty()) {
        return false;
    }
    struct stat status{};
    if (::stat(std::string{utf8_path}.c_str(), &status) != 0) {
        return false;
    }
    return S_ISDIR(status.st_mode) && (status.st_mode & 0077) == 0;
}

} // namespace refrain::platform::file

#else

// Windows implementation: see src/platform/windows/file.cpp.

#endif // !_WIN32
