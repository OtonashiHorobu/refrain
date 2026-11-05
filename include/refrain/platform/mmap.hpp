// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace refrain::platform {

// Read-only memory mapping of a whole file. This is the platform boundary
// for `mmap`/`munmap` (POSIX) and `CreateFileMappingW`/`MapViewOfFile`/
// `UnmapViewOfFile` (Windows): no other layer may include <sys/mman.h> or
// <windows.h>.
//
// The mapping is owned by the handle and released in its destructor; the span
// returned by `data()` is valid exactly as long as the handle (or a handle
// moved from it) is alive. An empty regular file is a valid mapping: `size()`
// is 0 and `data()` is an empty span. Non-regular files (directories, devices,
// FIFOs) and missing/unreadable files are `io_error`.
//
// PIMPL: the OS-specific state stays in `src/platform/{posix,windows}/mmap.cpp`
// so this header never includes an OS header. The class is move-only.
class mapped_file {
  public:
    mapped_file() noexcept;
    mapped_file(const mapped_file &) = delete;
    auto operator=(const mapped_file &) -> mapped_file & = delete;
    mapped_file(mapped_file &&other) noexcept;
    auto operator=(mapped_file &&other) noexcept -> mapped_file &;
    ~mapped_file();

    /// Map `path` read-only. Empty paths are `invalid_argument`; a missing,
    /// unreadable, non-regular or unmappable file is `io_error`.
    [[nodiscard]] static auto open(std::string_view path)
        -> result<mapped_file>;

    /// True when this handle owns a mapping (including a valid empty file).
    [[nodiscard]] auto valid() const noexcept -> bool;

    /// Read-only view of the mapped bytes (empty for an empty file or an
    /// invalid handle). Do not outlive this handle.
    [[nodiscard]] auto data() const noexcept -> std::span<const std::byte>;

    /// Mapped size in bytes (0 for an invalid handle or an empty file).
    [[nodiscard]] auto size() const noexcept -> std::uint64_t;

  private:
    struct impl;
    explicit mapped_file(std::unique_ptr<impl> state) noexcept;

    std::unique_ptr<impl> impl_{};
};

} // namespace refrain::platform
