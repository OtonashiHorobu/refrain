// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The structure and structure-set documents the starts, references and
// Beardifier stages consume.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/data/json.hpp>
#include <refrain/worldgen/structure/structure_set.hpp>

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
    data::json::value config{};
};

struct structure_data_set {
    std::unordered_map<std::string, structure_document> structures{};
    std::unordered_map<std::string, structure_set> sets{};
    std::unordered_map<std::string, std::vector<std::string>> biome_tags{};

    [[nodiscard]] auto allows_biome(const structure_document &structure,
                                    std::string_view biome) const -> bool;
};

[[nodiscard]] auto decode_structure_document(std::string_view id,
                                             const data::json::value &document)
    -> result<structure_document>;

[[nodiscard]] auto load_structure_data(std::string_view data_root)
    -> result<structure_data_set>;

} // namespace refrain::worldgen::structure
