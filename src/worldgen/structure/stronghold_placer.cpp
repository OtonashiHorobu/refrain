// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Stronghold piece placement. Every piece writes its oriented local box into
// the chunk being placed, using the smooth-stone selector for the shell and
// the piece-specific interiors for doors, halls, libraries and the portal
// room.

#include <refrain/worldgen/structure/placement_stage.hpp>

#include <refrain/worldgen/chunk/block_state_codec.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

namespace refrain::worldgen::structure {

namespace {

constexpr std::string_view cave_air_name = "minecraft:cave_air";

struct stronghold_writer {
    const structure_place_context &context;
    const structure_piece_record &piece;
    template_transform transform{};
    mutable std::unordered_map<block_state_id, block_state_id> rotated{};
    block_state_id cave_air;
    block_state_id stone_bricks;
    block_state_id cracked;
    block_state_id mossy;
    block_state_id infested;
    block_state_id smooth_slab;
    block_state_id smooth_slab_double;
    block_state_id stone_brick_slab;
    block_state_id stone_brick_stairs;
    block_state_id cobblestone_stairs;
    block_state_id cobblestone;
    block_state_id water;
    block_state_id lava;
    block_state_id oak_planks;
    block_state_id bookshelf;
    block_state_id cobweb;
    block_state_id oak_fence;
    block_state_id torch;
    block_state_id ladder_south;
    block_state_id ladder_west;
    block_state_id bars_west;
    block_state_id bars_east;
    block_state_id bars_ns;
    block_state_id bars_we;
    block_state_id bars_nse;
    block_state_id door_oak_lower;
    block_state_id door_oak_upper;
    block_state_id door_iron_lower;
    block_state_id door_iron_upper;
    block_state_id door_iron_west_lower;
    block_state_id door_iron_west_upper;
    block_state_id button_north;
    block_state_id button_south;
    block_state_id spawner;
    block_state_id end_portal;
    std::array<block_state_id, 4> wall_torch{}; // north, south, west, east
    std::array<std::array<block_state_id, 2>, 4> frames{}; // 4 facings x eye
    jint chunk_min_x;
    jint chunk_max_x;
    jint chunk_min_z;
    jint chunk_max_z;
    jint chunk_min_y;
    jint chunk_max_y;

    stronghold_writer(const structure_place_context &place_context,
                      const structure_piece_record &place_piece)
        : context{place_context}, piece{place_piece},
          cave_air{resolve(cave_air_name)},
          stone_bricks{resolve("minecraft:stone_bricks")},
          cracked{resolve("minecraft:cracked_stone_bricks")},
          mossy{resolve("minecraft:mossy_stone_bricks")},
          infested{resolve("minecraft:infested_stone_bricks")},
          smooth_slab{resolve("minecraft:smooth_stone_slab")},
          smooth_slab_double{
              resolve("minecraft:smooth_stone_slab[type=double]")},
          stone_brick_slab{resolve("minecraft:stone_brick_slab")},
          stone_brick_stairs{
              resolve("minecraft:stone_brick_stairs[facing=north,half=bottom,"
                      "shape=straight,waterlogged=false]")},
          cobblestone_stairs{
              resolve("minecraft:cobblestone_stairs[facing=south,half=bottom,"
                      "shape=straight,waterlogged=false]")},
          cobblestone{resolve("minecraft:cobblestone")},
          water{resolve("minecraft:water")}, lava{resolve("minecraft:lava")},
          oak_planks{resolve("minecraft:oak_planks")},
          bookshelf{resolve("minecraft:bookshelf")},
          cobweb{resolve("minecraft:cobweb")},
          oak_fence{resolve("minecraft:oak_fence")},
          torch{resolve("minecraft:torch")},
          ladder_south{
              resolve("minecraft:ladder[facing=south,waterlogged=false]")},
          ladder_west{
              resolve("minecraft:ladder[facing=west,waterlogged=false]")},
          bars_west{resolve("minecraft:iron_bars[west=true]")},
          bars_east{resolve("minecraft:iron_bars[east=true]")},
          bars_ns{resolve("minecraft:iron_bars[north=true,south=true]")},
          bars_we{resolve("minecraft:iron_bars[east=true,west=true]")},
          bars_nse{
              resolve("minecraft:iron_bars[east=true,north=true,south=true]")},
          door_oak_lower{
              resolve("minecraft:oak_door[facing=north,half=lower,hinge=left,"
                      "open=false,powered=false]")},
          door_oak_upper{
              resolve("minecraft:oak_door[facing=north,half=upper,hinge=left,"
                      "open=false,powered=false]")},
          door_iron_lower{
              resolve("minecraft:iron_door[facing=north,half=lower,hinge=left,"
                      "open=false,powered=false]")},
          door_iron_upper{
              resolve("minecraft:iron_door[facing=north,half=upper,hinge=left,"
                      "open=false,powered=false]")},
          door_iron_west_lower{
              resolve("minecraft:iron_door[facing=west,half=lower,hinge=left,"
                      "open=false,powered=false]")},
          door_iron_west_upper{
              resolve("minecraft:iron_door[facing=west,half=upper,hinge=left,"
                      "open=false,powered=false]")},
          button_north{resolve(
              "minecraft:stone_button[face=wall,facing=north,powered=false]")},
          button_south{resolve(
              "minecraft:stone_button[face=wall,facing=south,powered=false]")},
          spawner{resolve("minecraft:spawner")},
          end_portal{resolve("minecraft:end_portal")},
          wall_torch{resolve("minecraft:wall_torch[facing=north]"),
                     resolve("minecraft:wall_torch[facing=south]"),
                     resolve("minecraft:wall_torch[facing=west]"),
                     resolve("minecraft:wall_torch[facing=east]")},
          frames{{
              {resolve("minecraft:end_portal_frame[facing=north,eye=false]"),
               resolve("minecraft:end_portal_frame[facing=north,eye=true]")},
              {resolve("minecraft:end_portal_frame[facing=south,eye=false]"),
               resolve("minecraft:end_portal_frame[facing=south,eye=true]")},
              {resolve("minecraft:end_portal_frame[facing=west,eye=false]"),
               resolve("minecraft:end_portal_frame[facing=west,eye=true]")},
              {resolve("minecraft:end_portal_frame[facing=east,eye=false]"),
               resolve("minecraft:end_portal_frame[facing=east,eye=true]")},
          }},
          chunk_min_x{context.chunk->chunk_x() * 16},
          chunk_max_x{context.chunk->chunk_x() * 16 + 15},
          chunk_min_z{context.chunk->chunk_z() * 16},
          chunk_max_z{context.chunk->chunk_z() * 16 + 15},
          chunk_min_y{context.chunk->min_y() + 1},
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
            return cave_air;
        }
        return context.region->state_at(world_x(x, z), world_y(y),
                                        world_z(x, z));
    }

    [[nodiscard]] auto default_alias(block_state_id state) const
        -> block_state_id {
        const std::string_view name = context.registry->name(state);
        const auto has = [&](std::string_view base,
                             std::initializer_list<std::string_view> props) {
            if (!name.starts_with(base) || name.size() == base.size()) {
                return false;
            }
            for (const auto prop : props) {
                if (name.find(prop) == std::string_view::npos) {
                    return false;
                }
            }
            return true;
        };
        if (has("minecraft:stone_button[",
                {"face=wall", "facing=north", "powered=false"})) {
            return resolve("minecraft:stone_button");
        }
        if (has("minecraft:wall_torch[", {"facing=north"})) {
            return resolve("minecraft:wall_torch");
        }
        if (has("minecraft:ladder[", {"facing=north", "waterlogged=false"})) {
            return resolve("minecraft:ladder");
        }
        if (has("minecraft:end_portal_frame[", {"facing=north", "eye=false"})) {
            return resolve("minecraft:end_portal_frame");
        }
        return state;
    }

    [[nodiscard]] auto oriented_state(block_state_id state) const
        -> block_state_id {
        if (context.registry == nullptr) {
            return state;
        }
        if (const auto found = rotated.find(state); found != rotated.end()) {
            return found->second;
        }
        const auto transformed =
            transform_state(*context.registry, state, transform);
        const auto aliased = default_alias(transformed);
        rotated.emplace(state, aliased);
        return aliased;
    }

    void set(jint x, jint y, jint z, block_state_id state) const {
        if (inside(x, y, z)) {
            (void)context.region->set_block_state(world_x(x, z), world_y(y),
                                                  world_z(x, z),
                                                  oriented_state(state));
        }
    }

    void box(jint x0, jint y0, jint z0, jint x1, jint y1, jint z1,
             block_state_id edge, block_state_id fill,
             bool skip_air = false) const {
        for (jint y = y0; y <= y1; ++y) {
            for (jint x = x0; x <= x1; ++x) {
                for (jint z = z0; z <= z1; ++z) {
                    if (skip_air && context.states->traits(get(x, y, z)).air) {
                        continue;
                    }
                    if (y != y0 && y != y1 && x != x0 && x != x1 && z != z0 &&
                        z != z1) {
                        set(x, y, z, fill);
                    } else {
                        set(x, y, z, edge);
                    }
                }
            }
        }
    }

    void box_selector(jint x0, jint y0, jint z0, jint x1, jint y1, jint z1,
                      bool skip_air, worldgen_random &random) const {
        for (jint y = y0; y <= y1; ++y) {
            for (jint x = x0; x <= x1; ++x) {
                for (jint z = z0; z <= z1; ++z) {
                    if (skip_air && context.states->traits(get(x, y, z)).air) {
                        continue;
                    }
                    const bool edge = y == y0 || y == y1 || x == x0 ||
                                      x == x1 || z == z0 || z == z1;
                    block_state_id state = cave_air;
                    if (edge) {
                        const float selection = random.next_float();
                        if (selection < 0.2F) {
                            state = cracked;
                        } else if (selection < 0.5F) {
                            state = mossy;
                        } else if (selection < 0.55F) {
                            state = infested;
                        } else {
                            state = stone_bricks;
                        }
                    }
                    set(x, y, z, state);
                }
            }
        }
    }

    void maybe_box(worldgen_random &random, float probability, jint x0, jint y0,
                   jint z0, jint x1, jint y1, jint z1, block_state_id edge,
                   block_state_id fill) const {
        for (jint y = y0; y <= y1; ++y) {
            for (jint x = x0; x <= x1; ++x) {
                for (jint z = z0; z <= z1; ++z) {
                    if (random.next_float() > probability) {
                        continue;
                    }
                    if (y != y0 && y != y1 && x != x0 && x != x1 && z != z0 &&
                        z != z1) {
                        set(x, y, z, fill);
                    } else {
                        set(x, y, z, edge);
                    }
                }
            }
        }
    }

    void maybe_block(worldgen_random &random, float probability, jint x, jint y,
                     jint z, block_state_id state) const {
        if (random.next_float() < probability) {
            set(x, y, z, state);
        }
    }

    void chest(jint x, jint y, jint z, worldgen_random &random,
               std::string_view loot_table) const {
        if (!inside(x, y, z)) {
            return;
        }
        (void)place_loot_chest(context, world_x(x, z), world_y(y),
                               world_z(x, z), random, loot_table);
    }

    void spawner_block(jint x, jint y, jint z) const {
        if (!inside(x, y, z)) {
            return;
        }
        const jint wx = world_x(x, z);
        const jint wy = world_y(y);
        const jint wz = world_z(x, z);
        if (!context.region->set_block_state(wx, wy, wz, spawner)) {
            return;
        }
        data::nbt::compound entity;
        entity.emplace("id",
                       data::nbt::value{std::string{"minecraft:spawner"}});
        entity.emplace("x", data::nbt::value{static_cast<jint>(wx)});
        entity.emplace("y", data::nbt::value{static_cast<jint>(wy)});
        entity.emplace("z", data::nbt::value{static_cast<jint>(wz)});
        context.chunk->set_block_entity(
            block_pos{.x = wx & 15, .y = wy, .z = wz & 15}, std::move(entity));
    }

    void small_door(stronghold_door door, jint foot_x, jint foot_y,
                    jint foot_z) const {
        switch (door) {
        case stronghold_door::opening:
            box(foot_x, foot_y, foot_z, foot_x + 2, foot_y + 2, foot_z,
                cave_air, cave_air);
            break;
        case stronghold_door::wood_door:
            for (jint y = 0; y <= 2; ++y) {
                set(foot_x, foot_y + y, foot_z, stone_bricks);
            }
            set(foot_x + 1, foot_y + 2, foot_z, stone_bricks);
            set(foot_x + 2, foot_y + 2, foot_z, stone_bricks);
            set(foot_x + 2, foot_y + 1, foot_z, stone_bricks);
            set(foot_x + 2, foot_y, foot_z, stone_bricks);
            set(foot_x + 1, foot_y, foot_z, door_oak_lower);
            set(foot_x + 1, foot_y + 1, foot_z, door_oak_upper);
            break;
        case stronghold_door::grates:
            set(foot_x + 1, foot_y, foot_z, cave_air);
            set(foot_x + 1, foot_y + 1, foot_z, cave_air);
            set(foot_x, foot_y, foot_z, bars_west);
            set(foot_x, foot_y + 1, foot_z, bars_west);
            set(foot_x, foot_y + 2, foot_z, bars_we);
            set(foot_x + 1, foot_y + 2, foot_z, bars_we);
            set(foot_x + 2, foot_y + 2, foot_z, bars_we);
            set(foot_x + 2, foot_y + 1, foot_z, bars_east);
            set(foot_x + 2, foot_y, foot_z, bars_east);
            break;
        case stronghold_door::iron_door:
            for (jint y = 0; y <= 2; ++y) {
                set(foot_x, foot_y + y, foot_z, stone_bricks);
            }
            set(foot_x + 1, foot_y + 2, foot_z, stone_bricks);
            set(foot_x + 2, foot_y + 2, foot_z, stone_bricks);
            set(foot_x + 2, foot_y + 1, foot_z, stone_bricks);
            set(foot_x + 2, foot_y, foot_z, stone_bricks);
            set(foot_x + 1, foot_y, foot_z, door_iron_lower);
            set(foot_x + 1, foot_y + 1, foot_z, door_iron_upper);
            set(foot_x + 2, foot_y + 1, foot_z + 1, button_north);
            set(foot_x + 2, foot_y + 1, foot_z - 1, button_south);
            break;
        }
    }
};

void place_stairs_down(const stronghold_writer &writer,
                       worldgen_random &random) {
    const auto &data = *writer.piece.stronghold;
    writer.box_selector(0, 0, 0, 4, 10, 4, true, random);
    writer.small_door(data.door, 1, 7, 0);
    writer.small_door(stronghold_door::opening, 1, 1, 4);
    writer.set(2, 6, 1, writer.stone_bricks);
    writer.set(1, 5, 1, writer.stone_bricks);
    writer.set(1, 6, 1, writer.smooth_slab);
    writer.set(1, 5, 2, writer.stone_bricks);
    writer.set(1, 4, 3, writer.stone_bricks);
    writer.set(1, 5, 3, writer.smooth_slab);
    writer.set(2, 4, 3, writer.stone_bricks);
    writer.set(3, 3, 3, writer.stone_bricks);
    writer.set(3, 4, 3, writer.smooth_slab);
    writer.set(3, 3, 2, writer.stone_bricks);
    writer.set(3, 2, 1, writer.stone_bricks);
    writer.set(3, 3, 1, writer.smooth_slab);
    writer.set(2, 2, 1, writer.stone_bricks);
    writer.set(1, 1, 1, writer.stone_bricks);
    writer.set(1, 2, 1, writer.smooth_slab);
    writer.set(1, 1, 2, writer.stone_bricks);
    writer.set(1, 1, 3, writer.smooth_slab);
}

void place_straight(const stronghold_writer &writer, worldgen_random &random) {
    const auto &data = *writer.piece.stronghold;
    writer.box_selector(0, 0, 0, 4, 4, 6, true, random);
    writer.small_door(data.door, 1, 1, 0);
    writer.small_door(stronghold_door::opening, 1, 1, 6);
    writer.maybe_block(random, 0.1F, 1, 2, 1, writer.wall_torch[3]);
    writer.maybe_block(random, 0.1F, 3, 2, 1, writer.wall_torch[2]);
    writer.maybe_block(random, 0.1F, 1, 2, 5, writer.wall_torch[3]);
    writer.maybe_block(random, 0.1F, 3, 2, 5, writer.wall_torch[2]);
    if (data.left) {
        writer.box(0, 1, 2, 0, 3, 4, writer.cave_air, writer.cave_air);
    }
    if (data.right) {
        writer.box(4, 1, 2, 4, 3, 4, writer.cave_air, writer.cave_air);
    }
}

void place_prison_hall(const stronghold_writer &writer,
                       worldgen_random &random) {
    const auto &data = *writer.piece.stronghold;
    writer.box_selector(0, 0, 0, 8, 4, 10, true, random);
    writer.small_door(data.door, 1, 1, 0);
    writer.box(1, 1, 10, 3, 3, 10, writer.cave_air, writer.cave_air);
    writer.box_selector(4, 1, 1, 4, 3, 1, false, random);
    writer.box_selector(4, 1, 3, 4, 3, 3, false, random);
    writer.box_selector(4, 1, 7, 4, 3, 7, false, random);
    writer.box_selector(4, 1, 9, 4, 3, 9, false, random);
    for (jint y = 1; y <= 3; ++y) {
        writer.set(4, y, 4, writer.bars_ns);
        writer.set(4, y, 5, writer.bars_nse);
        writer.set(4, y, 6, writer.bars_ns);
        writer.set(5, y, 5, writer.bars_we);
        writer.set(6, y, 5, writer.bars_we);
        writer.set(7, y, 5, writer.bars_we);
    }
    writer.set(4, 3, 2, writer.bars_ns);
    writer.set(4, 3, 8, writer.bars_ns);
    writer.set(4, 1, 2, writer.door_iron_west_lower);
    writer.set(4, 2, 2, writer.door_iron_west_upper);
    writer.set(4, 1, 8, writer.door_iron_west_lower);
    writer.set(4, 2, 8, writer.door_iron_west_upper);
}

void place_chest_corridor(const stronghold_writer &writer,
                          worldgen_random &random) {
    const auto &data = *writer.piece.stronghold;
    writer.box_selector(0, 0, 0, 4, 4, 6, true, random);
    writer.small_door(data.door, 1, 1, 0);
    writer.small_door(stronghold_door::opening, 1, 1, 6);
    writer.box(3, 1, 2, 3, 1, 4, writer.stone_bricks, writer.stone_bricks);
    writer.set(3, 1, 1, writer.stone_brick_slab);
    writer.set(3, 1, 5, writer.stone_brick_slab);
    writer.set(3, 2, 2, writer.stone_brick_slab);
    writer.set(3, 2, 4, writer.stone_brick_slab);
    for (jint z = 2; z <= 4; ++z) {
        writer.set(2, 1, z, writer.stone_brick_slab);
    }
    writer.chest(3, 2, 3, random, "minecraft:chests/stronghold_corridor");
}

void place_filler_corridor(const stronghold_writer &writer) {
    const jint steps = writer.piece.stronghold->steps;
    for (jint i = 0; i < steps; ++i) {
        for (jint x = 0; x <= 4; ++x) {
            writer.set(x, 0, i, writer.stone_bricks);
        }
        for (jint y = 1; y <= 3; ++y) {
            writer.set(0, y, i, writer.stone_bricks);
            writer.set(1, y, i, writer.cave_air);
            writer.set(2, y, i, writer.cave_air);
            writer.set(3, y, i, writer.cave_air);
            writer.set(4, y, i, writer.stone_bricks);
        }
        for (jint x = 0; x <= 4; ++x) {
            writer.set(x, 4, i, writer.stone_bricks);
        }
    }
}

void place_five_crossing(const stronghold_writer &writer,
                         worldgen_random &random) {
    const auto &data = *writer.piece.stronghold;
    writer.box_selector(0, 0, 0, 9, 8, 10, true, random);
    writer.small_door(data.door, 4, 3, 0);
    if (data.left_low) {
        writer.box(0, 3, 1, 0, 5, 3, writer.cave_air, writer.cave_air);
    }
    if (data.right_low) {
        writer.box(9, 3, 1, 9, 5, 3, writer.cave_air, writer.cave_air);
    }
    if (data.left_high) {
        writer.box(0, 5, 7, 0, 7, 9, writer.cave_air, writer.cave_air);
    }
    if (data.right_high) {
        writer.box(9, 5, 7, 9, 7, 9, writer.cave_air, writer.cave_air);
    }
    writer.box(5, 1, 10, 7, 3, 10, writer.cave_air, writer.cave_air);
    writer.box_selector(1, 2, 1, 8, 2, 6, false, random);
    writer.box_selector(4, 1, 5, 4, 4, 9, false, random);
    writer.box_selector(8, 1, 5, 8, 4, 9, false, random);
    writer.box_selector(1, 4, 7, 3, 4, 9, false, random);
    writer.box_selector(1, 3, 5, 3, 3, 6, false, random);
    writer.box(1, 3, 4, 3, 3, 4, writer.smooth_slab, writer.smooth_slab);
    writer.box(1, 4, 6, 3, 4, 6, writer.smooth_slab, writer.smooth_slab);
    writer.box_selector(5, 1, 7, 7, 1, 8, false, random);
    writer.box(5, 1, 9, 7, 1, 9, writer.smooth_slab, writer.smooth_slab);
    writer.box(5, 2, 7, 7, 2, 7, writer.smooth_slab, writer.smooth_slab);
    writer.box(4, 5, 7, 4, 5, 9, writer.smooth_slab, writer.smooth_slab);
    writer.box(8, 5, 7, 8, 5, 9, writer.smooth_slab, writer.smooth_slab);
    writer.box(5, 5, 7, 7, 5, 9, writer.smooth_slab_double,
               writer.smooth_slab_double);
    writer.set(6, 5, 6, writer.wall_torch[1]);
}

void place_turn(const stronghold_writer &writer, bool left_turn,
                worldgen_random &random) {
    const auto &data = *writer.piece.stronghold;
    const jint orientation = writer.piece.orientation;
    writer.box_selector(0, 0, 0, 4, 4, 4, true, random);
    writer.small_door(data.door, 1, 1, 0);
    const bool vertical = orientation == 2 || orientation == 3;
    const bool open_right_edge = left_turn ? !vertical : vertical;
    const jint x = open_right_edge ? 4 : 0;
    writer.box(x, 1, 1, x, 3, 3, writer.cave_air, writer.cave_air);
}

void place_library(const stronghold_writer &writer, worldgen_random &random) {
    const auto &data = *writer.piece.stronghold;
    const jint height = data.tall ? 11 : 6;
    writer.box_selector(0, 0, 0, 13, height - 1, 14, true, random);
    writer.small_door(data.door, 4, 1, 0);
    writer.maybe_box(random, 0.07F, 2, 1, 1, 11, 4, 13, writer.cobweb,
                     writer.cobweb);
    for (jint d = 1; d <= 13; ++d) {
        if ((d - 1) % 4 == 0) {
            writer.box(1, 1, d, 1, 4, d, writer.oak_planks, writer.oak_planks);
            writer.box(12, 1, d, 12, 4, d, writer.oak_planks,
                       writer.oak_planks);
            writer.set(2, 3, d, writer.wall_torch[3]);
            writer.set(11, 3, d, writer.wall_torch[2]);
            if (data.tall) {
                writer.box(1, 6, d, 1, 9, d, writer.oak_planks,
                           writer.oak_planks);
                writer.box(12, 6, d, 12, 9, d, writer.oak_planks,
                           writer.oak_planks);
            }
        } else {
            writer.box(1, 1, d, 1, 4, d, writer.bookshelf, writer.bookshelf);
            writer.box(12, 1, d, 12, 4, d, writer.bookshelf, writer.bookshelf);
            if (data.tall) {
                writer.box(1, 6, d, 1, 9, d, writer.bookshelf,
                           writer.bookshelf);
                writer.box(12, 6, d, 12, 9, d, writer.bookshelf,
                           writer.bookshelf);
            }
        }
    }
    for (jint dx = 3; dx < 12; dx += 2) {
        writer.box(3, 1, dx, 4, 3, dx, writer.bookshelf, writer.bookshelf);
        writer.box(6, 1, dx, 7, 3, dx, writer.bookshelf, writer.bookshelf);
        writer.box(9, 1, dx, 10, 3, dx, writer.bookshelf, writer.bookshelf);
    }
    if (data.tall) {
        writer.box(1, 5, 1, 3, 5, 13, writer.oak_planks, writer.oak_planks);
        writer.box(10, 5, 1, 12, 5, 13, writer.oak_planks, writer.oak_planks);
        writer.box(4, 5, 1, 9, 5, 2, writer.oak_planks, writer.oak_planks);
        writer.box(4, 5, 12, 9, 5, 13, writer.oak_planks, writer.oak_planks);
        writer.set(9, 5, 11, writer.oak_planks);
        writer.set(8, 5, 11, writer.oak_planks);
        writer.set(9, 5, 10, writer.oak_planks);
        writer.box(3, 6, 3, 3, 6, 11, writer.oak_fence, writer.oak_fence);
        writer.box(10, 6, 3, 10, 6, 9, writer.oak_fence, writer.oak_fence);
        writer.box(4, 6, 2, 9, 6, 2, writer.oak_fence, writer.oak_fence);
        writer.box(4, 6, 12, 7, 6, 12, writer.oak_fence, writer.oak_fence);
        writer.set(3, 6, 2, writer.oak_fence);
        writer.set(3, 6, 12, writer.oak_fence);
        writer.set(10, 6, 2, writer.oak_fence);
        for (jint i = 0; i <= 2; ++i) {
            writer.set(8 + i, 6, 12 - i, writer.oak_fence);
            if (i != 2) {
                writer.set(8 + i, 6, 11 - i, writer.oak_fence);
            }
        }
        for (jint y = 1; y <= 7; ++y) {
            writer.set(10, y, 13, writer.ladder_south);
        }
        writer.set(6, 9, 7, writer.oak_fence);
        writer.set(7, 9, 7, writer.oak_fence);
        writer.set(6, 8, 7, writer.oak_fence);
        writer.set(7, 8, 7, writer.oak_fence);
        writer.set(6, 7, 7, writer.oak_fence);
        writer.set(7, 7, 7, writer.oak_fence);
        writer.set(5, 7, 7, writer.oak_fence);
        writer.set(8, 7, 7, writer.oak_fence);
        writer.set(6, 7, 6, writer.oak_fence);
        writer.set(6, 7, 8, writer.oak_fence);
        writer.set(7, 7, 6, writer.oak_fence);
        writer.set(7, 7, 8, writer.oak_fence);
        writer.set(5, 8, 7, writer.torch);
        writer.set(8, 8, 7, writer.torch);
        writer.set(6, 8, 6, writer.torch);
        writer.set(6, 8, 8, writer.torch);
        writer.set(7, 8, 6, writer.torch);
        writer.set(7, 8, 8, writer.torch);
    }
    writer.chest(3, 3, 5, random, "minecraft:chests/stronghold_library");
    if (data.tall) {
        writer.set(12, 9, 1, writer.cave_air);
        writer.chest(12, 8, 1, random, "minecraft:chests/stronghold_library");
    }
}

void place_portal_room(const stronghold_writer &writer,
                       worldgen_random &random) {
    writer.box_selector(0, 0, 0, 10, 7, 15, false, random);
    writer.small_door(stronghold_door::grates, 4, 1, 0);
    writer.box_selector(1, 6, 1, 1, 6, 14, false, random);
    writer.box_selector(9, 6, 1, 9, 6, 14, false, random);
    writer.box_selector(2, 6, 1, 8, 6, 2, false, random);
    writer.box_selector(2, 6, 14, 8, 6, 14, false, random);
    writer.box_selector(1, 1, 1, 2, 1, 4, false, random);
    writer.box_selector(8, 1, 1, 9, 1, 4, false, random);
    writer.box(1, 1, 1, 1, 1, 3, writer.lava, writer.lava);
    writer.box(9, 1, 1, 9, 1, 3, writer.lava, writer.lava);
    writer.box_selector(3, 1, 8, 7, 1, 12, false, random);
    writer.box(4, 1, 9, 6, 1, 11, writer.lava, writer.lava);
    for (jint z = 3; z < 14; z += 2) {
        writer.box(0, 3, z, 0, 4, z, writer.bars_ns, writer.bars_ns);
        writer.box(10, 3, z, 10, 4, z, writer.bars_ns, writer.bars_ns);
    }
    for (jint x = 2; x < 9; x += 2) {
        writer.box(x, 3, 15, x, 4, 15, writer.bars_we, writer.bars_we);
    }
    writer.box_selector(4, 1, 5, 6, 1, 7, false, random);
    writer.box_selector(4, 2, 6, 6, 2, 7, false, random);
    writer.box_selector(4, 3, 7, 6, 3, 7, false, random);
    for (jint x = 4; x <= 6; ++x) {
        writer.set(x, 1, 4, writer.stone_brick_stairs);
        writer.set(x, 2, 5, writer.stone_brick_stairs);
        writer.set(x, 3, 6, writer.stone_brick_stairs);
    }

    std::array<bool, 12> eyes{};
    bool all_eyes = true;
    for (std::size_t i = 0; i < eyes.size(); ++i) {
        eyes[i] = random.next_float() > 0.9F;
        all_eyes = all_eyes && eyes[i];
    }
    const auto frame = [&](std::size_t facing, bool eye, jint x, jint y,
                           jint z) {
        writer.set(x, y, z, writer.frames[facing][eye ? 1 : 0]);
    };
    frame(0, eyes[0], 4, 3, 8);
    frame(0, eyes[1], 5, 3, 8);
    frame(0, eyes[2], 6, 3, 8);
    frame(1, eyes[3], 4, 3, 12);
    frame(1, eyes[4], 5, 3, 12);
    frame(1, eyes[5], 6, 3, 12);
    frame(3, eyes[6], 3, 3, 9);
    frame(3, eyes[7], 3, 3, 10);
    frame(3, eyes[8], 3, 3, 11);
    frame(2, eyes[9], 7, 3, 9);
    frame(2, eyes[10], 7, 3, 10);
    frame(2, eyes[11], 7, 3, 11);
    if (all_eyes) {
        for (jint x = 4; x <= 6; ++x) {
            for (jint z = 9; z <= 11; ++z) {
                writer.set(x, 3, z, writer.end_portal);
            }
        }
    }
    writer.spawner_block(5, 3, 6);
}

void place_room_crossing(const stronghold_writer &writer,
                         worldgen_random &random) {
    const auto &data = *writer.piece.stronghold;
    writer.box_selector(0, 0, 0, 10, 6, 10, true, random);
    writer.small_door(data.door, 4, 1, 0);
    writer.box(4, 1, 10, 6, 3, 10, writer.cave_air, writer.cave_air);
    writer.box(0, 1, 4, 0, 3, 6, writer.cave_air, writer.cave_air);
    writer.box(10, 1, 4, 10, 3, 6, writer.cave_air, writer.cave_air);
    switch (data.variant) {
    case 0:
        writer.set(5, 1, 5, writer.stone_bricks);
        writer.set(5, 2, 5, writer.stone_bricks);
        writer.set(5, 3, 5, writer.stone_bricks);
        writer.set(4, 3, 5, writer.wall_torch[2]);
        writer.set(6, 3, 5, writer.wall_torch[3]);
        writer.set(5, 3, 4, writer.wall_torch[1]);
        writer.set(5, 3, 6, writer.wall_torch[0]);
        for (const auto &[x, z] :
             std::array<std::pair<jint, jint>, 8>{{{4, 4},
                                                   {4, 5},
                                                   {4, 6},
                                                   {6, 4},
                                                   {6, 5},
                                                   {6, 6},
                                                   {5, 4},
                                                   {5, 6}}}) {
            writer.set(x, 1, z, writer.smooth_slab);
        }
        break;
    case 1:
        for (jint i = 0; i < 5; ++i) {
            writer.set(3, 1, 3 + i, writer.stone_bricks);
            writer.set(7, 1, 3 + i, writer.stone_bricks);
            writer.set(3 + i, 1, 3, writer.stone_bricks);
            writer.set(3 + i, 1, 7, writer.stone_bricks);
        }
        writer.set(5, 1, 5, writer.stone_bricks);
        writer.set(5, 2, 5, writer.stone_bricks);
        writer.set(5, 3, 5, writer.stone_bricks);
        writer.set(5, 4, 5, writer.water);
        break;
    case 2:
        for (jint z = 1; z <= 9; ++z) {
            writer.set(1, 3, z, writer.cobblestone);
            writer.set(9, 3, z, writer.cobblestone);
        }
        for (jint x = 1; x <= 9; ++x) {
            writer.set(x, 3, 1, writer.cobblestone);
            writer.set(x, 3, 9, writer.cobblestone);
        }
        writer.set(5, 1, 4, writer.cobblestone);
        writer.set(5, 1, 6, writer.cobblestone);
        writer.set(5, 3, 4, writer.cobblestone);
        writer.set(5, 3, 6, writer.cobblestone);
        writer.set(4, 1, 5, writer.cobblestone);
        writer.set(6, 1, 5, writer.cobblestone);
        writer.set(4, 3, 5, writer.cobblestone);
        writer.set(6, 3, 5, writer.cobblestone);
        for (jint y = 1; y <= 3; ++y) {
            writer.set(4, y, 4, writer.cobblestone);
            writer.set(6, y, 4, writer.cobblestone);
            writer.set(4, y, 6, writer.cobblestone);
            writer.set(6, y, 6, writer.cobblestone);
        }
        writer.set(5, 3, 5, writer.wall_torch[0]);
        for (jint z = 2; z <= 8; ++z) {
            writer.set(2, 3, z, writer.oak_planks);
            writer.set(3, 3, z, writer.oak_planks);
            if (z <= 3 || z >= 7) {
                writer.set(4, 3, z, writer.oak_planks);
                writer.set(5, 3, z, writer.oak_planks);
                writer.set(6, 3, z, writer.oak_planks);
            }
            writer.set(7, 3, z, writer.oak_planks);
            writer.set(8, 3, z, writer.oak_planks);
        }
        writer.set(9, 1, 3, writer.ladder_west);
        writer.set(9, 2, 3, writer.ladder_west);
        writer.set(9, 3, 3, writer.ladder_west);
        writer.chest(3, 4, 8, random, "minecraft:chests/stronghold_crossing");
        break;
    default:
        break;
    }
}

void place_straight_stairs_down(const stronghold_writer &writer,
                                worldgen_random &random) {
    const auto &data = *writer.piece.stronghold;
    writer.box_selector(0, 0, 0, 4, 10, 7, true, random);
    writer.small_door(data.door, 1, 7, 0);
    writer.small_door(stronghold_door::opening, 1, 1, 7);
    for (jint i = 0; i < 6; ++i) {
        writer.set(1, 6 - i, 1 + i, writer.cobblestone_stairs);
        writer.set(2, 6 - i, 1 + i, writer.cobblestone_stairs);
        writer.set(3, 6 - i, 1 + i, writer.cobblestone_stairs);
        if (i < 5) {
            writer.set(1, 5 - i, 1 + i, writer.stone_bricks);
            writer.set(2, 5 - i, 1 + i, writer.stone_bricks);
            writer.set(3, 5 - i, 1 + i, writer.stone_bricks);
        }
    }
}

} // namespace

void place_stronghold_piece(const structure_place_context &context,
                            const structure_piece_record &piece,
                            worldgen_random &random) {
    if (!piece.stronghold.has_value()) {
        return;
    }
    // The selector and all piece interiors consume `level.getRandom()`, which
    // is the placement stream the caller passes in.
    const stronghold_writer writer{context, piece};
    switch (piece.stronghold->kind) {
    case stronghold_piece_kind::start:
    case stronghold_piece_kind::stairs_down:
        place_stairs_down(writer, random);
        break;
    case stronghold_piece_kind::straight:
        place_straight(writer, random);
        break;
    case stronghold_piece_kind::prison_hall:
        place_prison_hall(writer, random);
        break;
    case stronghold_piece_kind::chest_corridor:
        place_chest_corridor(writer, random);
        break;
    case stronghold_piece_kind::filler_corridor:
        place_filler_corridor(writer);
        break;
    case stronghold_piece_kind::five_crossing:
        place_five_crossing(writer, random);
        break;
    case stronghold_piece_kind::left_turn:
        place_turn(writer, true, random);
        break;
    case stronghold_piece_kind::right_turn:
        place_turn(writer, false, random);
        break;
    case stronghold_piece_kind::library:
        place_library(writer, random);
        break;
    case stronghold_piece_kind::portal_room:
        place_portal_room(writer, random);
        break;
    case stronghold_piece_kind::room_crossing:
        place_room_crossing(writer, random);
        break;
    case stronghold_piece_kind::straight_stairs_down:
        place_straight_stairs_down(writer, random);
        break;
    }
}

} // namespace refrain::worldgen::structure
