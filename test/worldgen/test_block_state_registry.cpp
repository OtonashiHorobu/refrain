// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/io.hpp>
#include <refrain/data/vanilla.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/terrain/surface.hpp>

#include "support/scratch_directory.hpp"

#include <boost/ut.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

using refrain::data::block_spec;
using refrain::data::build_vanilla_tables;
using refrain::data::state_spec;
using refrain::data::vanilla_spec;
using refrain::data::vanilla_tables;
using refrain::worldgen::block_state_id;
using refrain::worldgen::block_state_registry;
using refrain::worldgen::block_tag_set;
using refrain::worldgen::build_block_state_registry;
using refrain::worldgen::build_shared_block_state_registry;
using refrain::worldgen::load_block_tags;
using refrain::worldgen::state_traits;

auto block(std::string name, std::vector<state_spec> states) -> block_spec {
    block_spec result;
    result.name = std::move(name);
    result.states = std::move(states);
    return result;
}

auto air_block() -> block_spec {
    return block(
        "minecraft:air",
        {state_spec{.vanilla_id = 0, .values = {}, .is_default = true}});
}

auto single_state_block(std::string name, std::uint16_t vanilla_id)
    -> block_spec {
    return block(std::move(name), {state_spec{.vanilla_id = vanilla_id,
                                              .values = {},
                                              .is_default = true}});
}

auto water_block() -> block_spec {
    block_spec result;
    result.name = "minecraft:water";
    result.property_names = {"level"};
    std::vector<std::string> levels;
    for (int level = 0; level < 16; ++level) {
        levels.push_back(std::to_string(level));
    }
    result.property_values = {levels};
    for (int level = 0; level < 16; ++level) {
        result.states.push_back(state_spec{
            .vanilla_id = static_cast<std::uint16_t>(100 + level),
            .values = {std::to_string(level)},
            .is_default = level == 0,
        });
    }
    return result;
}

auto slab_block() -> block_spec {
    block_spec result;
    result.name = "minecraft:oak_slab";
    result.property_names = {"waterlogged"};
    result.property_values = {{"false", "true"}};
    result.states = {
        state_spec{.vanilla_id = 200, .values = {"false"}, .is_default = true},
        state_spec{.vanilla_id = 201, .values = {"true"}, .is_default = false},
    };
    return result;
}

auto sample_spec() -> vanilla_spec {
    vanilla_spec spec;
    spec.data_version = 5023;
    spec.blocks = {
        air_block(),
        single_state_block("minecraft:stone", 1),
        water_block(),
        single_state_block("minecraft:oak_leaves", 2),
        single_state_block("minecraft:bedrock", 3),
        single_state_block("minecraft:dirt", 4),
        slab_block(),
    };
    return spec;
}

} // namespace

using namespace boost::ut;
using namespace refrain::worldgen;

suite<"worldgen/block-state-registry"> block_state_registry_tests = [] {
    "interning keeps ids and traits"_test = [] {
        block_state_registry registry;
        expect(registry.air() == 0);
        expect(registry.name(registry.air()) == "minecraft:air");

        const auto stone = registry.intern("minecraft:stone",
                                           {.air = false,
                                            .blocks_motion = true,
                                            .blocks_motion_no_leaves = true},
                                           7, 42);
        expect(stone.has_value());
        if (!stone) {
            return;
        }
        expect(*stone != 0);
        expect(registry.name(*stone) == "minecraft:stone");
        expect(registry.block(*stone) == 7);
        expect(registry.vanilla_id(*stone) == 42);
        expect(registry.traits(*stone).blocks_motion);

        const auto again = registry.intern("minecraft:stone", state_traits{});
        expect(again.has_value() && *again == *stone);

        const auto water = registry.intern(
            "minecraft:water", {.air = false, .fluid = true}, 9, 100);
        expect(water.has_value());
        expect(registry.traits(*water).fluid);
    };

    "tags are sorted and queried by id"_test = [] {
        block_state_registry registry;
        const auto stone =
            registry.intern("minecraft:stone", {.blocks_motion = true});
        const auto dirt =
            registry.intern("minecraft:dirt", {.blocks_motion = true});
        expect(stone.has_value() && dirt.has_value());
        if (!stone || !dirt) {
            return;
        }
        const std::array ids{*stone, *dirt, *stone};
        registry.add_tag("minecraft:test_tag", ids);
        expect(registry.in_tag(*stone, "minecraft:test_tag"));
        expect(registry.in_tag(*dirt, "minecraft:test_tag"));
        expect(!registry.in_tag(registry.air(), "minecraft:test_tag"));
        expect(!registry.in_tag(*stone, "minecraft:missing"));
    };

    "tag sets resolve nested references and reject cycles"_test = [] {
        block_tag_set tags;
        tags.add("minecraft:base", {"minecraft:stone"});
        tags.add("minecraft:all", {"#minecraft:base", "minecraft:dirt"});
        expect(tags.has("minecraft:all"));
        const auto resolved = tags.resolve("minecraft:all");
        expect(resolved.has_value());
        if (resolved) {
            expect(resolved->size() == 2);
            expect((*resolved)[0] == "minecraft:dirt");
            expect((*resolved)[1] == "minecraft:stone");
        }
        expect(!tags.resolve("minecraft:missing").has_value());

        tags.add("minecraft:a", {"#minecraft:b"});
        tags.add("minecraft:b", {"#minecraft:a"});
        expect(!tags.resolve("minecraft:a").has_value());
    };

    "block tags load from a data root"_test = [] {
        const refrain::test_support::scratch_directory scratch;
        const auto root = scratch.path();
        std::filesystem::create_directories(root / "minecraft/tags/block");
        expect(refrain::core::io::write_text(
                   (root / "minecraft/tags/block/base.json").string(),
                   R"({"values":["minecraft:stone"]})")
                   .has_value());
        expect(refrain::core::io::write_text(
                   (root / "minecraft/tags/block/nested.json").string(),
                   R"({"values":["#minecraft:base","minecraft:dirt"]})")
                   .has_value());

        const auto tags = load_block_tags(root.string());
        expect(tags.has_value());
        if (tags) {
            const auto resolved = tags->resolve("minecraft:nested");
            expect(resolved.has_value());
            if (resolved) {
                expect(resolved->size() == 2);
            }
        }
    };

    "uninterned state traits follow the heightmap tags"_test = [] {
        block_state_registry registry;
        const auto stone = registry.intern("minecraft:stone",
                                           {.air = false,
                                            .blocks_motion = true,
                                            .blocks_motion_no_leaves = true});
        const auto leaves = registry.intern("minecraft:oak_leaves",
                                            {.air = false,
                                             .blocks_motion = true,
                                             .blocks_motion_no_leaves = false});
        expect(stone.has_value() && leaves.has_value());
        if (!stone || !leaves) {
            return;
        }
        const std::array<block_state_id, 2> motion{*stone, *leaves};
        const std::array<block_state_id, 1> motion_no_leaves{*stone};
        registry.add_tag("minecraft:blocks_motion_in_heightmap", motion);
        registry.add_tag("minecraft:blocks_motion_in_heightmap_no_leaves",
                         motion_no_leaves);

        const auto leaf_variant =
            registry.infer_traits("minecraft:oak_leaves[distance=7,"
                                  "persistent=false]");
        expect(leaf_variant.blocks_motion);
        expect(!leaf_variant.blocks_motion_no_leaves);
        const auto flower = registry.infer_traits("minecraft:dandelion");
        expect(!flower.blocks_motion);
        expect(!flower.blocks_motion_no_leaves);
        const auto water = registry.infer_traits("minecraft:water[level=0]");
        expect(water.fluid);
        expect(water.water);
        expect(!water.blocks_motion);
        const auto snowy = registry.infer_traits("minecraft:dirt[snowy=true]");
        expect(!snowy.air);
        expect(!snowy.fluid);

        block_state_registry untagged;
        const auto plain_leaves = untagged.infer_traits("minecraft:oak_leaves");
        expect(plain_leaves.blocks_motion);
        expect(!plain_leaves.blocks_motion_no_leaves);
        const auto plain_water = untagged.infer_traits("minecraft:water");
        expect(plain_water.fluid);
        expect(plain_water.water);
        expect(!plain_water.blocks_motion);
    };

    "the RVTB table and tag sets build a registry"_test = [] {
        const auto spec = sample_spec();
        const auto bytes = build_vanilla_tables(spec);
        expect(bytes.has_value());
        if (!bytes) {
            return;
        }
        const auto tables = vanilla_tables::open(*bytes);
        expect(tables.has_value());
        if (!tables) {
            return;
        }

        block_tag_set tags;
        tags.add("minecraft:blocks_motion_in_heightmap",
                 {"minecraft:stone", "minecraft:oak_leaves",
                  "minecraft:bedrock", "minecraft:dirt"});
        tags.add("minecraft:blocks_motion_in_heightmap_no_leaves",
                 {"minecraft:stone", "minecraft:bedrock", "minecraft:dirt"});
        tags.add("minecraft:uncarvable", {"minecraft:bedrock"});

        auto registry = build_block_state_registry(*tables, tags);
        expect(registry.has_value());
        if (!registry) {
            return;
        }
        const auto stone = registry->find("minecraft:stone");
        const auto leaves = registry->find("minecraft:oak_leaves");
        const auto water = registry->find("minecraft:water[level=0]");
        const auto slab =
            registry->find("minecraft:oak_slab[waterlogged=true]");
        const auto slab_default = registry->find("minecraft:oak_slab");
        const auto bedrock = registry->find("minecraft:bedrock");
        expect(stone.has_value());
        expect(leaves.has_value());
        expect(water.has_value());
        expect(slab.has_value());
        expect(bedrock.has_value());
        if (!stone || !leaves || !water || !slab || !bedrock) {
            return;
        }

        expect(registry->traits(*stone).blocks_motion);
        expect(registry->traits(*stone).blocks_motion_no_leaves);
        expect(registry->traits(*leaves).blocks_motion);
        expect(!registry->traits(*leaves).blocks_motion_no_leaves);
        expect(registry->traits(*water).fluid);
        expect(registry->traits(*water).water);
        expect(registry->traits(*water).air == false);
        expect(registry->traits(*slab).fluid);
        expect(registry->in_tag(*bedrock, "minecraft:uncarvable"));
        expect(slab_default.has_value());
        if (slab_default) {
            expect(*slab_default != *slab);
            expect(registry->name(*slab_default) ==
                   "minecraft:oak_slab[waterlogged=false]");
            const auto properties = registry->default_properties(*slab);
            expect(properties.size() == 1U);
            if (properties.size() == 1U) {
                expect(properties[0].name == "waterlogged");
                expect(properties[0].value == "false");
            }
        }
        expect(!registry->in_tag(*stone, "minecraft:uncarvable"));
        expect(registry->name(registry->air()) == "minecraft:air");
        expect(registry->vanilla_id(*stone) == 1);

        surface::block_states bound{*registry};
        expect(bound.intern("minecraft:stone") == *stone);
        expect(bound.name(*stone) == "minecraft:stone");
        expect(bound.properties(*water).water);
        expect(!bound.properties(*stone).fluid);
        const auto custom = bound.intern("minecraft:test_custom");
        expect(custom != surface::no_state);
        expect(registry->find("minecraft:test_custom").has_value());
    };
};

suite<"worldgen/shared-block-state-registry">
    shared_block_state_registry_tests = [] {
        "shared table assigns state ids before any concurrent reader"_test =
            [] {
                const auto spec = sample_spec();
                const auto bytes = build_vanilla_tables(spec);
                expect(bytes.has_value());
                if (!bytes) {
                    return;
                }
                const auto tables = vanilla_tables::open(*bytes);
                expect(tables.has_value());
                if (!tables) {
                    return;
                }
                block_tag_set tags;
                tags.add("minecraft:blocks_motion_in_heightmap",
                         {"minecraft:stone", "minecraft:dirt"});
                tags.add("minecraft:blocks_motion_in_heightmap_no_leaves",
                         {"minecraft:stone", "minecraft:dirt"});
                auto first = build_shared_block_state_registry(*tables, tags);
                auto second = build_shared_block_state_registry(*tables, tags);
                expect(first.has_value());
                expect(second.has_value());
                if (!first || !second) {
                    return;
                }
                // The id of a key is a function of the content table, not of
                // the order a worker happens to ask for it.
                expect(first->size() == second->size());
                for (block_state_id id = 0; id < first->size(); ++id) {
                    expect(first->name(id) == second->name(id));
                    expect(first->traits(id).air == second->traits(id).air);
                    expect(first->traits(id).fluid == second->traits(id).fluid);
                }
                // A default state is interned under its canonical spelling and
                // the bare block name both.
                const auto canonical = first->intern(
                    "minecraft:oak_slab[waterlogged=false]", state_traits{});
                const auto bare =
                    first->intern("minecraft:oak_slab", state_traits{});
                expect(canonical.has_value());
                expect(bare.has_value());
                if (canonical && bare) {
                    expect(*canonical != *bare);
                    expect(first->name(*canonical) ==
                           "minecraft:oak_slab[waterlogged=false]");
                    expect(first->name(*bare) == "minecraft:oak_slab");
                }
                // A key the data pack spells but the table only holds as a
                // non-default spelling resolves through the table without
                // appending anything.
                const auto water =
                    first->intern("minecraft:water[level=3]", state_traits{});
                expect(water.has_value());
                if (water) {
                    expect(first->name(*water) == "minecraft:water[level=3]");
                }
            };

        "a shared table hides interned keys from find and rejects unknown keys after freezing"_test =
            [] {
                const auto spec = sample_spec();
                const auto bytes = build_vanilla_tables(spec);
                expect(bytes.has_value());
                if (!bytes) {
                    return;
                }
                const auto tables = vanilla_tables::open(*bytes);
                expect(tables.has_value());
                if (!tables) {
                    return;
                }
                block_tag_set tags;
                auto registry =
                    build_shared_block_state_registry(*tables, tags);
                expect(registry.has_value());
                if (!registry) {
                    return;
                }
                // The per-generator registry only finds keys a data pack
                // interned; the shared table keeps that lookup behaviour and
                // resolves its complete state universe through `intern`.
                expect(!registry->find("minecraft:stone").has_value());
                expect(!registry->find("minecraft:oak_slab").has_value());
                const auto stone =
                    registry->intern("minecraft:stone", state_traits{});
                expect(stone.has_value());
                if (stone) {
                    expect(registry->name(*stone) == "minecraft:stone");
                }
                expect(!registry->frozen());
                registry->freeze();
                expect(registry->frozen());
                expect(registry->intern("minecraft:stone", state_traits{})
                           .has_value());
                const auto unknown = registry->intern(
                    "minecraft:not_in_the_table", state_traits{});
                expect(!unknown.has_value());
                if (!unknown) {
                    expect(unknown.error().code ==
                           refrain::error_code::not_found);
                }
                // Freezing does not change a known id.
                const auto after =
                    registry->intern("minecraft:stone", state_traits{});
                expect(after.has_value());
                if (stone && after) {
                    expect(*stone == *after);
                }
            };

        "a frozen shared table holds the content's complete state universe"_test =
            [] {
                const auto spec = sample_spec();
                const auto bytes = build_vanilla_tables(spec);
                expect(bytes.has_value());
                if (!bytes) {
                    return;
                }
                const auto tables = vanilla_tables::open(*bytes);
                expect(tables.has_value());
                if (!tables) {
                    return;
                }
                block_tag_set tags;
                auto registry =
                    build_shared_block_state_registry(*tables, tags);
                expect(registry.has_value());
                if (!registry) {
                    return;
                }
                // Every state of the content table and the bare spelling of
                // every block resolves before the table is frozen, so a later
                // lookup can only fail for a key the content does not define.
                for (std::uint32_t index = 0; index < tables->state_count();
                     ++index) {
                    const auto state = tables->state_at(index);
                    expect(registry->holds(state.key))
                        << "state " << state.key << " is missing";
                }
                for (std::uint32_t index = 0; index < tables->block_count();
                     ++index) {
                    const auto block =
                        tables->block_at(static_cast<std::uint16_t>(index));
                    expect(registry->holds(block.name))
                        << "default spelling of " << block.name
                        << " is missing";
                }
                expect(registry->holds("minecraft:air"));
                // The completeness check refuses a table that was not frozen.
                expect(!registry->verify_frozen().has_value());
                registry->freeze();
                const auto verified = registry->verify_frozen();
                expect(verified.has_value())
                    << (verified ? "" : verified.error().message);
                // Freezing does not hide the universe: a known state still
                // resolves, an unknown one still fails closed.
                for (std::uint32_t index = 0; index < tables->state_count();
                     ++index) {
                    const auto state = tables->state_at(index);
                    const auto id = registry->intern(state.key, state_traits{});
                    expect(id.has_value())
                        << "frozen table lost state " << state.key;
                }
                const auto unknown = registry->intern(
                    "minecraft:not_in_the_table", state_traits{});
                expect(!unknown.has_value());
                if (!unknown) {
                    expect(unknown.error().code ==
                           refrain::error_code::not_found);
                }
            };

        "shared table carries the per-block default properties"_test = [] {
            const auto spec = sample_spec();
            const auto bytes = build_vanilla_tables(spec);
            expect(bytes.has_value());
            if (!bytes) {
                return;
            }
            const auto tables = vanilla_tables::open(*bytes);
            expect(tables.has_value());
            if (!tables) {
                return;
            }
            block_tag_set tags;
            auto registry = build_shared_block_state_registry(*tables, tags);
            expect(registry.has_value());
            if (!registry) {
                return;
            }
            const auto defaults =
                registry->default_properties_for_block("minecraft:oak_slab");
            expect(defaults.size() == 1U);
            if (defaults.size() == 1U) {
                expect(defaults.front().name == "waterlogged");
                expect(defaults.front().value == "false");
            }
        };
    };
