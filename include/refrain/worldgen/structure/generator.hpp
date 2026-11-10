// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// A fixed-biome structure generator: fills the chunk and its neighbours,
// generates the chunk's starts and places every intersecting piece. The
// terrain stages match the structure oracle's reference world (empty surface
// rule, no carvers or features).

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/density/density_router.hpp>
#include <refrain/worldgen/dimension.hpp>
#include <refrain/worldgen/structure/structure_data.hpp>
#include <refrain/worldgen/structure/template.hpp>

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace refrain::worldgen::structure {

struct structure_generator_config {
    std::string data_root{};
    std::string templates_root{};
    std::string biome{"minecraft:plains"};
    jlong seed{0};
    /// Dimension whose profile selects the terrain bounds, sea level and noise
    /// settings.
    dimension_kind kind{dimension_kind::overworld};
    /// Level overrides. Unset fields come from the dimension profile.
    std::optional<jint> min_y{};
    std::optional<jint> height{};
    std::optional<jint> sea_level{};
    /// Optional block-state keys the template manager can resolve. A complete
    /// block table normally comes from content; this keeps partial registries
    /// from collapsing template blocks to air.
    std::vector<std::string> known_blocks{};
};

class structure_generator {
  public:
    static auto create(const structure_generator_config &config)
        -> result<std::unique_ptr<structure_generator>>;

    [[nodiscard]] auto generate(const chunk_pos &pos)
        -> result<std::unique_ptr<proto_chunk>>;

  private:
    structure_generator(structure_generator_config config,
                        const dimension_profile &profile,
                        density::noise_settings settings,
                        structure_data_set data, block_tag_set tags);

    [[nodiscard]] auto inferred_traits(std::string_view key) const
        -> state_traits;

    structure_generator_config config_{};
    const dimension_profile *profile_{nullptr};
    density::noise_settings settings_{};
    jint min_y_{0};
    jint height_{0};
    jint sea_level_{0};
    jint generation_min_y_{0};
    jint generation_height_{0};
    block_state_id air_state_{0};
    block_state_id default_block_state_{1};
    block_state_id default_fluid_state_{2};
    block_state_id lava_state_{3};
    structure_data_set data_{};
    block_tag_set tags_{};
    std::set<std::string, std::less<>> motion_{};
    std::set<std::string, std::less<>> motion_no_leaves_{};
    block_state_registry states_{};
    std::unique_ptr<structure_template_manager> templates_{};
};

} // namespace refrain::worldgen::structure
