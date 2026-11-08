// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/nbt.hpp>
#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/core/rng.hpp>
#include <refrain/worldgen/structure/generation.hpp>
#include <refrain/worldgen/structure/placement_stage.hpp>
#include <refrain/worldgen/structure/stage.hpp>
#include <refrain/worldgen/structure/start.hpp>
#include <refrain/worldgen/structure/structure_data.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace boost::ut;
using namespace refrain;
using namespace refrain::worldgen;

auto child(const data::nbt::compound *entries, std::string_view key)
    -> const data::nbt::value * {
    if (entries == nullptr) {
        return nullptr;
    }
    const auto found = entries->find(key);
    return found == entries->end() ? nullptr : &found->second;
}

auto read_chunk(std::string_view path, jint chunk_x, jint chunk_z)
    -> std::optional<data::nbt::value> {
    std::ifstream stream{std::string{path}, std::ios::binary};
    if (!stream) {
        return std::nullopt;
    }
    std::vector<char> raw((std::istreambuf_iterator<char>(stream)),
                          std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    const auto local_x = static_cast<std::size_t>(((chunk_x % 32) + 32) % 32);
    const auto local_z = static_cast<std::size_t>(((chunk_z % 32) + 32) % 32);
    const auto index = local_x + local_z * 32;
    if ((index + 1) * 4 > bytes.size()) {
        return std::nullopt;
    }
    const auto read_u32 = [&](std::size_t offset) {
        return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
               (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
               (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
               static_cast<std::uint32_t>(bytes[offset + 3]);
    };
    const auto offset = static_cast<std::size_t>(read_u32(index * 4) >> 8U);
    if (offset == 0 || offset * 4096 + 5 > bytes.size()) {
        return std::nullopt;
    }
    const auto length = read_u32(offset * 4096);
    if (length < 1 || offset * 4096 + 4 + length > bytes.size()) {
        return std::nullopt;
    }
    const auto compression = bytes[offset * 4096 + 4];
    if (std::to_integer<int>(compression) != 2) {
        return std::nullopt;
    }
    const auto payload = std::span{bytes.data() + offset * 4096 + 5,
                                   static_cast<std::size_t>(length) - 1U};
    auto parsed = data::nbt::read(payload, data::nbt::compression::zlib);
    if (!parsed) {
        return std::nullopt;
    }
    return std::move(*parsed);
}

auto mix(std::uint64_t &hash, std::string_view text) -> void {
    for (const char ch : text) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
        hash *= 1099511628211ULL;
    }
}

auto piece_digest(const structure::structure_piece &piece) -> std::string {
    const auto &data = *piece.stronghold;
    return piece.type + "," + std::to_string(piece.bounds.min_x) + "," +
           std::to_string(piece.bounds.min_y) + "," +
           std::to_string(piece.bounds.min_z) + "," +
           std::to_string(piece.bounds.max_x) + "," +
           std::to_string(piece.bounds.max_y) + "," +
           std::to_string(piece.bounds.max_z) + "," +
           std::to_string(piece.orientation) + "," +
           std::to_string(data.gen_depth) + "," +
           std::to_string(static_cast<int>(data.door)) + "," +
           std::to_string(data.left ? 1 : 0) +
           std::to_string(data.right ? 1 : 0) +
           std::to_string(data.left_low ? 1 : 0) +
           std::to_string(data.left_high ? 1 : 0) +
           std::to_string(data.right_low ? 1 : 0) +
           std::to_string(data.right_high ? 1 : 0) +
           std::to_string(data.tall ? 1 : 0) + "," +
           std::to_string(data.variant) + "," + std::to_string(data.steps) +
           ";";
}

auto start_digest(const structure::structure_start &start) -> std::uint64_t {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto &piece : start.pieces) {
        mix(hash, piece_digest(piece));
    }
    return hash;
}

auto ridge_digest(const std::vector<chunk_pos> &positions) -> std::uint64_t {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto &position : positions) {
        mix(hash, std::to_string(position.x) + "," +
                      std::to_string(position.z) + ";");
    }
    return hash;
}

auto make_registry() -> block_state_registry {
    block_state_registry states;
    (void)states.intern("minecraft:air", {.air = true});
    (void)states.intern(
        "minecraft:stone",
        {.air = false, .blocks_motion = true, .blocks_motion_no_leaves = true});
    (void)states.intern("minecraft:cave_air", {.air = true});
    (void)states.intern(
        "minecraft:stone_bricks",
        {.air = false, .blocks_motion = true, .blocks_motion_no_leaves = true});
    (void)states.intern(
        "minecraft:cracked_stone_bricks",
        {.air = false, .blocks_motion = true, .blocks_motion_no_leaves = true});
    (void)states.intern(
        "minecraft:mossy_stone_bricks",
        {.air = false, .blocks_motion = true, .blocks_motion_no_leaves = true});
    (void)states.intern(
        "minecraft:infested_stone_bricks",
        {.air = false, .blocks_motion = true, .blocks_motion_no_leaves = true});
    return states;
}

auto resolve_state(block_state_registry &states, std::string_view key)
    -> block_state_id {
    if (const auto found = states.find(key); found) {
        return *found;
    }
    const auto block = state_block_name(key);
    const bool air = is_air_block_name(block);
    const bool fluid = block == "minecraft:water" || block == "minecraft:lava";
    const auto interned =
        states.intern(key, {.air = air,
                            .fluid = fluid,
                            .water = block == "minecraft:water",
                            .blocks_motion = !air && !fluid,
                            .blocks_motion_no_leaves = !air && !fluid});
    return interned.value_or(no_block_state);
}

auto state_name(const block_state_registry &states, block_state_id state)
    -> std::string_view {
    return states.name(state);
}

} // namespace

suite<"worldgen/stronghold"> stronghold_tests = [] {
    "the concentric rings reproduce the reference candidates"_test = [] {
        structure::structure_data_set data;
        data.biome_tags["minecraft:stronghold_biased_to"] = {
            "minecraft:plains", "minecraft:desert"};
        structure::concentric_rings_placement placement{
            .distance = 32,
            .spread = 3,
            .count = 128,
            .preferred_biomes_tag = "minecraft:stronghold_biased_to",
        };
        const structure::structure_start_context context{
            .seed = 1,
            .data = &data,
            .min_y = -64,
            .world_height = 384,
            .sea_level = 63,
            .fixed_biome = true,
            .biome_at = [](jint, jint, jint) { return std::uint16_t{0}; },
            .biome_name = [](std::uint16_t) -> std::string_view {
                return "minecraft:plains";
            },
        };
        const auto positions =
            structure::concentric_ring_positions(1, placement, context);
        expect(positions.size() == 128U);
        if (positions.size() != 128U) {
            return;
        }
        expect(positions[0].x == -10 && positions[0].z == -118);
        expect(positions[1].x == 108 && positions[1].z == 51);
        expect(positions[2].x == -74 && positions[2].z == 49);
        expect(ridge_digest(positions) == 0xe672f68a349fe46cULL);
    };

    "the stronghold start lists the reference pieces"_test = [] {
        const auto start = structure::stronghold_start(
            chunk_pos{.x = 108, .z = 51}, 1, -64, 63);
        expect(start.pieces.size() == 97U);
        if (start.pieces.empty()) {
            return;
        }
        expect(start.pieces.front().type == "minecraft:shstart");
        expect(start.pieces.front().bounds.min_x == 1730 &&
               start.pieces.front().bounds.min_y == -45 &&
               start.pieces.front().bounds.min_z == 818);
        bool has_portal = false;
        for (const auto &piece : start.pieces) {
            has_portal = has_portal || piece.type == "minecraft:shpr";
        }
        expect(has_portal);
        expect(start.bounds.min_x == 1673 && start.bounds.min_y == -61 &&
               start.bounds.min_z == 791 && start.bounds.max_x == 1768 &&
               start.bounds.max_y == -35 && start.bounds.max_z == 912);
        expect(start_digest(start) == 0xc175ed012a9beb8dULL);
    };

    "the stronghold start is deterministic"_test = [] {
        const auto first = structure::stronghold_start(
            chunk_pos{.x = 108, .z = 51}, 1, -64, 63);
        const auto second = structure::stronghold_start(
            chunk_pos{.x = 108, .z = 51}, 1, -64, 63);
        expect(first.pieces.size() == second.pieces.size());
        expect(start_digest(first) == start_digest(second));
        const auto other = structure::stronghold_start(
            chunk_pos{.x = 108, .z = 51}, 2, -64, 63);
        expect(start_digest(first) != start_digest(other));
    };

    "the stronghold start matches the reference piece list"_test = [] {
        const char *region = std::getenv("REFRAIN_VANILLA_STRONGHOLD_REGION");
        if (region == nullptr) {
            return;
        }
        const auto document = read_chunk(region, 108, 51);
        expect(document.has_value());
        if (!document) {
            return;
        }
        const auto *structures = child(document->as_compound(), "structures");
        const auto *starts = structures == nullptr
                                 ? nullptr
                                 : child(structures->as_compound(), "starts");
        const auto *start = starts == nullptr ? nullptr
                                              : child(starts->as_compound(),
                                                      "minecraft:stronghold");
        const auto *children = start == nullptr
                                   ? nullptr
                                   : child(start->as_compound(), "Children");
        const auto pieces = children == nullptr ? nullptr : children->as_list();
        expect(pieces != nullptr);
        if (pieces == nullptr) {
            return;
        }
        const auto generated = structure::stronghold_start(
            chunk_pos{.x = 108, .z = 51}, 1, -64, 63);
        expect(generated.pieces.size() == pieces->items.size());
        const auto count =
            std::min(generated.pieces.size(), pieces->items.size());
        for (std::size_t index = 0; index < count; ++index) {
            const auto *expected = pieces->items[index].as_compound();
            const auto *id = child(expected, "id");
            const auto *bb = child(expected, "BB");
            const auto *orientation = child(expected, "O");
            const auto *depth = child(expected, "GD");
            const auto box = bb == nullptr ? std::nullopt : bb->as_int_array();
            expect(id != nullptr && (box.has_value() && box->size() == 6U) &&
                   orientation != nullptr);
            if (id == nullptr || !(box.has_value() && box->size() == 6U) ||
                orientation == nullptr) {
                continue;
            }
            const auto &actual = generated.pieces[index];
            expect(id->as_string().value_or("") == actual.type);
            expect(orientation->as_int().value_or(-1) == actual.orientation);
            if (depth != nullptr) {
                expect(depth->as_int().value_or(-1) ==
                       actual.stronghold->gen_depth);
            }
            expect(actual.bounds.min_x == (*box)[0]);
            expect(actual.bounds.min_y == (*box)[1]);
            expect(actual.bounds.min_z == (*box)[2]);
            expect(actual.bounds.max_x == (*box)[3]);
            expect(actual.bounds.max_y == (*box)[4]);
            expect(actual.bounds.max_z == (*box)[5]);
        }
    };

    "the stronghold piece list digest pins a real candidate"_test = [] {
        if (std::getenv("REFRAIN_VANILLA_STRUCTURES") == nullptr) {
            return;
        }
        const auto start = structure::stronghold_start(
            chunk_pos{.x = 108, .z = 51}, 1, -64, 63);
        expect(start_digest(start) == 0xc175ed012a9beb8dULL);
    };

    "the stronghold block helpers carve the shell and rotate doors"_test = [] {
        auto states = make_registry();
        proto_chunk chunk{states, 0, 0, -64, 384};
        chunk_region region{chunk, 0};
        const auto stone = resolve_state(states, "minecraft:stone");
        for (jint y = 39; y <= 50; ++y) {
            for (jint z = 0; z <= 15; ++z) {
                for (jint x = 0; x <= 15; ++x) {
                    (void)region.set_block_state(x, y, z, stone);
                }
            }
        }
        structure::structure_piece piece{
            .type = "minecraft:shs",
            .bounds = {.min_x = 0,
                       .min_y = 39,
                       .min_z = 1,
                       .max_x = 4,
                       .max_y = 43,
                       .max_z = 7},
            .orientation = 0, // south: mirror left/right
            .stronghold =
                structure::stronghold_piece_data{
                    .kind = structure::stronghold_piece_kind::straight,
                    .door = structure::stronghold_door::iron_door,
                },
        };
        const auto record = structure::to_piece_record(piece);
        const structure::structure_place_context place_context{
            .region = &region,
            .chunk = &chunk,
            .states = &states,
            .state =
                [&states](std::string_view key) {
                    return resolve_state(states, key);
                },
            .registry = &states,
            .seed = 1,
            .sea_level = 63,
        };
        worldgen_random random{std::make_unique<xoroshiro_random_source>(1)};
        structure::place_structure_piece(place_context, record, random);
        // Interior columns are carved to cave air; edge columns become the
        // selector's stone brick variants.
        expect(state_name(states, region.state_at(2, 41, 4)) ==
               "minecraft:cave_air");
        const auto edge = state_name(states, region.state_at(0, 39, 1));
        expect(edge == "minecraft:stone_bricks" ||
               edge == "minecraft:cracked_stone_bricks" ||
               edge == "minecraft:mossy_stone_bricks" ||
               edge == "minecraft:infested_stone_bricks");
        // The iron door is placed from a north-facing source and mirrored to
        // south, as are the two buttons.
        const auto door =
            std::string{state_name(states, region.state_at(2, 40, 1))};
        expect(door.starts_with("minecraft:iron_door"));
        expect(door.find("facing=south") != std::string::npos);
        expect(state_name(states, region.state_at(3, 41, 2)) ==
               "minecraft:stone_button[face=wall,facing=south,powered=false]");
        expect(state_name(states, region.state_at(3, 41, 0)) ==
               "minecraft:stone_button");
    };
};
