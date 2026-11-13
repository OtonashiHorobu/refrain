// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 9acb7bb41c6b4563

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_WORLD_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_WORLD_HPP

#include <refrain/contracts/services/world_abi.h>
#include <refrain/contracts/abi.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <refrain/contracts/core.hpp>
#include <refrain/contracts/services/storage.hpp>
#include <refrain/core/error.hpp>
#include <refrain/core/schedule.hpp>

namespace refrain::services::world {

enum class world_error : std::uint32_t { // enum 'world_error'
    chunk_not_loaded = 1,
    out_of_bounds = 2,
    not_implemented = 3,
    internal = 4,
    already_open = 5,
    open_failed = 6,
    storage_unavailable = 7,
};

using world_config = ::refrain_services_world_world_config;
/// Zero-initialized `world_config` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_world_config() noexcept -> world_config {
    world_config value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
/// Read-only projection of the world's level record. A provider resolves it
/// from the store it was handed; nothing here lets a provider open the level
/// directory itself.
using level_info = ::refrain_services_world_level_info;
/// Zero-initialized `level_info` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_level_info() noexcept -> level_info {
    level_info value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
/// Boot projection of the world level. `has_chunks` is true once the store
/// holds a chunk, which tells the runtime that the spawn area does not need to
/// be filled.
using level_projection = ::refrain_services_world_level_projection;
/// Zero-initialized `level_projection` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_level_projection() noexcept -> level_projection {
    level_projection value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
/// Distance-adaptive zstd settings a provider applies to the chunks it stores.
/// Levels are zstd compression levels; radii are horizontal chunk distances
/// from the player. A chunk inside `near_radius` is stored with `near_level`,
/// one inside `mid_radius` with `mid_level`, and everything beyond with
/// `far_level`.
using zstd_policy = ::refrain_services_world_zstd_policy;
/// Zero-initialized `zstd_policy` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_zstd_policy() noexcept -> zstd_policy {
    zstd_policy value{};
    ::refrain::contracts::init_struct(value);
    return value;
}

static_assert(sizeof(world_config) == 72);
static_assert(alignof(world_config) == 8);
static_assert(offsetof(world_config, struct_size) == 0);
static_assert(offsetof(world_config, storage) == 8);
static_assert(offsetof(world_config, seed) == 64);
static_assert(sizeof(level_info) == 72);
static_assert(alignof(level_info) == 8);
static_assert(offsetof(level_info, struct_size) == 0);
static_assert(offsetof(level_info, data_version) == 4);
static_assert(offsetof(level_info, seed) == 8);
static_assert(offsetof(level_info, spawn_x) == 16);
static_assert(offsetof(level_info, spawn_y) == 20);
static_assert(offsetof(level_info, spawn_z) == 24);
static_assert(offsetof(level_info, time) == 32);
static_assert(offsetof(level_info, level_name) == 40);
static_assert(offsetof(level_info, game_mode) == 56);
static_assert(offsetof(level_info, hardcore) == 60);
static_assert(offsetof(level_info, difficulty) == 64);
static_assert(offsetof(level_info, difficulty_locked) == 68);
static_assert(sizeof(level_projection) == 88);
static_assert(alignof(level_projection) == 8);
static_assert(offsetof(level_projection, struct_size) == 0);
static_assert(offsetof(level_projection, level) == 8);
static_assert(offsetof(level_projection, has_chunks) == 80);
static_assert(sizeof(zstd_policy) == 24);
static_assert(alignof(zstd_policy) == 4);
static_assert(offsetof(zstd_policy, struct_size) == 0);
static_assert(offsetof(zstd_policy, near_level) == 4);
static_assert(offsetof(zstd_policy, mid_level) == 8);
static_assert(offsetof(zstd_policy, far_level) == 12);
static_assert(offsetof(zstd_policy, near_radius) == 16);
static_assert(offsetof(zstd_policy, mid_radius) == 20);

inline constexpr std::string_view service_id = "refrain:world";
inline constexpr std::uint32_t abi_major = 1;
inline constexpr std::uint32_t abi_minor = 2;
inline constexpr std::uint32_t abi_patch = 0;
inline constexpr ::refrain::contracts::side_kind side = ::refrain::contracts::side_kind::server;
inline constexpr ::refrain::contracts::thread_kind thread = ::refrain::contracts::thread_kind::sim;
inline constexpr ::refrain::contracts::composition_kind composition = ::refrain::contracts::composition_kind::exclusive;
inline constexpr std::string_view capabilities = "lighting,heightmaps,block_entities";
inline constexpr std::string_view required_services = "refrain:core>=1.0.0,refrain:data.packs>=1.0.0,refrain:storage>=1.0.0";
inline constexpr std::string_view service_ids[] = {"refrain:world"};

/// Type-safe wrapper around `refrain_services_world_vtable`. The provider (host) owns the table and `self`; this class is a borrowed call surface.
class provider final {
  public:
    provider(const ::refrain_services_world_vtable &table, void *self) noexcept
        : table_{&table}, self_{self} {}

    provider(const provider &) = delete;
    auto operator=(const provider &) -> provider & = delete;

    [[nodiscard]] auto table() const noexcept
        -> const ::refrain_services_world_vtable & {
        return *table_;
    }

    [[nodiscard]] auto self() const noexcept -> void * {
        return self_;
    }

    /// Open the singleton world over the given store. The provider owns the
    /// world until `close_world`; the server hosts exactly one world.
    [[nodiscard]] auto open_world(const ::refrain::services::world::world_config &config) const -> std::expected<void, ::refrain::services::world::world_error> {
        const auto abi_config = ::refrain::contracts::with_struct_size(config);
        if (const auto code = table_->open_world(self_, &abi_config); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::world::world_error>(code));
        }
        return {};
    }

    [[nodiscard]] auto close_world() const -> std::expected<void, ::refrain::services::world::world_error> {
        if (const auto code = table_->close_world(self_); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::world::world_error>(code));
        }
        return {};
    }

    /// Borrowed view of a loaded chunk; invalid after the chunk unloads.
    [[nodiscard]] auto chunk_at(const ::refrain::core::chunk_pos &pos) const -> std::expected<::refrain::core::chunk_view, ::refrain::services::world::world_error> {
        const auto abi_pos = ::refrain::contracts::with_struct_size(pos);
        ::refrain_core_chunk_view value{};
        ::refrain::contracts::init_struct(value);
        if (const auto code = table_->chunk_at(self_, &abi_pos, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::world::world_error>(code));
        }
        return value;
    }

    /// Block state id at `pos`; 0 is the provider's air id.
    [[nodiscard]] auto block_at(const ::refrain::core::block_pos &pos) const -> std::uint32_t {
        const auto abi_pos = ::refrain::contracts::with_struct_size(pos);
        return static_cast<std::uint32_t>(table_->block_at(self_, &abi_pos));
    }

    /// Maximum motion-blocking height at the column, in world Y coordinates.
    [[nodiscard]] auto height_at(const ::refrain::core::block_pos &pos) const -> std::int32_t {
        const auto abi_pos = ::refrain::contracts::with_struct_size(pos);
        return static_cast<std::int32_t>(table_->height_at(self_, &abi_pos));
    }

    /// True once the chunk reached the provider's "full" status.
    [[nodiscard]] auto is_chunk_loaded(const ::refrain::core::chunk_pos &pos) const -> bool {
        const auto abi_pos = ::refrain::contracts::with_struct_size(pos);
        return (table_->is_chunk_loaded(self_, &abi_pos) != 0);
    }

    /// Batched block mutation; returns the number of applied changes. Changes
    /// are committed at the next tick boundary, not immediately.
    [[nodiscard]] auto apply_block_changes(std::span<const ::refrain::core::block_change> changes) const -> std::expected<std::uint32_t, ::refrain::services::world::world_error> {
        ::uint32_t value{};
        if (const auto code = table_->apply_block_changes(self_, refrain_core_slice_block_change{changes.data(), static_cast<std::uint64_t>(changes.size())}, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::world::world_error>(code));
        }
        return static_cast<std::uint32_t>(value);
    }

    /// World tick counter (monotonic, survives save/load).
    [[nodiscard]] auto world_tick() const -> std::int64_t {
        return static_cast<std::int64_t>(table_->world_tick(self_));
    }

    /// Read-only boot projection: the level record and whether the world
    /// already holds a chunk. The provider answers from the store it was
    /// opened over; the level directory stays private to the storage provider.
    [[nodiscard]] auto level() const -> std::expected<::refrain::services::world::level_projection, ::refrain::services::world::world_error> {
        ::refrain_services_world_level_projection value{};
        ::refrain::contracts::init_struct(value);
        if (const auto code = table_->level(self_, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::world::world_error>(code));
        }
        return value;
    }

    /// Optional. Effective zstd level the provider stores chunks with; a
    /// provider that does not apply compression settings leaves the slot
    /// empty.
    [[nodiscard]] auto zstd_level() const -> std::int32_t {
        return static_cast<std::int32_t>(table_->zstd_level(self_));
    }

    /// Optional. Replace the zstd level chunks are stored with. A provider
    /// that does not apply compression settings leaves the slot empty, which
    /// means the setting does not apply to it rather than that the call
    /// failed.
    [[nodiscard]] auto set_zstd_level(std::int32_t level) const -> std::expected<void, ::refrain::services::world::world_error> {
        if (const auto code = table_->set_zstd_level(self_, level); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::world::world_error>(code));
        }
        return {};
    }

    /// Optional. Replace the distance-adaptive zstd policy. A provider that
    /// does not apply compression settings leaves the slot empty, which means
    /// the setting does not apply to it rather than that the call failed.
    [[nodiscard]] auto set_zstd_policy(const ::refrain::services::world::zstd_policy &policy) const -> std::expected<void, ::refrain::services::world::world_error> {
        const auto abi_policy = ::refrain::contracts::with_struct_size(policy);
        if (const auto code = table_->set_zstd_policy(self_, &abi_policy); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::world::world_error>(code));
        }
        return {};
    }

  private:
    const ::refrain_services_world_vtable *table_;
    void *self_;
};

inline constexpr std::array<::refrain::core::schedule::task_descriptor, 0>
    schedule_tasks{};

} // namespace refrain::services::world

#endif // REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_WORLD_HPP
