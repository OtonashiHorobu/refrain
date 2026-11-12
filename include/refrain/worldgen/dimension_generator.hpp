// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Terrain generation of one dimension: biomes, fill, surface and carvers, with
// the generation geometry, biome source, default blocks and material rule that
// the dimension profile selects. The dimension also selects the positional
// random algorithm of its noise settings, which is the legacy source for the
// nether and the end. The carver stage runs for the dimensions whose profile
// declares carvers; the end has none.

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
#include <string>
#include <string_view>
#include <unordered_map>

namespace refrain::worldgen {

namespace carver {
struct carver_biomes;
} // namespace carver

class stage_profile;

/// The terrain stages after biome selection. `fill_context` carries the
/// structure beardifier the fill samples; surface and carvers run over the
/// filled chunk. `carvers` names the table to run and null skips the stage;
/// `generate` passes the dimension's table. A caller that needs the reference
/// worlds of the structure oracle disables the surface pass and passes no
/// carvers.
struct terrain_stages {
    const density::sampler_context *fill_context{nullptr};
    const carver::carver_biomes *carvers{nullptr};
    bool surface{true};
    /// Records the wall-clock time of each terrain stage when non-null. The
    /// profile never affects the generated chunk.
    stage_profile *profile{nullptr};
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

/// Loaded dimension data that does not change during generation: the noise
/// settings and their density documents, material documents and rule,
/// climates, noise functions, block tags and possible biomes. Workers of one
/// configuration share one instance. The biome source, the block-state
/// registry, the surface state cache, the positional factories and the biome
/// search cache stay per generator instance.
struct dimension_shared_data;

class dimension_generator {
  public:
    /// Loads the immutable half of a dimension once. `create` calls it when it
    /// has no shared instance.
    [[nodiscard]] static auto load_shared(dimension_generator_config config)
        -> result<std::shared_ptr<const dimension_shared_data>>;

    static auto create(dimension_generator_config config)
        -> result<std::unique_ptr<dimension_generator>>;

    /// Builds a generator over already loaded immutable data. Block tags are
    /// applied to a fresh registry, so the registry and the biome search are
    /// still per generator.
    static auto create(dimension_generator_config config,
                       std::shared_ptr<const dimension_shared_data> shared)
        -> result<std::unique_ptr<dimension_generator>>;

    /// Generates one chunk to `terrain` status: biome cells, fill, surface and
    /// the dimension's carvers.
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

    /// The dimension's loaded carvers, or null when its profile declares no
    /// carver stage. The returned table is owned by the shared data and lives
    /// as long as this generator.
    [[nodiscard]] auto carvers() const noexcept
        -> const carver::carver_biomes *;

    [[nodiscard]] auto profile() const noexcept -> const dimension_profile & {
        return *profile_;
    }
    [[nodiscard]] auto settings() const noexcept
        -> const density::noise_settings &;
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
    [[nodiscard]] auto material_rule() const noexcept -> std::string_view;

    /// Generation geometry: the noise settings clamped to the level height.
    /// The nether and the end fill 128 blocks inside a 256-block level.
    [[nodiscard]] auto generation_min_y() const noexcept -> jint;
    [[nodiscard]] auto generation_height() const noexcept -> jint;

  private:
    dimension_generator(dimension_generator_config config,
                        std::shared_ptr<const dimension_shared_data> shared,
                        dimension_biome_source biome_source,
                        block_state_registry states);

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
    std::shared_ptr<const dimension_shared_data> shared_{};
    const dimension_profile *profile_{nullptr};
    dimension_biome_source biome_source_;
    block_state_registry states_{};
    surface::block_states rule_states_{};
    std::unique_ptr<positional_random_factory> root_factory_{};
    density::sampler_context sampler_context_{};
    climate_sampler sampler_;
    block_state_id air_state_{0};
    block_state_id default_block_state_{0};
    block_state_id default_fluid_state_{0};
    block_state_id lava_state_{0};
    std::unordered_map<std::string, std::unique_ptr<positional_random_factory>>
        factories_{};
};

} // namespace refrain::worldgen
