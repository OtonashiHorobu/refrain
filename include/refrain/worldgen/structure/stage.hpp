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
#include <refrain/worldgen/structure/template.hpp>
#include <refrain/worldgen/terrain/beardifier.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::worldgen {
class stage_profile;
} // namespace refrain::worldgen

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
    /// Generation depth a height provider samples over. The noise settings of
    /// the nether and the end are shorter than their level; zero means "same
    /// as the level bounds".
    jint generation_min_y{0};
    jint generation_height{0};
    /// True when the biome source returns one biome everywhere; ring
    /// placement then uses the fixed-biome search shape instead of sampling
    /// the biome grid.
    bool fixed_biome{false};
    std::function<std::uint16_t(jint quart_x, jint quart_y, jint quart_z)>
        biome_at{};
    std::function<std::string_view(std::uint16_t)> biome_name{};
    std::function<jint(jint block_x, jint block_z, heightmap_kind)> height_at{};
    /// Unbearded base-column block states, read by the nether fossil scan.
    /// Null leaves nether fossils ungenerated.
    std::function<block_state_id(jint block_x, jint block_y, jint block_z)>
        block_at{};
    /// State table for the base-column states; null leaves nether fossils
    /// ungenerated.
    const block_state_table *states{nullptr};
    /// Terrain opacity used by structures that probe corner columns. Null
    /// makes those structures fall back to the height map.
    std::function<bool(jint block_x, jint block_y, jint block_z)> opaque_at{};
    /// Whether the biome at a block is cold enough to snow, used by ruined
    /// portal material selection.
    std::function<bool(jint block_x, jint block_y, jint block_z)> biome_cold{};
    /// Template source for structure types that place `.nbt` pieces. Null
    /// leaves those types ungenerated.
    structure_template_manager *templates{nullptr};
    /// Restricts start generation to the selected structure sets. Empty
    /// accepts every set; a filter that selects none is an empty set.
    std::function<bool(std::string_view)> set_filter{};
    /// Records the wall-clock time of the starts and reference stages when
    /// non-null. The profile never affects the generated starts.
    stage_profile *profile{nullptr};
};

/// The beardifier inputs one chunk sees: the rigid pieces and junction sources
/// of every adapted start whose adapted bounding box reaches the chunk.
struct structure_beard {
    std::vector<beardifier_rigid> pieces{};
    std::vector<jigsaw_junction> junctions{};
    std::optional<bounding_box> affected_box{};
};

/// Collects the beardifier inputs: pieces within twelve blocks of `chunk`,
/// rigid jigsaw pieces only, plus every junction source in the chunk window.
/// The affected box is the union of the included boxes inflated by 24.
[[nodiscard]] auto collect_beard(const std::vector<structure_start> &starts,
                                 const structure_data_set &data,
                                 const chunk_pos &chunk) -> structure_beard;

/// One structure found by `locate_structure`.
struct located_structure {
    chunk_pos chunk{};
    std::string structure{};
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

/// Finds the nearest chunk whose starts contain `structure_id`, scanning
/// rings outward from `origin` up to `max_radius` chunks. Returns `nullopt`
/// when no such start exists in the square or the id is unknown.
[[nodiscard]] auto locate_structure(std::string_view structure_id,
                                    const chunk_pos &origin, jint max_radius,
                                    const structure_start_context &context)
    -> std::optional<located_structure>;

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
