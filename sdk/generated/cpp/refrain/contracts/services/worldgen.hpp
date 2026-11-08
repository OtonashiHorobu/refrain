// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 1bc5417b6dec5904

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_WORLDGEN_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_WORLDGEN_HPP

#include <refrain/contracts/services/worldgen_abi.h>
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

namespace refrain::services::worldgen {

enum class worldgen_error : std::uint32_t { // enum 'worldgen_error'
    invalid_config = 1,
    cancelled = 2,
    provider_unavailable = 3,
    internal = 4,
};

/// Generator stages, mirroring the vanilla chunk status pipeline.
enum class gen_stage : std::uint32_t { // enum 'gen_stage'
    empty = 0,
    biomes = 1,
    noise = 2,
    surface = 3,
    features = 4,
    full = 5,
};

using worldgen_config = ::refrain_services_worldgen_worldgen_config;
/// Zero-initialized `worldgen_config` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_worldgen_config() noexcept -> worldgen_config {
    worldgen_config value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
using generate_outcome = ::refrain_services_worldgen_generate_outcome;
/// Zero-initialized `generate_outcome` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_generate_outcome() noexcept -> generate_outcome {
    generate_outcome value{};
    ::refrain::contracts::init_struct(value);
    return value;
}

static_assert(sizeof(worldgen_config) == 40);
static_assert(alignof(worldgen_config) == 8);
static_assert(offsetof(worldgen_config, struct_size) == 0);
static_assert(offsetof(worldgen_config, seed) == 8);
static_assert(offsetof(worldgen_config, view_distance) == 16);
static_assert(offsetof(worldgen_config, flags) == 20);
static_assert(offsetof(worldgen_config, preset) == 24);
static_assert(sizeof(generate_outcome) == 24);
static_assert(alignof(generate_outcome) == 8);
static_assert(offsetof(generate_outcome, struct_size) == 0);
static_assert(offsetof(generate_outcome, stage) == 4);
static_assert(offsetof(generate_outcome, generated_blocks) == 8);
static_assert(offsetof(generate_outcome, elapsed_us) == 16);

inline constexpr std::string_view service_id = "refrain:worldgen";
inline constexpr std::uint32_t abi_major = 1;
inline constexpr std::uint32_t abi_minor = 0;
inline constexpr std::uint32_t abi_patch = 0;
inline constexpr ::refrain::contracts::side_kind side = ::refrain::contracts::side_kind::server;
inline constexpr ::refrain::contracts::thread_kind thread = ::refrain::contracts::thread_kind::worker;
inline constexpr ::refrain::contracts::composition_kind composition = ::refrain::contracts::composition_kind::exclusive;
inline constexpr std::string_view capabilities = "structures,biomes,carvers,noise";
inline constexpr std::string_view required_services = "refrain:core>=1.0.0,refrain:world>=1.0.0,refrain:data.packs>=1.0.0";
inline constexpr std::string_view service_ids[] = {"refrain:worldgen"};

/// Type-safe wrapper around `refrain_services_worldgen_vtable`. The provider (host) owns the table and `self`; this class is a borrowed call surface.
class provider final {
  public:
    provider(const ::refrain_services_worldgen_vtable &table, void *self) noexcept
        : table_{&table}, self_{self} {}

    provider(const provider &) = delete;
    auto operator=(const provider &) -> provider & = delete;

    [[nodiscard]] auto table() const noexcept
        -> const ::refrain_services_worldgen_vtable & {
        return *table_;
    }

    [[nodiscard]] auto self() const noexcept -> void * {
        return self_;
    }

    /// World-level setup; called once before any generation job is submitted.
    [[nodiscard]] auto configure(const ::refrain::services::worldgen::worldgen_config &config) const -> std::expected<void, ::refrain::services::worldgen::worldgen_error> {
        const auto abi_config = ::refrain::contracts::with_struct_size(config);
        if (const auto code = table_->configure(self_, &abi_config); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::worldgen::worldgen_error>(code));
        }
        return {};
    }

    /// Generate all stages up to `full` for one chunk column.
    [[nodiscard]] auto generate_chunk(::refrain::core::gen_ctx ctx, const ::refrain::core::chunk_pos &pos, ::refrain::core::chunk_buffer &buffer) const -> std::expected<::refrain::services::worldgen::generate_outcome, ::refrain::services::worldgen::worldgen_error> {
        const auto abi_pos = ::refrain::contracts::with_struct_size(pos);
        ::refrain::contracts::init_struct(buffer);
        ::refrain_services_worldgen_generate_outcome value{};
        ::refrain::contracts::init_struct(value);
        if (const auto code = table_->generate_chunk(self_, ctx.raw(), &abi_pos, &buffer, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::worldgen::worldgen_error>(code));
        }
        return value;
    }

    /// Advance one chunk to a specific stage (status-driven parallel pipeline).
    [[nodiscard]] auto generate_stage(::refrain::core::gen_ctx ctx, const ::refrain::core::chunk_pos &pos, ::refrain::services::worldgen::gen_stage target, ::refrain::core::chunk_buffer &buffer) const -> std::expected<::refrain::services::worldgen::generate_outcome, ::refrain::services::worldgen::worldgen_error> {
        const auto abi_pos = ::refrain::contracts::with_struct_size(pos);
        ::refrain::contracts::init_struct(buffer);
        ::refrain_services_worldgen_generate_outcome value{};
        ::refrain::contracts::init_struct(value);
        if (const auto code = table_->generate_stage(self_, ctx.raw(), &abi_pos, static_cast<refrain_services_worldgen_gen_stage>(target), &buffer, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::worldgen::worldgen_error>(code));
        }
        return value;
    }

    /// Structure placement query used by the session/chunk-streaming layer.
    [[nodiscard]] auto locate_structure(::refrain::core::gen_ctx ctx, const ::refrain::core::chunk_pos &origin, std::uint32_t structure_id) const -> std::expected<::refrain::core::block_pos, ::refrain::services::worldgen::worldgen_error> {
        const auto abi_origin = ::refrain::contracts::with_struct_size(origin);
        ::refrain_core_block_pos value{};
        ::refrain::contracts::init_struct(value);
        if (const auto code = table_->locate_structure(self_, ctx.raw(), &abi_origin, structure_id, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::worldgen::worldgen_error>(code));
        }
        return value;
    }

  private:
    const ::refrain_services_worldgen_vtable *table_;
    void *self_;
};

inline constexpr std::array<::refrain::core::schedule::task_descriptor, 0>
    schedule_tasks{};

} // namespace refrain::services::worldgen

#endif // REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_WORLDGEN_HPP
