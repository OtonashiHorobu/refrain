// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Stronghold start generation and concentric-ring candidate placement. The
// start builder reproduces the recursive piece fitters, their weighted draw
// order and the filler corridor fallback; ring positions reproduce the
// legacy-random ring geometry and the per-ring biome search.

#include <refrain/worldgen/structure/generation.hpp>

#include <refrain/worldgen/core/rng.hpp>
#include <refrain/worldgen/structure/stage.hpp>
#include <refrain/worldgen/structure/start.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::worldgen::structure {

namespace {

using stronghold_piece_kind::chest_corridor;
using stronghold_piece_kind::filler_corridor;
using stronghold_piece_kind::five_crossing;
using stronghold_piece_kind::left_turn;
using stronghold_piece_kind::library;
using stronghold_piece_kind::portal_room;
using stronghold_piece_kind::prison_hall;
using stronghold_piece_kind::right_turn;
using stronghold_piece_kind::room_crossing;
using stronghold_piece_kind::stairs_down;
using stronghold_piece_kind::start;
using stronghold_piece_kind::straight;
using stronghold_piece_kind::straight_stairs_down;

constexpr jint max_depth = 50;
constexpr jint lowest_y_position = 10;
constexpr jint magic_start_y = 64;
constexpr jint max_horizontal_distance = 112;
constexpr jint below_sea_offset = 10;

// Horizontal directions stored as `Direction.get2DDataValue`.
constexpr jint orientation_south = 0;
constexpr jint orientation_west = 1;
constexpr jint orientation_north = 2;
constexpr jint orientation_east = 3;

auto random_horizontal(worldgen_random &random) -> jint {
    static constexpr std::array<jint, 4> orientations{
        orientation_north, orientation_east, orientation_south,
        orientation_west};
    return orientations[static_cast<std::size_t>(random.next_int(4))];
}

auto make_bounding_box(jint west, jint floor, jint north, jint orientation,
                       jint width, jint height, jint depth) -> structure_bbox {
    const bool along_z =
        orientation == orientation_north || orientation == orientation_south;
    return structure_bbox{
        .min_x = west,
        .min_y = floor,
        .min_z = north,
        .max_x = west + (along_z ? width : depth) - 1,
        .max_y = floor + height - 1,
        .max_z = north + (along_z ? depth : width) - 1,
    };
}

/// `BoundingBox.orientBox`.
auto oriented_box(jint foot_x, jint foot_y, jint foot_z, jint off_x, jint off_y,
                  jint off_z, jint width, jint height, jint depth,
                  jint orientation) -> structure_bbox {
    switch (orientation) {
    case orientation_north:
        return structure_bbox{
            .min_x = foot_x + off_x,
            .min_y = foot_y + off_y,
            .min_z = foot_z - depth + 1 + off_z,
            .max_x = foot_x + width - 1 + off_x,
            .max_y = foot_y + height - 1 + off_y,
            .max_z = foot_z + off_z,
        };
    case orientation_west:
        return structure_bbox{
            .min_x = foot_x - depth + 1 + off_z,
            .min_y = foot_y + off_y,
            .min_z = foot_z + off_x,
            .max_x = foot_x + off_z,
            .max_y = foot_y + height - 1 + off_y,
            .max_z = foot_z + width - 1 + off_x,
        };
    case orientation_east:
        return structure_bbox{
            .min_x = foot_x + off_z,
            .min_y = foot_y + off_y,
            .min_z = foot_z + off_x,
            .max_x = foot_x + depth - 1 + off_z,
            .max_y = foot_y + height - 1 + off_y,
            .max_z = foot_z + width - 1 + off_x,
        };
    default: // south
        return structure_bbox{
            .min_x = foot_x + off_x,
            .min_y = foot_y + off_y,
            .min_z = foot_z + off_z,
            .max_x = foot_x + width - 1 + off_x,
            .max_y = foot_y + height - 1 + off_y,
            .max_z = foot_z + depth - 1 + off_z,
        };
    }
}

constexpr auto piece_type(stronghold_piece_kind kind) -> std::string_view {
    switch (kind) {
    case start:
        return "minecraft:shstart";
    case filler_corridor:
        return "minecraft:shfc";
    case chest_corridor:
        return "minecraft:shcc";
    case five_crossing:
        return "minecraft:sh5c";
    case left_turn:
        return "minecraft:shlt";
    case right_turn:
        return "minecraft:shrt";
    case library:
        return "minecraft:shli";
    case portal_room:
        return "minecraft:shpr";
    case prison_hall:
        return "minecraft:shph";
    case room_crossing:
        return "minecraft:shrc";
    case stairs_down:
        return "minecraft:shsd";
    case straight:
        return "minecraft:shs";
    case straight_stairs_down:
        return "minecraft:shssd";
    }
    return "minecraft:shs";
}

auto random_small_door(worldgen_random &random) -> stronghold_door {
    switch (random.next_int(5)) {
    case 2:
        return stronghold_door::wood_door;
    case 3:
        return stronghold_door::grates;
    case 4:
        return stronghold_door::iron_door;
    default:
        return stronghold_door::opening;
    }
}

struct piece_weight {
    stronghold_piece_kind kind;
    jint weight;
    jint max_place_count;
    jint place_count{0};
};

constexpr std::array<piece_weight, 11> base_weights{{
    {.kind = straight, .weight = 40, .max_place_count = 0},
    {.kind = prison_hall, .weight = 5, .max_place_count = 5},
    {.kind = left_turn, .weight = 20, .max_place_count = 0},
    {.kind = right_turn, .weight = 20, .max_place_count = 0},
    {.kind = room_crossing, .weight = 10, .max_place_count = 6},
    {.kind = straight_stairs_down, .weight = 5, .max_place_count = 5},
    {.kind = stairs_down, .weight = 5, .max_place_count = 5},
    {.kind = five_crossing, .weight = 5, .max_place_count = 4},
    {.kind = chest_corridor, .weight = 5, .max_place_count = 4},
    {.kind = library, .weight = 10, .max_place_count = 2},
    {.kind = portal_room, .weight = 20, .max_place_count = 1},
}};

struct created_piece {
    stronghold_piece_kind kind{start};
    structure_bbox bounds{};
    jint orientation{0};
    stronghold_piece_data data{};
};

struct stronghold_layout {
    worldgen_random *random{nullptr};
    std::vector<structure_piece> pieces{};
    std::vector<piece_weight> weights{};
    std::vector<std::size_t> pending{};
    std::optional<stronghold_piece_kind> imposed{};
    std::optional<stronghold_piece_kind> previous{};
    jint total_weight{0};
    bool portal_found{false};
    structure_bbox start_box{};

    void reset_weights() {
        weights.assign(base_weights.begin(), base_weights.end());
        for (auto &weight : weights) {
            weight.place_count = 0;
        }
        imposed.reset();
        previous.reset();
        total_weight = 0;
    }

    [[nodiscard]] auto update_weight() -> bool {
        bool has_any = false;
        total_weight = 0;
        for (const auto &weight : weights) {
            if (weight.max_place_count > 0 &&
                weight.place_count < weight.max_place_count) {
                has_any = true;
            }
            total_weight += weight.weight;
        }
        return has_any;
    }

    [[nodiscard]] auto collides(const structure_bbox &box) const -> bool {
        return std::any_of(pieces.begin(), pieces.end(),
                           [&](const structure_piece &piece) {
                               return piece.bounds.intersects(box);
                           });
    }

    void append(created_piece piece, bool pending_child) {
        stronghold_piece_data data = piece.data;
        data.kind = piece.kind;
        pieces.push_back(structure_piece{
            .type = std::string{piece_type(piece.kind)},
            .bounds = piece.bounds,
            .orientation = piece.orientation,
            .stronghold = data,
        });
        if (pending_child) {
            pending.push_back(pieces.size() - 1);
        }
    }

    [[nodiscard]] auto get(std::size_t index) const -> const structure_piece & {
        return pieces[index];
    }
};

[[nodiscard]] auto do_place(stronghold_piece_kind kind, jint place_count,
                            jint max_place_count, jint depth) -> bool {
    const bool available =
        max_place_count == 0 || place_count < max_place_count;
    if (kind == library) {
        return available && depth > 4;
    }
    if (kind == portal_room) {
        return available && depth > 5;
    }
    return available;
}

[[nodiscard]] auto is_valid_weight(jint place_count, jint max_place_count)
    -> bool {
    return max_place_count == 0 || place_count < max_place_count;
}

[[nodiscard]] auto find_and_create(stronghold_layout &layout,
                                   stronghold_piece_kind kind, jint foot_x,
                                   jint foot_y, jint foot_z, jint orientation,
                                   jint gen_depth)
    -> std::optional<created_piece> {
    auto &random = *layout.random;
    stronghold_piece_data data{.kind = kind, .gen_depth = gen_depth};
    structure_bbox box{};
    bool constructor_random = true;

    switch (kind) {
    case straight:
        box = oriented_box(foot_x, foot_y, foot_z, -1, -1, 0, 5, 5, 7,
                           orientation);
        break;
    case prison_hall:
        box = oriented_box(foot_x, foot_y, foot_z, -1, -1, 0, 9, 5, 11,
                           orientation);
        break;
    case left_turn:
    case right_turn:
        box = oriented_box(foot_x, foot_y, foot_z, -1, -1, 0, 5, 5, 5,
                           orientation);
        break;
    case room_crossing:
        box = oriented_box(foot_x, foot_y, foot_z, -4, -1, 0, 11, 7, 11,
                           orientation);
        break;
    case straight_stairs_down:
        box = oriented_box(foot_x, foot_y, foot_z, -1, -7, 0, 5, 11, 8,
                           orientation);
        break;
    case stairs_down:
        box = oriented_box(foot_x, foot_y, foot_z, -1, -7, 0, 5, 11, 5,
                           orientation);
        break;
    case five_crossing:
        box = oriented_box(foot_x, foot_y, foot_z, -4, -3, 0, 10, 9, 11,
                           orientation);
        break;
    case chest_corridor:
        box = oriented_box(foot_x, foot_y, foot_z, -1, -1, 0, 5, 5, 7,
                           orientation);
        break;
    case portal_room:
        box = oriented_box(foot_x, foot_y, foot_z, -4, -1, 0, 11, 8, 16,
                           orientation);
        constructor_random = false;
        break;
    case library: {
        box = oriented_box(foot_x, foot_y, foot_z, -4, -1, 0, 14, 11, 15,
                           orientation);
        if (box.min_y <= lowest_y_position || layout.collides(box)) {
            box = oriented_box(foot_x, foot_y, foot_z, -4, -1, 0, 14, 6, 15,
                               orientation);
            if (box.min_y <= lowest_y_position || layout.collides(box)) {
                return std::nullopt;
            }
        }
        break;
    }
    default:
        return std::nullopt;
    }

    if (box.min_y <= lowest_y_position || layout.collides(box)) {
        return std::nullopt;
    }

    if (constructor_random) {
        switch (kind) {
        case straight:
            data.door = random_small_door(random);
            data.left = random.next_int(2) == 0;
            data.right = random.next_int(2) == 0;
            break;
        case five_crossing:
            data.door = random_small_door(random);
            data.left_low = random.next_boolean();
            data.left_high = random.next_boolean();
            data.right_low = random.next_boolean();
            data.right_high = random.next_int(3) > 0;
            break;
        case room_crossing:
            data.door = random_small_door(random);
            data.variant = random.next_int(5);
            break;
        case library:
            data.door = random_small_door(random);
            data.tall = box.max_y - box.min_y + 1 > 6;
            break;
        default:
            data.door = random_small_door(random);
            break;
        }
    }

    return created_piece{
        .kind = kind, .bounds = box, .orientation = orientation, .data = data};
}

[[nodiscard]] auto filler_corridor_box(const stronghold_layout &layout,
                                       jint foot_x, jint foot_y, jint foot_z,
                                       jint orientation)
    -> std::optional<structure_bbox> {
    auto box =
        oriented_box(foot_x, foot_y, foot_z, -1, -1, 0, 5, 5, 4, orientation);
    const structure_piece *collision = nullptr;
    for (const auto &piece : layout.pieces) {
        if (piece.bounds.intersects(box)) {
            collision = &piece;
            break;
        }
    }
    if (collision == nullptr) {
        return std::nullopt;
    }
    if (collision->bounds.min_y == box.min_y) {
        for (jint depth = 2; depth >= 1; --depth) {
            box = oriented_box(foot_x, foot_y, foot_z, -1, -1, 0, 5, 5, depth,
                               orientation);
            if (!collision->bounds.intersects(box)) {
                return oriented_box(foot_x, foot_y, foot_z, -1, -1, 0, 5, 5,
                                    depth + 1, orientation);
            }
        }
    }
    return std::nullopt;
}

[[nodiscard]] auto generate_piece_from_small_door(stronghold_layout &layout,
                                                  jint foot_x, jint foot_y,
                                                  jint foot_z, jint orientation,
                                                  jint depth)
    -> std::optional<created_piece> {
    if (!layout.update_weight()) {
        return std::nullopt;
    }
    if (layout.imposed.has_value()) {
        const auto kind = *layout.imposed;
        layout.imposed.reset();
        if (auto piece = find_and_create(layout, kind, foot_x, foot_y, foot_z,
                                         orientation, depth)) {
            return piece;
        }
    }

    for (jint attempt = 0; attempt < 5; ++attempt) {
        jint selection = layout.random->next_int(layout.total_weight);
        for (std::size_t index = 0; index < layout.weights.size(); ++index) {
            selection -= layout.weights[index].weight;
            if (selection >= 0) {
                continue;
            }
            auto &weight = layout.weights[index];
            if (!do_place(weight.kind, weight.place_count,
                          weight.max_place_count, depth) ||
                (layout.previous.has_value() &&
                 *layout.previous == weight.kind)) {
                break;
            }
            auto piece = find_and_create(layout, weight.kind, foot_x, foot_y,
                                         foot_z, orientation, depth);
            if (!piece) {
                continue;
            }
            ++weight.place_count;
            layout.previous = weight.kind;
            if (!is_valid_weight(weight.place_count, weight.max_place_count)) {
                layout.weights.erase(layout.weights.begin() +
                                     static_cast<std::ptrdiff_t>(index));
            }
            return piece;
        }
    }

    if (auto box =
            filler_corridor_box(layout, foot_x, foot_y, foot_z, orientation)) {
        if (box->min_y > 1) {
            stronghold_piece_data data{
                .kind = filler_corridor,
                .steps = (orientation == orientation_north ||
                          orientation == orientation_south)
                             ? box->max_z - box->min_z + 1
                             : box->max_x - box->min_x + 1,
                .gen_depth = depth,
            };
            return created_piece{.kind = filler_corridor,
                                 .bounds = *box,
                                 .orientation = orientation,
                                 .data = data};
        }
    }
    return std::nullopt;
}

auto generate_and_add(stronghold_layout &layout, jint foot_x, jint foot_y,
                      jint foot_z, jint orientation, jint depth) -> void {
    if (depth > max_depth) {
        return;
    }
    if (std::abs(foot_x - layout.start_box.min_x) > max_horizontal_distance ||
        std::abs(foot_z - layout.start_box.min_z) > max_horizontal_distance) {
        return;
    }
    if (auto piece = generate_piece_from_small_door(
            layout, foot_x, foot_y, foot_z, orientation, depth + 1)) {
        layout.append(*piece, true);
    }
}

void forward_child(stronghold_layout &layout, const structure_bbox &box,
                   jint orientation, jint gen_depth, jint x_off, jint y_off) {
    switch (orientation) {
    case orientation_north:
        generate_and_add(layout, box.min_x + x_off, box.min_y + y_off,
                         box.min_z - 1, orientation_north, gen_depth);
        break;
    case orientation_south:
        generate_and_add(layout, box.min_x + x_off, box.min_y + y_off,
                         box.max_z + 1, orientation_south, gen_depth);
        break;
    case orientation_west:
        generate_and_add(layout, box.min_x - 1, box.min_y + y_off,
                         box.min_z + x_off, orientation_west, gen_depth);
        break;
    case orientation_east:
        generate_and_add(layout, box.max_x + 1, box.min_y + y_off,
                         box.min_z + x_off, orientation_east, gen_depth);
        break;
    default:
        break;
    }
}

void left_child(stronghold_layout &layout, const structure_bbox &box,
                jint orientation, jint gen_depth, jint y_off, jint z_off) {
    switch (orientation) {
    case orientation_north:
    case orientation_south:
        generate_and_add(layout, box.min_x - 1, box.min_y + y_off,
                         box.min_z + z_off, orientation_west, gen_depth);
        break;
    case orientation_west:
    case orientation_east:
        generate_and_add(layout, box.min_x + z_off, box.min_y + y_off,
                         box.min_z - 1, orientation_north, gen_depth);
        break;
    default:
        break;
    }
}

void right_child(stronghold_layout &layout, const structure_bbox &box,
                 jint orientation, jint gen_depth, jint y_off, jint z_off) {
    switch (orientation) {
    case orientation_north:
    case orientation_south:
        generate_and_add(layout, box.max_x + 1, box.min_y + y_off,
                         box.min_z + z_off, orientation_east, gen_depth);
        break;
    case orientation_west:
    case orientation_east:
        generate_and_add(layout, box.min_x + z_off, box.min_y + y_off,
                         box.max_z + 1, orientation_south, gen_depth);
        break;
    default:
        break;
    }
}

void add_children(stronghold_layout &layout, std::size_t index) {
    const structure_bbox box = layout.get(index).bounds;
    const jint orientation = layout.get(index).orientation;
    const stronghold_piece_data data = *layout.get(index).stronghold;
    switch (data.kind) {
    case start:
    case stairs_down:
        if (data.source) {
            layout.imposed = five_crossing;
        }
        forward_child(layout, box, orientation, data.gen_depth, 1, 1);
        break;
    case straight:
        forward_child(layout, box, orientation, data.gen_depth, 1, 1);
        if (data.left) {
            left_child(layout, box, orientation, data.gen_depth, 1, 2);
        }
        if (data.right) {
            right_child(layout, box, orientation, data.gen_depth, 1, 2);
        }
        break;
    case prison_hall:
    case chest_corridor:
    case straight_stairs_down:
        forward_child(layout, box, orientation, data.gen_depth, 1, 1);
        break;
    case left_turn:
        if (orientation != orientation_north &&
            orientation != orientation_east) {
            right_child(layout, box, orientation, data.gen_depth, 1, 1);
        } else {
            left_child(layout, box, orientation, data.gen_depth, 1, 1);
        }
        break;
    case right_turn:
        if (orientation != orientation_north &&
            orientation != orientation_east) {
            left_child(layout, box, orientation, data.gen_depth, 1, 1);
        } else {
            right_child(layout, box, orientation, data.gen_depth, 1, 1);
        }
        break;
    case room_crossing:
        forward_child(layout, box, orientation, data.gen_depth, 4, 1);
        left_child(layout, box, orientation, data.gen_depth, 1, 4);
        right_child(layout, box, orientation, data.gen_depth, 1, 4);
        break;
    case five_crossing: {
        jint z_off_a = 3;
        jint z_off_b = 5;
        if (orientation == orientation_west ||
            orientation == orientation_north) {
            z_off_a = 8 - z_off_a;
            z_off_b = 8 - z_off_b;
        }
        forward_child(layout, box, orientation, data.gen_depth, 5, 1);
        if (data.left_low) {
            left_child(layout, box, orientation, data.gen_depth, z_off_a, 1);
        }
        if (data.left_high) {
            left_child(layout, box, orientation, data.gen_depth, z_off_b, 7);
        }
        if (data.right_low) {
            right_child(layout, box, orientation, data.gen_depth, z_off_a, 1);
        }
        if (data.right_high) {
            right_child(layout, box, orientation, data.gen_depth, z_off_b, 7);
        }
        break;
    }
    case portal_room:
        layout.portal_found = true;
        break;
    case library:
    case filler_corridor:
        break;
    }
}

auto union_bounds(const std::vector<structure_piece> &pieces)
    -> structure_bbox {
    structure_bbox box = pieces.front().bounds;
    for (const auto &piece : pieces) {
        box.min_x = std::min(box.min_x, piece.bounds.min_x);
        box.min_y = std::min(box.min_y, piece.bounds.min_y);
        box.min_z = std::min(box.min_z, piece.bounds.min_z);
        box.max_x = std::max(box.max_x, piece.bounds.max_x);
        box.max_y = std::max(box.max_y, piece.bounds.max_y);
        box.max_z = std::max(box.max_z, piece.bounds.max_z);
    }
    return box;
}

auto preferred_biome_match(const structure_start_context &context,
                           const concentric_rings_placement &placement,
                           std::string_view biome) -> bool {
    if (!placement.preferred_biomes_tag.empty()) {
        if (context.data == nullptr) {
            return false;
        }
        return context.data->biome_matches_tag(placement.preferred_biomes_tag,
                                               biome);
    }
    return std::find(placement.preferred_biomes_ids.begin(),
                     placement.preferred_biomes_ids.end(),
                     biome) != placement.preferred_biomes_ids.end();
}

auto fixed_ring_position(const structure_start_context &context,
                         const concentric_rings_placement &placement,
                         jint origin_x, jint origin_z, worldgen_random &fork)
    -> chunk_pos {
    const auto biome =
        context.biome_at ? context.biome_at(0, 0, 0) : std::uint16_t{0};
    const auto name =
        context.biome_name ? context.biome_name(biome) : std::string_view{};
    if (context.biome_at && context.biome_name &&
        preferred_biome_match(context, placement, name)) {
        const jint x = origin_x - max_horizontal_distance +
                       fork.next_int(2 * max_horizontal_distance + 1);
        const jint z = origin_z - max_horizontal_distance +
                       fork.next_int(2 * max_horizontal_distance + 1);
        return chunk_pos{.x = x >> 4, .z = z >> 4};
    }
    return chunk_pos{.x = origin_x >> 4, .z = origin_z >> 4};
}

auto sampled_ring_position(const structure_start_context &context,
                           const concentric_rings_placement &placement,
                           jint origin_x, jint origin_z, worldgen_random &fork)
    -> chunk_pos {
    if (!context.biome_at || !context.biome_name) {
        return chunk_pos{.x = origin_x >> 4, .z = origin_z >> 4};
    }
    const jint center_x = origin_x >> 2;
    const jint center_z = origin_z >> 2;
    constexpr jint radius = max_horizontal_distance >> 2;
    std::optional<chunk_pos> result;
    jint found = 0;
    for (jint z = -radius; z <= radius; ++z) {
        for (jint x = -radius; x <= radius; ++x) {
            const jint noise_x = center_x + x;
            const jint noise_z = center_z + z;
            const auto biome = context.biome_at(noise_x, 0, noise_z);
            if (!preferred_biome_match(context, placement,
                                       context.biome_name(biome))) {
                continue;
            }
            if (!result.has_value() || fork.next_int(found + 1) == 0) {
                result = chunk_pos{.x = noise_x >> 2, .z = noise_z >> 2};
            }
            ++found;
        }
    }
    if (result.has_value()) {
        return *result;
    }
    return chunk_pos{.x = origin_x >> 4, .z = origin_z >> 4};
}

} // namespace

auto concentric_ring_positions(jlong seed,
                               const concentric_rings_placement &placement,
                               const structure_start_context &context)
    -> std::vector<chunk_pos> {
    std::vector<chunk_pos> positions;
    if (placement.count <= 0) {
        return positions;
    }
    positions.reserve(static_cast<std::size_t>(placement.count));
    worldgen_random random{std::make_unique<legacy_random_source>(0)};
    random.set_seed(seed);
    constexpr double pi = 3.14159265358979323846;
    double angle = random.next_double() * pi * 2.0;
    jint position_in_circle = 0;
    jint circle = 0;
    jint spread = placement.spread;
    for (jint i = 0; i < placement.count; ++i) {
        const double distance =
            4.0 * static_cast<double>(placement.distance) +
            static_cast<double>(placement.distance) *
                static_cast<double>(circle) * 6.0 +
            (random.next_double() - 0.5) *
                (static_cast<double>(placement.distance) * 2.5);
        const auto initial_x =
            static_cast<jint>(std::floor(std::cos(angle) * distance + 0.5));
        const auto initial_z =
            static_cast<jint>(std::floor(std::sin(angle) * distance + 0.5));
        const jint origin_x = initial_x * 16 + 8;
        const jint origin_z = initial_z * 16 + 8;
        worldgen_random fork{
            std::make_unique<legacy_random_source>(random.next_long())};
        if (context.fixed_biome) {
            positions.push_back(fixed_ring_position(context, placement,
                                                    origin_x, origin_z, fork));
        } else {
            positions.push_back(sampled_ring_position(
                context, placement, origin_x, origin_z, fork));
        }
        angle += (pi * 2.0) / static_cast<double>(spread);
        if (++position_in_circle == spread) {
            ++circle;
            position_in_circle = 0;
            spread += 2 * spread / (circle + 1);
            spread = std::min(spread, placement.count - i);
            angle += random.next_double() * pi * 2.0;
        }
    }
    return positions;
}

auto stronghold_start(const chunk_pos &chunk, jlong seed, jint min_y,
                      jint sea_level, jint max_tries) -> structure_start {
    const jint west = chunk.x * 16 + 2;
    const jint north = chunk.z * 16 + 2;
    for (jint tries = 0; tries < max_tries; ++tries) {
        auto random =
            worldgen_random{std::make_unique<legacy_random_source>(0)};
        random.set_large_feature_seed(seed + tries, chunk.x, chunk.z);
        stronghold_layout layout{.random = &random};
        layout.reset_weights();
        const jint orientation = random_horizontal(random);
        layout.start_box = make_bounding_box(west, magic_start_y, north,
                                             orientation, 5, 11, 5);
        layout.pieces.push_back(structure_piece{
            .type = std::string{piece_type(start)},
            .bounds = layout.start_box,
            .orientation = orientation,
            .stronghold =
                stronghold_piece_data{
                    .kind = start,
                    .door = stronghold_door::opening,
                    .source = true,
                    .gen_depth = 0,
                },
        });
        add_children(layout, 0);
        while (!layout.pending.empty()) {
            const auto index = static_cast<std::size_t>(
                random.next_int(static_cast<jint>(layout.pending.size())));
            const std::size_t piece_index = layout.pending[index];
            layout.pending.erase(layout.pending.begin() +
                                 static_cast<std::ptrdiff_t>(index));
            add_children(layout, piece_index);
        }

        const auto box = union_bounds(layout.pieces);
        jint top = box.max_y - box.min_y + 1 + min_y + 1;
        const jint lowest = sea_level - below_sea_offset;
        if (top < lowest) {
            top += random.next_int(lowest - top);
        }
        const jint dy = top - box.max_y;
        for (auto &piece : layout.pieces) {
            piece.bounds.min_y += dy;
            piece.bounds.max_y += dy;
        }
        if (!layout.pieces.empty() && layout.portal_found) {
            structure_start result{
                .structure = "minecraft:stronghold",
                .chunk = chunk,
                .references = 0,
                .max_references = 1,
                .bounds = union_bounds(layout.pieces),
            };
            result.pieces = std::move(layout.pieces);
            return result;
        }
    }
    return {};
}

} // namespace refrain::worldgen::structure
