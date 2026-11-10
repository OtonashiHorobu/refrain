// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/contracts/host/platform.hpp>
#include <refrain/core/error.hpp>

#include <cstdint>
#include <span>
#include <string_view>

namespace refrain::host {

// Abstract host platform context behind the sealed `refrain:host.platform`
// service. Handles are host-owned table indices; a handle is single-threaded
// from the caller's perspective (the host may serialize access internally).
class platform_context {
  public:
    platform_context() = default;
    platform_context(const platform_context &) = delete;
    auto operator=(const platform_context &) -> platform_context & = delete;
    platform_context(platform_context &&) = delete;
    auto operator=(platform_context &&) -> platform_context & = delete;
    virtual ~platform_context() = default;

    [[nodiscard]] virtual auto
    file_open(std::string_view path,
              refrain::host::platform::file_open_flags flags)
        -> refrain::result<refrain::host::platform::file> = 0;
    [[nodiscard]] virtual auto file_close(refrain::host::platform::file handle)
        -> refrain::status = 0;
    [[nodiscard]] virtual auto file_read(refrain::host::platform::file handle,
                                         std::uint64_t offset,
                                         std::span<std::uint8_t> buffer)
        -> refrain::result<std::uint64_t> = 0;
    [[nodiscard]] virtual auto file_write(refrain::host::platform::file handle,
                                          std::uint64_t offset,
                                          std::span<const std::uint8_t> data)
        -> refrain::result<std::uint64_t> = 0;
    [[nodiscard]] virtual auto file_size(refrain::host::platform::file handle)
        -> refrain::result<std::uint64_t> = 0;
    [[nodiscard]] virtual auto file_sync(refrain::host::platform::file handle)
        -> refrain::status = 0;
    [[nodiscard]] virtual auto file_exists(std::string_view path) -> bool = 0;

    [[nodiscard]] virtual auto mmap_open(std::string_view path)
        -> refrain::result<refrain::host::platform::map> = 0;
    [[nodiscard]] virtual auto mmap_data(refrain::host::platform::map handle)
        -> refrain::result<std::span<const std::uint8_t>> = 0;
    [[nodiscard]] virtual auto mmap_close(refrain::host::platform::map handle)
        -> refrain::status = 0;

    virtual void crash_annotation(std::string_view key,
                                  std::string_view value) noexcept = 0;
    [[nodiscard]] virtual auto temp_directory() noexcept
        -> std::string_view = 0;
};

} // namespace refrain::host
