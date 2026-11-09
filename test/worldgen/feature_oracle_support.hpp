// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Reference chunk comparison shared by the feature oracles. Feature writes
// carry meaningful block states, so the comparison keeps every property and
// only elides the default values a state carries when a feature places it
// without naming them.

#pragma once

#include "worldgen/full_stage_support.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::worldgen::test_support {

/// Normalises palette state strings for comparison: properties are sorted by
/// name, then the explicit defaults a placed feature writes are elided.
inline auto canonical_state_name(std::string_view input) -> std::string {
    const auto canon_sort = [](std::string text) -> std::string {
        const auto open = text.find('[');
        if (open == std::string::npos) {
            return text;
        }
        const auto close = text.rfind(']');
        if (close == std::string::npos || close < open) {
            return text;
        }
        std::vector<std::pair<std::string, std::string>> properties;
        std::string_view body{text.data() + open + 1, close - open - 1};
        while (!body.empty()) {
            const auto comma = body.find(',');
            const auto entry = body.substr(0, comma);
            const auto equals = entry.find('=');
            if (equals != std::string_view::npos) {
                properties.emplace_back(std::string{entry.substr(0, equals)},
                                        std::string{entry.substr(equals + 1)});
            }
            if (comma == std::string_view::npos) {
                break;
            }
            body = body.substr(comma + 1);
        }
        std::sort(properties.begin(), properties.end());
        std::string rebuilt = text.substr(0, open);
        rebuilt.push_back('[');
        for (std::size_t i = 0; i < properties.size(); ++i) {
            if (i != 0) {
                rebuilt.push_back(',');
            }
            rebuilt.append(properties[i].first);
            rebuilt.push_back('=');
            rebuilt.append(properties[i].second);
        }
        rebuilt.push_back(']');
        return rebuilt;
    };
    const std::string owned = canon_sort(std::string{input});
    std::string_view name{owned};

    if (name.size() > 8 && name.ends_with("[axis=y]")) {
        const auto base = name.substr(0, name.size() - 8);
        if (base.ends_with("_log") || base.ends_with("_stem") ||
            base.ends_with("_wood")) {
            return std::string{base};
        }
    }
    if (name == "minecraft:bone_block[axis=y]") {
        return "minecraft:bone_block";
    }
    if (name == "minecraft:wall_torch[facing=north]" ||
        name == "minecraft:torch[waterlogged=false]") {
        return std::string{name.substr(0, name.find('['))};
    }
    if (name == "minecraft:snow[layers=1]") {
        return "minecraft:snow";
    }
    constexpr std::string_view fluid_default = "[level=0]";
    if (name.ends_with(fluid_default) &&
        (name.starts_with("minecraft:lava") ||
         name.starts_with("minecraft:water"))) {
        return std::string{name.substr(0, name.size() - fluid_default.size())};
    }
    constexpr std::string_view honey_default = ",honey_level=0]";
    if (name.ends_with(honey_default)) {
        std::string canonical{
            name.substr(0, name.size() - honey_default.size())};
        canonical.push_back(']');
        return canonical;
    }
    constexpr std::string_view leaf_defaults =
        "[distance=7,persistent=false,waterlogged=false]";
    if (name.size() > leaf_defaults.size() && name.ends_with(leaf_defaults)) {
        return std::string{name.substr(0, name.size() - leaf_defaults.size())};
    }
    return std::string{name};
}

/// Compares a feature-decorated chunk with a decoded reference chunk. Block
/// states are compared after the default-property normalisation above and all
/// four final heightmaps are compared exactly.
[[nodiscard]] inline auto
compare_feature_chunks(const proto_chunk &actual, const decoded &reference,
                       const data::nbt::value &document, jint min_y,
                       jint level_height) -> chunk_comparison {
    chunk_comparison result;
    for (jint y = min_y; y < min_y + level_height; ++y) {
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                const auto index = static_cast<std::size_t>(y - min_y) * 256U +
                                   static_cast<std::size_t>(z) * 16U +
                                   static_cast<std::size_t>(x);
                const auto expected = canonical_state_name(
                    reference.registry.name(reference.blocks[index]));
                const auto state_name =
                    actual.table().name(actual.state_at(x, y, z));
                const auto got =
                    canonical_state_name(std::string_view{state_name});
                if (got == expected) {
                    continue;
                }
                if (result.block_mismatches < mismatch_print_limit()) {
                    const auto below_actual =
                        y > min_y
                            ? actual.table().name(actual.state_at(x, y - 1, z))
                            : std::string_view{};
                    const auto below_expected =
                        y > min_y
                            ? reference.registry.name(
                                  reference.blocks
                                      [static_cast<std::size_t>(y - 1 - min_y) *
                                           256U +
                                       static_cast<std::size_t>(z) * 16U +
                                       static_cast<std::size_t>(x)])
                            : std::string_view{};
                    std::fprintf(stderr,
                                 "feature mismatch (%d,%d,%d): got %s want %s "
                                 "(below got %s want %s)\n",
                                 static_cast<int>(x), static_cast<int>(y),
                                 static_cast<int>(z), got.c_str(),
                                 expected.c_str(),
                                 std::string{below_actual}.c_str(),
                                 std::string{below_expected}.c_str());
                }
                ++result.block_mismatches;
            }
        }
    }
    for (const auto *kind_name :
         {"WORLD_SURFACE", "OCEAN_FLOOR", "MOTION_BLOCKING",
          "MOTION_BLOCKING_NO_LEAVES"}) {
        const auto kind = std::string_view{kind_name} == "WORLD_SURFACE"
                              ? heightmap_kind::world_surface
                          : std::string_view{kind_name} == "OCEAN_FLOOR"
                              ? heightmap_kind::ocean_floor
                          : std::string_view{kind_name} == "MOTION_BLOCKING"
                              ? heightmap_kind::motion_blocking
                              : heightmap_kind::motion_blocking_no_leaves;
        for (std::size_t index = 0; index < 256U; ++index) {
            const auto expected = reference_heightmap(
                document, kind_name, index, min_y, level_height);
            if (!expected.has_value()) {
                continue;
            }
            const auto x = static_cast<jint>(index % 16U);
            const auto z = static_cast<jint>(index / 16U);
            const auto got = actual.first_available(kind, x, z);
            if (got == *expected) {
                continue;
            }
            if (result.heightmap_mismatches <
                static_cast<jint>(mismatch_print_limit())) {
                std::fprintf(
                    stderr, "feature heightmap %s (%d,%d): got %d want %d\n",
                    kind_name, static_cast<int>(x), static_cast<int>(z),
                    static_cast<int>(got), static_cast<int>(*expected));
            }
            ++result.heightmap_mismatches;
        }
    }
    return result;
}

} // namespace refrain::worldgen::test_support
