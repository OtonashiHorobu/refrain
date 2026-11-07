// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/nbt.hpp>
#include <refrain/worldgen/structure/generation.hpp>
#include <refrain/worldgen/structure/stage.hpp>
#include <refrain/worldgen/structure/structure_data.hpp>

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
                       static_cast<std::uint64_t>(chunk_x));
                expect((static_cast<std::uint64_t>((*packed)[0]) &
                        0xFFFFFFFFU) == static_cast<std::uint64_t>(chunk_z));
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
};
