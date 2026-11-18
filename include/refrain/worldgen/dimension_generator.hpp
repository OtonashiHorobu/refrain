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

namespace refrain::data {
class content_view;
class registry_generation;
} // namespace refrain::data

namespace refrain::worldgen {

namespace carver {
struct carver_biomes;
} // namespace carver

class stage_profile;

struct generation_materialization;
class generation_data;

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
    /// The content's block-state table container (`<version>/tables.rvtb`).
    /// Empty derives it as the sibling `tables.rvtb` of the data directory,
    /// the layout an installed content root has. A shared generator table is
    /// built from it; without the container there is no shared table.
    std::string tables_path{};
    dimension_kind kind{dimension_kind::overworld};
    jlong seed{0};
    /// Fixed biome key. When set every biome cell is this biome, matching a
    /// reference world whose preset declares a fixed biome source.
    std::string fixed_biome{};
    /// Block tags to apply instead of loading them from `data_root`. Null
    /// loads the installed datapack's tags. The pointed tag set must outlive
    /// the generator and every generator sharing the loaded data: it is owned
    /// by the content generation snapshot the generator is bound to.
    const block_tag_set *block_tags{nullptr};
    /// Block/item/entity registry mapping of the content generation the
    /// generator is bound to. Null for a boot generator. The mapping is
    /// immutable; the view-defined block names it carries seed a
    /// per-generator state table in content-key order, while the numeric entry
    /// ids are an identity for data-layer consumers and are not consumed by
    /// chunk generation.
    std::shared_ptr<const data::registry_generation> registries{};
};

/// One published content generation as chunk generation consumes it.
///
/// Chunk generation reads worldgen documents from a filesystem root, while a
/// published content view is a layered stack of packs. `generation_data` binds
/// the two: it materializes the view's `data/` overlay on top of the installed
/// datapack root once, keeps the merged tree alive for every generator of the
/// generation, and carries the immutable registry mapping and merged block tags
/// the view defines. A view that changes nothing chunk generation reads keeps
/// the installed root untouched, so an ordinary recipe/loot reload costs no
/// copy and produces the same chunks as the boot generator.
///
/// Tag documents merge with the datapack rules instead of replacing the
/// installed file: `replace: true` clears the installed values and the lower
/// view layers, otherwise the values append. Every tag kind follows the rule,
/// so the biome tags feature predicates and structure matching read keep the
/// installed values when a view adds to them.
///
/// The object is immutable and shared by every worker generator bound to the
/// same published view generation. The materialized tree is removed when the
/// last generator that references the generation is destroyed.
class generation_data {
  public:
    generation_data(const generation_data &) = delete;
    auto operator=(const generation_data &) -> generation_data & = delete;
    generation_data(generation_data &&) = delete;
    auto operator=(generation_data &&) -> generation_data & = delete;
    ~generation_data();

    /// Bind one published view to the installed datapack roots.
    ///
    /// `view` is the published content view, `registries` its registry
    /// mapping. `base_data_root` is the installed datapack `data` directory
    /// and `base_templates_root` the extracted structure template root; both
    /// stay unchanged when the view does not carry generation input. Fails
    /// with `io_error` naming the path when the merged tree cannot be
    /// materialized.
    [[nodiscard]] static auto
    build(std::shared_ptr<const data::content_view> view,
          std::shared_ptr<const data::registry_generation> registries,
          std::uint64_t view_generation, std::uint64_t registry_generation,
          std::string_view base_data_root, std::string_view base_templates_root)
        -> result<std::shared_ptr<const generation_data>>;

    [[nodiscard]] auto view_generation() const noexcept -> std::uint64_t {
        return view_generation_;
    }
    [[nodiscard]] auto registry_generation() const noexcept -> std::uint64_t {
        return registry_generation_;
    }

    /// Datapack `data` root chunk generation reads (`<root>/<namespace>/...`).
    [[nodiscard]] auto data_root() const noexcept -> const std::string & {
        return data_root_;
    }
    /// Root of the extracted structure templates
    /// (`<root>/<namespace>/structure/**.nbt`).
    [[nodiscard]] auto templates_root() const noexcept -> const std::string & {
        return templates_root_;
    }

    /// Merged block tags the generation applies.
    [[nodiscard]] auto block_tags() const noexcept -> const block_tag_set & {
        return block_tags_;
    }

    /// Registry mapping the generation binds; never null after `build`.
    [[nodiscard]] auto registries() const noexcept
        -> const data::registry_generation & {
        return *registries_;
    }

    /// Shared handle of the registry mapping, for a consumer that must keep it
    /// alive next to the generation.
    [[nodiscard]] auto registries_handle() const noexcept
        -> const std::shared_ptr<const data::registry_generation> & {
        return registries_;
    }

    /// The published view the generation was built from.
    [[nodiscard]] auto view() const noexcept -> const data::content_view & {
        return *view_;
    }

    /// True when the view contributed at least one document chunk generation
    /// reads, so the generator may produce different content than the boot
    /// generator. False for a data-only reload.
    [[nodiscard]] auto overlays_generation_content() const noexcept -> bool {
        return overlays_generation_content_;
    }

  private:
    generation_data(std::shared_ptr<const data::content_view> view,
                    std::shared_ptr<const data::registry_generation> registries,
                    std::uint64_t view_generation,
                    std::uint64_t registry_generation);

    std::shared_ptr<const data::content_view> view_{};
    std::shared_ptr<const data::registry_generation> registries_{};
    std::uint64_t view_generation_{0};
    std::uint64_t registry_generation_{0};
    std::string data_root_{};
    std::string templates_root_{};
    block_tag_set block_tags_{};
    std::shared_ptr<const generation_materialization> materialized_{};
    bool overlays_generation_content_{false};
};

/// Materialized content trees of this process that could not be removed when
/// their last generator released them. A non-zero count means the temporary
/// directory still holds a tree; the path was logged by the caller and an
/// operator can remove it. Diagnostics for operators and tests.
[[nodiscard]] auto materialization_cleanup_failures() noexcept -> std::uint64_t;

/// Loaded dimension data that does not change during generation: the noise
/// settings and their density documents, material documents and rule,
/// climates, noise functions, block tags and possible biomes. Workers of one
/// configuration share one instance. The biome source, the surface state
/// cache, the positional factories and the biome search cache stay per
/// generator instance. The block-state registry is per generator on the
/// independent-worker paths and one frozen instance owned by a scheduler when
/// several workers must resolve state keys to the same ids.
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

    /// Builds a generator over already loaded immutable data and an existing
    /// block-state table. Several generators may share one frozen table so a
    /// chunk's state ids mean the same on every worker.
    static auto create(dimension_generator_config config,
                       std::shared_ptr<const dimension_shared_data> shared,
                       std::shared_ptr<block_state_registry> states)
        -> result<std::unique_ptr<dimension_generator>>;

    /// Biomes the dimension source can select, or the configured fixed biome.
    [[nodiscard]] static auto
    possible_biomes(const dimension_generator_config &config,
                    const std::shared_ptr<const dimension_shared_data> &shared)
        -> result<std::vector<std::string>>;

    /// Builds the block-state table every scheduler worker shares. The table
    /// is the content's complete state list, interned in key order, so ids do
    /// not depend on which worker first needs a key; callers freeze it before
    /// concurrent generation. Fails when the content ships no table container,
    /// because a per-generator first-use table cannot be shared.
    [[nodiscard]] static auto build_shared_states(
        dimension_generator_config config,
        const std::shared_ptr<const dimension_shared_data> &shared)
        -> result<std::shared_ptr<block_state_registry>>;

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
        return *states_;
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
                        std::shared_ptr<block_state_registry> states);

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
    std::shared_ptr<block_state_registry> states_{};
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
    /// The compiled material rule and the context it captures by address.
    /// The rule depends only on the seed and the loaded documents, so it is
    /// compiled once; the context is rebound per chunk at this stable address
    /// and keeps its update counters increasing, which stops a cached rule
    /// closure from reusing a result of the previous chunk.
    std::optional<surface::material_context> rule_context_{};
    std::optional<surface::rule_evaluator> compiled_rule_{};
};

} // namespace refrain::worldgen
