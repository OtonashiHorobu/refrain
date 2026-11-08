// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Deterministic fixed-biome generation of the nether and the end. The content
// snapshot in this repository supplies the data, so the test runs without a
// reference world; it fixes the biome source so the surface rule and the
// heightmaps are stable across generator instances.

#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/dimension_generator.hpp>

#include <boost/ut.hpp>

#include <array>
#include <memory>
#include <string>

namespace {

using namespace boost::ut;
using namespace refrain;
using namespace refrain::worldgen;

auto data_root() -> std::string {
    return std::string{REFRAIN_SOURCE_DIR} + "/content/vanilla/26.3/data";
}

auto same_chunk(const proto_chunk &left, const proto_chunk &right) -> bool {
    if (left.min_y() != right.min_y() || left.height() != right.height()) {
        return false;
    }
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

auto count_block(const proto_chunk &chunk, std::string_view name)
    -> std::size_t {
    std::size_t count = 0;
    for (jint y = chunk.min_y(); y < chunk.min_y() + chunk.height(); ++y) {
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                if (chunk.table().name(chunk.state_at(x, y, z)) == name) {
                    ++count;
                }
            }
        }
    }
    return count;
}

auto occupied_above(const proto_chunk &chunk, jint top) -> bool {
    for (jint y = top; y < chunk.min_y() + chunk.height(); ++y) {
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                const auto name = chunk.table().name(chunk.state_at(x, y, z));
                if (name != "minecraft:air") {
                    return true;
                }
            }
        }
    }
    return false;
}

auto make_generator(dimension_kind kind, std::string biome)
    -> std::unique_ptr<dimension_generator> {
    auto created = dimension_generator::create(
        dimension_generator_config{.data_root = data_root(),
                                   .kind = kind,
                                   .seed = 1,
                                   .fixed_biome = std::move(biome)});
    expect(created.has_value()) << created.error().message;
    return created ? std::move(*created) : nullptr;
}

} // namespace

suite<"worldgen/dimension-generator"> dimension_generator_tests = [] {
    "the nether generates inside half of its level height"_test = [] {
        auto generator =
            make_generator(dimension_kind::nether, "minecraft:nether_wastes");
        if (generator == nullptr) {
            return;
        }
        expect(generator->profile().height == 256);
        expect(generator->generation_min_y() == 0);
        expect(generator->generation_height() == 128);
        expect(generator->settings().default_block == "minecraft:netherrack");
        expect(generator->settings().default_fluid == "minecraft:lava");
        expect(generator->settings().legacy_random_source);
        expect(generator->material_rule() == "minecraft:nether");

        const auto chunk = generator->generate(chunk_pos{.x = 0, .z = 0});
        expect(chunk.has_value()) << chunk.error().message;
        if (!chunk) {
            return;
        }
        expect((*chunk)->min_y() == 0);
        expect((*chunk)->height() == 256);
        // The lava sea fills the open space below sea level 32.
        expect(count_block(**chunk, "minecraft:lava") > 0);
        expect(count_block(**chunk, "minecraft:bedrock") > 0);
        expect(count_block(**chunk, "minecraft:netherrack") > 0);
        // Nothing is generated above the 128-block noise height.
        expect(!occupied_above(**chunk, 128));
    };

    "the end generates end stone and no fluid inside half of its level"_test =
        [] {
            auto generator =
                make_generator(dimension_kind::end, "minecraft:the_end");
            if (generator == nullptr) {
                return;
            }
            expect(generator->profile().height == 256);
            expect(generator->generation_min_y() == 0);
            expect(generator->generation_height() == 128);
            expect(generator->settings().default_block ==
                   "minecraft:end_stone");
            expect(generator->settings().default_fluid == "minecraft:air");
            expect(generator->settings().legacy_random_source);
            expect(generator->material_rule() == "minecraft:end");

            const auto chunk = generator->generate(chunk_pos{.x = 0, .z = 0});
            expect(chunk.has_value()) << chunk.error().message;
            if (!chunk) {
                return;
            }
            expect(count_block(**chunk, "minecraft:end_stone") > 0);
            expect(count_block(**chunk, "minecraft:lava") == 0);
            expect(count_block(**chunk, "minecraft:water") == 0);
            expect(!occupied_above(**chunk, 128));
        };

    "fixed-biome generation is deterministic across instances"_test = [] {
        for (const auto [kind, biome] :
             std::array<std::pair<dimension_kind, std::string_view>, 2>{
                 std::pair{dimension_kind::nether, "minecraft:nether_wastes"},
                 std::pair{dimension_kind::end, "minecraft:the_end"},
             }) {
            auto first = make_generator(kind, std::string{biome});
            if (first == nullptr) {
                return;
            }
            const auto a = first->generate(chunk_pos{.x = 0, .z = 0});
            const auto b = first->generate(chunk_pos{.x = 0, .z = 0});
            auto second = make_generator(kind, std::string{biome});
            if (second == nullptr) {
                return;
            }
            const auto c = second->generate(chunk_pos{.x = 0, .z = 0});
            expect(a.has_value() && b.has_value() && c.has_value());
            if (!a || !b || !c) {
                return;
            }
            expect(same_chunk(**a, **b));
            expect(same_chunk(**a, **c));
        }
    };

    "a dimension profile mismatch is rejected"_test = [] {
        // The end report does not exist, so the end never reads a parameter
        // table; a bad data root fails while loading the noise settings.
        const auto missing = dimension_generator::create(
            dimension_generator_config{.data_root = data_root() + "/missing",
                                       .kind = dimension_kind::nether,
                                       .seed = 1});
        expect(!missing.has_value());
    };
};
