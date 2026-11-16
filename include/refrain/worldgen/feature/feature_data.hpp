// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The data pack documents the feature stage reads: configured feature bodies,
// placed feature references and each biome's eleven decoration lists.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/data/json.hpp>
#include <refrain/worldgen/biome/biome_tags.hpp>
#include <refrain/worldgen/feature/feature_plan.hpp>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace refrain::worldgen::feature {

struct configured_feature_document {
    std::string id{};
    data::json::value document{};
};

struct placed_feature_document {
    std::string id{};
    std::string feature{};
    data::json::value placement{};
};

struct state_provider_document {
    std::string id{};
    data::json::value document{};
};

struct feature_data_set {
    std::unordered_map<std::string, configured_feature_document> features{};
    std::unordered_map<std::string, state_provider_document> state_providers{};
    std::unordered_map<std::string, placed_feature_document> placed{};
    std::vector<biome_feature_list> biomes{};
    /// The biome tag tree under `tags/worldgen/biome`; the biome membership
    /// predicate resolves `#tag` entries through it.
    biome_tags tags{};
};

/// Reads `worldgen/feature`, `worldgen/placed_feature`, `worldgen/biome` and
/// `tags/worldgen/biome` under every namespace directory of `data_root`.
[[nodiscard]] auto load_feature_data(std::string_view data_root)
    -> result<feature_data_set>;

} // namespace refrain::worldgen::feature
