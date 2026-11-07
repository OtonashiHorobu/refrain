// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Structure starts and their pieces: geometry and reference accounting that
// the starts/references stages share with the placement and Beardifier code.

#pragma once

#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/core/java_compat.hpp>

#include <string>
#include <vector>

namespace refrain::worldgen::structure {

struct structure_bbox {
    jint min_x{0};
    jint min_y{0};
    jint min_z{0};
    jint max_x{0};
    jint max_y{0};
    jint max_z{0};

    [[nodiscard]] auto intersects(const structure_bbox &other) const -> bool;
    [[nodiscard]] auto is_close_to(const chunk_pos &chunk, jint radius) const
        -> bool;
    [[nodiscard]] auto intersects_chunk(const chunk_pos &chunk) const -> bool;
};

struct structure_piece {
    std::string type{};
    structure_bbox bounds{};
    jint orientation{-1}; ///< `Direction.get2DDataValue`, -1 when unset

    /// Template pieces (`minecraft:igl`) carry the template id and the
    /// placement transform; procedural pieces leave these empty.
    std::string template_id{};
    block_pos template_origin{};
    jint template_rotation{0}; ///< 0 none, 1 clockwise 90, 2 180, 3 ccw 90
    jint template_mirror{0};
    block_pos template_pivot{};
    jint height_position{-1};
};

struct structure_start {
    std::string structure{};
    std::string step{};
    chunk_pos chunk{};
    std::vector<structure_piece> pieces{};
    jint references{0};
    jint max_references{1};
    structure_bbox bounds{};

    [[nodiscard]] auto can_add_reference() const -> bool {
        return references < max_references;
    }
    void add_reference() { ++references; }
    [[nodiscard]] auto intersects(const structure_bbox &other) const -> bool {
        return bounds.intersects(other);
    }
};

/// The starts whose pieces can reach `chunk` within `radius` blocks; the
/// target chunk references these during the starts stage.
[[nodiscard]] auto starts_for_chunk(const std::vector<structure_start> &starts,
                                    const chunk_pos &chunk, jint radius)
    -> std::vector<const structure_start *>;

/// Adds one reference from `chunk` to every start whose bounding box overlaps
/// the chunk and which still has a free reference slot. `radius` names the
/// source-chunk search radius the caller scanned; the reference test itself is
/// the exact block-box overlap.
[[nodiscard]] auto collect_references(std::vector<structure_start> &starts,
                                      const chunk_pos &chunk, jint radius)
    -> std::vector<structure_reference>;

} // namespace refrain::worldgen::structure
