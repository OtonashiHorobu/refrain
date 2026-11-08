// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Determinism check for the fixed-biome structure generator. Inputs:
// REFRAIN_VANILLA_WORLDGEN, REFRAIN_VANILLA_STRUCTURES and optionally
// REFRAIN_STRUCTURE_GENERATOR_SEED/BIOME/CHUNK_X/CHUNK_Z.

#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/structure/generator.hpp>

#include <boost/ut.hpp>

#include <cstdlib>
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
};
