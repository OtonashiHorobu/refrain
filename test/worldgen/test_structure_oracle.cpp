// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/nbt.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/structure/generation.hpp>
#include <refrain/worldgen/structure/stage.hpp>
#include <refrain/worldgen/structure/structure_data.hpp>
#include <refrain/worldgen/structure/template.hpp>

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using refrain::worldgen::chunk_pos;
using refrain::worldgen::jint;
using refrain::worldgen::jlong;
using refrain::worldgen::structure::selected_structure;
using refrain::worldgen::structure::spread_type;

auto child(const refrain::data::nbt::compound *entries, std::string_view key)
    -> const refrain::data::nbt::value * {
    if (entries == nullptr) {
        return nullptr;
    }
    const auto found = entries->find(key);
    return found == entries->end() ? nullptr : &found->second;
}

auto read_chunk(std::string_view path, jint chunk_x, jint chunk_z)
    -> std::optional<refrain::data::nbt::value> {
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
    const auto payload =
        std::span{bytes.data() + offset * 4096 + 5, length - 1};
    auto parsed = refrain::data::nbt::read(
        payload, refrain::data::nbt::compression::zlib);
    if (!parsed) {
        return std::nullopt;
    }
    return std::move(*parsed);
}

} // namespace

using namespace boost::ut;

suite<"worldgen/structure-oracle"> structure_oracle_tests = [] {
    "buried starts and references match the reference chunk"_test = [] {
        if (std::getenv("REFRAIN_VANILLA_STRUCTURE_REAL") != nullptr) {
            return;
        }
        const char *region = std::getenv("REFRAIN_VANILLA_STRUCTURE_REGION");
        if (region == nullptr) {
            return;
        }
        jint chunk_x = 6;
        jint chunk_z = -2;
        if (const char *value =
                std::getenv("REFRAIN_VANILLA_STRUCTURE_CHUNK_X");
            value != nullptr) {
            chunk_x = std::atoi(value);
        }
        if (const char *value =
                std::getenv("REFRAIN_VANILLA_STRUCTURE_CHUNK_Z");
            value != nullptr) {
            chunk_z = std::atoi(value);
        }
        jlong seed = 12345;
        if (const char *value = std::getenv("REFRAIN_VANILLA_ORACLE_SEED");
            value != nullptr) {
            seed = std::strtoll(value, nullptr, 10);
        }
        const auto root = read_chunk(region, chunk_x, chunk_z);
        expect(root.has_value()) << "cannot read reference chunk";
        if (!root) {
            return;
        }
        const auto *structures = child(root->as_compound(), "structures");
        expect(structures != nullptr);
        if (structures == nullptr) {
            return;
        }
        const auto *starts = child(structures->as_compound(), "starts");
        const auto *references = child(structures->as_compound(), "References");
        expect(starts != nullptr && references != nullptr);
        if (starts == nullptr || references == nullptr) {
            return;
        }
        expect(!starts->as_compound()->empty());
        expect(!references->as_compound()->empty());

        const std::vector<selected_structure> selections{
            {.id = "minecraft:buried_treasure",
             .placement = {.spacing = 1,
                           .separation = 0,
                           .salt = 0,
                           .spread = spread_type::linear,
                           .frequency = 1.0F}},
            {.id = "oracle:buried",
             .placement = {.spacing = 1,
                           .separation = 0,
                           .salt = 0,
                           .spread = spread_type::linear,
                           .frequency = 1.0F}},
        };
        const auto generated =
            refrain::worldgen::structure::generate_buried_starts(
                seed, chunk_pos{.x = chunk_x, .z = chunk_z}, selections, 8);

        std::map<std::string, jint> expected_references;
        for (const auto &[id, value] : *references->as_compound()) {
            const auto longs = value.as_long_array();
            expect(longs.has_value() && longs->size() == 1U);
            if (longs && !longs->empty()) {
                expected_references[id] = static_cast<jint>((*longs)[0]);
            }
        }
        expect(!expected_references.empty());
        for (const auto &[id, value] : *starts->as_compound()) {
            const auto *chunk = child(value.as_compound(), "ChunkX");
            const auto *chunk_z_value = child(value.as_compound(), "ChunkZ");
            expect(chunk != nullptr && chunk_z_value != nullptr);
            if (chunk == nullptr || chunk_z_value == nullptr) {
                continue;
            }
            const auto expected_x = chunk->as_int();
            const auto expected_z = chunk_z_value->as_int();
            expect(expected_x.has_value() && expected_z.has_value());
            if (!expected_x || !expected_z) {
                continue;
            }
            bool found = false;
            const refrain::worldgen::structure::structure_start *matched =
                nullptr;
            for (const auto &start : generated.starts) {
                if (start.structure == id && start.chunk.x == *expected_x &&
                    start.chunk.z == *expected_z) {
                    found = true;
                    matched = &start;
                }
            }
            expect(found) << id << " start missing";
            if (matched == nullptr) {
                continue;
            }
            const auto *children = child(value.as_compound(), "Children");
            const auto *child_list =
                children == nullptr ? nullptr : children->as_list();
            expect(child_list != nullptr && child_list->items.size() == 1U);
            if (child_list == nullptr || child_list->items.empty()) {
                continue;
            }
            const auto *bb = child(child_list->items[0].as_compound(), "BB");
            const auto values =
                bb == nullptr ? std::nullopt : bb->as_int_array();
            expect(values.has_value() && values->size() == 6U);
            if (values && values->size() == 6U) {
                expect(matched->bounds.min_x == (*values)[0]);
                expect(matched->bounds.min_y == (*values)[1]);
                expect(matched->bounds.min_z == (*values)[2]);
                expect(matched->bounds.max_x == (*values)[3]);
                expect(matched->bounds.max_y == (*values)[4]);
                expect(matched->bounds.max_z == (*values)[5]);
            }
        }

        std::map<std::string, jint> actual_references;
        for (const auto &reference : generated.references) {
            actual_references[reference.structure] += reference.count;
        }
        for (const auto &[id, count] : expected_references) {
            (void)count;
            expect(actual_references.contains(id))
                << id << " reference missing";
        }
    };

    "real structure data reproduces the buried start"_test = [] {
        if (std::getenv("REFRAIN_VANILLA_STRUCTURE_REAL") == nullptr) {
            return;
        }
        const char *region = std::getenv("REFRAIN_VANILLA_STRUCTURE_REGION");
        const char *root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        if (region == nullptr || root == nullptr) {
            return;
        }
        jint chunk_x = 3;
        jint chunk_z = 13;
        if (const char *value =
                std::getenv("REFRAIN_VANILLA_STRUCTURE_CHUNK_X");
            value != nullptr) {
            chunk_x = std::atoi(value);
        }
        if (const char *value =
                std::getenv("REFRAIN_VANILLA_STRUCTURE_CHUNK_Z");
            value != nullptr) {
            chunk_z = std::atoi(value);
        }
        jlong seed = 5086;
        if (const char *value = std::getenv("REFRAIN_VANILLA_ORACLE_SEED");
            value != nullptr) {
            seed = std::strtoll(value, nullptr, 10);
        }
        const auto data =
            refrain::worldgen::structure::load_structure_data(root);
        expect(data.has_value()) << "cannot load real structure data";
        if (!data) {
            return;
        }
        const refrain::worldgen::structure::structure_start_context context{
            .seed = seed,
            .data = &*data,
            .min_y = -64,
            .world_height = 384,
            .sea_level = 63,
            .biome_at = [](jint, jint, jint) { return std::uint16_t{0}; },
            .biome_name = [](std::uint16_t) -> std::string_view {
                return "minecraft:beach";
            },
            .height_at =
                [](jint, jint, refrain::worldgen::heightmap_kind) {
                    return jint{90};
                },
        };
        const auto generated =
            refrain::worldgen::structure::generate_structure_starts(
                chunk_pos{.x = chunk_x, .z = chunk_z}, context);
        const auto area = refrain::worldgen::structure::generate_structure_area(
            chunk_pos{.x = chunk_x, .z = chunk_z}, context, 8);
        jint buried_references = 0;
        for (const auto &reference : area.references) {
            if (reference.structure == "minecraft:buried_treasure") {
                buried_references += reference.count;
            }
        }
        expect(generated.size() == 1U);
        if (generated.empty()) {
            return;
        }
        expect(generated.front().structure == "minecraft:buried_treasure");
        expect(generated.front().chunk.x == chunk_x);
        expect(generated.front().chunk.z == chunk_z);
        expect(generated.front().pieces.size() == 1U);
        if (!generated.front().pieces.empty()) {
            expect(generated.front().pieces.front().type == "minecraft:btp");
            expect(generated.front().pieces.front().orientation == -1);
        }

        const auto reference = read_chunk(region, chunk_x, chunk_z);
        expect(reference.has_value());
        if (!reference) {
            return;
        }
        const auto *structures = child(reference->as_compound(), "structures");
        const auto *starts = structures == nullptr
                                 ? nullptr
                                 : child(structures->as_compound(), "starts");
        expect(starts != nullptr);
        if (starts == nullptr) {
            return;
        }
        const auto *buried =
            child(starts->as_compound(), "minecraft:buried_treasure");
        expect(buried != nullptr);
        const auto *references = child(structures->as_compound(), "References");
        if (references == nullptr) {
            return;
        }
        const auto *buried_reference =
            child(references->as_compound(), "minecraft:buried_treasure");
        if (buried_reference == nullptr) {
            return;
        }
        if (buried_reference != nullptr) {
            const auto packed = buried_reference->as_long_array();
            expect(packed.has_value() && packed->size() == 1U);
            if (packed && packed->size() == 1U) {
                expect(buried_references == 1);
                expect((static_cast<std::uint64_t>((*packed)[0]) >> 32U) ==
                       static_cast<std::uint64_t>(chunk_z));
                expect((static_cast<std::uint64_t>((*packed)[0]) &
                        0xFFFFFFFFU) == static_cast<std::uint64_t>(chunk_x));
            }
        }
        if (buried == nullptr) {
            return;
        }
        const auto *children = child(buried->as_compound(), "Children");
        expect(children != nullptr && children->as_list() != nullptr);
        if (children == nullptr || children->as_list() == nullptr ||
            children->as_list()->items.empty()) {
            return;
        }
        const auto *bb =
            child(children->as_list()->items[0].as_compound(), "BB");
        const auto values = bb == nullptr ? std::nullopt : bb->as_int_array();
        expect(values.has_value() && values->size() == 6U);
        if (values && values->size() == 6U && (*values)[1] == 90) {
            // A chunk that only reached `structure_starts` keeps the start
            // box; a full chunk has already run the piece and moved it.
            expect(generated.front().bounds.min_x == (*values)[0]);
            expect(generated.front().bounds.min_y == (*values)[1]);
            expect(generated.front().bounds.min_z == (*values)[2]);
            expect(generated.front().bounds.max_x == (*values)[3]);
            expect(generated.front().bounds.max_y == (*values)[4]);
            expect(generated.front().bounds.max_z == (*values)[5]);
        }
    };

    "a real igloo start reproduces its template pieces"_test = [] {
        if (std::getenv("REFRAIN_VANILLA_IGLOO_REAL") == nullptr) {
            return;
        }
        const char *region = std::getenv("REFRAIN_VANILLA_IGLOO_REGION");
        const char *root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        const char *templates_root = std::getenv("REFRAIN_VANILLA_STRUCTURES");
        if (region == nullptr || root == nullptr || templates_root == nullptr) {
            return;
        }
        jint chunk_x = 0;
        jint chunk_z = 10;
        if (const char *value = std::getenv("REFRAIN_VANILLA_IGLOO_CHUNK_X");
            value != nullptr) {
            chunk_x = std::atoi(value);
        }
        if (const char *value = std::getenv("REFRAIN_VANILLA_IGLOO_CHUNK_Z");
            value != nullptr) {
            chunk_z = std::atoi(value);
        }
        jlong seed = 1;
        if (const char *value = std::getenv("REFRAIN_VANILLA_IGLOO_SEED");
            value != nullptr) {
            seed = std::strtoll(value, nullptr, 10);
        }
        const auto data =
            refrain::worldgen::structure::load_structure_data(root);
        expect(data.has_value()) << "cannot load real structure data";
        if (!data) {
            return;
        }
        refrain::worldgen::block_state_registry registry;
        refrain::worldgen::structure::structure_template_manager manager{
            templates_root, registry};
        const refrain::worldgen::structure::structure_start_context context{
            .seed = seed,
            .data = &*data,
            .min_y = -64,
            .world_height = 384,
            .sea_level = 63,
            .biome_at = [](jint, jint, jint) { return std::uint16_t{0}; },
            .biome_name = [](std::uint16_t) -> std::string_view {
                return "minecraft:snowy_taiga";
            },
            .height_at =
                [](jint, jint, refrain::worldgen::heightmap_kind) {
                    return jint{90};
                },
            .templates = &manager,
        };
        const auto generated =
            refrain::worldgen::structure::generate_structure_starts(
                chunk_pos{.x = chunk_x, .z = chunk_z}, context);
        const refrain::worldgen::structure::structure_start *igloo = nullptr;
        for (const auto &start : generated) {
            if (start.structure == "minecraft:igloo") {
                igloo = &start;
            }
        }
        expect(igloo != nullptr) << "igloo start missing";
        if (igloo == nullptr) {
            return;
        }
        expect(igloo->pieces.size() == 1U);
        if (igloo->pieces.empty()) {
            return;
        }
        const auto &piece = igloo->pieces.front();
        expect(piece.template_id == "minecraft:igloo/top");
        expect(piece.template_origin.x == 0);
        expect(piece.template_origin.y == 90);
        expect(piece.template_origin.z == 160);
        expect(piece.template_rotation == 2);
        expect(piece.bounds.min_x == 0);
        expect(piece.bounds.min_y == 90);
        expect(piece.bounds.min_z == 163);
        expect(piece.bounds.max_x == 6);
        expect(piece.bounds.max_y == 94);
        expect(piece.bounds.max_z == 170);

        const auto reference = read_chunk(region, chunk_x, chunk_z);
        expect(reference.has_value());
        if (!reference) {
            return;
        }
        const auto *structures = child(reference->as_compound(), "structures");
        const auto *starts = structures == nullptr
                                 ? nullptr
                                 : child(structures->as_compound(), "starts");
        const auto *igloo_start =
            starts == nullptr ? nullptr
                              : child(starts->as_compound(), "minecraft:igloo");
        expect(igloo_start != nullptr);
        if (igloo_start == nullptr) {
            return;
        }
        const auto *children = child(igloo_start->as_compound(), "Children");
        const auto *pieces_list =
            children == nullptr ? nullptr : children->as_list();
        expect(pieces_list != nullptr && pieces_list->items.size() == 1U);
        if (pieces_list == nullptr || pieces_list->items.empty()) {
            return;
        }
        const auto *expected = pieces_list->items.front().as_compound();
        const auto *template_id = child(expected, "Template");
        const auto *template_x = child(expected, "TPX");
        const auto *template_y = child(expected, "TPY");
        const auto *template_z = child(expected, "TPZ");
        expect(template_id != nullptr && template_x != nullptr &&
               template_y != nullptr && template_z != nullptr);
        if (template_id == nullptr || template_x == nullptr ||
            template_y == nullptr || template_z == nullptr) {
            return;
        }
        expect(template_id->as_string() == piece.template_id);
        expect(template_x->as_int() == piece.template_origin.x);
        expect(template_y->as_int() == piece.template_origin.y);
        expect(template_z->as_int() == piece.template_origin.z);
        const auto *bb = child(expected, "BB");
        const auto values = bb == nullptr ? std::nullopt : bb->as_int_array();
        expect(values.has_value() && values->size() == 6U);
        if (values && values->size() == 6U) {
            // A full chunk stores the piece box after postProcess moved the
            // template to the entrance height; the absolute y of that move
            // depends on the structure-time heightmap, which is not saved.
            // Compare the horizontal extent and the box height instead.
            expect(piece.bounds.min_x == (*values)[0]);
            expect(piece.bounds.min_z == (*values)[2]);
            expect(piece.bounds.max_x == (*values)[3]);
            expect(piece.bounds.max_z == (*values)[5]);
            const auto expected_height = (*values)[4] - (*values)[1];
            const auto piece_height = piece.bounds.max_y - piece.bounds.min_y;
            expect(expected_height == piece_height);
            if (expected_height == piece_height && expected_height == 0) {
                expect(piece.bounds.min_y == (*values)[1]);
            }
        }
    };

    "a real swamp hut start reproduces its piece geometry"_test = [] {
        if (std::getenv("REFRAIN_VANILLA_SWAMP_REAL") == nullptr) {
            return;
        }
        const char *region = std::getenv("REFRAIN_VANILLA_SWAMP_REGION");
        const char *root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        if (region == nullptr || root == nullptr) {
            return;
        }
        jint chunk_x = 9;
        jint chunk_z = 2;
        jlong seed = 1;
        if (const char *value = std::getenv("REFRAIN_VANILLA_SWAMP_CHUNK_X");
            value != nullptr) {
            chunk_x = std::atoi(value);
        }
        if (const char *value = std::getenv("REFRAIN_VANILLA_SWAMP_CHUNK_Z");
            value != nullptr) {
            chunk_z = std::atoi(value);
        }
        if (const char *value = std::getenv("REFRAIN_VANILLA_SWAMP_SEED");
            value != nullptr) {
            seed = std::strtoll(value, nullptr, 10);
        }
        const auto data =
            refrain::worldgen::structure::load_structure_data(root);
        expect(data.has_value());
        if (!data) {
            return;
        }
        const refrain::worldgen::structure::structure_start_context context{
            .seed = seed,
            .data = &*data,
            .min_y = -64,
            .world_height = 384,
            .sea_level = 63,
            .biome_at = [](jint, jint, jint) { return std::uint16_t{0}; },
            .biome_name = [](std::uint16_t) -> std::string_view {
                return "minecraft:swamp";
            },
            .height_at =
                [](jint, jint, refrain::worldgen::heightmap_kind) {
                    return jint{90};
                },
        };
        const auto generated =
            refrain::worldgen::structure::generate_structure_starts(
                chunk_pos{.x = chunk_x, .z = chunk_z}, context);
        const refrain::worldgen::structure::structure_start *hut = nullptr;
        for (const auto &start : generated) {
            if (start.structure == "minecraft:swamp_hut") {
                hut = &start;
            }
        }
        expect(hut != nullptr) << "swamp hut start missing";
        if (hut == nullptr) {
            return;
        }
        expect(hut->pieces.size() == 1U);
        if (hut->pieces.empty()) {
            return;
        }
        const auto &piece = hut->pieces.front();
        expect(piece.type == "minecraft:tesh");

        const auto reference = read_chunk(region, chunk_x, chunk_z);
        expect(reference.has_value());
        if (!reference) {
            return;
        }
        const auto *structures = child(reference->as_compound(), "structures");
        const auto *starts = structures == nullptr
                                 ? nullptr
                                 : child(structures->as_compound(), "starts");
        const auto *hut_start =
            starts == nullptr
                ? nullptr
                : child(starts->as_compound(), "minecraft:swamp_hut");
        expect(hut_start != nullptr);
        if (hut_start == nullptr) {
            return;
        }
        const auto *children = child(hut_start->as_compound(), "Children");
        const auto *pieces =
            children == nullptr ? nullptr : children->as_list();
        expect(pieces != nullptr && pieces->items.size() == 1U);
        if (pieces == nullptr || pieces->items.empty()) {
            return;
        }
        const auto *expected = pieces->items.front().as_compound();
        const auto *bb = child(expected, "BB");
        const auto values = bb == nullptr ? std::nullopt : bb->as_int_array();
        expect(values.has_value() && values->size() == 6U);
        if (!values || values->size() != 6U) {
            return;
        }
        const auto *orientation = child(expected, "O");
        const auto orientation_value =
            orientation == nullptr ? std::nullopt : orientation->as_int();
        expect(orientation_value.has_value());
        if (!orientation_value) {
            return;
        }
        expect(piece.orientation == *orientation_value);
        expect(piece.bounds.min_x == (*values)[0]);
        expect(piece.bounds.min_z == (*values)[2]);
        expect(piece.bounds.max_x == (*values)[3]);
        expect(piece.bounds.max_z == (*values)[5]);
        expect(piece.bounds.max_y - piece.bounds.min_y == 6);

        const auto *hpos = child(expected, "HPos");
        const auto hpos_value = hpos == nullptr ? std::nullopt : hpos->as_int();
        expect(hpos_value.has_value());
        if (!hpos_value) {
            return;
        }
        expect(piece.bounds.min_y + (*hpos_value - 64) == (*values)[1]);
        expect(piece.bounds.max_y + (*hpos_value - 64) == (*values)[4]);
    };

    "a real desert pyramid start reproduces its piece geometry"_test = [] {
        if (std::getenv("REFRAIN_VANILLA_PYRAMID_REAL") == nullptr) {
            return;
        }
        const char *region = std::getenv("REFRAIN_VANILLA_PYRAMID_REGION");
        const char *root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        if (region == nullptr || root == nullptr) {
            return;
        }
        jint chunk_x = 0;
        jint chunk_z = 10;
        jlong seed = 2;
        if (const char *value = std::getenv("REFRAIN_VANILLA_PYRAMID_CHUNK_X");
            value != nullptr) {
            chunk_x = std::atoi(value);
        }
        if (const char *value = std::getenv("REFRAIN_VANILLA_PYRAMID_CHUNK_Z");
            value != nullptr) {
            chunk_z = std::atoi(value);
        }
        if (const char *value = std::getenv("REFRAIN_VANILLA_PYRAMID_SEED");
            value != nullptr) {
            seed = std::strtoll(value, nullptr, 10);
        }
        const auto data =
            refrain::worldgen::structure::load_structure_data(root);
        expect(data.has_value());
        if (!data) {
            return;
        }
        const refrain::worldgen::structure::structure_start_context context{
            .seed = seed,
            .data = &*data,
            .min_y = -64,
            .world_height = 384,
            .sea_level = 63,
            .biome_at = [](jint, jint, jint) { return std::uint16_t{0}; },
            .biome_name = [](std::uint16_t) -> std::string_view {
                return "minecraft:desert";
            },
            .height_at =
                [](jint, jint, refrain::worldgen::heightmap_kind) {
                    return jint{90};
                },
        };
        const auto generated =
            refrain::worldgen::structure::generate_structure_starts(
                chunk_pos{.x = chunk_x, .z = chunk_z}, context);
        const refrain::worldgen::structure::structure_start *pyramid = nullptr;
        for (const auto &start : generated) {
            if (start.structure == "minecraft:desert_pyramid") {
                pyramid = &start;
            }
        }
        expect(pyramid != nullptr) << "pyramid start missing";
        if (pyramid == nullptr) {
            return;
        }
        expect(pyramid->pieces.size() == 1U);
        if (pyramid->pieces.empty()) {
            return;
        }
        const auto &piece = pyramid->pieces.front();
        expect(piece.type == "minecraft:tedp");
        expect(piece.bounds.max_x - piece.bounds.min_x == 20);
        expect(piece.bounds.max_z - piece.bounds.min_z == 20);
        expect(piece.bounds.max_y - piece.bounds.min_y == 14);

        const auto reference = read_chunk(region, chunk_x, chunk_z);
        expect(reference.has_value());
        if (!reference) {
            return;
        }
        const auto *structures = child(reference->as_compound(), "structures");
        const auto *starts = structures == nullptr
                                 ? nullptr
                                 : child(structures->as_compound(), "starts");
        const auto *pyramid_start =
            starts == nullptr
                ? nullptr
                : child(starts->as_compound(), "minecraft:desert_pyramid");
        expect(pyramid_start != nullptr);
        if (pyramid_start == nullptr) {
            return;
        }
        const auto *children = child(pyramid_start->as_compound(), "Children");
        const auto *pieces =
            children == nullptr ? nullptr : children->as_list();
        expect(pieces != nullptr && pieces->items.size() == 1U);
        if (pieces == nullptr || pieces->items.empty()) {
            return;
        }
        const auto *expected = pieces->items.front().as_compound();
        const auto *bb = child(expected, "BB");
        const auto values = bb == nullptr ? std::nullopt : bb->as_int_array();
        expect(values.has_value() && values->size() == 6U);
        if (!values || values->size() != 6U) {
            return;
        }
        const auto *orientation = child(expected, "O");
        const auto orientation_value =
            orientation == nullptr ? std::nullopt : orientation->as_int();
        expect(orientation_value.has_value());
        if (!orientation_value) {
            return;
        }
        expect(piece.orientation == *orientation_value);
        expect(piece.bounds.min_x == (*values)[0]);
        expect(piece.bounds.min_z == (*values)[2]);
        expect(piece.bounds.max_x == (*values)[3]);
        expect(piece.bounds.max_z == (*values)[5]);
        expect((*values)[4] - (*values)[1] == 14);
    };

    "a structure set selects the same chunks across a generated region"_test =
        [] {
            if (std::getenv("REFRAIN_VANILLA_IGLOO_GRID") == nullptr) {
                return;
            }
            const char *region = std::getenv("REFRAIN_VANILLA_IGLOO_REGION");
            const char *root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
            const char *templates_root =
                std::getenv("REFRAIN_VANILLA_STRUCTURES");
            if (region == nullptr || root == nullptr) {
                return;
            }
            std::string structure_id = "minecraft:igloo";
            if (const char *value =
                    std::getenv("REFRAIN_VANILLA_GRID_STRUCTURE");
                value != nullptr) {
                structure_id = value;
            }
            std::string biome_name = "minecraft:snowy_taiga";
            if (const char *value = std::getenv("REFRAIN_VANILLA_GRID_BIOME");
                value != nullptr) {
                biome_name = value;
            }
            jlong seed = 1;
            if (const char *value = std::getenv("REFRAIN_VANILLA_GRID_SEED");
                value != nullptr) {
                seed = std::strtoll(value, nullptr, 10);
            }
            const auto data =
                refrain::worldgen::structure::load_structure_data(root);
            expect(data.has_value());
            if (!data) {
                return;
            }
            refrain::worldgen::block_state_registry registry;
            std::optional<
                refrain::worldgen::structure::structure_template_manager>
                manager;
            if (templates_root != nullptr) {
                manager.emplace(templates_root, registry);
            }
            const refrain::worldgen::structure::structure_start_context context{
                .seed = seed,
                .data = &*data,
                .min_y = -64,
                .world_height = 384,
                .sea_level = 63,
                .biome_at = [](jint, jint, jint) { return std::uint16_t{0}; },
                .biome_name = [&biome_name](std::uint16_t) -> std::string_view {
                    return biome_name;
                },
                .height_at =
                    [](jint, jint, refrain::worldgen::heightmap_kind) {
                        return jint{90};
                    },
                .templates = manager ? &*manager : nullptr,
            };
            std::vector<refrain::worldgen::structure::structure_start> starts;
            for (jint chunk_x = -12; chunk_x <= 19; ++chunk_x) {
                for (jint chunk_z = -12; chunk_z <= 19; ++chunk_z) {
                    auto generated =
                        refrain::worldgen::structure::generate_structure_starts(
                            chunk_pos{.x = chunk_x, .z = chunk_z}, context);
                    for (auto &start : generated) {
                        starts.push_back(std::move(start));
                    }
                }
            }
            jint mismatches = 0;
            jint checked = 0;
            for (jint chunk_x = -4; chunk_x <= 11; ++chunk_x) {
                for (jint chunk_z = -4; chunk_z <= 11; ++chunk_z) {
                    const auto reference = read_chunk(region, chunk_x, chunk_z);
                    if (!reference) {
                        continue;
                    }
                    const auto *structures =
                        child(reference->as_compound(), "structures");
                    const auto *starts_value =
                        structures == nullptr
                            ? nullptr
                            : child(structures->as_compound(), "starts");
                    const auto *expected_start =
                        starts_value == nullptr
                            ? nullptr
                            : child(starts_value->as_compound(), structure_id);
                    const refrain::worldgen::structure::structure_start *mine =
                        nullptr;
                    for (const auto &start : starts) {
                        if (start.structure == structure_id &&
                            start.chunk.x == chunk_x &&
                            start.chunk.z == chunk_z) {
                            mine = &start;
                        }
                    }
                    const bool expected = expected_start != nullptr;
                    if (expected != (mine != nullptr)) {
                        ++mismatches;
                        if (mismatches < 6) {
                            std::fprintf(
                                stderr,
                                "%s presence mismatch chunk (%d,%d): "
                                "expected=%d ours=%d\n",
                                structure_id.c_str(), static_cast<int>(chunk_x),
                                static_cast<int>(chunk_z), expected ? 1 : 0,
                                mine != nullptr ? 1 : 0);
                        }
                        continue;
                    }
                    if (expected && mine != nullptr) {
                        const auto *children =
                            child(expected_start->as_compound(), "Children");
                        const auto *pieces =
                            children == nullptr ? nullptr : children->as_list();
                        if (pieces == nullptr || pieces->items.empty() ||
                            mine->pieces.empty()) {
                            ++mismatches;
                            continue;
                        }
                        const auto *expected_piece =
                            pieces->items.front().as_compound();
                        const auto &actual = mine->pieces.front();
                        const auto *bb = child(expected_piece, "BB");
                        const auto values =
                            bb == nullptr ? std::nullopt : bb->as_int_array();
                        const auto *orientation = child(expected_piece, "O");
                        const auto orientation_value =
                            orientation == nullptr ? std::nullopt
                                                   : orientation->as_int();
                        bool piece_mismatch = false;
                        if (!values || values->size() != 6U) {
                            piece_mismatch = true;
                        } else {
                            piece_mismatch =
                                actual.bounds.min_x != (*values)[0] ||
                                actual.bounds.min_z != (*values)[2] ||
                                actual.bounds.max_x != (*values)[3] ||
                                actual.bounds.max_z != (*values)[5];
                        }
                        if (!orientation_value ||
                            actual.orientation != *orientation_value) {
                            piece_mismatch = true;
                        }
                        const auto *template_id =
                            child(expected_piece, "Template");
                        if (template_id != nullptr) {
                            const auto *template_x =
                                child(expected_piece, "TPX");
                            const auto *template_y =
                                child(expected_piece, "TPY");
                            const auto *template_z =
                                child(expected_piece, "TPZ");
                            const auto template_text = template_id->as_string();
                            if (!template_text ||
                                *template_text != actual.template_id ||
                                !template_x || !template_y || !template_z ||
                                template_x->as_int() !=
                                    actual.template_origin.x ||
                                template_y->as_int() !=
                                    actual.template_origin.y ||
                                template_z->as_int() !=
                                    actual.template_origin.z) {
                                piece_mismatch = true;
                            }
                        }
                        if (piece_mismatch) {
                            ++mismatches;
                            if (mismatches < 6) {
                                std::fprintf(
                                    stderr, "%s piece mismatch chunk (%d,%d)\n",
                                    structure_id.c_str(),
                                    static_cast<int>(chunk_x),
                                    static_cast<int>(chunk_z));
                            }
                        }
                    }
                    const auto *references =
                        structures == nullptr
                            ? nullptr
                            : child(structures->as_compound(), "References");
                    const auto *structure_reference =
                        references == nullptr
                            ? nullptr
                            : child(references->as_compound(), structure_id);
                    jint expected_references = 0;
                    if (structure_reference != nullptr) {
                        const auto packed =
                            structure_reference->as_long_array();
                        if (!packed) {
                            ++mismatches;
                            continue;
                        }
                        expected_references = static_cast<jint>(packed->size());
                    }
                    jint actual_references = 0;
                    for (const auto &start : starts) {
                        if (start.structure == structure_id &&
                            start.bounds.intersects_chunk(
                                chunk_pos{.x = chunk_x, .z = chunk_z})) {
                            ++actual_references;
                        }
                    }
                    if (expected_references != actual_references) {
                        ++mismatches;
                        if (mismatches < 6) {
                            std::fprintf(stderr,
                                         "%s reference mismatch chunk (%d,%d): "
                                         "expected=%d ours=%d\n",
                                         structure_id.c_str(),
                                         static_cast<int>(chunk_x),
                                         static_cast<int>(chunk_z),
                                         static_cast<int>(expected_references),
                                         static_cast<int>(actual_references));
                        }
                    }
                    ++checked;
                }
            }
            std::fprintf(stderr, "%s grid checked %d chunks\n",
                         structure_id.c_str(), static_cast<int>(checked));
            expect(checked > 0);
            expect(mismatches == 0);
        };

    "a real basement igloo start reproduces every piece"_test = [] {
        if (std::getenv("REFRAIN_VANILLA_IGLOO_BASEMENT_REAL") == nullptr) {
            return;
        }
        const char *region =
            std::getenv("REFRAIN_VANILLA_IGLOO_BASEMENT_REGION");
        if (region == nullptr) {
            region = std::getenv("REFRAIN_VANILLA_IGLOO_REGION");
        }
        const char *root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        const char *templates_root = std::getenv("REFRAIN_VANILLA_STRUCTURES");
        if (region == nullptr || root == nullptr || templates_root == nullptr) {
            return;
        }
        const jint chunk_x = 9;
        const jint chunk_z = 2;
        const jlong seed = 3;
        refrain::worldgen::block_state_registry registry;
        refrain::worldgen::structure::structure_template_manager manager{
            templates_root, registry};
        const auto start = refrain::worldgen::structure::igloo_start(
            chunk_pos{.x = chunk_x, .z = chunk_z}, seed, manager);
        const auto reference = read_chunk(region, chunk_x, chunk_z);
        expect(reference.has_value());
        if (!reference) {
            return;
        }
        const auto *structures = child(reference->as_compound(), "structures");
        const auto *starts = structures == nullptr
                                 ? nullptr
                                 : child(structures->as_compound(), "starts");
        const auto *igloo_start =
            starts == nullptr ? nullptr
                              : child(starts->as_compound(), "minecraft:igloo");
        expect(igloo_start != nullptr);
        if (igloo_start == nullptr) {
            return;
        }
        const auto *children = child(igloo_start->as_compound(), "Children");
        const auto *pieces =
            children == nullptr ? nullptr : children->as_list();
        expect(pieces != nullptr);
        if (pieces == nullptr) {
            return;
        }
        expect(start.pieces.size() == pieces->items.size());
        if (start.pieces.size() != pieces->items.size()) {
            return;
        }
        for (std::size_t index = 0; index < pieces->items.size(); ++index) {
            const auto *expected = pieces->items[index].as_compound();
            const auto &actual = start.pieces[index];
            const auto *template_id = child(expected, "Template");
            const auto *template_x = child(expected, "TPX");
            const auto *template_y = child(expected, "TPY");
            const auto *template_z = child(expected, "TPZ");
            const auto *rotation = child(expected, "Rot");
            const auto *bb = child(expected, "BB");
            const auto values =
                bb == nullptr ? std::nullopt : bb->as_int_array();
            expect(template_id != nullptr && template_x != nullptr &&
                   template_y != nullptr && template_z != nullptr &&
                   rotation != nullptr);
            if (template_id == nullptr || template_x == nullptr ||
                template_y == nullptr || template_z == nullptr ||
                rotation == nullptr || !values || values->size() != 6U) {
                continue;
            }
            expect(template_id->as_string() == actual.template_id);
            expect(template_x->as_int() == actual.template_origin.x);
            expect(template_y->as_int() == actual.template_origin.y);
            expect(template_z->as_int() == actual.template_origin.z);
            expect(values->size() == 6U);
            if (values->size() == 6U) {
                expect(actual.bounds.min_x == (*values)[0]);
                expect(actual.bounds.min_z == (*values)[2]);
                expect(actual.bounds.max_x == (*values)[3]);
                expect(actual.bounds.max_z == (*values)[5]);
            }
            const auto rot_text = rotation->as_string();
            expect(rot_text.has_value());
            if (rot_text) {
                const jint expected_rotation =
                    *rot_text == "CLOCKWISE_180"         ? 2
                    : *rot_text == "CLOCKWISE_90"        ? 1
                    : *rot_text == "COUNTERCLOCKWISE_90" ? 3
                                                         : 0;
                expect(actual.template_rotation == expected_rotation);
            }
        }
    };

    "a beached shipwreck start matches the reference template and rotation"_test =
        [] {
            if (std::getenv("REFRAIN_VANILLA_WRECK_REAL") == nullptr) {
                return;
            }
            const char *region = std::getenv("REFRAIN_VANILLA_WRECK_REGION");
            const char *root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
            const char *templates_root =
                std::getenv("REFRAIN_VANILLA_STRUCTURES");
            if (region == nullptr || root == nullptr ||
                templates_root == nullptr) {
                return;
            }
            const jint chunk_x = 4;
            const jint chunk_z = 8;
            const jlong seed = 1;
            const auto data =
                refrain::worldgen::structure::load_structure_data(root);
            expect(data.has_value());
            if (!data) {
                return;
            }
            refrain::worldgen::block_state_registry registry;
            refrain::worldgen::structure::structure_template_manager manager{
                templates_root, registry};
            const refrain::worldgen::structure::structure_start_context context{
                .seed = seed,
                .data = &*data,
                .min_y = -64,
                .world_height = 384,
                .sea_level = 63,
                .biome_at = [](jint, jint, jint) { return std::uint16_t{0}; },
                .biome_name = [](std::uint16_t) -> std::string_view {
                    return "minecraft:beach";
                },
                .height_at =
                    [](jint, jint, refrain::worldgen::heightmap_kind) {
                        return jint{70};
                    },
                .templates = &manager,
            };
            const auto starts =
                refrain::worldgen::structure::generate_structure_starts(
                    chunk_pos{.x = chunk_x, .z = chunk_z}, context);
            const refrain::worldgen::structure::structure_start *wreck =
                nullptr;
            for (const auto &start : starts) {
                if (start.structure == "minecraft:shipwreck_beached") {
                    wreck = &start;
                }
            }
            expect(wreck != nullptr);
            if (wreck == nullptr) {
                return;
            }
            const auto reference = read_chunk(region, chunk_x, chunk_z);
            expect(reference.has_value());
            if (!reference) {
                return;
            }
            const auto *structures =
                child(reference->as_compound(), "structures");
            const auto *starts_value =
                structures == nullptr
                    ? nullptr
                    : child(structures->as_compound(), "starts");
            const auto *expected = starts_value == nullptr
                                       ? nullptr
                                       : child(starts_value->as_compound(),
                                               "minecraft:shipwreck_beached");
            expect(expected != nullptr);
            if (expected == nullptr) {
                return;
            }
            const auto *children = child(expected->as_compound(), "Children");
            const auto *pieces =
                children == nullptr ? nullptr : children->as_list();
            expect(pieces != nullptr && pieces->items.size() == 1U);
            if (pieces == nullptr || pieces->items.size() != 1U) {
                return;
            }
            const auto *piece = pieces->items.front().as_compound();
            expect(wreck->pieces.size() == 1U);
            if (wreck->pieces.empty()) {
                return;
            }
            const auto &actual = wreck->pieces.front();
            const auto *template_id = child(piece, "Template");
            const auto *rotation = child(piece, "Rot");
            const auto *template_x = child(piece, "TPX");
            const auto *template_z = child(piece, "TPZ");
            const auto *bb = child(piece, "BB");
            const auto values =
                bb == nullptr ? std::nullopt : bb->as_int_array();
            expect(template_id != nullptr && rotation != nullptr &&
                   template_x != nullptr && template_z != nullptr);
            if (template_id == nullptr || rotation == nullptr ||
                template_x == nullptr || template_z == nullptr) {
                return;
            }
            expect(template_id->as_string() == actual.template_id);
            expect(template_x->as_int() == actual.template_origin.x);
            expect(template_z->as_int() == actual.template_origin.z);
            const auto rot_text = rotation->as_string();
            expect(rot_text.has_value());
            if (rot_text) {
                const jint expected_rotation =
                    *rot_text == "CLOCKWISE_180"         ? 2
                    : *rot_text == "CLOCKWISE_90"        ? 1
                    : *rot_text == "COUNTERCLOCKWISE_90" ? 3
                                                         : 0;
                expect(actual.template_rotation == expected_rotation);
            }
            expect(values.has_value() && values->size() == 6U);
            if (values && values->size() == 6U) {
                expect(actual.bounds.min_x == (*values)[0]);
                expect(actual.bounds.min_z == (*values)[2]);
                expect(actual.bounds.max_x == (*values)[3]);
                expect(actual.bounds.max_z == (*values)[5]);
            }
        };

    "an ocean shipwreck start picks the ocean template list"_test = [] {
        if (std::getenv("REFRAIN_VANILLA_WRECK_OCEAN_REAL") == nullptr) {
            return;
        }
        const char *region = std::getenv("REFRAIN_VANILLA_WRECK_OCEAN_REGION");
        const char *root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        const char *templates_root = std::getenv("REFRAIN_VANILLA_STRUCTURES");
        if (region == nullptr || root == nullptr || templates_root == nullptr) {
            return;
        }
        const jint chunk_x = 4;
        const jint chunk_z = 8;
        const jlong seed = 1;
        const auto data =
            refrain::worldgen::structure::load_structure_data(root);
        expect(data.has_value());
        if (!data) {
            return;
        }
        refrain::worldgen::block_state_registry registry;
        refrain::worldgen::structure::structure_template_manager manager{
            templates_root, registry};
        const refrain::worldgen::structure::structure_start_context context{
            .seed = seed,
            .data = &*data,
            .min_y = -64,
            .world_height = 384,
            .sea_level = 63,
            .biome_at = [](jint, jint, jint) { return std::uint16_t{0}; },
            .biome_name = [](std::uint16_t) -> std::string_view {
                return "minecraft:ocean";
            },
            .height_at =
                [](jint, jint, refrain::worldgen::heightmap_kind) {
                    return jint{70};
                },
            .templates = &manager,
        };
        const auto starts =
            refrain::worldgen::structure::generate_structure_starts(
                chunk_pos{.x = chunk_x, .z = chunk_z}, context);
        const refrain::worldgen::structure::structure_start *wreck = nullptr;
        for (const auto &start : starts) {
            if (start.structure == "minecraft:shipwreck") {
                wreck = &start;
            }
        }
        expect(wreck != nullptr);
        if (wreck == nullptr) {
            return;
        }
        const auto reference = read_chunk(region, chunk_x, chunk_z);
        expect(reference.has_value());
        if (!reference) {
            return;
        }
        const auto *structures = child(reference->as_compound(), "structures");
        const auto *starts_value =
            structures == nullptr ? nullptr
                                  : child(structures->as_compound(), "starts");
        const auto *expected =
            starts_value == nullptr
                ? nullptr
                : child(starts_value->as_compound(), "minecraft:shipwreck");
        expect(expected != nullptr);
        if (expected == nullptr) {
            return;
        }
        const auto *children = child(expected->as_compound(), "Children");
        const auto *pieces =
            children == nullptr ? nullptr : children->as_list();
        expect(pieces != nullptr && pieces->items.size() == 1U);
        if (pieces == nullptr || pieces->items.size() != 1U ||
            wreck->pieces.size() != 1U) {
            return;
        }
        const auto *piece = pieces->items.front().as_compound();
        const auto &actual = wreck->pieces.front();
        const auto *template_id = child(piece, "Template");
        const auto *rotation = child(piece, "Rot");
        const auto *template_x = child(piece, "TPX");
        const auto *template_z = child(piece, "TPZ");
        const auto *bb = child(piece, "BB");
        const auto values = bb == nullptr ? std::nullopt : bb->as_int_array();
        expect(template_id != nullptr && rotation != nullptr &&
               template_x != nullptr && template_z != nullptr);
        if (template_id == nullptr || rotation == nullptr ||
            template_x == nullptr || template_z == nullptr) {
            return;
        }
        expect(template_id->as_string() == actual.template_id);
        expect(template_x->as_int() == actual.template_origin.x);
        expect(template_z->as_int() == actual.template_origin.z);
        const auto rot_text = rotation->as_string();
        expect(rot_text.has_value());
        if (rot_text) {
            const jint expected_rotation = *rot_text == "CLOCKWISE_180"  ? 2
                                           : *rot_text == "CLOCKWISE_90" ? 1
                                           : *rot_text == "COUNTERCLOCKWISE_90"
                                               ? 3
                                               : 0;
            expect(actual.template_rotation == expected_rotation);
        }
        expect(values.has_value() && values->size() == 6U);
        if (values && values->size() == 6U) {
            expect(actual.bounds.min_x == (*values)[0]);
            expect(actual.bounds.min_z == (*values)[2]);
            expect(actual.bounds.max_x == (*values)[3]);
            expect(actual.bounds.max_z == (*values)[5]);
        }
    };

    "a warm ocean ruin start picks the reference template and rotation"_test =
        [] {
            if (std::getenv("REFRAIN_VANILLA_RUIN_REAL") == nullptr) {
                return;
            }
            const char *region = std::getenv("REFRAIN_VANILLA_RUIN_REGION");
            const char *root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
            const char *templates_root =
                std::getenv("REFRAIN_VANILLA_STRUCTURES");
            if (region == nullptr || root == nullptr ||
                templates_root == nullptr) {
                return;
            }
            const jint chunk_x = 5;
            const jint chunk_z = 10;
            const jlong seed = 1;
            const auto data =
                refrain::worldgen::structure::load_structure_data(root);
            expect(data.has_value());
            if (!data) {
                return;
            }
            refrain::worldgen::block_state_registry registry;
            refrain::worldgen::structure::structure_template_manager manager{
                templates_root, registry};
            const refrain::worldgen::structure::structure_start_context context{
                .seed = seed,
                .data = &*data,
                .min_y = -64,
                .world_height = 384,
                .sea_level = 63,
                .biome_at = [](jint, jint, jint) { return std::uint16_t{0}; },
                .biome_name = [](std::uint16_t) -> std::string_view {
                    return "minecraft:warm_ocean";
                },
                .height_at =
                    [](jint, jint, refrain::worldgen::heightmap_kind) {
                        return jint{70};
                    },
                .templates = &manager,
            };
            const auto starts =
                refrain::worldgen::structure::generate_structure_starts(
                    chunk_pos{.x = chunk_x, .z = chunk_z}, context);
            const refrain::worldgen::structure::structure_start *ruin = nullptr;
            for (const auto &start : starts) {
                if (start.structure == "minecraft:ocean_ruin_warm") {
                    ruin = &start;
                }
            }
            expect(ruin != nullptr);
            if (ruin == nullptr) {
                return;
            }
            const auto reference = read_chunk(region, chunk_x, chunk_z);
            expect(reference.has_value());
            if (!reference) {
                return;
            }
            const auto *structures =
                child(reference->as_compound(), "structures");
            const auto *starts_value =
                structures == nullptr
                    ? nullptr
                    : child(structures->as_compound(), "starts");
            const auto *expected = starts_value == nullptr
                                       ? nullptr
                                       : child(starts_value->as_compound(),
                                               "minecraft:ocean_ruin_warm");
            expect(expected != nullptr);
            if (expected == nullptr) {
                return;
            }
            const auto *children = child(expected->as_compound(), "Children");
            const auto *pieces =
                children == nullptr ? nullptr : children->as_list();
            expect(pieces != nullptr);
            if (pieces == nullptr) {
                return;
            }
            expect(ruin->pieces.size() == pieces->items.size());
            if (ruin->pieces.size() != pieces->items.size()) {
                return;
            }
            for (std::size_t index = 0; index < pieces->items.size(); ++index) {
                const auto *piece = pieces->items[index].as_compound();
                const auto &actual = ruin->pieces[index];
                const auto *template_id = child(piece, "Template");
                const auto *rotation = child(piece, "Rot");
                const auto *template_x = child(piece, "TPX");
                const auto *template_z = child(piece, "TPZ");
                const auto *integrity = child(piece, "Integrity");
                const auto *large = child(piece, "IsLarge");
                const auto *bb = child(piece, "BB");
                const auto values =
                    bb == nullptr ? std::nullopt : bb->as_int_array();
                expect(template_id != nullptr && rotation != nullptr &&
                       template_x != nullptr && template_z != nullptr &&
                       integrity != nullptr && large != nullptr);
                if (template_id == nullptr || rotation == nullptr ||
                    template_x == nullptr || template_z == nullptr ||
                    integrity == nullptr || large == nullptr) {
                    continue;
                }
                expect(template_id->as_string() == actual.template_id);
                expect(template_x->as_int() == actual.template_origin.x);
                expect(template_z->as_int() == actual.template_origin.z);
                expect(std::abs(integrity->as_float().value_or(0.0F) -
                                actual.integrity) < 0.0001F);
                expect((large->as_byte().value_or(0) != 0) == actual.large);
                const auto rot_text = rotation->as_string();
                expect(rot_text.has_value());
                if (rot_text) {
                    const jint expected_rotation =
                        *rot_text == "CLOCKWISE_180"         ? 2
                        : *rot_text == "CLOCKWISE_90"        ? 1
                        : *rot_text == "COUNTERCLOCKWISE_90" ? 3
                                                             : 0;
                    expect(actual.template_rotation == expected_rotation);
                }
                expect(values.has_value() && values->size() == 6U);
                if (values && values->size() == 6U) {
                    expect(actual.bounds.min_x == (*values)[0]);
                    expect(actual.bounds.min_z == (*values)[2]);
                    expect(actual.bounds.max_x == (*values)[3]);
                    expect(actual.bounds.max_z == (*values)[5]);
                }
            }
        };

    "a cold ocean ruin start adds the cracked and mossy layers"_test = [] {
        if (std::getenv("REFRAIN_VANILLA_RUIN_COLD_REAL") == nullptr) {
            return;
        }
        const char *region = std::getenv("REFRAIN_VANILLA_RUIN_COLD_REGION");
        const char *root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        const char *templates_root = std::getenv("REFRAIN_VANILLA_STRUCTURES");
        if (region == nullptr || root == nullptr || templates_root == nullptr) {
            return;
        }
        const jint chunk_x = 5;
        const jint chunk_z = 10;
        const jlong seed = 1;
        const auto data =
            refrain::worldgen::structure::load_structure_data(root);
        expect(data.has_value());
        if (!data) {
            return;
        }
        refrain::worldgen::block_state_registry registry;
        refrain::worldgen::structure::structure_template_manager manager{
            templates_root, registry};
        const refrain::worldgen::structure::structure_start_context context{
            .seed = seed,
            .data = &*data,
            .min_y = -64,
            .world_height = 384,
            .sea_level = 63,
            .biome_at = [](jint, jint, jint) { return std::uint16_t{0}; },
            .biome_name = [](std::uint16_t) -> std::string_view {
                return "minecraft:cold_ocean";
            },
            .height_at =
                [](jint, jint, refrain::worldgen::heightmap_kind) {
                    return jint{70};
                },
            .templates = &manager,
        };
        const auto starts =
            refrain::worldgen::structure::generate_structure_starts(
                chunk_pos{.x = chunk_x, .z = chunk_z}, context);
        const refrain::worldgen::structure::structure_start *ruin = nullptr;
        for (const auto &start : starts) {
            if (start.structure == "minecraft:ocean_ruin_cold") {
                ruin = &start;
            }
        }
        expect(ruin != nullptr);
        if (ruin == nullptr) {
            return;
        }
        const auto reference = read_chunk(region, chunk_x, chunk_z);
        expect(reference.has_value());
        if (!reference) {
            return;
        }
        const auto *structures = child(reference->as_compound(), "structures");
        const auto *starts_value =
            structures == nullptr ? nullptr
                                  : child(structures->as_compound(), "starts");
        const auto *expected = starts_value == nullptr
                                   ? nullptr
                                   : child(starts_value->as_compound(),
                                           "minecraft:ocean_ruin_cold");
        expect(expected != nullptr);
        if (expected == nullptr) {
            return;
        }
        const auto *children = child(expected->as_compound(), "Children");
        const auto *pieces =
            children == nullptr ? nullptr : children->as_list();
        expect(pieces != nullptr && pieces->items.size() == 3U);
        if (pieces == nullptr || pieces->items.size() != 3U ||
            ruin->pieces.size() != 3U) {
            return;
        }
        for (std::size_t index = 0; index < 3U; ++index) {
            const auto *piece = pieces->items[index].as_compound();
            const auto &actual = ruin->pieces[index];
            const auto *template_id = child(piece, "Template");
            const auto *rotation = child(piece, "Rot");
            const auto *template_x = child(piece, "TPX");
            const auto *template_z = child(piece, "TPZ");
            const auto *integrity = child(piece, "Integrity");
            const auto *large = child(piece, "IsLarge");
            const auto *bb = child(piece, "BB");
            const auto values =
                bb == nullptr ? std::nullopt : bb->as_int_array();
            expect(template_id != nullptr && rotation != nullptr &&
                   template_x != nullptr && template_z != nullptr &&
                   integrity != nullptr && large != nullptr);
            if (template_id == nullptr || rotation == nullptr ||
                template_x == nullptr || template_z == nullptr ||
                integrity == nullptr || large == nullptr) {
                continue;
            }
            expect(template_id->as_string() == actual.template_id);
            expect(template_x->as_int() == actual.template_origin.x);
            expect(template_z->as_int() == actual.template_origin.z);
            expect(std::abs(integrity->as_float().value_or(0.0F) -
                            actual.integrity) < 0.0001F);
            expect((large->as_byte().value_or(0) != 0) == actual.large);
            const auto rot_text = rotation->as_string();
            expect(rot_text.has_value());
            if (rot_text) {
                const jint expected_rotation =
                    *rot_text == "CLOCKWISE_180"         ? 2
                    : *rot_text == "CLOCKWISE_90"        ? 1
                    : *rot_text == "COUNTERCLOCKWISE_90" ? 3
                                                         : 0;
                expect(actual.template_rotation == expected_rotation);
            }
            expect(values.has_value() && values->size() == 6U);
            if (values && values->size() == 6U) {
                expect(actual.bounds.min_x == (*values)[0]);
                expect(actual.bounds.min_z == (*values)[2]);
                expect(actual.bounds.max_x == (*values)[3]);
                expect(actual.bounds.max_z == (*values)[5]);
            }
        }
    };

    "a real mineshaft start reproduces the recursive piece tree"_test = [] {
        if (std::getenv("REFRAIN_VANILLA_MINE_REAL") == nullptr) {
            return;
        }
        const char *region = std::getenv("REFRAIN_VANILLA_MINE_REGION");
        if (region == nullptr) {
            return;
        }
        const jint chunk_x = -4;
        const jint chunk_z = 0;
        const jlong seed = 1;
        const auto start = refrain::worldgen::structure::mineshaft_start(
            chunk_pos{.x = chunk_x, .z = chunk_z}, seed, -64, 63, false);
        const auto reference = read_chunk(region, chunk_x, chunk_z);
        expect(reference.has_value());
        if (!reference) {
            return;
        }
        const auto *structures = child(reference->as_compound(), "structures");
        const auto *starts_value =
            structures == nullptr ? nullptr
                                  : child(structures->as_compound(), "starts");
        const auto *expected =
            starts_value == nullptr
                ? nullptr
                : child(starts_value->as_compound(), "minecraft:mineshaft");
        expect(expected != nullptr);
        if (expected == nullptr) {
            return;
        }
        const auto *children = child(expected->as_compound(), "Children");
        const auto *pieces =
            children == nullptr ? nullptr : children->as_list();
        expect(pieces != nullptr);
        if (pieces == nullptr) {
            return;
        }
        expect(start.pieces.size() == pieces->items.size());
        if (start.pieces.size() != pieces->items.size()) {
            std::fprintf(stderr, "mineshaft pieces ours=%zu ref=%zu\n",
                         start.pieces.size(), pieces->items.size());
            const auto print =
                [](const char *tag, std::size_t index, std::string_view id,
                   jint o,
                   const refrain::worldgen::structure::structure_bbox &b) {
                    std::fprintf(
                        stderr, "%s %zu %.*s O=%d BB=[%d,%d,%d,%d,%d,%d]\n",
                        tag, index, static_cast<int>(id.size()), id.data(),
                        static_cast<int>(o), static_cast<int>(b.min_x),
                        static_cast<int>(b.min_y), static_cast<int>(b.min_z),
                        static_cast<int>(b.max_x), static_cast<int>(b.max_y),
                        static_cast<int>(b.max_z));
                };
            for (std::size_t i = 0;
                 i < std::min<std::size_t>(10, start.pieces.size()); ++i) {
                print("ours", i, start.pieces[i].type,
                      start.pieces[i].orientation, start.pieces[i].bounds);
            }
            for (std::size_t i = 0;
                 i < std::min<std::size_t>(10, pieces->items.size()); ++i) {
                const auto *p = pieces->items[i].as_compound();
                const auto *id = child(p, "id");
                const auto *bb = child(p, "BB");
                const auto *o = child(p, "O");
                const auto vals =
                    bb == nullptr ? std::nullopt : bb->as_int_array();
                if (id == nullptr || bb == nullptr || o == nullptr || !vals ||
                    vals->size() != 6U) {
                    continue;
                }
                refrain::worldgen::structure::structure_bbox b{
                    .min_x = (*vals)[0],
                    .min_y = (*vals)[1],
                    .min_z = (*vals)[2],
                    .max_x = (*vals)[3],
                    .max_y = (*vals)[4],
                    .max_z = (*vals)[5]};
                print("ref ", i, id->as_string().value_or("?"),
                      o->as_int().value_or(0), b);
            }
            return;
        }
        for (std::size_t index = 0; index < pieces->items.size(); ++index) {
            const auto *piece = pieces->items[index].as_compound();
            const auto &actual = start.pieces[index];
            const auto *id = child(piece, "id");
            const auto *bb = child(piece, "BB");
            const auto *orientation = child(piece, "O");
            const auto values =
                bb == nullptr ? std::nullopt : bb->as_int_array();
            expect(id != nullptr && bb != nullptr && orientation != nullptr);
            if (id == nullptr || bb == nullptr || orientation == nullptr ||
                !values || values->size() != 6U) {
                continue;
            }
            const bool mismatch =
                id->as_string() != actual.type ||
                orientation->as_int().value_or(0) != actual.orientation ||
                actual.bounds.min_x != (*values)[0] ||
                actual.bounds.min_y != (*values)[1] ||
                actual.bounds.min_z != (*values)[2] ||
                actual.bounds.max_x != (*values)[3] ||
                actual.bounds.max_y != (*values)[4] ||
                actual.bounds.max_z != (*values)[5];
            if (mismatch && index < 60) {
                std::fprintf(
                    stderr,
                    "mine %zu ours %s O=%d [%d,%d,%d,%d,%d,%d] ref %.*s O=%d "
                    "[%d,%d,%d,%d,%d,%d]\n",
                    index, actual.type.c_str(),
                    static_cast<int>(actual.orientation),
                    static_cast<int>(actual.bounds.min_x),
                    static_cast<int>(actual.bounds.min_y),
                    static_cast<int>(actual.bounds.min_z),
                    static_cast<int>(actual.bounds.max_x),
                    static_cast<int>(actual.bounds.max_y),
                    static_cast<int>(actual.bounds.max_z),
                    static_cast<int>(id->as_string().value_or("").size()),
                    id->as_string().value_or("").data(),
                    orientation->as_int().value_or(0), (*values)[0],
                    (*values)[1], (*values)[2], (*values)[3], (*values)[4],
                    (*values)[5]);
            }
            expect(id->as_string() == actual.type);
            expect(orientation->as_int() == actual.orientation);
            expect(actual.bounds.min_x == (*values)[0]);
            expect(actual.bounds.min_y == (*values)[1]);
            expect(actual.bounds.min_z == (*values)[2]);
            expect(actual.bounds.max_x == (*values)[3]);
            expect(actual.bounds.max_y == (*values)[4]);
            expect(actual.bounds.max_z == (*values)[5]);
        }
    };
};
