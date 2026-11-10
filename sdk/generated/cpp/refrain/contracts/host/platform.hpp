// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 04d13b3ba7228746

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_HOST_PLATFORM_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_HOST_PLATFORM_HPP

#include <refrain/contracts/host/platform_abi.h>
#include <refrain/contracts/abi.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <refrain/core/error.hpp>
#include <refrain/core/schedule.hpp>

namespace refrain::host::platform {

enum class platform_error : std::uint32_t { // enum 'platform_error'
    invalid_argument = 1,
    not_found = 2,
    permission_denied = 3,
    io_error = 4,
    out_of_memory = 5,
    unsupported = 6,
    already_exists = 7,
    read_only = 8,
    limit_exceeded = 9,
    internal = 10,
};

enum class file_open_flags : std::uint32_t { // flags 'file_open_flags'
    read_only = 1,
    read_write = 2,
    create = 4,
    exclusive = 8,
    truncate = 16,
    no_follow = 32,
};

constexpr auto operator|(file_open_flags lhs, file_open_flags rhs) noexcept -> file_open_flags {
    return static_cast<file_open_flags>(static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}
constexpr auto operator&(file_open_flags lhs, file_open_flags rhs) noexcept -> file_open_flags {
    return static_cast<file_open_flags>(static_cast<std::uint32_t>(lhs) & static_cast<std::uint32_t>(rhs));
}
constexpr auto operator~(file_open_flags value) noexcept -> file_open_flags {
    return static_cast<file_open_flags>(~static_cast<std::uint32_t>(value));
}

namespace detail {
struct file_tag;
struct map_tag;
} // namespace detail

using file = ::refrain::contracts::handle<detail::file_tag>;
using map = ::refrain::contracts::handle<detail::map_tag>;



inline constexpr std::string_view service_id = "refrain:host.platform";
inline constexpr std::uint32_t abi_major = 1;
inline constexpr std::uint32_t abi_minor = 0;
inline constexpr std::uint32_t abi_patch = 0;
inline constexpr ::refrain::contracts::side_kind side = ::refrain::contracts::side_kind::both;
inline constexpr ::refrain::contracts::thread_kind thread = ::refrain::contracts::thread_kind::any;
inline constexpr ::refrain::contracts::composition_kind composition = ::refrain::contracts::composition_kind::sealed;
inline constexpr std::string_view capabilities = "file,mmap,crash";
inline constexpr std::string_view required_services = "refrain:core>=1.0.0";
inline constexpr std::string_view service_ids[] = {"refrain:host.platform"};

/// Type-safe wrapper around `refrain_host_platform_vtable`. The provider (host) owns the table and `self`; this class is a borrowed call surface.
class provider final {
  public:
    provider(const ::refrain_host_platform_vtable &table, void *self) noexcept
        : table_{&table}, self_{self} {}

    provider(const provider &) = delete;
    auto operator=(const provider &) -> provider & = delete;

    [[nodiscard]] auto table() const noexcept
        -> const ::refrain_host_platform_vtable & {
        return *table_;
    }

    [[nodiscard]] auto self() const noexcept -> void * {
        return self_;
    }

    /// Open a host file. Exactly one of `read_only`/`read_write` must be set.
    /// `no_follow` refuses a symlink at the final path component.
    [[nodiscard]] auto file_open(std::string_view path, ::refrain::host::platform::file_open_flags flags) const -> std::expected<::refrain::host::platform::file, ::refrain::host::platform::platform_error> {
        ::refrain_host_platform_handle_file value{};
        if (const auto code = table_->file_open(self_, ::refrain::contracts::to_borrowed(path), static_cast<refrain_host_platform_file_open_flags>(flags), &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::host::platform::platform_error>(code));
        }
        return ::refrain::host::platform::file{value};
    }

    [[nodiscard]] auto file_close(::refrain::host::platform::file handle) const -> std::expected<void, ::refrain::host::platform::platform_error> {
        if (const auto code = table_->file_close(self_, handle.raw()); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::host::platform::platform_error>(code));
        }
        return {};
    }

    /// Read exactly `buffer.len` bytes at `offset` into the caller's buffer.
    /// Returns the byte count (the whole buffer) on success; a short read is
    /// `io_error`.
    [[nodiscard]] auto file_read(::refrain::host::platform::file handle, std::uint64_t offset, std::span<std::uint8_t> buffer) const -> std::expected<std::uint64_t, ::refrain::host::platform::platform_error> {
        ::uint64_t value{};
        if (const auto code = table_->file_read(self_, handle.raw(), offset, ::refrain::contracts::to_mut(buffer), &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::host::platform::platform_error>(code));
        }
        return static_cast<std::uint64_t>(value);
    }

    /// Write `data` at `offset`, extending the file as needed.
    [[nodiscard]] auto file_write(::refrain::host::platform::file handle, std::uint64_t offset, std::span<const std::uint8_t> data) const -> std::expected<std::uint64_t, ::refrain::host::platform::platform_error> {
        ::uint64_t value{};
        if (const auto code = table_->file_write(self_, handle.raw(), offset, ::refrain::contracts::to_borrowed(data), &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::host::platform::platform_error>(code));
        }
        return static_cast<std::uint64_t>(value);
    }

    [[nodiscard]] auto file_size(::refrain::host::platform::file handle) const -> std::expected<std::uint64_t, ::refrain::host::platform::platform_error> {
        ::uint64_t value{};
        if (const auto code = table_->file_size(self_, handle.raw(), &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::host::platform::platform_error>(code));
        }
        return static_cast<std::uint64_t>(value);
    }

    /// Flush data and length to stable storage.
    [[nodiscard]] auto file_sync(::refrain::host::platform::file handle) const -> std::expected<void, ::refrain::host::platform::platform_error> {
        if (const auto code = table_->file_sync(self_, handle.raw()); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::host::platform::platform_error>(code));
        }
        return {};
    }

    [[nodiscard]] auto file_exists(std::string_view path) const -> bool {
        return (table_->file_exists(self_, ::refrain::contracts::to_borrowed(path)) != 0);
    }

    /// Map a file read-only. The returned bytes from `mmap_data` stay valid
    /// until `mmap_close`.
    [[nodiscard]] auto mmap_open(std::string_view path) const -> std::expected<::refrain::host::platform::map, ::refrain::host::platform::platform_error> {
        ::refrain_host_platform_handle_map value{};
        if (const auto code = table_->mmap_open(self_, ::refrain::contracts::to_borrowed(path), &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::host::platform::platform_error>(code));
        }
        return ::refrain::host::platform::map{value};
    }

    [[nodiscard]] auto mmap_data(::refrain::host::platform::map handle) const -> std::expected<std::span<const std::uint8_t>, ::refrain::host::platform::platform_error> {
        ::refrain_bytes value{};
        if (const auto code = table_->mmap_data(self_, handle.raw(), &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::host::platform::platform_error>(code));
        }
        return ::refrain::contracts::as_bytes(value);
    }

    [[nodiscard]] auto mmap_close(::refrain::host::platform::map handle) const -> std::expected<void, ::refrain::host::platform::platform_error> {
        if (const auto code = table_->mmap_close(self_, handle.raw()); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::host::platform::platform_error>(code));
        }
        return {};
    }

    /// Attach a key/value to the next crash report. Never fails; oversized or
    /// invalid annotations are dropped by the crash reporter.
    auto crash_annotation(std::string_view key, std::string_view value) const -> void {
        table_->crash_annotation(self_, ::refrain::contracts::to_borrowed(key), ::refrain::contracts::to_borrowed(value));
    }

    /// Host temporary directory as a borrowed UTF-8 string.
    [[nodiscard]] auto temp_directory() const -> std::string_view {
        return ::refrain::contracts::as_string_view(table_->temp_directory(self_));
    }

  private:
    const ::refrain_host_platform_vtable *table_;
    void *self_;
};

inline constexpr std::array<::refrain::core::schedule::task_descriptor, 0>
    schedule_tasks{};

} // namespace refrain::host::platform

#endif // REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_HOST_PLATFORM_HPP
