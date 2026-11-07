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
};

struct structure_piece {
    std::string type{};
    structure_bbox bounds{};
};

struct structure_start {
    std::string structure{};
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

} // namespace refrain::worldgen::structure
