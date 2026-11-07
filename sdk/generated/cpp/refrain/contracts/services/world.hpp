// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: eca131f75aa31435

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
};

using world_config = ::refrain_services_world_world_config;
/// Zero-initialized `world_config` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_world_config() noexcept -> world_config {
    world_config value{};
    ::refrain::contracts::init_struct(value);
    return value;
}

static_assert(sizeof(world_config) == 32);
static_assert(alignof(world_config) == 8);
static_assert(offsetof(world_config, struct_size) == 0);
static_assert(offsetof(world_config, world_path) == 8);
static_assert(offsetof(world_config, seed) == 24);

inline constexpr std::string_view service_id = "refrain:world";
inline constexpr std::uint32_t abi_major = 1;
inline constexpr std::uint32_t abi_minor = 0;
inline constexpr std::uint32_t abi_patch = 0;
inline constexpr ::refrain::contracts::side_kind side = ::refrain::contracts::side_kind::server;
inline constexpr ::refrain::contracts::thread_kind thread = ::refrain::contracts::thread_kind::sim;
inline constexpr ::refrain::contracts::composition_kind composition = ::refrain::contracts::composition_kind::exclusive;
inline constexpr std::string_view capabilities = "lighting,heightmaps,block_entities";
inline constexpr std::string_view required_services = "refrain:core>=1.0.0,refrain:data.packs>=1.0.0";
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

    /// Open the singleton world. The provider owns it until
    /// `close_world`; the server hosts exactly one world.
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

  private:
    const ::refrain_services_world_vtable *table_;
    void *self_;
};

inline constexpr std::array<::refrain::core::schedule::task_descriptor, 0>
    schedule_tasks{};

} // namespace refrain::services::world

#endif // REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_WORLD_HPP
