// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: dde5b4dbceca0c8c

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_STORAGE_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_STORAGE_HPP

#include <refrain/contracts/services/storage_abi.h>
#include <refrain/contracts/abi.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <refrain/contracts/core.hpp>
#include <refrain/core/error.hpp>
#include <refrain/core/schedule.hpp>

namespace refrain::services::storage {

enum class storage_error : std::uint32_t { // enum 'storage_error'
    open_failed = 1,
    not_found = 2,
    corrupt_data = 3,
    io_error = 4,
    version_mismatch = 5,
    read_only = 6,
};

using world_open_config = ::refrain_services_storage_world_open_config;
/// Zero-initialized `world_open_config` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_world_open_config() noexcept -> world_open_config {
    world_open_config value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
using chunk_key = ::refrain_services_storage_chunk_key;
/// Zero-initialized `chunk_key` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_chunk_key() noexcept -> chunk_key {
    chunk_key value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
using chunk_read_result = ::refrain_services_storage_chunk_read_result;
/// Zero-initialized `chunk_read_result` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_chunk_read_result() noexcept -> chunk_read_result {
    chunk_read_result value{};
    ::refrain::contracts::init_struct(value);
    return value;
}

static_assert(sizeof(world_open_config) == 48);
static_assert(alignof(world_open_config) == 8);
static_assert(offsetof(world_open_config, struct_size) == 0);
static_assert(offsetof(world_open_config, world_path) == 8);
static_assert(offsetof(world_open_config, gamepack_id) == 24);
static_assert(offsetof(world_open_config, flags) == 40);
static_assert(sizeof(chunk_key) == 20);
static_assert(alignof(chunk_key) == 4);
static_assert(offsetof(chunk_key, struct_size) == 0);
static_assert(offsetof(chunk_key, pos) == 4);
static_assert(offsetof(chunk_key, format_version) == 16);
static_assert(sizeof(chunk_read_result) == 24);
static_assert(alignof(chunk_read_result) == 8);
static_assert(offsetof(chunk_read_result, struct_size) == 0);
static_assert(offsetof(chunk_read_result, bytes_read) == 4);
static_assert(offsetof(chunk_read_result, format_version) == 8);
static_assert(offsetof(chunk_read_result, record_version) == 16);

inline constexpr std::string_view service_id = "refrain:storage";
inline constexpr std::uint32_t abi_major = 1;
inline constexpr std::uint32_t abi_minor = 0;
inline constexpr std::uint32_t abi_patch = 0;
inline constexpr ::refrain::contracts::side_kind side = ::refrain::contracts::side_kind::server;
inline constexpr ::refrain::contracts::thread_kind thread = ::refrain::contracts::thread_kind::io;
inline constexpr ::refrain::contracts::composition_kind composition = ::refrain::contracts::composition_kind::exclusive;
inline constexpr std::string_view capabilities = "transactions,compaction,anvil-import,anvil-export";
inline constexpr std::string_view required_services = "refrain:core>=1.0.0";
inline constexpr std::string_view service_ids[] = {"refrain:storage"};

/// Type-safe wrapper around `refrain_services_storage_vtable`. The provider (host) owns the table and `self`; this class is a borrowed call surface.
class provider final {
  public:
    provider(const ::refrain_services_storage_vtable &table, void *self) noexcept
        : table_{&table}, self_{self} {}

    provider(const provider &) = delete;
    auto operator=(const provider &) -> provider & = delete;

    [[nodiscard]] auto table() const noexcept
        -> const ::refrain_services_storage_vtable & {
        return *table_;
    }

    [[nodiscard]] auto self() const noexcept -> void * {
        return self_;
    }

    /// Open (or create) a world store. The returned handle is owned by the
    /// caller and released with close_world.
    [[nodiscard]] auto open_world(const ::refrain::services::storage::world_open_config &config) const -> std::expected<::refrain::core::storage_world, ::refrain::services::storage::storage_error> {
        const auto abi_config = ::refrain::contracts::with_struct_size(config);
        ::refrain_core_handle_storage_world value{};
        if (const auto code = table_->open_world(self_, &abi_config, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::storage::storage_error>(code));
        }
        return ::refrain::core::storage_world{value};
    }

    [[nodiscard]] auto close_world(::refrain::core::storage_world world) const -> std::expected<void, ::refrain::services::storage::storage_error> {
        if (const auto code = table_->close_world(self_, world.raw()); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::storage::storage_error>(code));
        }
        return {};
    }

    /// Read one chunk record into a caller provided buffer. The buffer's
    /// word_count is updated to the actual size.
    [[nodiscard]] auto read_chunk(::refrain::core::storage_world world, const ::refrain::services::storage::chunk_key &key, ::refrain::core::chunk_buffer &buffer) const -> std::expected<::refrain::services::storage::chunk_read_result, ::refrain::services::storage::storage_error> {
        const auto abi_key = ::refrain::contracts::with_struct_size(key);
        ::refrain::contracts::init_struct(buffer);
        ::refrain_services_storage_chunk_read_result value{};
        ::refrain::contracts::init_struct(value);
        if (const auto code = table_->read_chunk(self_, world.raw(), &abi_key, &buffer, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::storage::storage_error>(code));
        }
        return value;
    }

    /// Write one chunk record; the payload is borrowed for the duration of
    /// the call (the provider copies before returning).
    [[nodiscard]] auto write_chunk(::refrain::core::storage_world world, const ::refrain::services::storage::chunk_key &key, std::span<const std::uint8_t> payload) const -> std::expected<void, ::refrain::services::storage::storage_error> {
        const auto abi_key = ::refrain::contracts::with_struct_size(key);
        if (const auto code = table_->write_chunk(self_, world.raw(), &abi_key, ::refrain::contracts::to_borrowed(payload)); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::storage::storage_error>(code));
        }
        return {};
    }

    /// Flush pending writes (group commit). Called at tick boundaries.
    [[nodiscard]] auto sync(::refrain::core::storage_world world) const -> std::expected<void, ::refrain::services::storage::storage_error> {
        if (const auto code = table_->sync(self_, world.raw()); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::storage::storage_error>(code));
        }
        return {};
    }

    [[nodiscard]] auto is_open(::refrain::core::storage_world world) const -> bool {
        return (table_->is_open(self_, world.raw()) != 0);
    }

  private:
    const ::refrain_services_storage_vtable *table_;
    void *self_;
};

inline constexpr std::array<::refrain::core::schedule::task_descriptor, 0>
    schedule_tasks{};

} // namespace refrain::services::storage

#endif // REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_STORAGE_HPP
