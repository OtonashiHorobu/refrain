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
#include <refrain/worldgen/structure/structure_data.hpp>

#include <boost/ut.hpp>

#include <array>
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

struct reference_piece {
    std::string id;
    std::vector<jint> box;
};

auto reference_pieces(const data::nbt::value &document,
                      std::string_view start_id)
    -> std::optional<std::vector<reference_piece>> {
    const auto *structures = child(document.as_compound(), "structures");
    const auto *starts = structures == nullptr
                             ? nullptr
                             : child(structures->as_compound(), "starts");
    const auto *start =
        starts == nullptr ? nullptr : child(starts->as_compound(), start_id);
    const auto *children =
        start == nullptr ? nullptr : child(start->as_compound(), "Children");
    const auto *list = children == nullptr ? nullptr : children->as_list();
    if (list == nullptr) {
        return std::nullopt;
    }
    std::vector<reference_piece> pieces;
    for (const auto &entry : list->items) {
        const auto *compound = entry.as_compound();
        const auto *id = child(compound, "id");
        const auto *bb = child(compound, "BB");
        const auto box = bb == nullptr ? std::nullopt : bb->as_int_array();
        if (id == nullptr || !box || box->size() != 6U) {
            return std::nullopt;
        }
        pieces.push_back(reference_piece{
            .id = std::string{id->as_string().value_or("")},
            .box = std::vector<jint>{box->begin(), box->end()},
        });
    }
    return pieces;
}

auto openings_text(const structure::monument_room_state &room) -> std::string {
    std::string text;
    for (const bool open : room.openings) {
        text.push_back(open ? '1' : '0');
    }
    return text;
}

auto digest_piece(std::uint64_t &hash, const structure::structure_piece &piece)
    -> void {
    const auto mix = [&](std::string_view text) {
        for (const char ch : text) {
            hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
            hash *= 1099511628211ULL;
        }
    };
    auto line = piece.type + "," + std::to_string(piece.bounds.min_x) + "," +
                std::to_string(piece.bounds.min_y) + "," +
                std::to_string(piece.bounds.min_z) + "," +
                std::to_string(piece.bounds.max_x) + "," +
                std::to_string(piece.bounds.max_y) + "," +
                std::to_string(piece.bounds.max_z) + "," +
                std::to_string(piece.orientation);
    if (piece.monument) {
        line += "," + std::to_string(static_cast<int>(piece.monument->kind)) +
                "," + std::to_string(piece.monument->main_design);
        for (const auto &room : piece.monument->rooms) {
            line += "," + std::to_string(room.index) + ":" +
                    openings_text(room) + ":" +
                    std::to_string(room.up_connected ? 1 : 0);
        }
    }
    line += ";";
    mix(line);
    for (const auto &nested : piece.children) {
        digest_piece(hash, nested);
    }
}

auto start_digest(const structure::structure_start &start) -> std::uint64_t {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto &piece : start.pieces) {
        digest_piece(hash, piece);
    }
    return hash;
}

auto make_registry() -> block_state_registry {
    block_state_registry states;
    (void)states.intern("minecraft:air", {.air = true});
    (void)states.intern(
        "minecraft:stone",
        {.air = false, .blocks_motion = true, .blocks_motion_no_leaves = true});
    (void)states.intern("minecraft:water",
                        {.air = false, .fluid = true, .water = true});
    (void)states.intern(
        "minecraft:prismarine",
        {.air = false, .blocks_motion = true, .blocks_motion_no_leaves = true});
    (void)states.intern(
        "minecraft:prismarine_bricks",
        {.air = false, .blocks_motion = true, .blocks_motion_no_leaves = true});
    (void)states.intern(
        "minecraft:dark_prismarine",
        {.air = false, .blocks_motion = true, .blocks_motion_no_leaves = true});
    (void)states.intern(
        "minecraft:sea_lantern",
        {.air = false, .blocks_motion = true, .blocks_motion_no_leaves = true});
    (void)states.intern(
        "minecraft:gold_block",
        {.air = false, .blocks_motion = true, .blocks_motion_no_leaves = true});
    return states;
}

auto state_of(const block_state_registry &states, std::string_view name)
    -> block_state_id {
    return states.find(name).value_or(no_block_state);
}

auto piece_name(const block_state_registry &states, block_state_id state)
    -> std::string_view {
    return state_block_name(states.name(state));
}

} // namespace

suite<"worldgen/monument"> monument_tests = [] {
    "the monument biome gate resolves nested tags"_test = [] {
        structure::structure_data_set data;
        data.biome_tags["minecraft:required_ocean_monument_surrounding"] = {
            "#minecraft:is_ocean", "#minecraft:is_river"};
        data.biome_tags["minecraft:is_ocean"] = {"minecraft:deep_ocean",
                                                 "minecraft:ocean"};
        data.biome_tags["minecraft:is_river"] = {"minecraft:river"};
        expect(data.biome_matches_tag(
            "minecraft:required_ocean_monument_surrounding",
            "minecraft:deep_ocean"));
        expect(data.biome_matches_tag(
            "minecraft:required_ocean_monument_surrounding",
            "minecraft:river"));
        expect(!data.biome_matches_tag(
            "minecraft:required_ocean_monument_surrounding",
            "minecraft:plains"));
        expect(!data.biome_matches_tag("minecraft:missing",
                                       "minecraft:deep_ocean"));
    };

    "the room graph fits the reference piece order"_test = [] {
        const auto start =
            structure::ocean_monument_start(chunk_pos{.x = 12, .z = 23}, 1);
        expect(start.pieces.size() == 1U);
        if (start.pieces.size() != 1U) {
            return;
        }
        const auto &building = start.pieces.front();
        expect(building.type == "minecraft:omb");
        expect(building.orientation == 0);
        expect(building.bounds.min_x == 163 && building.bounds.min_y == 39 &&
               building.bounds.min_z == 339 && building.bounds.max_x == 220 &&
               building.bounds.max_y == 61 && building.bounds.max_z == 396);
        expect(building.children.size() == 31U);
        if (building.children.size() != 31U) {
            return;
        }
        expect(building.children.front().type == "minecraft:omentry");
        expect(building.children[1].type == "minecraft:omcr");
        expect(building.children[building.children.size() - 3].type ==
               "minecraft:omwr");
        expect(building.children[building.children.size() - 2].type ==
               "minecraft:omwr");
        expect(building.children.back().type == "minecraft:ompenthouse");
        std::array<bool, 75> seen_rooms{};
        for (const auto &piece : building.children) {
            if (!piece.monument ||
                piece.monument->kind ==
                    structure::monument_piece_kind::wing_room ||
                piece.monument->kind ==
                    structure::monument_piece_kind::penthouse) {
                continue;
            }
            const jint index = piece.monument->rooms[0].index;
            expect(index >= 0 && index < 75);
            if (index < 0 || index >= 75) {
                continue;
            }
            expect(!seen_rooms[static_cast<std::size_t>(index)]);
            seen_rooms[static_cast<std::size_t>(index)] = true;
        }
        expect(start_digest(start) == 0x92d3b6bfea3392f7ULL);
    };

    "the monument start is deterministic"_test = [] {
        const auto first =
            structure::ocean_monument_start(chunk_pos{.x = 12, .z = 23}, 1);
        const auto second =
            structure::ocean_monument_start(chunk_pos{.x = 12, .z = 23}, 1);
        expect(first.pieces.size() == second.pieces.size());
        expect(start_digest(first) == start_digest(second));
        const auto other =
            structure::ocean_monument_start(chunk_pos{.x = 12, .z = 23}, 2);
        expect(start_digest(first) != start_digest(other));
    };

    "the monument start matches the reference piece list"_test = [] {
        const char *region = std::getenv("REFRAIN_VANILLA_MONUMENT_REGION");
        if (region == nullptr) {
            return;
        }
        const auto reference = read_chunk(region, 12, 23);
        expect(reference.has_value());
        if (!reference) {
            return;
        }
        const auto pieces = reference_pieces(*reference, "minecraft:monument");
        expect(pieces.has_value());
        if (!pieces) {
            return;
        }
        const auto start =
            structure::ocean_monument_start(chunk_pos{.x = 12, .z = 23}, 1);
        expect(start.pieces.size() == pieces->size());
        const auto count = std::min(start.pieces.size(), pieces->size());
        for (std::size_t i = 0; i < count; ++i) {
            const auto &expected = (*pieces)[i];
            const auto &actual = start.pieces[i];
            expect(expected.id == actual.type);
            const std::vector<jint> actual_box{
                actual.bounds.min_x, actual.bounds.min_y, actual.bounds.min_z,
                actual.bounds.max_x, actual.bounds.max_y, actual.bounds.max_z};
            expect(actual_box == expected.box);
        }
    };

    "the monument piece list digest pins a real candidate"_test = [] {
        if (std::getenv("REFRAIN_VANILLA_STRUCTURES") == nullptr) {
            return;
        }
        const auto start =
            structure::ocean_monument_start(chunk_pos{.x = 12, .z = 23}, 1);
        expect(start_digest(start) == 0x92d3b6bfea3392f7ULL);
    };

    "the monument block helpers write the room floor and walls"_test = [] {
        auto states = make_registry();
        proto_chunk chunk{states, 0, 0, -64, 384};
        chunk_region region{chunk, 0};
        const auto water = state_of(states, "minecraft:water");
        const auto stone = state_of(states, "minecraft:stone");
        for (jint y = 39; y <= 47; ++y) {
            for (jint z = 0; z <= 15; ++z) {
                for (jint x = 0; x <= 15; ++x) {
                    (void)region.set_block_state(x, y, z,
                                                 y >= 39 ? water : stone);
                }
            }
        }
        structure::structure_piece piece{
            .type = "minecraft:omcr",
            .bounds = {.min_x = 0,
                       .min_y = 39,
                       .min_z = 0,
                       .max_x = 15,
                       .max_y = 46,
                       .max_z = 15},
            .orientation = 0,
            .monument =
                structure::monument_piece_data{
                    .kind = structure::monument_piece_kind::core_room,
                },
        };
        const auto record = structure::to_piece_record(piece);
        const structure::structure_place_context context{
            .region = &region,
            .chunk = &chunk,
            .states = &states,
            .state =
                [&states](std::string_view key) {
                    return state_of(states, key);
                },
            .registry = &states,
            .seed = 1,
            .sea_level = 63,
        };
        worldgen_random random{std::make_unique<xoroshiro_random_source>(1)};
        structure::place_structure_piece(context, record, random);
        expect(piece_name(states, region.state_at(1, 47, 0)) ==
               "minecraft:prismarine");
        expect(piece_name(states, region.state_at(0, 46, 0)) ==
               "minecraft:prismarine_bricks");
        expect(piece_name(states, region.state_at(7, 43, 7)) ==
               "minecraft:gold_block");
        expect(piece_name(states, region.state_at(8, 44, 8)) ==
               "minecraft:gold_block");
        expect(piece_name(states, region.state_at(6, 42, 6)) ==
               "minecraft:sea_lantern");
        expect(piece_name(states, region.state_at(9, 42, 9)) ==
               "minecraft:sea_lantern");
        expect(region.state_at(1, 39, 1) == water);
    };

    "the monument water box keeps water and fills below the sea level"_test =
        [] {
            auto states = make_registry();
            proto_chunk chunk{states, 0, 0, -64, 384};
            chunk_region region{chunk, 0};
            const auto water = state_of(states, "minecraft:water");
            const auto stone = state_of(states, "minecraft:stone");
            for (jint y = 39; y <= 46; ++y) {
                for (jint z = 0; z <= 7; ++z) {
                    for (jint x = 0; x <= 7; ++x) {
                        (void)region.set_block_state(x, y, z, stone);
                    }
                }
            }
            auto room = structure::monument_room_state{};
            room.index = 0;
            room.openings[static_cast<std::size_t>(
                structure::monument_direction::north)] = true;
            structure::structure_piece piece{
                .type = "minecraft:omentry",
                .bounds = {.min_x = 0,
                           .min_y = 39,
                           .min_z = 0,
                           .max_x = 7,
                           .max_y = 42,
                           .max_z = 7},
                .orientation = 0,
                .monument =
                    structure::monument_piece_data{
                        .kind = structure::monument_piece_kind::entry_room,
                        .rooms = {room},
                    },
            };
            const auto record = structure::to_piece_record(piece);
            const structure::structure_place_context context{
                .region = &region,
                .chunk = &chunk,
                .states = &states,
                .state =
                    [&states](std::string_view key) {
                        return state_of(states, key);
                    },
                .registry = &states,
                .seed = 1,
                .sea_level = 63,
            };
            worldgen_random random{
                std::make_unique<xoroshiro_random_source>(1)};
            structure::place_structure_piece(context, record, random);
            // The north opening water box replaces the stone with water.
            expect(region.state_at(3, 40, 7) == water);
            expect(region.state_at(4, 41, 7) == water);
            // The wall boxes overwrite the water fill.
            expect(piece_name(states, region.state_at(0, 42, 0)) ==
                   "minecraft:prismarine_bricks");
            expect(piece_name(states, region.state_at(7, 42, 7)) ==
                   "minecraft:prismarine_bricks");
        };
};
