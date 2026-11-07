// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// `FeatureSorter`: the per-step global feature order. The order is a
// first-encounter index plus a depth-first topological sort over the edges
// implied by each biome's feature lists; the decoration stage seeds every
// placed feature with its position in the resulting per-step list.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/worldgen/core/java_compat.hpp>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace refrain::worldgen::feature {

/// One biome's `features` list, indexed by decoration step.
struct biome_feature_list {
    std::string biome{};
    std::vector<std::vector<std::string>> steps{};
};

struct step_features {
    std::vector<std::string> features{};
    std::unordered_map<std::string, jint> index{};
};

class feature_sorter {
  public:
    /// `FeatureSorter.buildFeaturesPerStep`. `possible_biomes` supplies the
    /// iteration order of `biomeSource.possibleBiomes()`; every listed biome
    /// must have a `biome_feature_list`.
    [[nodiscard]] static auto
    build(const std::vector<std::string> &possible_biomes,
          const std::vector<biome_feature_list> &lists)
        -> result<std::vector<step_features>>;
};

} // namespace refrain::worldgen::feature
