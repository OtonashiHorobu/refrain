// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Nether fortress and end city tests. The env-gated cases compare the complete
// piece list of one reference start against a reference world; the synthetic
// cases pin the start invariants, the structure-set dispatch and the end city
// chest marker without a reference world.

#include <refrain/core/io.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/structure/generation.hpp>
#include <refrain/worldgen/structure/placement_stage.hpp>
#include <refrain/worldgen/structure/stage.hpp>
#include <refrain/worldgen/structure/structure_data.hpp>
#include <refrain/worldgen/structure/template.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace boost::ut;
using namespace refrain;
using namespace refrain::worldgen;
using namespace refrain::worldgen::structure;

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
    if (std::to_integer<int>(bytes[offset * 4096 + 4]) != 2) {
        return std::nullopt;
    }
    const auto payload =
        std::span{bytes.data() + offset * 4096 + 5, length - 1};
    auto parsed = data::nbt::read(payload, data::nbt::compression::zlib);
    if (!parsed) {
        return std::nullopt;
    }
    return std::move(*parsed);
}

auto env_chunk(const char *x_name, const char *z_name)
    -> std::optional<chunk_pos> {
    const char *x = std::getenv(x_name);
    const char *z = std::getenv(z_name);
    if (x == nullptr || z == nullptr) {
        return std::nullopt;
    }
    return chunk_pos{.x = std::atoi(x), .z = std::atoi(z)};
}

auto env_seed() -> jlong {
    if (const char *value = std::getenv("REFRAIN_VANILLA_ORACLE_SEED");
        value != nullptr) {
        return std::strtoll(value, nullptr, 10);
    }
    return 1;
}

auto piece_bounds(const data::nbt::value &entry)
    -> std::optional<structure_bbox> {
    const auto *bb = child(entry.as_compound(), "BB");
    const auto values = bb == nullptr ? std::nullopt : bb->as_int_array();
    if (!values.has_value() || values->size() != 6U) {
        return std::nullopt;
    }
    return structure_bbox{.min_x = (*values)[0],
                          .min_y = (*values)[1],
                          .min_z = (*values)[2],
                          .max_x = (*values)[3],
                          .max_y = (*values)[4],
                          .max_z = (*values)[5]};
}

auto fortress_piece_id(fortress_piece_kind kind) -> std::string_view {
    switch (kind) {
    case fortress_piece_kind::bridge_crossing:
        return "minecraft:nebcr";
    case fortress_piece_kind::bridge_end_filler:
        return "minecraft:nebef";
    case fortress_piece_kind::bridge_straight:
        return "minecraft:nebs";
    case fortress_piece_kind::castle_corridor_stairs:
        return "minecraft:neccs";
    case fortress_piece_kind::castle_corridor_t_balcony:
        return "minecraft:nectb";
    case fortress_piece_kind::castle_entrance:
        return "minecraft:nece";
    case fortress_piece_kind::castle_small_corridor:
        return "minecraft:nesc";
    case fortress_piece_kind::castle_small_corridor_crossing:
        return "minecraft:nescsc";
    case fortress_piece_kind::castle_small_corridor_left_turn:
        return "minecraft:nesclt";
    case fortress_piece_kind::castle_small_corridor_right_turn:
        return "minecraft:nescrt";
    case fortress_piece_kind::castle_stalk_room:
        return "minecraft:necsr";
    case fortress_piece_kind::monster_throne:
        return "minecraft:nemt";
    case fortress_piece_kind::room_crossing:
        return "minecraft:nerc";
    case fortress_piece_kind::stairs_room:
        return "minecraft:nesr";
    }
    return {};
}

auto rotation_name(template_rotation rotation) -> std::string_view {
    switch (rotation) {
    case template_rotation::none:
        return "NONE";
    case template_rotation::clockwise_90:
        return "CLOCKWISE_90";
    case template_rotation::clockwise_180:
        return "CLOCKWISE_180";
    case template_rotation::counterclockwise_90:
        return "COUNTERCLOCKWISE_90";
    }
    return "NONE";
}

auto ints(std::initializer_list<jint> values) -> data::nbt::value {
    auto list = data::nbt::make_list(data::nbt::tag_type::int_);
    for (const auto value : values) {
        (void)list.push(data::nbt::value{value});
    }
    return list;
}

auto palette_entry(std::string name, data::nbt::compound properties = {})
    -> data::nbt::compound {
    data::nbt::compound entry;
    entry["Name"] = data::nbt::value{std::move(name)};
    if (!properties.empty()) {
        entry["Properties"] = data::nbt::value{std::move(properties)};
    }
    return entry;
}

auto block_entry(std::initializer_list<jint> pos, jint state,
                 data::nbt::compound nbt = {}) -> data::nbt::compound {
    data::nbt::compound entry;
    entry["pos"] = ints(pos);
    entry["state"] = data::nbt::value{state};
    if (!nbt.empty()) {
        entry["nbt"] = data::nbt::value{std::move(nbt)};
    }
    return entry;
}

auto write_synthetic_template(const std::filesystem::path &root,
                              std::string_view namespace_name,
                              std::string_view directory, std::string_view name,
                              jint size,
                              const data::nbt::compound *palette_override,
                              const data::nbt::list_value *blocks_override)
    -> bool {
    data::nbt::compound root_tag;
    root_tag["size"] = ints({size, size, size});
    if (palette_override != nullptr && blocks_override != nullptr) {
        root_tag["palette"] = data::nbt::value{*palette_override};
        root_tag["blocks"] = data::nbt::value{*blocks_override};
    } else {
        auto palette = data::nbt::make_list(data::nbt::tag_type::compound);
        (void)palette.push(data::nbt::value{palette_entry("minecraft:stone")});
        root_tag["palette"] = std::move(palette);
        auto blocks = data::nbt::make_list(data::nbt::tag_type::compound);
        (void)blocks.push(data::nbt::value{block_entry({0, 0, 0}, 0)});
        root_tag["blocks"] = std::move(blocks);
    }
    const auto path = root / std::string{namespace_name} / "structure" /
                      std::string{directory} / (std::string{name} + ".nbt");
    std::filesystem::create_directories(path.parent_path());
    return data::nbt::write_file(path.string(), data::nbt::value{root_tag}, "",
                                 data::nbt::compression::gzip)
        .has_value();
}

auto make_end_city_templates(const std::filesystem::path &root) -> bool {
    static constexpr std::array<std::string_view, 19> names{{
        "base_floor",     "base_roof",           "second_floor_1",
        "second_floor_2", "second_roof",         "third_floor_1",
        "third_floor_2",  "third_roof",          "tower_base",
        "tower_piece",    "tower_top",           "bridge_end",
        "bridge_piece",   "bridge_steep_stairs", "bridge_gentle_stairs",
        "ship",           "fat_tower_base",      "fat_tower_middle",
        "fat_tower_top",
    }};
    for (const auto name : names) {
        if (!write_synthetic_template(root, "minecraft", "end_city", name, 1,
                                      nullptr, nullptr)) {
            return false;
        }
    }
    return true;
}

auto make_end_city_manager(const std::filesystem::path &root,
                           block_state_registry &states)
    -> std::unique_ptr<structure_template_manager> {
    (void)states.intern("minecraft:air", {.air = true});
    return std::make_unique<structure_template_manager>(root.string(), states);
}

auto same_bounds(const structure_bbox &left, const structure_bbox &right)
    -> bool {
    return left.min_x == right.min_x && left.min_y == right.min_y &&
           left.min_z == right.min_z && left.max_x == right.max_x &&
           left.max_y == right.max_y && left.max_z == right.max_z;
}

auto union_of(const std::vector<structure_piece> &pieces) -> structure_bbox {
    structure_bbox bounds = pieces.front().bounds;
    for (const auto &piece : pieces) {
        bounds.min_x = std::min(bounds.min_x, piece.bounds.min_x);
        bounds.min_y = std::min(bounds.min_y, piece.bounds.min_y);
        bounds.min_z = std::min(bounds.min_z, piece.bounds.min_z);
        bounds.max_x = std::max(bounds.max_x, piece.bounds.max_x);
        bounds.max_y = std::max(bounds.max_y, piece.bounds.max_y);
        bounds.max_z = std::max(bounds.max_z, piece.bounds.max_z);
    }
    return bounds;
}

auto same_start(const structure::structure_start &left,
                const structure::structure_start &right) -> bool {
    if (left.pieces.size() != right.pieces.size() ||
        !same_bounds(left.bounds, right.bounds)) {
        return false;
    }
    for (std::size_t i = 0; i < left.pieces.size(); ++i) {
        const auto &a = left.pieces[i];
        const auto &b = right.pieces[i];
        if (a.type != b.type || !same_bounds(a.bounds, b.bounds) ||
            a.template_id != b.template_id ||
            a.template_rotation != b.template_rotation ||
            a.overwrite != b.overwrite ||
            a.template_origin.x != b.template_origin.x ||
            a.template_origin.y != b.template_origin.y ||
            a.template_origin.z != b.template_origin.z) {
            return false;
        }
    }
    return true;
}

} // namespace

suite<"worldgen/dim-structures"> dim_structure_tests = [] {
    "the fortress start is deterministic and inside its moved height band"_test =
        [] {
            const auto first =
                fortress_start(chunk_pos{.x = 3, .z = -2}, 12345);
            const auto second =
                fortress_start(chunk_pos{.x = 3, .z = -2}, 12345);
            expect(!first.pieces.empty());
            expect(first.pieces.size() > 5U);
            expect(first.pieces.front().fortress.has_value());
            expect(first.pieces.front().fortress->kind ==
                   fortress_piece_kind::bridge_crossing);
            expect(same_start(first, second));
            const auto bounds = union_of(first.pieces);
            expect(same_bounds(bounds, first.bounds));
            expect(first.bounds.min_y >= 48);
            expect(first.bounds.min_y <= 70);
            bool castle = false;
            for (const auto &piece : first.pieces) {
                expect(piece.type == "minecraft:fortress");
                expect(piece.fortress.has_value());
                if (!piece.fortress.has_value()) {
                    continue;
                }
                if (piece.fortress->kind ==
                        fortress_piece_kind::castle_small_corridor ||
                    piece.fortress->kind ==
                        fortress_piece_kind::castle_entrance) {
                    castle = true;
                }
                expect(piece.orientation >= 0 && piece.orientation <= 3);
            }
            expect(castle);
        };

    "the end city start repeats the fixed floor and its rotations"_test = [] {
        const auto directory =
            std::filesystem::path{core::io::temp_directory()} /
            "refrain-end-city-synthetic";
        std::filesystem::remove_all(directory);
        expect(make_end_city_templates(directory));
        block_state_registry states;
        const auto manager = make_end_city_manager(directory, states);
        const block_pos origin{.x = 16, .y = 80, .z = 32};
        auto first_random =
            worldgen_random{std::make_unique<legacy_random_source>(7)};
        auto second_random =
            worldgen_random{std::make_unique<legacy_random_source>(7)};
        const auto first = end_city_start(
            *manager, origin, template_rotation::none, first_random);
        const auto second = end_city_start(
            *manager, origin, template_rotation::none, second_random);
        expect(first.has_value() && second.has_value());
        if (!first || !second) {
            return;
        }
        expect(same_start(*first, *second));
        expect(first->pieces.size() >= 4U);
        expect(first->pieces[0].template_id == "minecraft:end_city/base_floor");
        expect(first->pieces[0].template_origin.x == origin.x);
        expect(first->pieces[0].template_origin.y == origin.y);
        expect(first->pieces[0].template_origin.z == origin.z);
        expect(first->pieces[1].template_id ==
               "minecraft:end_city/second_floor_1");
        expect(!first->pieces[1].overwrite);
        expect(first->pieces[3].template_id == "minecraft:end_city/third_roof");
        expect(first->pieces[3].overwrite);
        expect(same_bounds(first->bounds, union_of(first->pieces)));
        auto turned_random =
            worldgen_random{std::make_unique<legacy_random_source>(7)};
        const auto turned = end_city_start(
            *manager, origin, template_rotation::clockwise_90, turned_random);
        expect(turned.has_value());
        if (turned) {
            expect(turned->pieces[0].template_rotation ==
                   static_cast<jint>(template_rotation::clockwise_90));
        }
        std::filesystem::remove_all(directory);
    };

    "structure sets dispatch to the fortress and the end city"_test = [] {
        const auto directory =
            std::filesystem::path{core::io::temp_directory()} /
            "refrain-end-city-dispatch";
        std::filesystem::remove_all(directory);
        expect(make_end_city_templates(directory));
        block_state_registry states;
        const auto manager = make_end_city_manager(directory, states);

        structure_data_set data;
        data.structures.emplace(
            "minecraft:fortress",
            structure_document{.id = "minecraft:fortress",
                               .type = "minecraft:fortress",
                               .step = "underground_decoration",
                               .biome_ids = {"minecraft:nether_wastes"}});
        data.structures.emplace(
            "minecraft:end_city",
            structure_document{.id = "minecraft:end_city",
                               .type = "minecraft:end_city",
                               .step = "surface_structures",
                               .biome_ids = {"minecraft:end_highlands"}});
        const auto random_spread =
            random_spread_placement{.spacing = 1,
                                    .separation = 0,
                                    .salt = 0,
                                    .spread = spread_type::linear,
                                    .frequency = 1.0F};
        data.sets.emplace(
            "minecraft:fortresses",
            structure_set{.id = "minecraft:fortresses",
                          .structures = {{.structure = "minecraft:fortress",
                                          .weight = 1}},
                          .random_spread = random_spread});
        data.sets.emplace(
            "minecraft:end_cities",
            structure_set{.id = "minecraft:end_cities",
                          .structures = {{.structure = "minecraft:end_city",
                                          .weight = 1}},
                          .random_spread = random_spread});

        const auto context_for = [&](std::string_view biome) {
            return structure_start_context{
                .seed = 12345,
                .data = &data,
                .min_y = 0,
                .world_height = 256,
                .sea_level = 32,
                .fixed_biome = true,
                .biome_at = [](jint, jint, jint) { return std::uint16_t{0}; },
                .biome_name = [biome](std::uint16_t) { return biome; },
                .height_at = [](jint, jint, heightmap_kind) -> jint {
                    return 61;
                },
                .templates = manager.get(),
            };
        };
        const auto fortress_starts = generate_structure_starts(
            chunk_pos{.x = 0, .z = 0}, context_for("minecraft:nether_wastes"));
        expect(fortress_starts.size() == 1U);
        if (!fortress_starts.empty()) {
            expect(fortress_starts.front().structure == "minecraft:fortress");
            expect(fortress_starts.front().step == "underground_decoration");
        }
        const auto end_starts = generate_structure_starts(
            chunk_pos{.x = 0, .z = 0}, context_for("minecraft:end_highlands"));
        expect(end_starts.size() == 1U);
        if (!end_starts.empty()) {
            expect(end_starts.front().structure == "minecraft:end_city");
            expect(end_starts.front().step == "surface_structures");
            expect(end_starts.front().pieces.front().template_origin.y == 60);
        }
        std::filesystem::remove_all(directory);
    };

    "an end city chest marker seeds the chest below it"_test = [] {
        const auto directory =
            std::filesystem::path{core::io::temp_directory()} /
            "refrain-end-city-marker";
        std::filesystem::remove_all(directory);
        block_state_registry states;
        (void)states.intern("minecraft:air", {.air = true});
        auto chest = states.intern(
            "minecraft:chest[facing=north,type=single,waterlogged=false]",
            {.air = false, .blocks_motion = true});
        auto marker_state =
            states.intern("minecraft:structure_block[mode=data]",
                          {.air = false, .blocks_motion = true});
        expect(chest.has_value() && marker_state.has_value());
        if (!chest || !marker_state) {
            return;
        }
        auto palette = data::nbt::make_list(data::nbt::tag_type::compound);
        (void)palette.push(data::nbt::value{palette_entry("minecraft:chest")});
        (void)palette.push(
            data::nbt::value{palette_entry("minecraft:structure_block")});
        data::nbt::compound marker_nbt;
        marker_nbt["id"] =
            data::nbt::value{std::string{"minecraft:structure_block"}};
        marker_nbt["mode"] = data::nbt::value{std::string{"DATA"}};
        marker_nbt["metadata"] = data::nbt::value{std::string{"Chest"}};
        auto blocks = data::nbt::make_list(data::nbt::tag_type::compound);
        (void)blocks.push(data::nbt::value{block_entry({0, 0, 0}, 0)});
        (void)blocks.push(
            data::nbt::value{block_entry({0, 1, 0}, 1, marker_nbt)});
        data::nbt::compound root_tag;
        root_tag["size"] = ints({1, 2, 1});
        root_tag["palette"] = std::move(palette);
        root_tag["blocks"] = std::move(blocks);
        const auto path =
            directory / "minecraft" / "structure" / "end_city" / "marker.nbt";
        std::filesystem::create_directories(path.parent_path());
        expect(data::nbt::write_file(path.string(), data::nbt::value{root_tag},
                                     "", data::nbt::compression::gzip)
                   .has_value());
        structure_template_manager manager{directory.string(), states};

        proto_chunk chunk{states, 0, 0, 0, 256};
        chunk_region region{chunk, 0};
        const auto resolve = [&](std::string_view key) {
            if (const auto found = states.find(key); found) {
                return *found;
            }
            return states.intern(key, {.air = false, .blocks_motion = true})
                .value_or(no_block_state);
        };
        const structure_place_context context{
            .region = &region,
            .chunk = &chunk,
            .states = &states,
            .state = resolve,
            .templates = &manager,
            .registry = &states,
        };
        structure_piece_record piece{
            .type = "minecraft:end_city",
            .min_x = 0,
            .min_y = 0,
            .min_z = 0,
            .max_x = 0,
            .max_y = 1,
            .max_z = 0,
            .template_id = "minecraft:end_city/marker",
            .template_x = 0,
            .template_y = 0,
            .template_z = 0,
            .template_rotation = 0,
            .overwrite = true,
        };
        auto random =
            worldgen_random{std::make_unique<legacy_random_source>(11)};
        place_structure_piece(context, piece, random);
        expect(state_block_name(states.name(chunk.state_at(0, 0, 0))) ==
               "minecraft:chest");
        const auto *entity =
            chunk.block_entity_at(block_pos{.x = 0, .y = 0, .z = 0});
        expect(entity != nullptr);
        if (entity != nullptr) {
            const auto table = entity->find("LootTable");
            expect(table != entity->end());
            if (table != entity->end()) {
                expect(table->second.as_string().value_or("") ==
                       "minecraft:chests/end_city_treasure");
            }
            expect(entity->find("LootTableSeed") != entity->end());
        }
        std::filesystem::remove_all(directory);
    };
};

suite<"worldgen/dim-structures-oracle"> dim_structure_oracle_tests = [] {
    "the fortress piece list matches the reference start"_test = [] {
        const char *region = std::getenv("REFRAIN_VANILLA_FORTRESS_REGION");
        const auto chunk = env_chunk("REFRAIN_VANILLA_FORTRESS_CHUNK_X",
                                     "REFRAIN_VANILLA_FORTRESS_CHUNK_Z");
        if (region == nullptr || !chunk.has_value()) {
            return;
        }
        const auto document = read_chunk(region, chunk->x, chunk->z);
        expect(document.has_value());
        if (!document) {
            return;
        }
        const auto *structures = child(document->as_compound(), "structures");
        const auto *starts = structures == nullptr
                                 ? nullptr
                                 : child(structures->as_compound(), "starts");
        const auto *starts_compound =
            starts == nullptr ? nullptr : starts->as_compound();
        expect(starts_compound != nullptr);
        if (starts_compound == nullptr) {
            return;
        }
        const auto found = starts_compound->find("minecraft:fortress");
        expect(found != starts_compound->end());
        if (found == starts_compound->end()) {
            return;
        }
        const auto *children = child(found->second.as_compound(), "Children");
        const auto *child_list =
            children == nullptr ? nullptr : children->as_list();
        expect(child_list != nullptr);
        if (child_list == nullptr) {
            return;
        }
        const auto start = fortress_start(*chunk, env_seed());
        expect(start.structure == "minecraft:fortress");
        expect(start.pieces.size() == child_list->items.size());
        if (start.pieces.size() != child_list->items.size()) {
            return;
        }
        for (std::size_t index = 0; index < start.pieces.size(); ++index) {
            const auto &piece = start.pieces[index];
            const auto &entry = child_list->items[index];
            const auto expected = piece_bounds(entry);
            expect(expected.has_value());
            if (!expected) {
                continue;
            }
            expect(same_bounds(piece.bounds, *expected));
            const auto *orientation = child(entry.as_compound(), "O");
            expect(orientation != nullptr);
            if (orientation != nullptr) {
                expect(orientation->as_int().value_or(-1) == piece.orientation);
            }
            if (!piece.fortress.has_value()) {
                expect(false);
                continue;
            }
            const auto *id = child(entry.as_compound(), "id");
            expect(id != nullptr);
            if (id != nullptr) {
                expect(id->as_string().value_or("") ==
                       fortress_piece_id(piece.fortress->kind));
            }
            if (piece.fortress->kind ==
                fortress_piece_kind::bridge_end_filler) {
                const auto *seed = child(entry.as_compound(), "Seed");
                expect(seed != nullptr);
                if (seed != nullptr) {
                    const auto value = seed->as_int();
                    expect(value.has_value());
                    if (value.has_value()) {
                        expect(static_cast<std::int32_t>(
                                   piece.fortress->self_seed) == *value);
                    }
                }
            }
            if (piece.fortress->kind ==
                    fortress_piece_kind::castle_small_corridor_left_turn ||
                piece.fortress->kind ==
                    fortress_piece_kind::castle_small_corridor_right_turn) {
                // A saved reference piece clears this flag once its chest was
                // placed, so only a set flag is comparable: it proves the
                // constructor drew a chest.
                const auto *chest = child(entry.as_compound(), "Chest");
                if (chest != nullptr && chest->as_byte().value_or(0) != 0) {
                    expect(piece.fortress->chest);
                }
            }
        }
    };

    "the end city piece list matches the reference start"_test = [] {
        const char *region = std::getenv("REFRAIN_VANILLA_END_CITY_REGION");
        const char *templates = std::getenv("REFRAIN_VANILLA_STRUCTURES");
        const auto chunk = env_chunk("REFRAIN_VANILLA_END_CITY_CHUNK_X",
                                     "REFRAIN_VANILLA_END_CITY_CHUNK_Z");
        if (region == nullptr || templates == nullptr || !chunk.has_value()) {
            return;
        }
        const auto document = read_chunk(region, chunk->x, chunk->z);
        expect(document.has_value());
        if (!document) {
            return;
        }
        const auto *structures = child(document->as_compound(), "structures");
        const auto *starts = structures == nullptr
                                 ? nullptr
                                 : child(structures->as_compound(), "starts");
        const auto *starts_compound =
            starts == nullptr ? nullptr : starts->as_compound();
        expect(starts_compound != nullptr);
        if (starts_compound == nullptr) {
            return;
        }
        const auto found = starts_compound->find("minecraft:end_city");
        expect(found != starts_compound->end());
        if (found == starts_compound->end()) {
            return;
        }
        const auto *children = child(found->second.as_compound(), "Children");
        const auto *child_list =
            children == nullptr ? nullptr : children->as_list();
        expect(child_list != nullptr);
        if (child_list == nullptr || child_list->items.empty()) {
            return;
        }
        const auto &first = child_list->items.front();
        const auto *tx = child(first.as_compound(), "TPX");
        const auto *ty = child(first.as_compound(), "TPY");
        const auto *tz = child(first.as_compound(), "TPZ");
        const auto *rotation_value = child(first.as_compound(), "Rot");
        expect(tx != nullptr && ty != nullptr && tz != nullptr &&
               rotation_value != nullptr);
        if (tx == nullptr || ty == nullptr || tz == nullptr ||
            rotation_value == nullptr) {
            return;
        }
        const auto origin = block_pos{.x = tx->as_int().value_or(0),
                                      .y = ty->as_int().value_or(0),
                                      .z = tz->as_int().value_or(0)};
        const auto rotation_text = rotation_value->as_string();
        expect(rotation_text.has_value());
        if (!rotation_text) {
            return;
        }
        block_state_registry states;
        (void)states.intern("minecraft:air", {.air = true});
        structure_template_manager manager{templates, states};
        auto random =
            worldgen_random{std::make_unique<legacy_random_source>(0)};
        random.set_large_feature_seed(env_seed(), chunk->x, chunk->z);
        const auto rotation_index = random.next_int(4);
        const auto rotation = static_cast<template_rotation>(rotation_index);
        expect(rotation_name(rotation) == *rotation_text);
        const auto start = end_city_start(manager, origin, rotation, random);
        expect(start.has_value());
        if (!start) {
            return;
        }
        expect(start->pieces.size() == child_list->items.size());
        if (start->pieces.size() != child_list->items.size()) {
            return;
        }
        for (std::size_t index = 0; index < start->pieces.size(); ++index) {
            const auto &piece = start->pieces[index];
            const auto &entry = child_list->items[index];
            const auto expected = piece_bounds(entry);
            expect(expected.has_value());
            if (!expected) {
                continue;
            }
            expect(same_bounds(piece.bounds, *expected));
            const auto *name = child(entry.as_compound(), "Template");
            expect(name != nullptr);
            if (name != nullptr) {
                expect(piece.template_id ==
                       "minecraft:end_city/" +
                           std::string{name->as_string().value_or("")});
            }
            const auto *rot = child(entry.as_compound(), "Rot");
            expect(rot != nullptr);
            if (rot != nullptr) {
                expect(rotation_name(static_cast<template_rotation>(
                           piece.template_rotation)) ==
                       rot->as_string().value_or(""));
            }
            const auto *overwrite = child(entry.as_compound(), "OW");
            if (overwrite != nullptr) {
                expect((overwrite->as_byte().value_or(1) != 0) ==
                       piece.overwrite);
            }
            const auto *template_x = child(entry.as_compound(), "TPX");
            const auto *template_y = child(entry.as_compound(), "TPY");
            const auto *template_z = child(entry.as_compound(), "TPZ");
            if (template_x != nullptr && template_y != nullptr &&
                template_z != nullptr) {
                expect(piece.template_origin.x ==
                       template_x->as_int().value_or(0));
                expect(piece.template_origin.y ==
                       template_y->as_int().value_or(0));
                expect(piece.template_origin.z ==
                       template_z->as_int().value_or(0));
            }
        }
    };
};
