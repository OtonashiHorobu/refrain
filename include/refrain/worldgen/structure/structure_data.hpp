// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The structure and structure-set documents the starts, references and
// terrain-adjustment stages consume.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/data/json.hpp>
#include <refrain/worldgen/structure/jigsaw.hpp>
#include <refrain/worldgen/structure/structure_set.hpp>

#include <map>
#include <optional>
#include <span>
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
    /// Ordered by id. Placement iterates the sets to decide the order starts
    /// are generated and randomness is consumed, so the container has to
    /// define that order itself; a hash map would make it depend on insertion
    /// order and the standard library implementation.
    std::map<std::string, structure_set, std::less<>> sets{};
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

/// The sets whose structures can appear with one of `possible_biomes`, in
/// ascending id order. A structure set is only usable when at
/// least one of its entries names a biome a dimension's source can produce,
/// which is the filter the reference applies when it loads a generator. It
/// changes no generated start: a start whose biome list excludes every
/// possible biome fails its biome check anyway, so dropping the set only
/// avoids the search that would have ended in that failure.
[[nodiscard]] auto
applicable_structure_sets(const structure_data_set &data,
                          std::span<const std::string> possible_biomes)
    -> std::vector<const structure_set *>;

/// Ordinal of a decoration step name, i.e. its position in the reference
/// generation-step enumeration. Unknown names fall back to
/// `surface_structures`, the structure default.
[[nodiscard]] auto generation_step_ordinal(std::string_view step) -> jint;

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
