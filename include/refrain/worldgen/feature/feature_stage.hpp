// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// `ChunkGenerator.applyBiomeDecoration`'s feature half: the per-step candidate
// set, the decoration seed, and the FeaturePlacer work stack.

#pragma once

#include <refrain/worldgen/feature/feature.hpp>
#include <refrain/worldgen/feature/feature_plan.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace refrain::worldgen::feature {

struct placed_feature_runtime {
    std::string id{};
    const feature *configured{nullptr};
    std::vector<std::unique_ptr<placement_modifier>> placement{};
};

/// Places every structure of one step before that step's features. The
/// callback receives the step index and the index of the structure type within
/// the step; the random stream is already seeded exactly like the reference.
using structure_placer =
    std::function<bool(jint step, jint index, worldgen_random &random)>;

/// Runs every step's candidate features. `biomes` are the biome keys of the
/// 3x3 neighbourhood; `lists` supplies their feature lists.
void decorate(
    feature_context &context, const std::vector<step_features> &plan,
    const std::unordered_map<std::string, placed_feature_runtime> &placed,
    const std::vector<std::string> &biomes,
    const std::vector<biome_feature_list> &lists, jlong seed,
    const structure_placer &structures = {});

} // namespace refrain::worldgen::feature
