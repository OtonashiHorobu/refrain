// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Structure starts assembly for the starts/references stages.

#pragma once

#include <refrain/worldgen/chunk/heightmap.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/structure/placement.hpp>
#include <refrain/worldgen/structure/start.hpp>
#include <refrain/worldgen/structure/structure_data.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::worldgen::structure {

/// The world access a starts stage needs. Biomes are queried at quart
/// resolution; heights return the first occupied block below the heightmap
/// top, matching `getFirstOccupiedHeight`.
struct structure_start_context {
    jlong seed{0};
    const structure_data_set *data{nullptr};
    jint min_y{-64};
    jint world_height{384};
    jint sea_level{63};
    std::function<std::uint16_t(jint quart_x, jint quart_y, jint quart_z)>
        biome_at{};
    std::function<std::string_view(std::uint16_t)> biome_name{};
    std::function<jint(jint block_x, jint block_z, heightmap_kind)> height_at{};
};

struct structure_area {
    std::vector<structure_start> starts{};
    std::vector<structure_reference> references{};
};

/// Generates starts for every chunk in the radius-`radius` square around
/// `target`, then collects the references into `target`. Starts are returned
/// for the whole square so callers can write each source chunk's own list.
[[nodiscard]] auto
generate_structure_area(const chunk_pos &target,
                        const structure_start_context &context, jint radius)
    -> structure_area;

/// Generates every structure start owned by `chunk`, in structure-set order.
/// Unsupported structure types produce no start; their sets are still advanced
/// exactly like the reference, so an unsupported candidate does not consume a
/// weighted draw meant for a later entry.
[[nodiscard]] auto
generate_structure_starts(const chunk_pos &chunk,
                          const structure_start_context &context)
    -> std::vector<structure_start>;

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

/// Copies the starts owned by `chunk` and every reference into the chunk's
/// structure containers.
void write_structure_stage(proto_chunk &chunk,
                           const buried_starts_result &result);

} // namespace refrain::worldgen::structure
