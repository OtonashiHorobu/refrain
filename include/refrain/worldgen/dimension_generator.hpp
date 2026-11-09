// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Terrain generation of one dimension: biomes, fill and surface, with the
// generation geometry, biome source, default blocks and material rule that the
// dimension profile selects. The dimension also selects the positional random
// algorithm of its noise settings, which is the legacy source for the nether
// and the end. The nether and the end have no carver stage; carvers belong to
// the overworld pipeline that owns the carver data.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/worldgen/biome/biome_climate.hpp>
#include <refrain/worldgen/biome/biome_source.hpp>
#include <refrain/worldgen/biome/dimension_biome_source.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/density/density_router.hpp>
#include <refrain/worldgen/dimension.hpp>
#include <refrain/worldgen/terrain/material.hpp>
#include <refrain/worldgen/terrain/surface.hpp>
#include <refrain/worldgen/terrain/terrain.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>

namespace refrain::worldgen {

namespace carver {
struct carver_biomes;
} // namespace carver

/// The terrain stages after biome selection. `fill_context` carries the
/// structure beardifier the fill samples; surface and carvers run over the
/// filled chunk. A caller that needs the reference worlds of the structure
/// oracle disables the surface pass and passes no carvers.
struct terrain_stages {
    const density::sampler_context *fill_context{nullptr};
    const carver::carver_biomes *carvers{nullptr};
    bool surface{true};
};

struct dimension_generator_config {
    /// The datapack `data` directory (`<version>/data`).
    std::string data_root{};
    dimension_kind kind{dimension_kind::overworld};
    jlong seed{0};
    /// Fixed biome key. When set every biome cell is this biome, matching a
    /// reference world whose preset declares a fixed biome source.
    std::string fixed_biome{};
};

class dimension_generator {
  public:
    static auto create(dimension_generator_config config)
        -> result<std::unique_ptr<dimension_generator>>;

    /// Generates one chunk to `terrain` status: biome cells, fill, surface.
    [[nodiscard]] auto generate(const chunk_pos &pos)
        -> result<std::unique_ptr<proto_chunk>>;

    /// Runs the terrain stages with explicit options. The chunk reaches
    /// `terrain` status even when the surface pass is disabled, so its final
    /// heightmaps cover every block written by the stages that did run.
    [[nodiscard]] auto generate_terrain(const chunk_pos &pos,
                                        const terrain_stages &stages)
        -> result<std::unique_ptr<proto_chunk>>;

    /// Fills one chunk without biomes, a beardifier, surface or carvers and
    /// leaves it at `biomes` status. The structure starts stage reads the
    /// worldgen heightmaps of these columns.
    [[nodiscard]] auto generate_height_column(const chunk_pos &pos)
        -> result<std::unique_ptr<proto_chunk>>;

    /// Clears the multi-noise search cache. A generator that must produce the
    /// same chunk regardless of which chunks ran before resets it before
    /// selecting biomes.
    void reset_biome_search();

    /// Biome of a quart cell through the dimension source.
    [[nodiscard]] auto biome_at_quart(jint quart_x, jint quart_y, jint quart_z)
        -> std::uint16_t;
    [[nodiscard]] auto biome_name(std::uint16_t id) const -> std::string_view;
    /// Temperature of `biome` at a world position; null for an unknown biome.
    [[nodiscard]] auto biome_temperature(std::string_view biome, jint x, jint y,
                                         jint z) const -> std::optional<float>;

    [[nodiscard]] auto profile() const noexcept -> const dimension_profile & {
        return *profile_;
    }
    [[nodiscard]] auto settings() const noexcept
        -> const density::noise_settings & {
        return settings_;
    }
    [[nodiscard]] auto biome_source() noexcept -> dimension_biome_source & {
        return biome_source_;
    }
    [[nodiscard]] auto sampler() noexcept -> climate_sampler & {
        return sampler_;
    }
    [[nodiscard]] auto states() noexcept -> block_state_registry & {
        return states_;
    }
    /// Material-rule document id (`minecraft:nether`).
    [[nodiscard]] auto material_rule() const noexcept -> std::string_view {
        return settings_.material_rule;
    }

    /// Generation geometry: the noise settings clamped to the level height.
    /// The nether and the end fill 128 blocks inside a 256-block level.
    [[nodiscard]] auto generation_min_y() const noexcept -> jint {
        return generation_min_y_;
    }
    [[nodiscard]] auto generation_height() const noexcept -> jint {
        return generation_height_;
    }

  private:
    dimension_generator(dimension_generator_config config,
                        const dimension_profile &profile,
                        density::noise_settings settings,
                        dimension_biome_source biome_source,
                        block_state_registry states,
                        surface::material_documents documents,
                        surface::rule material_rule, biome_climates climates,
                        jint generation_min_y, jint generation_height);

    /// An aquifer borrows its positional random factory, so both live
    /// together for the whole terrain pass.
    struct aquifer_holder {
        std::unique_ptr<positional_random_factory> factory{};
        std::unique_ptr<terrain::aquifer> aquifer{};
    };
    [[nodiscard]] auto
    make_aquifer(jint chunk_x, jint chunk_z,
                 const density::sampler_context &context) const
        -> aquifer_holder;
    [[nodiscard]] auto make_chunk(const chunk_pos &pos) const
        -> std::unique_ptr<proto_chunk>;

    dimension_generator_config config_{};
    const dimension_profile *profile_{nullptr};
    density::noise_settings settings_{};
    dimension_biome_source biome_source_;
    block_state_registry states_{};
    surface::block_states rule_states_{};
    surface::material_documents documents_{};
    surface::rule material_rule_{};
    biome_climates climates_{};
    biome_temperatures temperatures_{};
    surface::noise_registry noises_;
    status noises_status_{};
    std::unique_ptr<positional_random_factory> root_factory_{};
    density::sampler_context sampler_context_{};
    climate_sampler sampler_;
    std::set<std::string> possible_biomes_{};
    jint generation_min_y_{0};
    jint generation_height_{0};
    block_state_id air_state_{0};
    block_state_id default_block_state_{0};
    block_state_id default_fluid_state_{0};
    block_state_id lava_state_{0};
    std::unordered_map<std::string, std::unique_ptr<positional_random_factory>>
        factories_{};
};

} // namespace refrain::worldgen
