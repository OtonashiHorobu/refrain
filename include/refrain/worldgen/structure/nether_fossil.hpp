// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Nether fossil starts: a surface scan along one column picks a ground block,
// then one of the fourteen fossil templates is placed there with a random
// quarter turn.

#pragma once

#include <refrain/worldgen/chunk/block_state.hpp>
#include <refrain/worldgen/structure/start.hpp>
#include <refrain/worldgen/structure/structure_data.hpp>
#include <refrain/worldgen/structure/template.hpp>

#include <functional>
#include <optional>

namespace refrain::worldgen::structure {

struct nether_fossil_generation_context {
    const structure_document *structure{nullptr};
    structure_template_manager *templates{nullptr};
    /// Base-column block states: the unbearded terrain the scan reads.
    const block_state_table *states{nullptr};
    std::function<block_state_id(jint block_x, jint block_y, jint block_z)>
        block_at{};
    jlong seed{0};
    jint min_y{0};
    jint world_height{256};
    jint sea_level{32};
    /// Height-provider sampling range; zero generation height falls back to
    /// the level bounds.
    jint generation_min_y{0};
    jint generation_height{0};
    /// Biome validity at the chosen ground block.
    std::function<bool(const block_pos &)> valid_biome{};
};

/// Picks a column in `chunk`, samples the structure's height provider and
/// scans down from it for air above soul sand or a sturdy block. Places one
/// fossil template at the found block, rotated and mirrored not at all.
[[nodiscard]] auto
nether_fossil_start(const chunk_pos &chunk,
                    const nether_fossil_generation_context &context)
    -> std::optional<structure_start>;

} // namespace refrain::worldgen::structure
