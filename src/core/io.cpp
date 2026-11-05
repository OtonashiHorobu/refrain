// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/io.hpp>
#include <refrain/platform/file.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace refrain::core::io {
namespace {

namespace fs = std::filesystem;

/// UTF-8 <-> std::filesystem::path helpers.
///
/// `std::filesystem::path`'s narrow-char constructors interpret the input in
/// the native narrow encoding (on Windows that is the active ANSI code page),
/// while every path string crossing this module's API is UTF-8. Constructing
/// from `std::u8string` performs the standard UTF-8 conversion on every
/// platform (on POSIX the bytes are the native bytes already), so non-ASCII
/// paths - e.g. a user name with CJK characters on Windows - resolve correctly
/// instead of failing with a mangled name.
[[nodiscard]] auto to_path(std::string_view path) -> fs::path {
    std::u8string utf8{};
    utf8.reserve(path.size());
    for (const char character : path) {
        utf8.push_back(
            static_cast<char8_t>(static_cast<unsigned char>(character)));
    }
    return fs::path{utf8};
}

/// Render a path as UTF-8 for diagnostics; `path::string()` would use the
/// native narrow encoding on Windows and could garble non-ASCII names.
[[nodiscard]] auto from_path(const fs::path &path) -> std::string {
    const auto utf8{path.u8string()};
    std::string text{};
    text.reserve(utf8.size());
    for (const char8_t character : utf8) {
        text.push_back(static_cast<char>(character));
    }
    return text;
}

/// Best-effort errno mapping for iostream failures.
///
/// The C++ standard does not require a failing formatted/stream operation to
/// set `errno`, so a stale value must never masquerade as the real cause.
/// Callers reset `errno = 0` immediately before the operation and this helper
/// reports "no OS detail" when the C library left it untouched; `io_failure()`
/// then omits the suffix instead of printing a bogus "Success".
[[nodiscard]] auto stream_error() noexcept -> std::error_code {
    const auto code{errno};
    if (code == 0) {
        return {};
    }
    return std::error_code{code, std::generic_category()};
}

[[nodiscard]] auto io_failure(std::string_view what, const fs::path &target,
                              const std::error_code &ec) -> error {
    std::string message{what};
    if (!target.empty()) {
        message += " '";
        message += from_path(target);
        message += "'";
    }
    if (ec) {
        message += ": ";
        message += ec.message();
    }
    return make_error(error_code::io_error, std::move(message));
}

[[nodiscard]] auto empty_path_error() -> error {
    return make_error(error_code::invalid_argument, "empty path");
}

/// Binary write through `std::ofstream`; never throws on failure. Atomic
/// replacement (fsync/rename) lives in the platform layer.
[[nodiscard]] auto write_bytes(const fs::path &target,
                               std::span<const std::byte> data) -> status {
    errno = 0;
    std::ofstream out{target, std::ios::binary | std::ios::trunc};
    if (!out) {
        return std::unexpected{
            io_failure("failed to open for writing", target, stream_error())};
    }
    if (!data.empty()) {
        out.write(reinterpret_cast<const char *>(data.data()), // NOLINT
                  static_cast<std::streamsize>(data.size()));
        if (!out) {
            return std::unexpected{
                io_failure("failed to write", target, stream_error())};
        }
    }
    errno = 0;
    out.flush();
    if (!out) {
        return std::unexpected{
            io_failure("failed to write", target, stream_error())};
    }
    errno = 0;
    out.close();
    if (!out) {
        return std::unexpected{io_failure("failed to close after writing",
                                          target, stream_error())};
    }
    return status_ok();
}

} // namespace

auto read_file(std::string_view path, const read_options &options)
    -> result<std::vector<std::byte>> {
    const fs::path target = to_path(path);
    if (target.empty()) {
        return std::unexpected{empty_path_error()};
    }

    // Untrusted input: reject a symlinked manifest before it can redirect the
    // read outside the instance root. `symlink_status` does not follow the
    // final component, so this inspects the path the caller named.
    std::error_code link_error;
    const auto link_status = fs::symlink_status(target, link_error);
    if (link_error) {
        return std::unexpected{
            io_failure("failed to query file status", target, link_error)};
    }
    if (!options.allow_symlink && fs::is_symlink(link_status)) {
        return std::unexpected{make_error(
            error_code::permission_denied,
            fmt::format("refusing to follow symlink '{}'", from_path(target)))};
    }

    // Only regular files are readable. A directory, FIFO or device has no
    // meaningful content and opening a FIFO would block forever, so this check
    // must happen before the stream is opened.
    std::error_code status_error;
    const auto file_status = fs::status(target, status_error);
    if (status_error) {
        return std::unexpected{
            io_failure("failed to query file status", target, status_error)};
    }
    if (file_status.type() == fs::file_type::not_found) {
        return std::unexpected{
            io_failure("failed to open for reading", target, {})};
    }
    if (!fs::is_regular_file(file_status)) {
        return std::unexpected{io_failure(
            "cannot read a non-regular file (directory, FIFO or device)",
            target, {})};
    }

    // The declared size is only a hint (the file may grow between this check
    // and the read), so the loop below enforces the same limit again. It is
    // checked *before* reserving: an oversized file never allocates.
    std::error_code size_error;
    const auto hinted_size = fs::file_size(target, size_error);
    if (size_error) {
        return std::unexpected{
            io_failure("failed to query file size", target, size_error)};
    }
    if (hinted_size > static_cast<std::uintmax_t>(options.max_bytes)) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            fmt::format("file '{}' is {} bytes, exceeding the {} byte limit",
                        from_path(target), hinted_size, options.max_bytes))};
    }

    errno = 0;
    std::ifstream in{target, std::ios::binary};
    if (!in) {
        return std::unexpected{
            io_failure("failed to open for reading", target, stream_error())};
    }

    std::vector<std::byte> bytes;
    if (hinted_size > 0) {
        // Clamp instead of narrowing silently on 32-bit hosts (uintmax_t ->
        // size_t). Never reserve more than the caller's limit.
        const auto within_limit{std::min<std::uintmax_t>(
            hinted_size, static_cast<std::uintmax_t>(options.max_bytes))};
        const auto hint{static_cast<std::size_t>(std::min<std::uintmax_t>(
            within_limit, static_cast<std::uintmax_t>(
                              std::numeric_limits<std::size_t>::max())))};
        if (hint > 0) {
            bytes.reserve(hint);
        }
    }

    constexpr std::size_t chunk_size = 64U * 1024U;
    std::vector<char> chunk(chunk_size);
    std::uint64_t total{0};
    while (in) {
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const auto read_count = in.gcount();
        if (read_count > 0) {
            const auto count = static_cast<std::uint64_t>(read_count);
            // `total <= max_bytes` is an invariant, so the subtraction cannot
            // underflow even when the limit is 0.
            if (count > options.max_bytes - total) {
                return std::unexpected{make_error(
                    error_code::invalid_argument,
                    fmt::format("file '{}' grew past the {} byte read limit",
                                from_path(target), options.max_bytes))};
            }
            total += count;
            const auto *first = chunk.data();
            const auto count_size = static_cast<std::size_t>(read_count);
            const auto chunk_bytes =
                std::as_bytes(std::span<const char>{first, count_size});
            bytes.insert(bytes.end(), chunk_bytes.begin(), chunk_bytes.end());
        }
    }
    if (!in.eof()) {
        return std::unexpected{
            io_failure("failed to read", target, stream_error())};
    }
    return bytes;
}

auto read_text(std::string_view path, const read_options &options)
    -> result<std::string> {
    auto bytes = read_file(path, options);
    if (!bytes) {
        return std::unexpected{std::move(bytes.error())};
    }
    if (bytes->empty()) {
        return std::string{};
    }
    return std::string{reinterpret_cast<const char *>(bytes->data()), // NOLINT
                       bytes->size()};
}

auto write_file(std::string_view path, std::span<const std::byte> data)
    -> status {
    const fs::path target = to_path(path);
    if (target.empty()) {
        return std::unexpected{empty_path_error()};
    }
    return write_bytes(target, data);
}

auto write_text(std::string_view path, std::string_view text) -> status {
    return write_file(
        path, std::as_bytes(std::span<const char>{text.data(), text.size()}));
}

auto atomic_write_file(std::string_view path, std::span<const std::byte> data)
    -> status {
    if (path.empty()) {
        return std::unexpected{empty_path_error()};
    }
    // Platform layer owns the OS-specific durability steps (fsync, atomic
    // rename, parent-directory flush); see refrain/platform/file.hpp.
    return platform::file::atomic_replace(path, data);
}

auto atomic_write_text(std::string_view path, std::string_view text) -> status {
    return atomic_write_file(
        path, std::as_bytes(std::span<const char>{text.data(), text.size()}));
}

auto status_at(std::string_view path) -> result<file_status> {
    const fs::path target = to_path(path);
    if (target.empty()) {
        return std::unexpected{empty_path_error()};
    }

    std::error_code link_error;
    const auto link_status{fs::symlink_status(target, link_error)};
    if (link_error && link_error != std::errc::no_such_file_or_directory) {
        return std::unexpected{
            io_failure("failed to query file status", target, link_error)};
    }
    file_status out{};
    out.symlink = !link_error && fs::is_symlink(link_status);

    std::error_code target_error;
    const auto target_status{fs::status(target, target_error)};
    if (target_error && target_error != std::errc::no_such_file_or_directory) {
        return std::unexpected{
            io_failure("failed to query file status", target, target_error)};
    }
    out.exists = !target_error && fs::exists(target_status);
    out.regular = !target_error && fs::is_regular_file(target_status);
    out.directory = !target_error && fs::is_directory(target_status);
    return out;
}

auto exists(std::string_view path) -> bool {
    const auto status{status_at(path)};
    return status && status->exists;
}

auto is_regular_file(std::string_view path) -> bool {
    const auto status{status_at(path)};
    return status && status->regular;
}

auto is_directory(std::string_view path) -> bool {
    const auto status{status_at(path)};
    return status && status->directory;
}

auto is_symlink(std::string_view path) -> bool {
    const auto status{status_at(path)};
    return status && status->symlink;
}

auto create_directories(std::string_view path) -> status {
    const fs::path target = to_path(path);
    if (target.empty()) {
        return std::unexpected{empty_path_error()};
    }

    std::error_code create_error;
    const bool created = fs::create_directories(target, create_error);
    if (create_error) {
        return std::unexpected{
            io_failure("failed to create directories", target, create_error)};
    }

    std::error_code lookup_error;
    if (!created && !fs::is_directory(target, lookup_error)) {
        return std::unexpected{
            io_failure("failed to create directories", target, lookup_error)};
    }
    return status_ok();
}

auto file_size(std::string_view path) -> result<std::uint64_t> {
    const fs::path target = to_path(path);
    if (target.empty()) {
        return std::unexpected{empty_path_error()};
    }

    std::error_code size_error;
    const auto size = fs::file_size(target, size_error);
    if (size_error) {
        return std::unexpected{
            io_failure("failed to query file size", target, size_error)};
    }
    // fs::file_size() yields uintmax_t; narrow explicitly with a checked
    // conversion so a platform whose uintmax_t exceeds 64 bits cannot truncate
    // silently. (On the supported 64-bit targets this is a no-op.)
    if constexpr (sizeof(std::uintmax_t) > sizeof(std::uint64_t)) {
        if (size > static_cast<std::uintmax_t>(
                       std::numeric_limits<std::uint64_t>::max())) {
            return std::unexpected{make_error(
                error_code::io_error,
                "failed to query file size: value exceeds the 64-bit range")};
        }
    }
    return static_cast<std::uint64_t>(size);
}

auto list_directory(std::string_view path) -> result<std::vector<std::string>> {
    const fs::path target = to_path(path);
    if (target.empty()) {
        return std::unexpected{empty_path_error()};
    }

    std::error_code iterate_error;
    fs::directory_iterator iterator{target, iterate_error};
    if (iterate_error) {
        return std::unexpected{
            io_failure("failed to list directory", target, iterate_error)};
    }

    std::vector<std::string> names;
    const fs::directory_iterator end;
    while (iterator != end) {
        // Emit names as UTF-8 (not the native narrow encoding).
        names.push_back(from_path(iterator->path().filename()));
        iterator.increment(iterate_error);
        if (iterate_error) {
            return std::unexpected{
                io_failure("failed to list directory", target, iterate_error)};
        }
    }

    std::sort(names.begin(), names.end());
    return names;
}

auto parent_directory(std::string_view path) -> std::string {
    const auto parent = to_path(path).parent_path();
    if (parent.empty()) {
        return ".";
    }
    return from_path(parent);
}

auto current_path() -> result<std::string> {
    std::error_code lookup_error;
    const fs::path directory = fs::current_path(lookup_error);
    if (lookup_error) {
        return std::unexpected{io_failure("failed to query current path",
                                          fs::path{}, lookup_error)};
    }
    return from_path(directory); // UTF-8 on every platform
}

auto temp_directory() -> std::string {
    // Platform layer decides the OS convention (TMPDIR / GetTempPath); the
    // standard library already provides the lookup, so keep it in one place.
    return platform::file::temp_directory();
}

} // namespace refrain::core::io
