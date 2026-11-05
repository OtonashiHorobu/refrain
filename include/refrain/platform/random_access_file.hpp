// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace refrain::platform::file {

// Random-access, durable file primitive for storage engines (RWF regions and
// record files). `refrain::core::io` intentionally stays a whole-file
// helper (config/manifest/tooling reads); append-only region writes need
// positioned writes and real fsync/fdatasync without exposing <unistd.h> or
// <windows.h> above the platform layer.
//
// Contract:
// * One handle is single-threaded. Concurrent access to the same path from
//   several handles is the caller's responsibility (RWF regions are
//   single-writer).
// * `write_at` may extend the file. Bytes between the old end and `offset`
//   read back as zero (POSIX hole semantics; Windows writes zeroes).
// * `sync()` is data + size durability (POSIX fdatasync / Win32
//   FlushFileBuffers). Directory entries created elsewhere need
//   `sync_directory()` after rename/create.
// * Nothing throws; errors are `std::expected` values.

enum class open_mode {
    read_only,
    read_write,
};

struct open_options {
    /// Create the file when missing (`O_CREAT` / `OPEN_ALWAYS`).
    bool create{false};
    /// Fail when the file already exists (`O_EXCL` / `CREATE_NEW`).
    bool exclusive{false};
    /// Truncate an existing file to zero bytes (`O_TRUNC` /
    /// `TRUNCATE_EXISTING`).
    bool truncate{false};
    /// Refuse to follow a symbolic link at the final path component
    /// (`O_NOFOLLOW` on POSIX; reparse-point check on Windows). Storage
    /// readers use this so a crafted world directory cannot redirect writes
    /// or reads outside its root.
    bool no_follow{false};
};

class random_access_file {
  public:
    random_access_file() noexcept;
    ~random_access_file();

    random_access_file(random_access_file &&other) noexcept;
    auto operator=(random_access_file &&other) noexcept -> random_access_file &;

    random_access_file(const random_access_file &) = delete;
    auto operator=(const random_access_file &) -> random_access_file & = delete;

    /// Open (and optionally create/truncate) a file.
    ///
    /// Fails with `not_found` when missing, `already_exists` with
    /// `options.exclusive`, `permission_denied` for EACCES, `io_error`
    /// otherwise, and `invalid_argument` for an empty path.
    [[nodiscard]] static auto open(std::string_view path, open_mode mode,
                                   open_options options = {})
        -> result<random_access_file>;

    [[nodiscard]] auto valid() const noexcept -> bool;

    /// Read exactly `out.size()` bytes at `offset`. A short read (EOF) fails
    /// with `io_error`.
    [[nodiscard]] auto read_at(std::uint64_t offset, std::span<std::byte> out)
        -> status;

    /// Write `data` at `offset`, extending the file as needed.
    [[nodiscard]] auto write_at(std::uint64_t offset,
                                std::span<const std::byte> data) -> status;

    /// Flush data + length to stable storage.
    [[nodiscard]] auto sync() -> status;

    /// Set the file length (grow or shrink).
    [[nodiscard]] auto truncate(std::uint64_t size) -> status;

    [[nodiscard]] auto size() -> result<std::uint64_t>;

    /// UTF-8 path this handle was opened with (empty when invalid).
    [[nodiscard]] auto path() const -> std::string_view;

  private:
    struct impl;
    explicit random_access_file(std::unique_ptr<impl> state) noexcept;

    std::unique_ptr<impl> impl_;
};

/// Flush a directory entry (create/rename visibility). Windows returns success:
/// NTFS journals metadata; the documented guarantee is POSIX durability.
[[nodiscard]] auto sync_directory(std::string_view utf8_path) -> status;

/// Atomically replace `to` with `from` (POSIX rename / Win32 MoveFileEx with
/// MOVEFILE_REPLACE_EXISTING). Callers that need durability then call
/// `sync_directory(parent)`.
[[nodiscard]] auto rename_replace(std::string_view from, std::string_view to)
    -> status;

} // namespace refrain::platform::file
