// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/structure/stage.hpp>

#include <algorithm>

namespace refrain::worldgen::structure {

namespace {

auto adjustment_of(terrain_adaptation adaptation) -> terrain_adjustment {
    switch (adaptation) {
    case terrain_adaptation::bury:
        return terrain_adjustment::bury;
    case terrain_adaptation::beard_thin:
        return terrain_adjustment::beard_thin;
    case terrain_adaptation::beard_box:
        return terrain_adjustment::beard_box;
    case terrain_adaptation::encapsulate:
        return terrain_adjustment::encapsulate;
    case terrain_adaptation::none:
        break;
    }
    return terrain_adjustment::none;
}

auto as_terrain_box(const structure_bbox &box) -> bounding_box {
    return bounding_box{
        .min_x = box.min_x,
        .min_y = box.min_y,
        .min_z = box.min_z,
        .max_x = box.max_x,
        .max_y = box.max_y,
        .max_z = box.max_z,
    };
}

void include_box(std::optional<bounding_box> &encompassing,
                 const bounding_box &box) {
    if (!encompassing) {
        encompassing = box;
        return;
    }
    encompassing = bounding_box::encapsulating(*encompassing, box);
}

} // namespace

auto collect_beard(const std::vector<structure_start> &starts,
                   const structure_data_set &data, const chunk_pos &chunk)
    -> structure_beard {
    structure_beard beard;
    const jint chunk_min_x = chunk.x * 16;
    const jint chunk_min_z = chunk.z * 16;
    constexpr jint radius = 12;
    for (const auto &start : starts) {
        const auto document = data.structures.find(start.structure);
        if (document == data.structures.end() ||
            document->second.adaptation == terrain_adaptation::none) {
            continue;
        }
        const auto adjustment = adjustment_of(document->second.adaptation);
        structure_bbox adapted = start.bounds;
        adapted.min_x -= radius;
        adapted.min_y -= radius;
        adapted.min_z -= radius;
        adapted.max_x += radius;
        adapted.max_y += radius;
        adapted.max_z += radius;
        if (!adapted.intersects_chunk(chunk)) {
            continue;
        }
        for (const auto &piece : start.pieces) {
            if (!piece.bounds.is_close_to(chunk, radius)) {
                continue;
            }
            if (piece.jigsaw.has_value()) {
                if (piece.jigsaw->element.projection ==
                    pool_projection::rigid) {
                    beard.pieces.push_back(beardifier_rigid{
                        .box = as_terrain_box(piece.bounds),
                        .adjustment = adjustment,
                        .ground_level_delta = piece.jigsaw->ground_level_delta,
                    });
                    include_box(beard.affected_box,
                                as_terrain_box(piece.bounds));
                }
                for (const auto &junction : piece.jigsaw->junctions) {
                    if (junction.source_x <= chunk_min_x - radius ||
                        junction.source_z <= chunk_min_z - radius ||
                        junction.source_x >= chunk_min_x + 15 + radius ||
                        junction.source_z >= chunk_min_z + 15 + radius) {
                        continue;
                    }
                    beard.junctions.push_back(jigsaw_junction{
                        .source_x = junction.source_x,
                        .source_ground_y = junction.source_ground_y,
                        .source_z = junction.source_z,
                    });
                    include_box(beard.affected_box,
                                bounding_box{
                                    .min_x = junction.source_x,
                                    .min_y = junction.source_ground_y,
                                    .min_z = junction.source_z,
                                    .max_x = junction.source_x,
                                    .max_y = junction.source_ground_y,
                                    .max_z = junction.source_z,
                                });
                }
                continue;
            }
            beard.pieces.push_back(beardifier_rigid{
                .box = as_terrain_box(piece.bounds),
                .adjustment = adjustment,
                .ground_level_delta = 0,
            });
            include_box(beard.affected_box, as_terrain_box(piece.bounds));
        }
    }
    if (beard.affected_box) {
        beard.affected_box = beard.affected_box->inflated_by(24);
    }
    return beard;
}

} // namespace refrain::worldgen::structure
