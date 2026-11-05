// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::core::io {

// Small synchronous filesystem helpers used by the kernel, tools and tests.
// Async IO (io_uring) lands with the storage pipeline; these functions are for
// bootstrap/config/tooling paths only.
//
// All path strings are UTF-8 on every platform, including diagnostics and
// `list_directory`/`current_path` output. Error diagnostics are best-effort: a
// failed stream operation reports the operating-system detail only when the C
// library provides it, and never a stale/bogus cause.

/// Resource/safety limits for `read_file`/`read_text`.
///
/// Manifests and other untrusted files must not be able to make the engine
/// allocate unbounded memory or follow a link outside the instance root, so
/// callers that read them pass `{.max_bytes = ..., .allow_symlink = false}`.
struct read_options {
    /// Hard cap on the number of bytes read. A file whose size already exceeds
    /// it fails before any buffer is reserved; a file that grows past it while
    /// being read fails as well (`invalid_argument` in both cases).
    std::uint64_t max_bytes{64ULL * 1024ULL * 1024ULL};
    /// When false, a symlink at `path` is refused (`permission_denied`)
    /// instead of being followed.
    bool allow_symlink{true};
};

/// Read a whole regular file.
///
/// Fails with `invalid_argument` for an empty path or a file over
/// `options.max_bytes`, `permission_denied` for a symlink when
/// `options.allow_symlink` is false, and `io_error` for a missing/unreadable
/// file or a non-regular one (directory, FIFO, device). A FIFO is never opened,
/// so the call cannot block on it.
[[nodiscard]] auto read_file(std::string_view path,
                             const read_options &options = {})
    -> result<std::vector<std::byte>>;

/// `read_file` as UTF-8 text (same limits).
[[nodiscard]] auto read_text(std::string_view path,
                             const read_options &options = {})
    -> result<std::string>;

[[nodiscard]] auto write_file(std::string_view path,
                              std::span<const std::byte> data) -> status;

[[nodiscard]] auto write_text(std::string_view path, std::string_view text)
    -> status;

/// Write to a temporary sibling file, fsync and atomically rename over `path`.
/// Guarantees readers see either the old or the new content, never a torn file.
[[nodiscard]] auto atomic_write_file(std::string_view path,
                                     std::span<const std::byte> data) -> status;

[[nodiscard]] auto atomic_write_text(std::string_view path,
                                     std::string_view text) -> status;

/// Metadata for one path. `exists`/`regular`/`directory` describe the target
/// (following a symlink); `symlink` describes the path itself. Unlike the
/// bool convenience helpers below, `status_at()` reports filesystem errors
/// (e.g. permission denied) instead of folding them into `false`.
struct file_status {
    bool exists{false};
    bool regular{false};
    bool directory{false};
    bool symlink{false};
};

/// Parse-free metadata query. Missing paths are a successful
/// `file_status{exists=false}`; other filesystem errors are `io_error`.
[[nodiscard]] auto status_at(std::string_view path) -> result<file_status>;

[[nodiscard]] auto exists(std::string_view path) -> bool;

[[nodiscard]] auto is_regular_file(std::string_view path) -> bool;

[[nodiscard]] auto is_directory(std::string_view path) -> bool;

/// True when `path` itself is a symbolic link (the link is inspected, not its
/// target). Walkers use this before `is_regular_file()`/`read_file()` so a link
/// can never be followed out of the tree being scanned.
[[nodiscard]] auto is_symlink(std::string_view path) -> bool;

[[nodiscard]] auto create_directories(std::string_view path) -> status;

[[nodiscard]] auto file_size(std::string_view path) -> result<std::uint64_t>;

/// Directory entry names (not paths), sorted for deterministic output.
[[nodiscard]] auto list_directory(std::string_view path)
    -> result<std::vector<std::string>>;

[[nodiscard]] auto current_path() -> result<std::string>;

/// Parent directory of `path` as a UTF-8 string ("." when there is no parent).
/// Uses the platform-native path rules, unlike the virtual-path
/// `data::parent_path()` (which only splits on '/').
[[nodiscard]] auto parent_directory(std::string_view path) -> std::string;

[[nodiscard]] auto temp_directory() -> std::string;

} // namespace refrain::core::io
