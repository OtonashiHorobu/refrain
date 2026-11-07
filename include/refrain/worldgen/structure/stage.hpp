// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Structure starts assembly for the starts/references stages.

#pragma once

#include <refrain/worldgen/structure/placement.hpp>
#include <refrain/worldgen/structure/start.hpp>

#include <string>
#include <vector>

namespace refrain::worldgen::structure {

struct selected_structure {
    std::string id{};
    random_spread_placement placement{};
};

/// Generates the buried-treasure starts in the radius-`radius` chunk square
/// around `target`, then returns their references into `target`.
struct buried_starts_result {
    std::vector<structure_start> starts{};
    std::vector<structure_reference> references{};
};

[[nodiscard]] auto
generate_buried_starts(jlong seed, const chunk_pos &target,
                       const std::vector<selected_structure> &structures,
                       jint radius) -> buried_starts_result;

} // namespace refrain::worldgen::structure
