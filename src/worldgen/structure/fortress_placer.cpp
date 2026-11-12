// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Nether fortress piece placement. Every bridge and castle section writes its
// oriented local box; the vertical columns that carry the bridge deck are
// extended down through replaceable terrain until they meet solid ground.

#include <refrain/worldgen/structure/placement_stage.hpp>

#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/core/rng.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace refrain::worldgen::structure {

namespace {

using fortress_piece_kind::bridge_crossing;
using fortress_piece_kind::bridge_end_filler;
using fortress_piece_kind::bridge_straight;
using fortress_piece_kind::castle_corridor_stairs;
using fortress_piece_kind::castle_corridor_t_balcony;
using fortress_piece_kind::castle_entrance;
using fortress_piece_kind::castle_small_corridor;
using fortress_piece_kind::castle_small_corridor_crossing;
using fortress_piece_kind::castle_small_corridor_left_turn;
using fortress_piece_kind::castle_small_corridor_right_turn;
using fortress_piece_kind::castle_stalk_room;
using fortress_piece_kind::monster_throne;
using fortress_piece_kind::room_crossing;
using fortress_piece_kind::stairs_room;

struct fortress_writer {
    const structure_place_context &context;
    const structure_piece_record &piece;
    template_transform transform{};
    mutable std::unordered_map<block_state_id, block_state_id> rotated{};
    block_state_id air{no_block_state};
    block_state_id bricks{no_block_state};
    block_state_id fence{no_block_state};
    block_state_id fence_ns{no_block_state};
    block_state_id fence_nse{no_block_state};
    block_state_id fence_nsw{no_block_state};
    block_state_id fence_we{no_block_state};
    block_state_id fence_es{no_block_state};
    block_state_id fence_ws{no_block_state};
    block_state_id fence_ne{no_block_state};
    block_state_id fence_nw{no_block_state};
    block_state_id fence_west{no_block_state};
    block_state_id fence_east{no_block_state};
    block_state_id stairs_north{no_block_state};
    block_state_id stairs_south{no_block_state};
    block_state_id stairs_east{no_block_state};
    block_state_id stairs_west{no_block_state};
    block_state_id soul_sand{no_block_state};
    block_state_id nether_wart{no_block_state};
    block_state_id lava{no_block_state};
    block_state_id spawner{no_block_state};
    jint chunk_min_x;
    jint chunk_max_x;
    jint chunk_min_z;
    jint chunk_max_z;
    jint chunk_min_y;
    jint chunk_max_y;

    fortress_writer(const structure_place_context &place_context,
                    const structure_piece_record &place_piece)
        : context{place_context}, piece{place_piece},
          air{resolve("minecraft:air")},
          bricks{resolve("minecraft:nether_bricks")},
          fence{resolve("minecraft:nether_brick_fence")},
          fence_ns{
              resolve("minecraft:nether_brick_fence[north=true,south=true]")},
          fence_nse{resolve("minecraft:nether_brick_fence[east=true,"
                            "north=true,south=true]")},
          fence_nsw{resolve("minecraft:nether_brick_fence[north=true,"
                            "south=true,west=true]")},
          fence_we{
              resolve("minecraft:nether_brick_fence[east=true,west=true]")},
          fence_es{
              resolve("minecraft:nether_brick_fence[east=true,south=true]")},
          fence_ws{
              resolve("minecraft:nether_brick_fence[south=true,west=true]")},
          fence_ne{
              resolve("minecraft:nether_brick_fence[east=true,north=true]")},
          fence_nw{
              resolve("minecraft:nether_brick_fence[north=true,west=true]")},
          fence_west{resolve("minecraft:nether_brick_fence[west=true]")},
          fence_east{resolve("minecraft:nether_brick_fence[east=true]")},
          stairs_north{
              resolve("minecraft:nether_brick_stairs[facing=north,half=bottom,"
                      "shape=straight,waterlogged=false]")},
          stairs_south{
              resolve("minecraft:nether_brick_stairs[facing=south,half=bottom,"
                      "shape=straight,waterlogged=false]")},
          stairs_east{resolve("minecraft:nether_brick_stairs[facing=east,"
                              "half=bottom,shape=straight,waterlogged=false]")},
          stairs_west{resolve("minecraft:nether_brick_stairs[facing=west,"
                              "half=bottom,shape=straight,waterlogged=false]")},
          soul_sand{resolve("minecraft:soul_sand")},
          nether_wart{resolve("minecraft:nether_wart[age=0]")},
          lava{resolve("minecraft:lava")},
          spawner{resolve("minecraft:spawner")},
          chunk_min_x{context.chunk->chunk_x() * 16},
          chunk_max_x{context.chunk->chunk_x() * 16 + 15},
          chunk_min_z{context.chunk->chunk_z() * 16},
          chunk_max_z{context.chunk->chunk_z() * 16 + 15},
          chunk_min_y{context.chunk->min_y()},
          chunk_max_y{context.chunk->min_y() + context.chunk->height() - 1} {
        switch (piece.orientation) {
        case 0: // south
            transform.mirror = template_mirror::left_right;
            break;
        case 1: // west
            transform.mirror = template_mirror::left_right;
            transform.rotation = template_rotation::clockwise_90;
            break;
        case 3: // east
            transform.rotation = template_rotation::clockwise_90;
            break;
        default: // north
            break;
        }
    }

    [[nodiscard]] auto resolve(std::string_view key) const -> block_state_id {
        return context.state ? context.state(key) : no_block_state;
    }

    [[nodiscard]] auto world_x(jint x, jint z) const -> jint {
        switch (piece.orientation) {
        case 2: // north
        case 0: // south
            return piece.min_x + x;
        case 1: // west
            return piece.max_x - z;
        default: // east
            return piece.min_x + z;
        }
    }

    [[nodiscard]] auto world_z(jint x, jint z) const -> jint {
        switch (piece.orientation) {
        case 2: // north
            return piece.max_z - z;
        case 0: // south
            return piece.min_z + z;
        case 1: // west
        case 3: // east
            return piece.min_z + x;
        default:
            return piece.min_z + z;
        }
    }

    [[nodiscard]] auto world_y(jint y) const -> jint { return piece.min_y + y; }

    [[nodiscard]] auto inside(jint x, jint y, jint z) const -> bool {
        const jint wx = world_x(x, z);
        const jint wz = world_z(x, z);
        const jint wy = world_y(y);
        return wx >= chunk_min_x && wx <= chunk_max_x && wz >= chunk_min_z &&
               wz <= chunk_max_z && wy >= chunk_min_y && wy <= chunk_max_y;
    }

    [[nodiscard]] auto get(jint x, jint y, jint z) const -> block_state_id {
        if (!inside(x, y, z)) {
            return air;
        }
        return context.region->state_at(world_x(x, z), world_y(y),
                                        world_z(x, z));
    }

    [[nodiscard]] auto oriented_state(block_state_id state) const
        -> block_state_id {
        const auto found = rotated.find(state);
        if (found != rotated.end()) {
            return found->second;
        }
        const auto transformed =
            transform_state(*context.registry, state, transform);
        rotated.emplace(state, transformed);
        return transformed;
    }

    void set(jint x, jint y, jint z, block_state_id state) const {
        if (inside(x, y, z)) {
            (void)context.region->set_block_state(world_x(x, z), world_y(y),
                                                  world_z(x, z),
                                                  oriented_state(state));
        }
    }

    void box(jint x0, jint y0, jint z0, jint x1, jint y1, jint z1,
             block_state_id state) const {
        for (jint y = y0; y <= y1; ++y) {
            for (jint x = x0; x <= x1; ++x) {
                for (jint z = z0; z <= z1; ++z) {
                    set(x, y, z, state);
                }
            }
        }
    }

    void fill_column_down(jint x, jint start_y, jint z,
                          block_state_id state) const {
        if (!inside(x, start_y, z)) {
            return;
        }
        const jint wx = world_x(x, z);
        const jint wz = world_z(x, z);
        jint wy = world_y(start_y);
        while (wy > chunk_min_y + 1) {
            const auto current = context.region->state_at(wx, wy, wz);
            const auto name = context.states->name(current);
            const auto traits = context.states->traits(current);
            const bool replaceable =
                traits.air || traits.fluid || name == "minecraft:glow_lichen" ||
                name.starts_with("minecraft:seagrass") ||
                name.starts_with("minecraft:tall_seagrass");
            if (!replaceable) {
                break;
            }
            (void)context.region->set_block_state(wx, wy, wz,
                                                  oriented_state(state));
            --wy;
        }
    }

    // TODO: read the nether bridge loot table from the structure document
    // instead of naming it here; the reference structure hard-codes the id, so
    // the structure document has no field for it yet.
    void chest(jint x, jint y, jint z, worldgen_random &random) const {
        if (!inside(x, y, z)) {
            return;
        }
        (void)place_loot_chest(context, world_x(x, z), world_y(y),
                               world_z(x, z), random,
                               "minecraft:chests/nether_bridge");
    }

    void spawner_block(jint x, jint y, jint z) const {
        if (!inside(x, y, z)) {
            return;
        }
        (void)context.region->set_block_state(world_x(x, z), world_y(y),
                                              world_z(x, z), spawner);
    }
};

void place_bridge_crossing(const fortress_writer &w) {
    w.box(7, 3, 0, 11, 4, 18, w.bricks);
    w.box(0, 3, 7, 18, 4, 11, w.bricks);
    w.box(8, 5, 0, 10, 7, 18, w.air);
    w.box(0, 5, 8, 18, 7, 10, w.air);
    w.box(7, 5, 0, 7, 5, 7, w.bricks);
    w.box(7, 5, 11, 7, 5, 18, w.bricks);
    w.box(11, 5, 0, 11, 5, 7, w.bricks);
    w.box(11, 5, 11, 11, 5, 18, w.bricks);
    w.box(0, 5, 7, 7, 5, 7, w.bricks);
    w.box(11, 5, 7, 18, 5, 7, w.bricks);
    w.box(0, 5, 11, 7, 5, 11, w.bricks);
    w.box(11, 5, 11, 18, 5, 11, w.bricks);
    w.box(7, 2, 0, 11, 2, 5, w.bricks);
    w.box(7, 2, 13, 11, 2, 18, w.bricks);
    w.box(7, 0, 0, 11, 1, 3, w.bricks);
    w.box(7, 0, 15, 11, 1, 18, w.bricks);
    for (jint x = 7; x <= 11; ++x) {
        for (jint z = 0; z <= 2; ++z) {
            w.fill_column_down(x, -1, z, w.bricks);
            w.fill_column_down(x, -1, 18 - z, w.bricks);
        }
    }
    w.box(0, 2, 7, 5, 2, 11, w.bricks);
    w.box(13, 2, 7, 18, 2, 11, w.bricks);
    w.box(0, 0, 7, 3, 1, 11, w.bricks);
    w.box(15, 0, 7, 18, 1, 11, w.bricks);
    for (jint x = 0; x <= 2; ++x) {
        for (jint z = 7; z <= 11; ++z) {
            w.fill_column_down(x, -1, z, w.bricks);
            w.fill_column_down(18 - x, -1, z, w.bricks);
        }
    }
}

void place_bridge_end_filler(const fortress_writer &w,
                             const fortress_piece_data &data) {
    legacy_random_source random{data.self_seed};
    for (jint x = 0; x <= 4; ++x) {
        for (jint y = 3; y <= 4; ++y) {
            const jint z = random.next_int(8);
            w.box(x, y, 0, x, y, z, w.bricks);
        }
    }
    jint z = random.next_int(8);
    w.box(0, 5, 0, 0, 5, z, w.bricks);
    z = random.next_int(8);
    w.box(4, 5, 0, 4, 5, z, w.bricks);
    for (jint x = 0; x <= 4; ++x) {
        const jint zx = random.next_int(5);
        w.box(x, 2, 0, x, 2, zx, w.bricks);
    }
    for (jint x = 0; x <= 4; ++x) {
        for (jint y = 0; y <= 1; ++y) {
            const jint zx = random.next_int(3);
            w.box(x, y, 0, x, y, zx, w.bricks);
        }
    }
}

void place_bridge_straight(const fortress_writer &w) {
    w.box(0, 3, 0, 4, 4, 18, w.bricks);
    w.box(1, 5, 0, 3, 7, 18, w.air);
    w.box(0, 5, 0, 0, 5, 18, w.bricks);
    w.box(4, 5, 0, 4, 5, 18, w.bricks);
    w.box(0, 2, 0, 4, 2, 5, w.bricks);
    w.box(0, 2, 13, 4, 2, 18, w.bricks);
    w.box(0, 0, 0, 4, 1, 3, w.bricks);
    w.box(0, 0, 15, 4, 1, 18, w.bricks);
    for (jint x = 0; x <= 4; ++x) {
        for (jint z = 0; z <= 2; ++z) {
            w.fill_column_down(x, -1, z, w.bricks);
            w.fill_column_down(x, -1, 18 - z, w.bricks);
        }
    }
    w.box(0, 1, 1, 0, 4, 1, w.fence_nse);
    w.box(0, 3, 4, 0, 4, 4, w.fence_nse);
    w.box(0, 3, 14, 0, 4, 14, w.fence_nse);
    w.box(0, 1, 17, 0, 4, 17, w.fence_nse);
    w.box(4, 1, 1, 4, 4, 1, w.fence_nsw);
    w.box(4, 3, 4, 4, 4, 4, w.fence_nsw);
    w.box(4, 3, 14, 4, 4, 14, w.fence_nsw);
    w.box(4, 1, 17, 4, 4, 17, w.fence_nsw);
}

void place_castle_corridor_stairs(const fortress_writer &w) {
    for (jint step = 0; step <= 9; ++step) {
        const jint floor = std::max(1, 7 - step);
        const jint roof = std::min(std::max(floor + 5, 14 - step), 13);
        const jint z = step;
        w.box(0, 0, step, 4, floor, step, w.bricks);
        w.box(1, floor + 1, step, 3, roof - 1, step, w.air);
        if (step <= 6) {
            w.set(1, floor + 1, step, w.stairs_south);
            w.set(2, floor + 1, step, w.stairs_south);
            w.set(3, floor + 1, step, w.stairs_south);
        }
        w.box(0, roof, step, 4, roof, step, w.bricks);
        w.box(0, floor + 1, step, 0, roof - 1, step, w.bricks);
        w.box(4, floor + 1, step, 4, roof - 1, step, w.bricks);
        if ((step & 1) == 0) {
            w.box(0, floor + 2, step, 0, floor + 3, step, w.fence_ns);
            w.box(4, floor + 2, step, 4, floor + 3, step, w.fence_ns);
        }
        for (jint x = 0; x <= 4; ++x) {
            w.fill_column_down(x, -1, z, w.bricks);
        }
    }
}

void place_castle_corridor_t_balcony(const fortress_writer &w) {
    w.box(0, 0, 0, 8, 1, 8, w.bricks);
    w.box(0, 2, 0, 8, 5, 8, w.air);
    w.box(0, 6, 0, 8, 6, 5, w.bricks);
    w.box(0, 2, 0, 2, 5, 0, w.bricks);
    w.box(6, 2, 0, 8, 5, 0, w.bricks);
    w.box(1, 3, 0, 1, 4, 0, w.fence_we);
    w.box(7, 3, 0, 7, 4, 0, w.fence_we);
    w.box(0, 2, 4, 8, 2, 8, w.bricks);
    w.box(1, 1, 4, 2, 2, 4, w.air);
    w.box(6, 1, 4, 7, 2, 4, w.air);
    w.box(1, 3, 8, 7, 3, 8, w.fence_we);
    w.set(0, 3, 8, w.fence_es);
    w.set(8, 3, 8, w.fence_ws);
    w.box(0, 3, 6, 0, 3, 7, w.fence_ns);
    w.box(8, 3, 6, 8, 3, 7, w.fence_ns);
    w.box(0, 3, 4, 0, 5, 5, w.bricks);
    w.box(8, 3, 4, 8, 5, 5, w.bricks);
    w.box(1, 3, 5, 2, 5, 5, w.bricks);
    w.box(6, 3, 5, 7, 5, 5, w.bricks);
    w.box(1, 4, 5, 1, 5, 5, w.fence_we);
    w.box(7, 4, 5, 7, 5, 5, w.fence_we);
    for (jint z = 0; z <= 5; ++z) {
        for (jint x = 0; x <= 8; ++x) {
            w.fill_column_down(x, -1, z, w.bricks);
        }
    }
}

void place_castle_entrance(const fortress_writer &w) {
    w.box(0, 3, 0, 12, 4, 12, w.bricks);
    w.box(0, 5, 0, 12, 13, 12, w.air);
    w.box(0, 5, 0, 1, 12, 12, w.bricks);
    w.box(11, 5, 0, 12, 12, 12, w.bricks);
    w.box(2, 5, 11, 4, 12, 12, w.bricks);
    w.box(8, 5, 11, 10, 12, 12, w.bricks);
    w.box(5, 9, 11, 7, 12, 12, w.bricks);
    w.box(2, 5, 0, 4, 12, 1, w.bricks);
    w.box(8, 5, 0, 10, 12, 1, w.bricks);
    w.box(5, 9, 0, 7, 12, 1, w.bricks);
    w.box(2, 11, 2, 10, 12, 10, w.bricks);
    w.box(5, 8, 0, 7, 8, 0, w.fence);
    for (jint i = 1; i <= 11; i += 2) {
        w.box(i, 10, 0, i, 11, 0, w.fence_we);
        w.box(i, 10, 12, i, 11, 12, w.fence_we);
        w.box(0, 10, i, 0, 11, i, w.fence_ns);
        w.box(12, 10, i, 12, 11, i, w.fence_ns);
        w.set(i, 13, 0, w.bricks);
        w.set(i, 13, 12, w.bricks);
        w.set(0, 13, i, w.bricks);
        w.set(12, 13, i, w.bricks);
        if (i != 11) {
            w.set(i + 1, 13, 0, w.fence_we);
            w.set(i + 1, 13, 12, w.fence_we);
            w.set(0, 13, i + 1, w.fence_ns);
            w.set(12, 13, i + 1, w.fence_ns);
        }
    }
    w.set(0, 13, 0, w.fence_ne);
    w.set(0, 13, 12, w.fence_es);
    w.set(12, 13, 12, w.fence_ws);
    w.set(12, 13, 0, w.fence_nw);
    for (jint z = 3; z <= 9; z += 2) {
        w.box(1, 7, z, 1, 8, z, w.fence_nsw);
        w.box(11, 7, z, 11, 8, z, w.fence_nse);
    }
    w.box(4, 2, 0, 8, 2, 12, w.bricks);
    w.box(0, 2, 4, 12, 2, 8, w.bricks);
    w.box(4, 0, 0, 8, 1, 3, w.bricks);
    w.box(4, 0, 9, 8, 1, 12, w.bricks);
    w.box(0, 0, 4, 3, 1, 8, w.bricks);
    w.box(9, 0, 4, 12, 1, 8, w.bricks);
    for (jint x = 4; x <= 8; ++x) {
        for (jint z = 0; z <= 2; ++z) {
            w.fill_column_down(x, -1, z, w.bricks);
            w.fill_column_down(x, -1, 12 - z, w.bricks);
        }
    }
    for (jint x = 0; x <= 2; ++x) {
        for (jint z = 4; z <= 8; ++z) {
            w.fill_column_down(x, -1, z, w.bricks);
            w.fill_column_down(12 - x, -1, z, w.bricks);
        }
    }
    w.box(5, 5, 5, 7, 5, 7, w.bricks);
    w.box(6, 1, 6, 6, 4, 6, w.air);
    w.set(6, 0, 6, w.bricks);
    w.set(6, 5, 6, w.lava);
}

void place_castle_small_corridor_crossing(const fortress_writer &w) {
    w.box(0, 0, 0, 4, 1, 4, w.bricks);
    w.box(0, 2, 0, 4, 5, 4, w.air);
    w.box(0, 2, 0, 0, 5, 0, w.bricks);
    w.box(4, 2, 0, 4, 5, 0, w.bricks);
    w.box(0, 2, 4, 0, 5, 4, w.bricks);
    w.box(4, 2, 4, 4, 5, 4, w.bricks);
    w.box(0, 6, 0, 4, 6, 4, w.bricks);
    for (jint x = 0; x <= 4; ++x) {
        for (jint z = 0; z <= 4; ++z) {
            w.fill_column_down(x, -1, z, w.bricks);
        }
    }
}

void place_castle_small_corridor_left_turn(const fortress_writer &w,
                                           const fortress_piece_data &data,
                                           worldgen_random &random) {
    w.box(0, 0, 0, 4, 1, 4, w.bricks);
    w.box(0, 2, 0, 4, 5, 4, w.air);
    w.box(4, 2, 0, 4, 5, 4, w.bricks);
    w.box(4, 3, 1, 4, 4, 1, w.fence_ns);
    w.box(4, 3, 3, 4, 4, 3, w.fence_ns);
    w.box(0, 2, 0, 0, 5, 0, w.bricks);
    w.box(0, 2, 4, 3, 5, 4, w.bricks);
    w.box(1, 3, 4, 1, 4, 4, w.fence_we);
    w.box(3, 3, 4, 3, 4, 4, w.fence_we);
    if (data.chest) {
        w.chest(3, 2, 3, random);
    }
    w.box(0, 6, 0, 4, 6, 4, w.bricks);
    for (jint x = 0; x <= 4; ++x) {
        for (jint z = 0; z <= 4; ++z) {
            w.fill_column_down(x, -1, z, w.bricks);
        }
    }
}

void place_castle_small_corridor(const fortress_writer &w) {
    w.box(0, 0, 0, 4, 1, 4, w.bricks);
    w.box(0, 2, 0, 4, 5, 4, w.air);
    w.box(0, 2, 0, 0, 5, 4, w.bricks);
    w.box(4, 2, 0, 4, 5, 4, w.bricks);
    w.box(0, 3, 1, 0, 4, 1, w.fence_ns);
    w.box(0, 3, 3, 0, 4, 3, w.fence_ns);
    w.box(4, 3, 1, 4, 4, 1, w.fence_ns);
    w.box(4, 3, 3, 4, 4, 3, w.fence_ns);
    w.box(0, 6, 0, 4, 6, 4, w.bricks);
    for (jint x = 0; x <= 4; ++x) {
        for (jint z = 0; z <= 4; ++z) {
            w.fill_column_down(x, -1, z, w.bricks);
        }
    }
}

void place_castle_small_corridor_right_turn(const fortress_writer &w,
                                            const fortress_piece_data &data,
                                            worldgen_random &random) {
    w.box(0, 0, 0, 4, 1, 4, w.bricks);
    w.box(0, 2, 0, 4, 5, 4, w.air);
    w.box(0, 2, 0, 0, 5, 4, w.bricks);
    w.box(0, 3, 1, 0, 4, 1, w.fence_ns);
    w.box(0, 3, 3, 0, 4, 3, w.fence_ns);
    w.box(4, 2, 0, 4, 5, 0, w.bricks);
    w.box(1, 2, 4, 4, 5, 4, w.bricks);
    w.box(1, 3, 4, 1, 4, 4, w.fence_we);
    w.box(3, 3, 4, 3, 4, 4, w.fence_we);
    if (data.chest) {
        w.chest(1, 2, 3, random);
    }
    w.box(0, 6, 0, 4, 6, 4, w.bricks);
    for (jint x = 0; x <= 4; ++x) {
        for (jint z = 0; z <= 4; ++z) {
            w.fill_column_down(x, -1, z, w.bricks);
        }
    }
}

void place_castle_stalk_room(const fortress_writer &w) {
    w.box(0, 3, 0, 12, 4, 12, w.bricks);
    w.box(0, 5, 0, 12, 13, 12, w.air);
    w.box(0, 5, 0, 1, 12, 12, w.bricks);
    w.box(11, 5, 0, 12, 12, 12, w.bricks);
    w.box(2, 5, 11, 4, 12, 12, w.bricks);
    w.box(8, 5, 11, 10, 12, 12, w.bricks);
    w.box(5, 9, 11, 7, 12, 12, w.bricks);
    w.box(2, 5, 0, 4, 12, 1, w.bricks);
    w.box(8, 5, 0, 10, 12, 1, w.bricks);
    w.box(5, 9, 0, 7, 12, 1, w.bricks);
    w.box(2, 11, 2, 10, 12, 10, w.bricks);
    for (jint i = 1; i <= 11; i += 2) {
        w.box(i, 10, 0, i, 11, 0, w.fence_we);
        w.box(i, 10, 12, i, 11, 12, w.fence_we);
        w.box(0, 10, i, 0, 11, i, w.fence_ns);
        w.box(12, 10, i, 12, 11, i, w.fence_ns);
        w.set(i, 13, 0, w.bricks);
        w.set(i, 13, 12, w.bricks);
        w.set(0, 13, i, w.bricks);
        w.set(12, 13, i, w.bricks);
        if (i != 11) {
            w.set(i + 1, 13, 0, w.fence_we);
            w.set(i + 1, 13, 12, w.fence_we);
            w.set(0, 13, i + 1, w.fence_ns);
            w.set(12, 13, i + 1, w.fence_ns);
        }
    }
    w.set(0, 13, 0, w.fence_ne);
    w.set(0, 13, 12, w.fence_es);
    w.set(12, 13, 12, w.fence_ws);
    w.set(12, 13, 0, w.fence_nw);
    for (jint z = 3; z <= 9; z += 2) {
        w.box(1, 7, z, 1, 8, z, w.fence_nsw);
        w.box(11, 7, z, 11, 8, z, w.fence_nse);
    }
    for (jint i = 0; i <= 6; ++i) {
        const jint z = i + 4;
        for (jint x = 5; x <= 7; ++x) {
            w.set(x, 5 + i, z, w.stairs_north);
        }
        if (z >= 5 && z <= 8) {
            w.box(5, 5, z, 7, i + 4, z, w.bricks);
        } else if (z >= 9 && z <= 10) {
            w.box(5, 8, z, 7, i + 4, z, w.bricks);
        }
        if (i >= 1) {
            w.box(5, 6 + i, z, 7, 9 + i, z, w.air);
        }
    }
    for (jint x = 5; x <= 7; ++x) {
        w.set(x, 12, 11, w.stairs_north);
    }
    w.box(5, 6, 7, 5, 7, 7, w.fence_nse);
    w.box(7, 6, 7, 7, 7, 7, w.fence_nsw);
    w.box(5, 13, 12, 7, 13, 12, w.air);
    w.box(2, 5, 2, 3, 5, 3, w.bricks);
    w.box(2, 5, 9, 3, 5, 10, w.bricks);
    w.box(2, 5, 4, 2, 5, 8, w.bricks);
    w.box(9, 5, 2, 10, 5, 3, w.bricks);
    w.box(9, 5, 9, 10, 5, 10, w.bricks);
    w.box(10, 5, 4, 10, 5, 8, w.bricks);
    w.set(4, 5, 2, w.stairs_west);
    w.set(4, 5, 3, w.stairs_west);
    w.set(4, 5, 9, w.stairs_west);
    w.set(4, 5, 10, w.stairs_west);
    w.set(8, 5, 2, w.stairs_east);
    w.set(8, 5, 3, w.stairs_east);
    w.set(8, 5, 9, w.stairs_east);
    w.set(8, 5, 10, w.stairs_east);
    w.box(3, 4, 4, 4, 4, 8, w.soul_sand);
    w.box(8, 4, 4, 9, 4, 8, w.soul_sand);
    w.box(3, 5, 4, 4, 5, 8, w.nether_wart);
    w.box(8, 5, 4, 9, 5, 8, w.nether_wart);
    w.box(4, 2, 0, 8, 2, 12, w.bricks);
    w.box(0, 2, 4, 12, 2, 8, w.bricks);
    w.box(4, 0, 0, 8, 1, 3, w.bricks);
    w.box(4, 0, 9, 8, 1, 12, w.bricks);
    w.box(0, 0, 4, 3, 1, 8, w.bricks);
    w.box(9, 0, 4, 12, 1, 8, w.bricks);
    for (jint x = 4; x <= 8; ++x) {
        for (jint z = 0; z <= 2; ++z) {
            w.fill_column_down(x, -1, z, w.bricks);
            w.fill_column_down(x, -1, 12 - z, w.bricks);
        }
    }
    for (jint x = 0; x <= 2; ++x) {
        for (jint z = 4; z <= 8; ++z) {
            w.fill_column_down(x, -1, z, w.bricks);
            w.fill_column_down(12 - x, -1, z, w.bricks);
        }
    }
}

void place_monster_throne(const fortress_writer &w) {
    w.box(0, 2, 0, 6, 7, 7, w.air);
    w.box(1, 0, 0, 5, 1, 7, w.bricks);
    w.box(1, 2, 1, 5, 2, 7, w.bricks);
    w.box(1, 3, 2, 5, 3, 7, w.bricks);
    w.box(1, 4, 3, 5, 4, 7, w.bricks);
    w.box(1, 2, 0, 1, 4, 2, w.bricks);
    w.box(5, 2, 0, 5, 4, 2, w.bricks);
    w.box(1, 5, 2, 1, 5, 3, w.bricks);
    w.box(5, 5, 2, 5, 5, 3, w.bricks);
    w.box(0, 5, 3, 0, 5, 8, w.bricks);
    w.box(6, 5, 3, 6, 5, 8, w.bricks);
    w.box(1, 5, 8, 5, 5, 8, w.bricks);
    w.set(1, 6, 3, w.fence_west);
    w.set(5, 6, 3, w.fence_east);
    w.set(0, 6, 3, w.fence_ne);
    w.set(6, 6, 3, w.fence_nw);
    w.box(0, 6, 4, 0, 6, 7, w.fence_ns);
    w.box(6, 6, 4, 6, 6, 7, w.fence_ns);
    w.set(0, 6, 8, w.fence_es);
    w.set(6, 6, 8, w.fence_ws);
    w.box(1, 6, 8, 5, 6, 8, w.fence_we);
    w.set(1, 7, 8, w.fence_east);
    w.box(2, 7, 8, 4, 7, 8, w.fence_we);
    w.set(5, 7, 8, w.fence_west);
    w.set(2, 8, 8, w.fence_east);
    w.set(3, 8, 8, w.fence_we);
    w.set(4, 8, 8, w.fence_west);
    w.spawner_block(3, 5, 5);
    for (jint x = 0; x <= 6; ++x) {
        for (jint z = 0; z <= 6; ++z) {
            w.fill_column_down(x, -1, z, w.bricks);
        }
    }
}

void place_room_crossing(const fortress_writer &w) {
    w.box(0, 0, 0, 6, 1, 6, w.bricks);
    w.box(0, 2, 0, 6, 7, 6, w.air);
    w.box(0, 2, 0, 1, 6, 0, w.bricks);
    w.box(0, 2, 6, 1, 6, 6, w.bricks);
    w.box(5, 2, 0, 6, 6, 0, w.bricks);
    w.box(5, 2, 6, 6, 6, 6, w.bricks);
    w.box(0, 2, 0, 0, 6, 1, w.bricks);
    w.box(0, 2, 5, 0, 6, 6, w.bricks);
    w.box(6, 2, 0, 6, 6, 1, w.bricks);
    w.box(6, 2, 5, 6, 6, 6, w.bricks);
    w.box(2, 6, 0, 4, 6, 0, w.bricks);
    w.box(2, 5, 0, 4, 5, 0, w.fence_we);
    w.box(2, 6, 6, 4, 6, 6, w.bricks);
    w.box(2, 5, 6, 4, 5, 6, w.fence_we);
    w.box(0, 6, 2, 0, 6, 4, w.bricks);
    w.box(0, 5, 2, 0, 5, 4, w.fence_ns);
    w.box(6, 6, 2, 6, 6, 4, w.bricks);
    w.box(6, 5, 2, 6, 5, 4, w.fence_ns);
    for (jint x = 0; x <= 6; ++x) {
        for (jint z = 0; z <= 6; ++z) {
            w.fill_column_down(x, -1, z, w.bricks);
        }
    }
}

void place_stairs_room(const fortress_writer &w) {
    w.box(0, 0, 0, 6, 1, 6, w.bricks);
    w.box(0, 2, 0, 6, 10, 6, w.air);
    w.box(0, 2, 0, 1, 8, 0, w.bricks);
    w.box(5, 2, 0, 6, 8, 0, w.bricks);
    w.box(0, 2, 1, 0, 8, 6, w.bricks);
    w.box(6, 2, 1, 6, 8, 6, w.bricks);
    w.box(1, 2, 6, 5, 8, 6, w.bricks);
    w.box(0, 3, 2, 0, 5, 4, w.fence_ns);
    w.box(6, 3, 2, 6, 5, 2, w.fence_ns);
    w.box(6, 3, 4, 6, 5, 4, w.fence_ns);
    w.set(5, 2, 5, w.bricks);
    w.box(4, 2, 5, 4, 3, 5, w.bricks);
    w.box(3, 2, 5, 3, 4, 5, w.bricks);
    w.box(2, 2, 5, 2, 5, 5, w.bricks);
    w.box(1, 2, 5, 1, 6, 5, w.bricks);
    w.box(1, 7, 1, 5, 7, 4, w.bricks);
    w.box(6, 8, 2, 6, 8, 4, w.air);
    w.box(2, 6, 0, 4, 8, 0, w.bricks);
    w.box(2, 5, 0, 4, 5, 0, w.fence_we);
    for (jint x = 0; x <= 6; ++x) {
        for (jint z = 0; z <= 6; ++z) {
            w.fill_column_down(x, -1, z, w.bricks);
        }
    }
}

} // namespace

void place_fortress_piece(const structure_place_context &context,
                          const structure_piece_record &piece,
                          worldgen_random &random) {
    if (!piece.fortress.has_value() || context.registry == nullptr ||
        context.chunk == nullptr || context.region == nullptr ||
        context.states == nullptr) {
        return;
    }
    const fortress_writer writer{context, piece};
    const auto kind = piece.fortress->kind;
    switch (kind) {
    case bridge_crossing:
        place_bridge_crossing(writer);
        break;
    case bridge_end_filler:
        place_bridge_end_filler(writer, *piece.fortress);
        break;
    case bridge_straight:
        place_bridge_straight(writer);
        break;
    case castle_corridor_stairs:
        place_castle_corridor_stairs(writer);
        break;
    case castle_corridor_t_balcony:
        place_castle_corridor_t_balcony(writer);
        break;
    case castle_entrance:
        place_castle_entrance(writer);
        break;
    case castle_small_corridor:
        place_castle_small_corridor(writer);
        break;
    case castle_small_corridor_crossing:
        place_castle_small_corridor_crossing(writer);
        break;
    case castle_small_corridor_left_turn:
        place_castle_small_corridor_left_turn(writer, *piece.fortress, random);
        break;
    case castle_small_corridor_right_turn:
        place_castle_small_corridor_right_turn(writer, *piece.fortress, random);
        break;
    case castle_stalk_room:
        place_castle_stalk_room(writer);
        break;
    case monster_throne:
        place_monster_throne(writer);
        break;
    case room_crossing:
        place_room_crossing(writer);
        break;
    case stairs_room:
        place_stairs_room(writer);
        break;
    }
}

} // namespace refrain::worldgen::structure
