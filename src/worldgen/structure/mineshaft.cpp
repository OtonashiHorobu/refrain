// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/core/rng.hpp>
#include <refrain/worldgen/structure/generation.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <optional>
#include <vector>

namespace refrain::worldgen::structure {

namespace {

enum class ms_kind : std::uint8_t { room, corridor, crossing, stairs };
enum class ms_dir : std::uint8_t { north, south, west, east };

struct ms_box {
    jint min_x{0};
    jint min_y{0};
    jint min_z{0};
    jint max_x{0};
    jint max_y{0};
    jint max_z{0};

    [[nodiscard]] auto y_span() const -> jint { return max_y - min_y + 1; }
    [[nodiscard]] auto x_span() const -> jint { return max_x - min_x + 1; }
    [[nodiscard]] auto z_span() const -> jint { return max_z - min_z + 1; }

    void move(jint dx, jint dy, jint dz) {
        min_x += dx;
        max_x += dx;
        min_y += dy;
        max_y += dy;
        min_z += dz;
        max_z += dz;
    }
};

auto intersects(const ms_box &left, const ms_box &right) -> bool {
    return left.min_x <= right.max_x && left.max_x >= right.min_x &&
           left.min_y <= right.max_y && left.max_y >= right.min_y &&
           left.min_z <= right.max_z && left.max_z >= right.min_z;
}

struct ms_piece {
    ms_kind kind{ms_kind::room};
    ms_box box{};
    jint depth{0};
    ms_dir direction{ms_dir::north};
    bool two_floored{false};
    std::vector<ms_box> entrances{};
    jint sections{0};
    bool rails{false};
    bool spider{false};
};

struct ms_state {
    std::vector<ms_piece> pieces;
    ms_box room{};
    worldgen_random *random{nullptr};
};

auto find_collision(const ms_state &state, const ms_box &box) -> bool {
    for (const auto &piece : state.pieces) {
        if (intersects(piece.box, box)) {
            return true;
        }
    }
    return false;
}

void add_children(ms_state &state, std::size_t index);

auto generate_and_add(ms_state &state, jint foot_x, jint foot_y, jint foot_z,
                      ms_dir direction, jint depth)
    -> std::optional<std::size_t>;

auto create_random(ms_state &state, jint foot_x, jint foot_y, jint foot_z,
                   ms_dir direction, jint depth) -> std::optional<std::size_t> {
    auto &random = *state.random;
    const jint selection = random.next_int(100);
    auto add = [&](ms_kind kind, const ms_box &box, ms_dir dir,
                   bool two_floored) {
        const auto index = state.pieces.size();
        state.pieces.push_back(ms_piece{.kind = kind,
                                        .box = box,
                                        .depth = depth,
                                        .direction = dir,
                                        .two_floored = two_floored});
        add_children(state, index);
        return index;
    };
    if (selection >= 80) {
        jint height = 2;
        if (random.next_int(4) == 0) {
            height = 6;
        }
        ms_box box{};
        switch (direction) {
        case ms_dir::north:
            box = {-1, 0, -4, 3, height, 0};
            break;
        case ms_dir::south:
            box = {-1, 0, 0, 3, height, 4};
            break;
        case ms_dir::west:
            box = {-4, 0, -1, 0, height, 3};
            break;
        case ms_dir::east:
            box = {0, 0, -1, 4, height, 3};
            break;
        }
        box.move(foot_x, foot_y, foot_z);
        if (find_collision(state, box)) {
            return std::nullopt;
        }
        return add(ms_kind::crossing, box, direction, box.y_span() > 3);
    }
    if (selection >= 70) {
        ms_box box{};
        switch (direction) {
        case ms_dir::north:
            box = {0, -5, -8, 2, 2, 0};
            break;
        case ms_dir::south:
            box = {0, -5, 0, 2, 2, 8};
            break;
        case ms_dir::west:
            box = {-8, -5, 0, 0, 2, 2};
            break;
        case ms_dir::east:
            box = {0, -5, 0, 8, 2, 2};
            break;
        }
        box.move(foot_x, foot_y, foot_z);
        if (find_collision(state, box)) {
            return std::nullopt;
        }
        return add(ms_kind::stairs, box, direction, false);
    }
    jint length = random.next_int(3) + 2;
    while (length > 0) {
        const jint block_length = length * 5;
        ms_box box{};
        switch (direction) {
        case ms_dir::north:
            box = {0, 0, -(block_length - 1), 2, 2, 0};
            break;
        case ms_dir::south:
            box = {0, 0, 0, 2, 2, block_length - 1};
            break;
        case ms_dir::west:
            box = {-(block_length - 1), 0, 0, 0, 2, 2};
            break;
        case ms_dir::east:
            box = {0, 0, 0, block_length - 1, 2, 2};
            break;
        }
        box.move(foot_x, foot_y, foot_z);
        if (!find_collision(state, box)) {
            const bool rails = random.next_int(3) == 0;
            const bool spider =
                !rails && random.next_int(23) == 0;
            const auto index =
                add(ms_kind::corridor, box, direction, false);
            auto &corridor = state.pieces[index];
            corridor.sections = length;
            corridor.rails = rails;
            corridor.spider = spider;
            return index;
        }
        --length;
    }
    return std::nullopt;
}

auto generate_and_add(ms_state &state, jint foot_x, jint foot_y, jint foot_z,
                      ms_dir direction, jint depth)
    -> std::optional<std::size_t> {
    if (depth > 8) {
        return std::nullopt;
    }
    if (std::abs(foot_x - state.room.min_x) > 80 ||
        std::abs(foot_z - state.room.min_z) > 80) {
        return std::nullopt;
    }
    return create_random(state, foot_x, foot_y, foot_z, direction, depth + 1);
}

void add_children(ms_state &state, std::size_t index) {
    auto &random = *state.random;
    const auto piece = state.pieces[index];
    const auto box = piece.box;
    const jint depth = piece.depth;
    switch (piece.kind) {
    case ms_kind::room: {
        jint height_space = box.y_span() - 3 - 1;
        if (height_space <= 0) {
            height_space = 1;
        }
        jint position = 0;
        while (position < box.x_span()) {
            position += random.next_int(box.x_span());
            if (position + 3 > box.x_span()) {
                break;
            }
            if (const auto child_index = generate_and_add(
                    state, box.min_x + position,
                    box.min_y + random.next_int(height_space) + 1,
                    box.min_z - 1, ms_dir::north, depth);
                child_index) {
                const auto child_box = state.pieces[*child_index].box;
                state.pieces[index].entrances.push_back(
                    ms_box{child_box.min_x, child_box.min_y, box.min_z,
                           child_box.max_x, child_box.max_y, box.min_z + 1});
            }
            position += 4;
        }
        position = 0;
        while (position < box.x_span()) {
            position += random.next_int(box.x_span());
            if (position + 3 > box.x_span()) {
                break;
            }
            if (const auto child_index = generate_and_add(
                    state, box.min_x + position,
                    box.min_y + random.next_int(height_space) + 1,
                    box.max_z + 1, ms_dir::south, depth);
                child_index) {
                const auto child_box = state.pieces[*child_index].box;
                state.pieces[index].entrances.push_back(
                    ms_box{child_box.min_x, child_box.min_y, box.max_z - 1,
                           child_box.max_x, child_box.max_y, box.max_z});
            }
            position += 4;
        }
        position = 0;
        while (position < box.z_span()) {
            position += random.next_int(box.z_span());
            if (position + 3 > box.z_span()) {
                break;
            }
            if (const auto child_index = generate_and_add(
                    state, box.min_x - 1,
                    box.min_y + random.next_int(height_space) + 1,
                    box.min_z + position, ms_dir::west, depth);
                child_index) {
                const auto child_box = state.pieces[*child_index].box;
                state.pieces[index].entrances.push_back(
                    ms_box{box.min_x, child_box.min_y, child_box.min_z,
                           box.min_x + 1, child_box.max_y, child_box.max_z});
            }
            position += 4;
        }
        position = 0;
        while (position < box.z_span()) {
            position += random.next_int(box.z_span());
            if (position + 3 > box.z_span()) {
                break;
            }
            if (const auto child_index = generate_and_add(
                    state, box.max_x + 1,
                    box.min_y + random.next_int(height_space) + 1,
                    box.min_z + position, ms_dir::east, depth);
                child_index) {
                const auto child_box = state.pieces[*child_index].box;
                state.pieces[index].entrances.push_back(
                    ms_box{box.max_x - 1, child_box.min_y, child_box.min_z,
                           box.max_x, child_box.max_y, child_box.max_z});
            }
            position += 4;
        }
        break;
    }
    case ms_kind::corridor: {
        const jint end_selection = random.next_int(4);
        const jint y = box.min_y - 1 + random.next_int(3);
        switch (piece.direction) {
        case ms_dir::north:
            if (end_selection <= 1) {
                (void)generate_and_add(state, box.min_x, y, box.min_z - 1,
                                       ms_dir::north, depth);
            } else if (end_selection == 2) {
                (void)generate_and_add(state, box.min_x - 1, y, box.min_z,
                                       ms_dir::west, depth);
            } else {
                (void)generate_and_add(state, box.max_x + 1, y, box.min_z,
                                       ms_dir::east, depth);
            }
            break;
        case ms_dir::south:
            if (end_selection <= 1) {
                (void)generate_and_add(state, box.min_x, y, box.max_z + 1,
                                       ms_dir::south, depth);
            } else if (end_selection == 2) {
                (void)generate_and_add(state, box.min_x - 1, y, box.max_z - 3,
                                       ms_dir::west, depth);
            } else {
                (void)generate_and_add(state, box.max_x + 1, y, box.max_z - 3,
                                       ms_dir::east, depth);
            }
            break;
        case ms_dir::west:
            if (end_selection <= 1) {
                (void)generate_and_add(state, box.min_x - 1, y, box.min_z,
                                       ms_dir::west, depth);
            } else if (end_selection == 2) {
                (void)generate_and_add(state, box.min_x, y, box.min_z - 1,
                                       ms_dir::north, depth);
            } else {
                (void)generate_and_add(state, box.min_x, y, box.max_z + 1,
                                       ms_dir::south, depth);
            }
            break;
        case ms_dir::east:
            if (end_selection <= 1) {
                (void)generate_and_add(state, box.max_x + 1, y, box.min_z,
                                       ms_dir::east, depth);
            } else if (end_selection == 2) {
                (void)generate_and_add(state, box.max_x - 3, y, box.min_z - 1,
                                       ms_dir::north, depth);
            } else {
                (void)generate_and_add(state, box.max_x - 3, y, box.max_z + 1,
                                       ms_dir::south, depth);
            }
            break;
        }
        if (depth < 8) {
            if (piece.direction == ms_dir::north ||
                piece.direction == ms_dir::south) {
                for (jint z = box.min_z + 3; z + 3 <= box.max_z; z += 5) {
                    const jint selection = random.next_int(5);
                    if (selection == 0) {
                        (void)generate_and_add(state, box.min_x - 1, box.min_y,
                                               z, ms_dir::west, depth + 1);
                    } else if (selection == 1) {
                        (void)generate_and_add(state, box.max_x + 1, box.min_y,
                                               z, ms_dir::east, depth + 1);
                    }
                }
            } else {
                for (jint x = box.min_x + 3; x + 3 <= box.max_x; x += 5) {
                    const jint selection = random.next_int(5);
                    if (selection == 0) {
                        (void)generate_and_add(state, x, box.min_y,
                                               box.min_z - 1, ms_dir::north,
                                               depth + 1);
                    } else if (selection == 1) {
                        (void)generate_and_add(state, x, box.min_y,
                                               box.max_z + 1, ms_dir::south,
                                               depth + 1);
                    }
                }
            }
        }
        break;
    }
    case ms_kind::crossing: {
        switch (piece.direction) {
        case ms_dir::north:
            (void)generate_and_add(state, box.min_x + 1, box.min_y,
                                   box.min_z - 1, ms_dir::north, depth);
            (void)generate_and_add(state, box.min_x - 1, box.min_y,
                                   box.min_z + 1, ms_dir::west, depth);
            (void)generate_and_add(state, box.max_x + 1, box.min_y,
                                   box.min_z + 1, ms_dir::east, depth);
            break;
        case ms_dir::south:
            (void)generate_and_add(state, box.min_x + 1, box.min_y,
                                   box.max_z + 1, ms_dir::south, depth);
            (void)generate_and_add(state, box.min_x - 1, box.min_y,
                                   box.min_z + 1, ms_dir::west, depth);
            (void)generate_and_add(state, box.max_x + 1, box.min_y,
                                   box.min_z + 1, ms_dir::east, depth);
            break;
        case ms_dir::west:
            (void)generate_and_add(state, box.min_x + 1, box.min_y,
                                   box.min_z - 1, ms_dir::north, depth);
            (void)generate_and_add(state, box.min_x + 1, box.min_y,
                                   box.max_z + 1, ms_dir::south, depth);
            (void)generate_and_add(state, box.min_x - 1, box.min_y,
                                   box.min_z + 1, ms_dir::west, depth);
            break;
        case ms_dir::east:
            (void)generate_and_add(state, box.min_x + 1, box.min_y,
                                   box.min_z - 1, ms_dir::north, depth);
            (void)generate_and_add(state, box.min_x + 1, box.min_y,
                                   box.max_z + 1, ms_dir::south, depth);
            (void)generate_and_add(state, box.max_x + 1, box.min_y,
                                   box.min_z + 1, ms_dir::east, depth);
            break;
        }
        if (piece.two_floored) {
            if (random.next_boolean()) {
                (void)generate_and_add(state, box.min_x + 1, box.min_y + 3 + 1,
                                       box.min_z - 1, ms_dir::north, depth);
            }
            if (random.next_boolean()) {
                (void)generate_and_add(state, box.min_x - 1, box.min_y + 3 + 1,
                                       box.min_z + 1, ms_dir::west, depth);
            }
            if (random.next_boolean()) {
                (void)generate_and_add(state, box.max_x + 1, box.min_y + 3 + 1,
                                       box.min_z + 1, ms_dir::east, depth);
            }
            if (random.next_boolean()) {
                (void)generate_and_add(state, box.min_x + 1, box.min_y + 3 + 1,
                                       box.max_z + 1, ms_dir::south, depth);
            }
        }
        break;
    }
    case ms_kind::stairs:
        switch (piece.direction) {
        case ms_dir::north:
            (void)generate_and_add(state, box.min_x, box.min_y, box.min_z - 1,
                                   ms_dir::north, depth);
            break;
        case ms_dir::south:
            (void)generate_and_add(state, box.min_x, box.min_y, box.max_z + 1,
                                   ms_dir::south, depth);
            break;
        case ms_dir::west:
            (void)generate_and_add(state, box.min_x - 1, box.min_y, box.min_z,
                                   ms_dir::west, depth);
            break;
        case ms_dir::east:
            (void)generate_and_add(state, box.max_x + 1, box.min_y, box.min_z,
                                   ms_dir::east, depth);
            break;
        }
        break;
    }
}

auto piece_type_name(ms_kind kind) -> std::string_view {
    switch (kind) {
    case ms_kind::room:
        return "minecraft:msroom";
    case ms_kind::corridor:
        return "minecraft:mscorridor";
    case ms_kind::crossing:
        return "minecraft:mscrossing";
    case ms_kind::stairs:
        return "minecraft:msstairs";
    }
    return {};
}

auto direction_orientation(ms_dir direction) -> jint {
    switch (direction) {
    case ms_dir::south:
        return 0;
    case ms_dir::west:
        return 1;
    case ms_dir::north:
        return 2;
    case ms_dir::east:
        return 3;
    }
    return 2;
}

} // namespace

auto mineshaft_start(const chunk_pos &chunk, jlong seed, jint min_y,
                     jint sea_level, bool mesa) -> structure_start {
    (void)mesa;
    worldgen_random random{std::make_unique<legacy_random_source>(0)};
    random.set_large_feature_seed(seed, chunk.x, chunk.z);
    (void)random.next_double();
    const jint west = chunk.x * 16 + 2;
    const jint north = chunk.z * 16 + 2;
    ms_state state{.pieces = {}, .room = {}, .random = &random};
    state.room = {.min_x = west,
                  .min_y = 50,
                  .min_z = north,
                  .max_x = west + 7 + random.next_int(6),
                  .max_y = 54 + random.next_int(6),
                  .max_z = north + 7 + random.next_int(6)};
    state.pieces.push_back(ms_piece{.kind = ms_kind::room,
                                    .box = state.room,
                                    .depth = 0,
                                    .direction = ms_dir::north,
                                    .two_floored = false});
    add_children(state, 0);
    ms_box bounds = state.pieces.front().box;
    for (const auto &piece : state.pieces) {
        bounds.min_x = std::min(bounds.min_x, piece.box.min_x);
        bounds.min_y = std::min(bounds.min_y, piece.box.min_y);
        bounds.min_z = std::min(bounds.min_z, piece.box.min_z);
        bounds.max_x = std::max(bounds.max_x, piece.box.max_x);
        bounds.max_y = std::max(bounds.max_y, piece.box.max_y);
        bounds.max_z = std::max(bounds.max_z, piece.box.max_z);
    }
    const jint max_y = sea_level - 10;
    jint y1_position = bounds.y_span() + min_y + 1;
    if (y1_position < max_y) {
        y1_position += random.next_int(max_y - y1_position);
    }
    const jint dy = y1_position - bounds.max_y;
    for (auto &piece : state.pieces) {
        piece.box.move(0, dy, 0);
        for (auto &entrance : piece.entrances) {
            entrance.move(0, dy, 0);
        }
    }
    bounds.move(0, dy, 0);
    structure_start start{.structure = "minecraft:mineshaft",
                          .chunk = chunk,
                          .references = 0,
                          .max_references = 1,
                          .bounds = {.min_x = bounds.min_x,
                                     .min_y = bounds.min_y,
                                     .min_z = bounds.min_z,
                                     .max_x = bounds.max_x,
                                     .max_y = bounds.max_y,
                                     .max_z = bounds.max_z}};
    for (const auto &piece : state.pieces) {
        start.pieces.push_back(structure_piece{
            .type = std::string{piece_type_name(piece.kind)},
            .bounds = {.min_x = piece.box.min_x,
                       .min_y = piece.box.min_y,
                       .min_z = piece.box.min_z,
                       .max_x = piece.box.max_x,
                       .max_y = piece.box.max_y,
                       .max_z = piece.box.max_z},
            .orientation =
                (piece.kind == ms_kind::room || piece.kind == ms_kind::crossing)
                    ? -1
                    : direction_orientation(piece.direction),
            .entrances = [&piece] {
                std::vector<structure_bbox> boxes;
                boxes.reserve(piece.entrances.size());
                for (const auto &entrance : piece.entrances) {
                    boxes.push_back(
                        {.min_x = entrance.min_x,
                         .min_y = entrance.min_y,
                         .min_z = entrance.min_z,
                         .max_x = entrance.max_x,
                         .max_y = entrance.max_y,
                         .max_z = entrance.max_z});
                }
                return boxes;
            }(),
            .sections = piece.sections,
            .rails = piece.rails,
            .spider = piece.spider,
            .two_floored = piece.two_floored,
            .mesa = mesa,
        });
    }
    return start;
}

auto jungle_temple_start(
    const chunk_pos &chunk, jlong seed, jint sea_level,
    const std::function<jint(jint, jint, heightmap_kind)> &height_at)
    -> std::optional<structure_start> {
    constexpr jint width = 12;
    constexpr jint depth = 15;
    const jint min_x = chunk.x * 16;
    const jint min_z = chunk.z * 16;
    const jint corner_heights[4] = {
        height_at(min_x, min_z, heightmap_kind::world_surface_wg),
        height_at(min_x, min_z + depth, heightmap_kind::world_surface_wg),
        height_at(min_x + width, min_z, heightmap_kind::world_surface_wg),
        height_at(min_x + width, min_z + depth,
                  heightmap_kind::world_surface_wg),
    };
    const jint lowest =
        std::min(std::min(corner_heights[0], corner_heights[1]),
                 std::min(corner_heights[2], corner_heights[3]));
    if (lowest < sea_level) {
        return std::nullopt;
    }
    worldgen_random random{std::make_unique<legacy_random_source>(0)};
    random.set_large_feature_seed(seed, chunk.x, chunk.z);
    static constexpr std::array<jint, 4> horizontal{2, 3, 0, 1};
    const jint direction =
        horizontal[static_cast<std::size_t>(random.next_int(4))];
    const bool z_axis = direction == 2 || direction == 0;
    const jint max_x = min_x + (z_axis ? width - 1 : depth - 1);
    const jint max_z = min_z + (z_axis ? depth - 1 : width - 1);
    structure_start start{.structure = "minecraft:jungle_pyramid",
                          .chunk = chunk,
                          .references = 0,
                          .max_references = 1,
                          .bounds = {.min_x = min_x,
                                     .min_y = 64,
                                     .min_z = min_z,
                                     .max_x = max_x,
                                     .max_y = 73,
                                     .max_z = max_z}};
    start.pieces.push_back(structure_piece{
        .type = "minecraft:tejp",
        .bounds = start.bounds,
        .orientation = direction,
    });
    return start;
}

} // namespace refrain::worldgen::structure
