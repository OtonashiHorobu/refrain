// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Structure-set documents: weighted structure entries and their placement.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/worldgen/structure/placement.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::data::json {
class value;
} // namespace refrain::data::json

namespace refrain::worldgen::structure {

struct weighted_structure {
    std::string structure{};
    jint weight{1};
};

/// Stronghold ring candidates. The preferred biomes determine the final chunk
/// of each ring through the biome source's horizontal search; a tag is
/// resolved against the structure data.
struct concentric_rings_placement {
    jint distance{0};
    jint spread{0};
    jint count{0};
    jint salt{0};
    std::string preferred_biomes_tag{};
    std::vector<std::string> preferred_biomes_ids{};
};

struct structure_set {
    std::string id{};
    std::vector<weighted_structure> structures{};
    std::optional<random_spread_placement> random_spread{};
    std::optional<concentric_rings_placement> concentric_rings{};

    /// The weighted structure draw order: one entry per draw, without
    /// replacement, until the list is empty. A failed structure
    /// generation continues this order with the same random stream.
    [[nodiscard]] auto weighted_order(jlong seed, const chunk_pos &chunk) const
        -> std::vector<weighted_structure>;
};

[[nodiscard]] auto decode_structure_set(std::string_view id,
                                        const data::json::value &document)
    -> result<structure_set>;

} // namespace refrain::worldgen::structure
