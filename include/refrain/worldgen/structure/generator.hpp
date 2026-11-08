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
#include <refrain/worldgen/structure/structure_data.hpp>
#include <refrain/worldgen/structure/template.hpp>

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace refrain::worldgen::structure {

struct structure_generator_config {
    std::string data_root{};
    std::string templates_root{};
    std::string biome{"minecraft:plains"};
    jlong seed{0};
    jint min_y{-64};
    jint height{384};
    jint sea_level{63};
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
                        density::noise_settings settings,
                        structure_data_set data, block_tag_set tags);

    [[nodiscard]] auto inferred_traits(std::string_view key) const
        -> state_traits;

    structure_generator_config config_{};
    density::noise_settings settings_{};
    structure_data_set data_{};
    block_tag_set tags_{};
    std::set<std::string, std::less<>> motion_{};
    std::set<std::string, std::less<>> motion_no_leaves_{};
    block_state_registry states_{};
    std::unique_ptr<structure_template_manager> templates_{};
};

} // namespace refrain::worldgen::structure
