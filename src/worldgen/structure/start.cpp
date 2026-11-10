// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/structure/start.hpp>

#include <refrain/worldgen/chunk/proto_chunk.hpp>

#include <algorithm>

namespace refrain::worldgen::structure {

auto to_piece_record(const structure_piece &piece)
    -> ::refrain::worldgen::structure_piece_record {
    ::refrain::worldgen::structure_piece_record record{
        .type = piece.type,
        .min_x = piece.bounds.min_x,
        .min_y = piece.bounds.min_y,
        .min_z = piece.bounds.min_z,
        .max_x = piece.bounds.max_x,
        .max_y = piece.bounds.max_y,
        .max_z = piece.bounds.max_z,
        .orientation = piece.orientation,
        .template_id = piece.template_id,
        .template_x = piece.template_origin.x,
        .template_y = piece.template_origin.y,
        .template_z = piece.template_origin.z,
        .template_rotation = piece.template_rotation,
        .template_mirror = piece.template_mirror,
        .template_pivot_x = piece.template_pivot.x,
        .template_pivot_y = piece.template_pivot.y,
        .template_pivot_z = piece.template_pivot.z,
        .height_position = piece.height_position,
        .height_offset = piece.height_offset,
        .integrity = piece.integrity,
        .large = piece.large,
        .beached = piece.beached,
        .entrances = piece.entrances,
        .sections = piece.sections,
        .rails = piece.rails,
        .spider = piece.spider,
        .two_floored = piece.two_floored,
        .mesa = piece.mesa,
        .overwrite = piece.overwrite,
        .monument = piece.monument,
        .stronghold = piece.stronghold,
        .fortress = piece.fortress,
        .jigsaw = piece.jigsaw,
    };
    if (piece.portal) {
        record.portal_valid = true;
        record.portal_placement = piece.portal->placement;
        record.portal_mossiness = piece.portal->mossiness;
        record.portal_air_pocket = piece.portal->air_pocket;
        record.portal_overgrown = piece.portal->overgrown;
        record.portal_vines = piece.portal->vines;
        record.portal_blackstone = piece.portal->replace_with_blackstone;
        record.portal_cold = piece.portal->cold;
    }
    record.children.reserve(piece.children.size());
    for (const auto &child : piece.children) {
        record.children.push_back(to_piece_record(child));
    }
    return record;
}

auto start_reference_position(const structure_bbox &first) -> block_pos {
    return block_pos{
        .x = first.min_x + (first.max_x - first.min_x + 1) / 2,
        .y = first.min_y,
        .z = first.min_z + (first.max_z - first.min_z + 1) / 2,
    };
}

auto start_reference_position(
    const ::refrain::worldgen::structure_piece_record &first) -> block_pos {
    return start_reference_position(structure_bbox{
        .min_x = first.min_x,
        .min_y = first.min_y,
        .min_z = first.min_z,
        .max_x = first.max_x,
        .max_y = first.max_y,
        .max_z = first.max_z,
    });
}

auto structure_bbox::intersects(const structure_bbox &other) const -> bool {
    return min_x <= other.max_x && max_x >= other.min_x &&
           min_y <= other.max_y && max_y >= other.min_y &&
           min_z <= other.max_z && max_z >= other.min_z;
}

auto structure_bbox::is_close_to(const chunk_pos &chunk, jint radius) const
    -> bool {
    const jint chunk_min_x = chunk.x * 16;
    const jint chunk_min_z = chunk.z * 16;
    return max_x >= chunk_min_x - radius &&
           min_x <= chunk_min_x + 15 + radius &&
           max_z >= chunk_min_z - radius && min_z <= chunk_min_z + 15 + radius;
}

auto structure_bbox::intersects_chunk(const chunk_pos &chunk) const -> bool {
    const jint chunk_min_x = chunk.x * 16;
    const jint chunk_min_z = chunk.z * 16;
    return max_x >= chunk_min_x && min_x <= chunk_min_x + 15 &&
           max_z >= chunk_min_z && min_z <= chunk_min_z + 15;
}

auto starts_for_chunk(const std::vector<structure_start> &starts,
                      const chunk_pos &chunk, jint radius)
    -> std::vector<const structure_start *> {
    std::vector<const structure_start *> result;
    for (const auto &start : starts) {
        if (start.bounds.is_close_to(chunk, radius)) {
            result.push_back(&start);
        }
    }
    return result;
}

auto collect_references(std::vector<structure_start> &starts,
                        const chunk_pos &chunk, jint /*radius*/)
    -> std::vector<structure_reference> {
    std::vector<structure_reference> result;
    const auto add = [&](std::string_view structure, const chunk_pos &source) {
        for (auto &reference : result) {
            if (reference.structure == structure &&
                reference.chunk_x == source.x &&
                reference.chunk_z == source.z) {
                ++reference.count;
                return;
            }
        }
        result.push_back(
            structure_reference{.structure = std::string{structure},
                                .chunk_x = source.x,
                                .chunk_z = source.z,
                                .count = 1});
    };
    for (auto &start : starts) {
        if (!start.bounds.intersects_chunk(chunk) ||
            !start.can_add_reference()) {
            continue;
        }
        start.add_reference();
        add(start.structure, start.chunk);
    }
    return result;
}

} // namespace refrain::worldgen::structure
