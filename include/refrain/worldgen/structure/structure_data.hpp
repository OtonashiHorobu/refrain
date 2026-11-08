// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The structure and structure-set documents the starts, references and
// Beardifier stages consume.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/data/json.hpp>
#include <refrain/worldgen/structure/jigsaw.hpp>
#include <refrain/worldgen/structure/structure_set.hpp>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace refrain::worldgen::structure {

enum class terrain_adaptation : std::uint8_t {
    none,
    beard_thin,
    beard_box,
    bury,
    encapsulate,
};

struct structure_document {
    std::string id{};
    std::string type{};
    std::string step{};
    terrain_adaptation adaptation{terrain_adaptation::none};
    std::string biome_tag{};              ///< set for a `#tag` biome list
    std::vector<std::string> biome_ids{}; ///< resolved allowed biomes
    /// Decoded fields of a `minecraft:jigsaw` document.
    std::optional<jigsaw_structure_config> jigsaw{};
    data::json::value config{};
};

struct structure_data_set {
    std::unordered_map<std::string, structure_document> structures{};
    std::unordered_map<std::string, structure_set> sets{};
    std::unordered_map<std::string, std::vector<std::string>> biome_tags{};
    /// `worldgen/template_pool` documents, keyed by `namespace:path`.
    std::map<std::string, jigsaw_template_pool, std::less<>> template_pools{};
    /// `worldgen/processor_list` documents; the template manager decodes them
    /// lazily against its block-state registry.
    std::unordered_map<std::string, data::json::value> processor_lists{};

    [[nodiscard]] auto allows_biome(const structure_document &structure,
                                    std::string_view biome) const -> bool;
    /// Whether `biome` is in `tag`, resolving nested `#` entries. Missing
    /// tags contain nothing.
    [[nodiscard]] auto biome_matches_tag(std::string_view tag,
                                         std::string_view biome) const -> bool;
};

/// Position of `structure_id` among the structures of the same step, in the
/// registry order used for `setFeatureSeed(index, step)`. Structure documents
/// load in identifier order, so the order is the sorted id list.
[[nodiscard]] auto structure_step_index(const structure_data_set &data,
                                        std::string_view structure_id)
    -> std::optional<jint>;

[[nodiscard]] auto decode_structure_document(std::string_view id,
                                             const data::json::value &document)
    -> result<structure_document>;

[[nodiscard]] auto load_structure_data(std::string_view data_root)
    -> result<structure_data_set>;

} // namespace refrain::worldgen::structure
