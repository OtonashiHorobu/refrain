// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/platform/mmap.hpp>

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace refrain::platform::file {

// Native filesystem primitives that cannot be expressed portably with the C++
// standard library alone. Ordinary reads/writes/directories stay in
// `refrain::core::io` on top of <fstream>/<filesystem>; only the OS-specific
// durability/atomicity steps live here.

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

/// Convert an engine UTF-8 path to the native string accepted by OS APIs
/// (`std::wstring` on Windows, UTF-8 `std::string` on POSIX).
[[nodiscard]] auto native_string(std::string_view utf8_path)
    -> native_path_string;

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
/// POSIX: the final directory is forced to mode `0700` (crash databases contain
/// memory dumps and must not be readable/listable by other local users; the
/// default `create_directories` mode is often `0755`). Windows: default ACLs
/// for now. Idempotent; `status` reports the filesystem error.
[[nodiscard]] auto create_private_directory(std::string_view utf8_path)
    -> status;

/// True when `utf8_path` is an existing directory that only its owner can
/// access. POSIX checks the permission bits (no group/other bits); Windows ACL
/// inspection is not implemented yet, so the implementation only reports
/// existence. Lets callers/tests assert the owner-private contract without a
/// platform branch.
[[nodiscard]] auto is_owner_private_directory(std::string_view utf8_path)
    -> bool;

/// OS temp directory as a UTF-8 path (respects `TMPDIR` / `GetTempPathW`).
[[nodiscard]] auto temp_directory() -> std::string;

} // namespace refrain::platform::file
