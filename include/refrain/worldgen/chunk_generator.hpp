// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The production full-chunk generation service. One object owns the loaded
// worldgen data of a dimension, the terrain engine, the carvers, the feature
// registry and plan, the structure data and template manager. Generating a
// chunk runs the stages in dependency order and returns the mutable chunk the
// caller can turn into a level chunk.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/worldgen/carver/carver_data.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/dimension_generator.hpp>
#include <refrain/worldgen/feature/feature_data.hpp>
#include <refrain/worldgen/feature/feature_plan.hpp>
#include <refrain/worldgen/feature/feature_registry.hpp>
#include <refrain/worldgen/structure/structure_data.hpp>
#include <refrain/worldgen/structure/template.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::worldgen {

/// How far a generated chunk advances along the generation chain.
enum class generation_target : std::uint8_t {
    /// Biomes, fill, surface and carvers. The chunk holds no structures and
    /// no features.
    terrain = 0,
    /// Terrain plus structure starts, references, placed pieces and features.
    features = 1,
    /// The features target plus the final status.
    full = 2,
};

struct chunk_generator_config {
    /// The datapack `data` directory (`<version>/data`).
    std::string data_root{};
    /// Extracted structure templates, laid out as
    /// `<root>/<namespace>/structure/**.nbt`. An empty root still generates
    /// procedural pieces and features; template-backed pieces resolve to air.
    std::string templates_root{};
    jlong seed{0};
    dimension_kind kind{dimension_kind::overworld};
    /// Fixed biome key. Empty selects the dimension's biome source.
    std::string fixed_biome{};
    generation_target target{generation_target::full};
    /// Structure set ids that may generate. Empty enables every set; naming
    /// no existing set turns structure starts off.
    std::vector<std::string> structure_sets{};
    /// Reproduces the reference worlds of the structure oracle: terrain fill
    /// plus structure pieces only, with no surface pass, carvers or features.
    /// The chunk still reaches the configured target status.
    bool reference_world{false};
};

/// A reusable full-chunk generator. The object is not thread-safe: a single
/// instance must not generate concurrently. The batch entry points create one
/// independent instance per worker, so no random stream, registry or cache is
/// shared between workers.
class chunk_generator {
  public:
    [[nodiscard]] static auto create(chunk_generator_config config)
        -> result<std::unique_ptr<chunk_generator>>;

    /// Runs the configured stages for one chunk. Calling it again with the
    /// same position produces the same chunk: every multi-noise search is
    /// reset before the chunk's first biome is selected. The returned chunk
    /// borrows this object's block state table, so it must not outlive the
    /// generator.
    [[nodiscard]] auto generate(const chunk_pos &pos)
        -> result<std::unique_ptr<proto_chunk>>;

    /// Generates `positions` in parallel and returns the chunks in input
    /// order. `workers` is clamped to the position count; zero selects the
    /// hardware concurrency. Every worker owns its data, so the result is
    /// identical to serial generation. Worker instances are kept for later
    /// batches, so returned chunks stay valid as long as this object does.
    [[nodiscard]] auto generate_batch(std::span<const chunk_pos> positions,
                                      std::size_t workers = 0)
        -> result<std::vector<std::unique_ptr<proto_chunk>>>;

    /// Generates a `width` by `depth` rectangle of chunks starting at
    /// `origin`, x outer to the inner z loop, and returns them in that order.
    [[nodiscard]] auto generate_area(chunk_pos origin, jint width, jint depth,
                                     std::size_t workers = 0)
        -> result<std::vector<std::unique_ptr<proto_chunk>>>;

    [[nodiscard]] auto config() const noexcept
        -> const chunk_generator_config & {
        return config_;
    }
    [[nodiscard]] auto profile() const noexcept -> const dimension_profile & {
        return dimension_->profile();
    }
    [[nodiscard]] auto states() noexcept -> block_state_registry & {
        return dimension_->states();
    }
    [[nodiscard]] auto target() const noexcept -> generation_target {
        return config_.target;
    }

  private:
    chunk_generator(
        chunk_generator_config config,
        std::unique_ptr<dimension_generator> dimension,
        carver::loaded_carvers carvers, feature::feature_data_set feature_data,
        std::unique_ptr<feature::feature_registry> features,
        std::vector<feature::step_features> feature_plan,
        structure::structure_data_set structure_data,
        std::unique_ptr<structure::structure_template_manager> templates);

    [[nodiscard]] auto generate_impl(const chunk_pos &pos)
        -> result<std::unique_ptr<proto_chunk>>;

    chunk_generator_config config_{};
    std::unique_ptr<dimension_generator> dimension_{};
    carver::loaded_carvers carvers_{};
    feature::feature_data_set feature_data_{};
    std::unique_ptr<feature::feature_registry> features_{};
    std::vector<feature::step_features> feature_plan_{};
    structure::structure_data_set structure_data_{};
    std::unique_ptr<structure::structure_template_manager> templates_{};
    /// Extra generators created by the batch entry points. Their registries
    /// back the tables of the chunks they returned.
    std::vector<std::unique_ptr<chunk_generator>> workers_{};
    bool has_structures_{false};
    bool has_features_{false};
};

} // namespace refrain::worldgen
