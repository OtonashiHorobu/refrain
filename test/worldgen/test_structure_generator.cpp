// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Determinism check for the fixed-biome structure generator. Inputs:
// REFRAIN_VANILLA_WORLDGEN, REFRAIN_VANILLA_STRUCTURES and optionally
// REFRAIN_STRUCTURE_GENERATOR_SEED/BIOME/CHUNK_X/CHUNK_Z.

#include <refrain/world/anvil.hpp>
#include <refrain/world/chunk_codec.hpp>
#include <refrain/world/proto_bridge.hpp>
#include <refrain/world/store.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/structure/generator.hpp>

#include "support/scratch_directory.hpp"

#include <boost/ut.hpp>

#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>

namespace {

using namespace boost::ut;
using namespace refrain;
using namespace refrain::worldgen;

auto same_chunk(const proto_chunk &left, const proto_chunk &right) -> bool {
    for (jint y = left.min_y(); y < left.min_y() + left.height(); ++y) {
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                if (left.table().name(left.state_at(x, y, z)) !=
                    right.table().name(right.state_at(x, y, z))) {
                    return false;
                }
            }
        }
    }
    for (const auto kind :
         {heightmap_kind::world_surface, heightmap_kind::ocean_floor,
          heightmap_kind::motion_blocking,
          heightmap_kind::motion_blocking_no_leaves}) {
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                if (left.first_available(kind, x, z) !=
                    right.first_available(kind, x, z)) {
                    return false;
                }
            }
        }
    }
    return true;
}

} // namespace

suite<"worldgen/structure-generator"> structure_generator_tests = [] {
    "the generator is deterministic for one configuration"_test = [] {
        const char *data_root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        const char *templates_root = std::getenv("REFRAIN_VANILLA_STRUCTURES");
        if (data_root == nullptr || templates_root == nullptr) {
            return;
        }
        const auto env = [](const char *name, const char *fallback) {
            const char *value = std::getenv(name);
            return std::string{value == nullptr ? fallback : value};
        };
        const structure::structure_generator_config config{
            .data_root = data_root,
            .templates_root = templates_root,
            .biome =
                env("REFRAIN_STRUCTURE_GENERATOR_BIOME", "minecraft:plains"),
            .seed = std::strtoll(
                env("REFRAIN_STRUCTURE_GENERATOR_SEED", "1").c_str(), nullptr,
                10),
        };
        const auto pos = chunk_pos{
            .x = std::atoi(
                env("REFRAIN_STRUCTURE_GENERATOR_CHUNK_X", "0").c_str()),
            .z = std::atoi(
                env("REFRAIN_STRUCTURE_GENERATOR_CHUNK_Z", "0").c_str())};
        auto first = structure::structure_generator::create(config);
        auto second = structure::structure_generator::create(config);
        expect(first.has_value() && second.has_value());
        if (!first || !second) {
            return;
        }
        const auto a = (*first)->generate(pos);
        const auto b = (*first)->generate(pos);
        const auto c = (*second)->generate(pos);
        expect(a.has_value() && b.has_value() && c.has_value());
        if (!a || !b || !c) {
            return;
        }
        expect(same_chunk(**a, **b));
        expect(same_chunk(**a, **c));
    };

    "a generated structure chunk survives the storage bridge"_test = [] {
        const char *data_root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        const char *templates_root = std::getenv("REFRAIN_VANILLA_STRUCTURES");
        if (data_root == nullptr || templates_root == nullptr) {
            return;
        }
        const auto generator = structure::structure_generator::create(
            structure::structure_generator_config{
                .data_root = data_root,
                .templates_root = templates_root,
                .biome = "minecraft:jungle",
                .seed = 2,
            });
        expect(generator.has_value());
        if (!generator) {
            return;
        }
        const auto chunk = (*generator)->generate(chunk_pos{.x = 9, .z = 2});
        expect(chunk.has_value());
        if (!chunk) {
            return;
        }
        auto storage = world::from_proto_chunk(
            **chunk, {"minecraft:plains", "minecraft:jungle"});
        expect(storage.has_value());
        if (!storage) {
            return;
        }
        const auto payload = world::encode_chunk(
            *storage, nullptr, true, world::chunk_palette_mode::inline_keys);
        if (!payload) {
            std::fprintf(stderr, "encode error code=%d message=%s\n",
                         static_cast<int>(payload.error().code),
                         payload.error().message.c_str());
        }
        expect(payload.has_value());
        if (!payload) {
            return;
        }
        const auto decoded = world::decode_chunk(*payload);
        expect(decoded.has_value());
        if (!decoded) {
            return;
        }
        expect(world::validate(*decoded).has_value());
        std::map<std::int8_t, const ::refrain::storage::section_record *>
            sections;
        for (const auto &section : decoded->sections) {
            sections.emplace(section.y, &section);
        }
        std::size_t mismatches = 0;
        for (const auto &section : storage->sections) {
            const auto found = sections.find(section.y);
            if (found == sections.end()) {
                ++mismatches;
                continue;
            }
            const auto &other = *found->second;
            if (section.palette.size() != other.palette.size() ||
                section.data != other.data) {
                ++mismatches;
            }
        }
        expect(mismatches == 0);
    };

    "a generated structure chunk exports to Anvil and returns"_test = [&] {
        const char *data_root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        const char *templates_root = std::getenv("REFRAIN_VANILLA_STRUCTURES");
        if (data_root == nullptr || templates_root == nullptr ||
            std::getenv("REFRAIN_STRUCTURE_GENERATOR_ANVIL") == nullptr) {
            return;
        }
        const auto generator = structure::structure_generator::create(
            structure::structure_generator_config{
                .data_root = data_root,
                .templates_root = templates_root,
                .biome = "minecraft:jungle",
                .seed = 2,
            });
        expect(generator.has_value());
        if (!generator) {
            return;
        }
        const auto chunk = (*generator)->generate(chunk_pos{.x = 9, .z = 2});
        expect(chunk.has_value());
        if (!chunk) {
            return;
        }
        auto storage = world::from_proto_chunk(
            **chunk, {"minecraft:plains", "minecraft:jungle"});
        expect(storage.has_value());
        if (!storage) {
            return;
        }
        const refrain::test_support::scratch_directory scratch;
        const auto root = scratch.path() / "anvil";
        auto store = world::world_store::create(root.string());
        expect(store.has_value());
        if (!store) {
            return;
        }
        data::nbt::compound level{};
        level["DataVersion"] = data::nbt::value{std::int32_t{5023}};
        data::nbt::compound meta{};
        meta["level"] = data::nbt::value{std::move(level)};
        const auto meta_bytes =
            data::nbt::write(data::nbt::value{std::move(meta)}, "",
                             data::nbt::compression::none);
        expect(meta_bytes.has_value());
        if (!meta_bytes) {
            return;
        }
        expect(store->write_meta(*meta_bytes).has_value());
        expect(store->write_chunk({.x = 9, .z = 2}, *storage).has_value());
        expect(store->commit().has_value());
        const auto exported = scratch.path() / "export";
        const auto export_result =
            world::anvil::export_world(root.string(), exported.string(), true);
        expect(export_result.has_value());
        if (!export_result) {
            return;
        }
        const auto imported = scratch.path() / "import";
        const auto import_result = world::anvil::import_world(
            exported.string(), imported.string(), true);
        expect(import_result.has_value());
        if (import_result) {
            auto reopened = world::world_store::open(imported.string());
            expect(reopened.has_value());
            if (reopened) {
                const auto back = reopened->read_chunk({.x = 9, .z = 2});
                expect(back.has_value());
                if (back) {
                    std::map<std::int8_t,
                             const ::refrain::storage::section_record *>
                        sections;
                    for (const auto &section : back->sections) {
                        sections.emplace(section.y, &section);
                    }
                    const auto key_at = [](const auto &section,
                                           std::size_t index) {
                        if (section.kind ==
                            ::refrain::storage::section_kind::single_value) {
                            return section.palette.empty()
                                       ? std::string{}
                                       : section.palette.front().key;
                        }
                        std::vector<std::uint32_t> indices(4096, 0);
                        if (!world::unpack_palette_indices(
                                section.data, section.bits_per_entry, 4096,
                                section.palette.size(), indices)) {
                            return std::string{};
                        }
                        const auto id = indices[index];
                        return id < section.palette.size()
                                   ? section.palette[id].key
                                   : std::string{};
                    };
                    std::size_t mismatches = 0;
                    for (const auto &section : storage->sections) {
                        const auto found = sections.find(section.y);
                        if (found == sections.end()) {
                            ++mismatches;
                            continue;
                        }
                        for (std::size_t index = 0; index < 4096; ++index) {
                            if (key_at(section, index) !=
                                key_at(*found->second, index)) {
                                ++mismatches;
                            }
                        }
                    }
                    expect(mismatches == 0);
                }
            }
        }
    };
};
