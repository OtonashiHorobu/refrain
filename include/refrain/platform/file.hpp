// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/platform/mmap.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace refrain::platform::file {

// Native filesystem primitives that cannot be expressed portably with the C++
// standard library alone. Ordinary reads/writes/directories stay in
// `refrain::core::io` on top of <fstream>/<filesystem>; this layer owns the
// steps the standard library cannot express: durability/atomicity, owner-only
// directory permissions, and an untrusted-path open that refuses a symbolic
// link as part of the open itself instead of checking the path first.

/// Native string type for APIs that take an OS path directly (Crashpad's
/// `base::FilePath`, Win32 `CreateFileW`): `std::wstring` on Windows,
/// UTF-8 `std::string` elsewhere. It deliberately matches
/// `base::FilePath::StringType`, so kernel code can construct `base::FilePath`
/// without an `#if defined(_WIN32)` branch.
#if defined(_WIN32)
using native_path_string = std::wstring;
#else
using native_path_string = std::string;
#endif

/// Convert an engine UTF-8 path to the native `std::filesystem::path`
/// representation (Windows: wide characters via the u8 conversion; POSIX: bytes
/// unchanged). All OS-facing path handling goes through this helper instead of
/// platform branches scattered across kernel code.
[[nodiscard]] auto native_path(std::string_view utf8_path)
    -> std::filesystem::path;

/// Suffix of an executable file name on this platform (`".exe"` on Windows,
/// empty elsewhere). Code that derives a sibling executable name (for example
/// the crash handler shipped next to the server) does not branch on the
/// operating system.
[[nodiscard]] auto executable_suffix() noexcept -> std::string_view;

/// Convert an engine UTF-8 path to the native string accepted by OS APIs
/// (`std::wstring` on Windows, UTF-8 `std::string` on POSIX).
[[nodiscard]] auto native_string(std::string_view utf8_path)
    -> native_path_string;

/// Read-only handle for reading an untrusted path.
///
/// `open()` obtains the OS handle and classifies it in one step, so callers do
/// not have to query the path and open it afterwards: between a separate query
/// and a later open the path can change, and the guarantee this handle makes
/// (the final component is the object that was opened, and a link is refused
/// rather than followed) would not hold. With `follow_symlink == false` a
/// symbolic link at the final path component is refused by the open itself
/// (`O_NOFOLLOW` on POSIX, the reparse-point check on the opened Windows
/// handle), and the open is issued non-blocking so a FIFO is rejected as
/// non-regular instead of waiting for a writer.
///
/// The handle is move-only; a moved-from handle is invalid. `size()` is the
/// size reported by the operating system at open time and is only a hint: the
/// file may grow or shrink while it is read.
class read_only_file {
  public:
    read_only_file() noexcept;
    ~read_only_file();

    read_only_file(read_only_file &&other) noexcept;
    auto operator=(read_only_file &&other) noexcept -> read_only_file &;

    read_only_file(const read_only_file &) = delete;
    auto operator=(const read_only_file &) -> read_only_file & = delete;

    /// Open `utf8_path` for reading. Errors: `invalid_argument` for an empty
    /// path; `permission_denied` when `follow_symlink` is false and the final
    /// component is a symbolic link; `io_error` for a missing or unreadable
    /// file and for a non-regular one (directory, FIFO, device).
    [[nodiscard]] static auto open(std::string_view utf8_path,
                                   bool follow_symlink)
        -> result<read_only_file>;

    [[nodiscard]] auto valid() const noexcept -> bool;

    /// Size reported by the operating system when the handle was opened.
    [[nodiscard]] auto size() const noexcept -> std::uint64_t;

    /// Read up to `out.size()` bytes at the current position; returns the
    /// number read (0 at end of file) or `io_error` with the path in the
    /// message.
    [[nodiscard]] auto read(std::span<std::byte> out) -> result<std::size_t>;

  private:
    struct impl;
    explicit read_only_file(std::unique_ptr<impl> state) noexcept;

    std::unique_ptr<impl> impl_{};
};

/// Atomically replace `path` with `data`.
///
/// Implementation: write a unique sibling temporary file, flush it to stable
/// storage (`fsync`/`FlushFileBuffers`), rename it over the destination
/// (POSIX `rename` / Win32 `MoveFileEx(..., MOVEFILE_REPLACE_EXISTING)`), then
/// flush the parent directory entry on POSIX. Readers observe either the old or
/// the new content, never a torn file. Temporary files are cleaned up on error.
///
/// Exception boundary: the temporary name is generated with
/// `std::to_chars` into a fixed buffer (no `std::format`, so name generation
/// itself cannot throw); the only remaining exception source is allocation
/// failure while building the target/temporary path (`std::bad_alloc`), which
/// is a hard-OOM situation the value-based `status` contract cannot represent.
/// Everything upstream of that is reported as `status`.
[[nodiscard]] auto atomic_replace(std::string_view path,
                                  std::span<const std::byte> data) -> status;

/// Create `utf8_path` (and missing parents) as an owner-private directory.
///
/// The final directory is forced to an owner-only permission set, so another
/// local account cannot read or list it even when the parent directory grants
/// broader access: POSIX sets mode `0700` (the default
/// `create_directories` mode is often `0755`); Windows installs an explicit
/// protected DACL that grants only the current user (SYSTEM and Administrators
/// keep access, the counterpart of POSIX root) and stops inheritance from the
/// parent. A parent directory that does not inherit the restriction is not a
/// leak: the final directory's own ACL decides access to its entries. The call
/// is idempotent; an existing directory is restricted as well, and `status`
/// reports the filesystem error.
[[nodiscard]] auto create_private_directory(std::string_view utf8_path)
    -> status;

/// True when `utf8_path` is an existing directory that only its owner can
/// access. POSIX checks the permission bits (no group/other bits); Windows
/// reads the owner and DACL and accepts only allow entries for the owner,
/// SYSTEM and Administrators (deny entries are ignored, a missing or
/// non-protected-with-foreign-grants DACL is rejected). Lets callers/tests
/// assert the owner-private contract without a platform branch.
[[nodiscard]] auto is_owner_private_directory(std::string_view utf8_path)
    -> bool;

/// OS temp directory as a UTF-8 path (respects `TMPDIR` / `GetTempPathW`).
[[nodiscard]] auto temp_directory() -> std::string;

} // namespace refrain::platform::file
