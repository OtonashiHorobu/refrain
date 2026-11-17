// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// RVTB v1 container and report-import tests.
//
// The fixtures are synthetic: the JSON documents below are written for these
// tests, not copied from the game.

#include <refrain/core/io.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/vanilla.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;
using namespace refrain::data;

auto air_block() -> block_spec {
    block_spec block;
    block.name = "minecraft:air";
    block.states.push_back(state_spec{
        .vanilla_id = 0,
        .values = {},
        .is_default = true,
    });
    return block;
}

auto log_block() -> block_spec {
    block_spec block;
    block.name = "minecraft:oak_log";
    block.property_names = {"axis"};
    block.property_values = {{"x", "y", "z"}};
    block.states = {
        state_spec{.vanilla_id = 10, .values = {"x"}, .is_default = false},
        state_spec{.vanilla_id = 11, .values = {"y"}, .is_default = true},
        state_spec{.vanilla_id = 12, .values = {"z"}, .is_default = false},
    };
    return block;
}

auto water_block() -> block_spec {
    block_spec block;
    block.name = "minecraft:water";
    block.property_names = {"level"};
    std::vector<std::string> levels;
    for (int level = 0; level < 16; ++level) {
        levels.push_back(std::to_string(level));
    }
    block.property_values = {levels};
    for (int level = 0; level < 16; ++level) {
        block.states.push_back(state_spec{
            .vanilla_id = static_cast<std::uint16_t>(100 + level),
            .values = {std::to_string(level)},
            .is_default = level == 0,
        });
    }
    return block;
}

auto sample_spec() -> vanilla_spec {
    vanilla_spec spec;
    spec.data_version = 5023;
    spec.blocks = {log_block(), air_block(), water_block()};
    spec.biome_presets = {
        biome_preset_spec{
            .name = "minecraft:overworld",
            .entries =
                {
                    biome_parameter_spec{
                        .biome = "minecraft:plains",
                        .min = {-10000, -10000, -11000, -10000, -10000, -10000},
                        .max = {10000, 10000, 10000, 10000, 10000, 10000},
                        .offset = 0,
                    },
                    biome_parameter_spec{
                        .biome = "minecraft:ocean",
                        .min = {-10000, -10000, -10500, -10000, -10000, -10000},
                        .max = {10000, 10000, -4550, 10000, 10000, 10000},
                        .offset = 2500,
                    },
                },
        },
        biome_preset_spec{
            .name = "minecraft:nether",
            .entries = {biome_parameter_spec{
                .biome = "minecraft:nether_wastes",
                .min = {0, 0, 0, 0, 0, 0},
                .max = {0, 0, 0, 0, 0, 0},
                .offset = 0,
            }},
        },
    };
    spec.registries = {
        registry_spec{
            .name = "minecraft:block",
            .entries =
                {
                    registry_entry_spec{.name = "minecraft:oak_log",
                                        .protocol_id = 1},
                    registry_entry_spec{.name = "minecraft:air",
                                        .protocol_id = 0},
                    registry_entry_spec{.name = "minecraft:water",
                                        .protocol_id = 2},
                },
        },
        registry_spec{
            .name = "minecraft:biome",
            .entries = {registry_entry_spec{.name = "minecraft:plains",
                                            .protocol_id = 0}},
        },
    };
    spec.items = {
        item_spec{
            .name = "minecraft:stone", .max_stack_size = 64, .max_damage = 0},
        item_spec{.name = "minecraft:oak_stairs",
                  .max_stack_size = 64,
                  .max_damage = 0},
        item_spec{.name = "minecraft:diamond_sword",
                  .max_stack_size = 1,
                  .max_damage = 1561},
    };
    // Light records for the same states: air is transparent, the log states
    // are opaque except the y-axis one which emits, and water damps by one
    // and carries an occlusion shape on its default state.
    light_shape_spec shape{};
    shape.faces[0].push_back({0.0, 0.0, 0.0, 1.0, 0.5, 1.0});
    shape.faces[1].push_back({0.0, 0.0, 0.0, 1.0, 0.5, 1.0});
    spec.light_shapes = {shape};
    for (const auto &block : spec.blocks) {
        for (const auto &state : block.states) {
            block_light_spec light{};
            light.vanilla_id = state.vanilla_id;
            if (block.name == "minecraft:air") {
                light.dampening = 0;
                light.can_occlude = false;
            } else if (block.name == "minecraft:oak_log") {
                light.emission = state.values.front() == "y" ? 15 : 0;
                light.dampening = 15;
            } else {
                light.dampening = 1;
                light.use_shape_for_light_occlusion = true;
                light.shape = state.vanilla_id == 100 ? 0 : no_light_shape;
            }
            spec.block_lights.push_back(light);
        }
    }
    return spec;
}

auto reports() -> vanilla_reports {
    vanilla_reports value;
    value.blocks_json = R"({
        "minecraft:stone": {
            "states": [{"id": 0, "default": true}]
        },
        "minecraft:oak_stairs": {
            "properties": {"facing": ["north", "south"], "half": ["top", "bottom"]},
            "states": [
                {"id": 1, "properties": {"facing": "north", "half": "bottom"}, "default": true},
                {"id": 2, "properties": {"facing": "south", "half": "bottom"}},
                {"id": 3, "properties": {"facing": "north", "half": "top"}},
                {"id": 4, "properties": {"facing": "south", "half": "top"}}
            ]
        }
    })";
    value.biome_presets.emplace_back("minecraft:overworld", R"({
        "biomes": [
            {"biome": "minecraft:plains",
             "parameters": {"temperature": [-0.45, -0.15], "humidity": 0.0,
                            "continentalness": {"min": -0.11, "max": 0.55},
                            "erosion": 0.0, "depth": 0.0, "weirdness": 0.0,
                            "offset": 0.0}},
            {"biome": "minecraft:ocean",
             "parameters": {"temperature": [-1.0, 1.0], "humidity": [-1.0, 1.0],
                            "continentalness": [-1.05, -0.455],
                            "erosion": [-1.0, 1.0], "depth": 0.0,
                            "weirdness": [-1.0, 1.0], "offset": 0.0}}
        ]
    })");
    value.registries_json = R"({
        "minecraft:block": {"entries": {
            "minecraft:stone": {"protocol_id": 1},
            "minecraft:oak_stairs": {"protocol_id": 2}
        }},
        "minecraft:biome": {"entries": {
            "minecraft:plains": {"protocol_id": 0}
        }}
    })";
    value.block_light_json = R"({
        "format": "refrain.block-light/1",
        "game_version": "26.3",
        "data_version": 5023,
        "block_count": 2,
        "state_count": 5,
        "shape_count": 1,
        "states": [
            [0, 0, 15, 1, -1],
            [1, 0, 0, 3, 0],
            [2, 0, 0, 1, -1],
            [3, 14, 1, 1, -1],
            [4, 0, 1, 1, -1]
        ],
        "shapes": [
            [[], [[0.0, 0.0, 0.0, 1.0, 0.5, 1.0]], [], [], [], []]
        ]
    })";
    value.block_mining_json = R"({
        "format": "refrain.block-mining/1",
        "game_version": "26.3",
        "data_version": 5023,
        "block_count": 2,
        "state_count": 5,
        "tool_count": 1,
        "tool_types": ["pickaxe", "axe", "shovel", "hoe", "shears", "sword"],
        "materials": [
            {"name": "wood", "harvest_level": 0},
            {"name": "stone", "harvest_level": 1},
            {"name": "copper", "harvest_level": 1},
            {"name": "iron", "harvest_level": 2},
            {"name": "diamond", "harvest_level": 3},
            {"name": "netherite", "harvest_level": 3},
            {"name": "gold", "harvest_level": 0}
        ],
        "blocks": [
            ["minecraft:oak_stairs", 2.0, 0, 2, 0, 1.0, 1.0, 0],
            ["minecraft:stone", 1.5, 1, 1, 0, 1.0, 1.0, 0]
        ],
        "tools": [
            {
                "id": "minecraft:wooden_pickaxe",
                "tool_type": "pickaxe",
                "material": "wood",
                "harvest_level": 0,
                "tier_speed": 2.0,
                "default_mining_speed": 1.0,
                "damage_per_block": 1,
                "can_destroy_blocks_in_creative": true,
                "max_damage": 59,
                "max_stack_size": 1,
                "rules": []
            }
        ],
        "tags": {}
    })";
    value.item_components.emplace_back("minecraft:stone", R"({
        "components": {"minecraft:max_stack_size": 64}
    })");
    value.item_components.emplace_back("minecraft:diamond_sword", R"({
        "components": {
            "minecraft:damage": 0,
            "minecraft:max_damage": 1561,
            "minecraft:max_stack_size": 1
        }
    })");
    return value;
}

auto build_sample_without_light() -> std::vector<std::byte> {
    auto spec = sample_spec();
    spec.light_shapes.clear();
    spec.block_lights.clear();
    auto bytes = build_vanilla_tables(spec);
    expect(bytes.has_value());
    if (!bytes.has_value()) {
        return {};
    }
    return std::move(*bytes);
}

auto build_sample() -> std::vector<std::byte> {
    auto bytes = build_vanilla_tables(sample_spec());
    expect(bytes.has_value());
    if (!bytes.has_value()) {
        return {};
    }
    return std::move(*bytes);
}

/// Scratch directory removed when the test returns, including early exits.
struct scratch_directory {
    std::filesystem::path path{};

    scratch_directory() {
        const auto stamp{
            std::chrono::steady_clock::now().time_since_epoch().count()};
        path = std::filesystem::temp_directory_path() /
               ("refrain_vanilla_" + std::to_string(stamp));
        std::error_code error;
        std::filesystem::create_directories(path, error);
    }
    scratch_directory(const scratch_directory &) = delete;
    auto operator=(const scratch_directory &) -> scratch_directory & = delete;
    ~scratch_directory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

auto write_text_file(const std::filesystem::path &path, std::string_view text)
    -> bool {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    return refrain::core::io::write_text(path.string(), text).has_value();
}

} // namespace

suite<"data/vanilla-tables"> vanilla_tables_tests = [] {
    "round trip exposes blocks states and properties"_test = [] {
        auto bytes = build_sample();
        auto tables = vanilla_tables::open(bytes);
        expect(tables.has_value());
        if (!tables.has_value()) {
            return;
        }
        expect(tables->block_count() == 3U);
        expect(tables->state_count() == 20U);
        expect(tables->property_count() == 2U);
        expect(tables->preset_count() == 2U);
        expect(tables->registry_count() == 2U);
        expect(tables->item_count() == 3U);
        expect(tables->data_version() == 5023);

        const auto air = tables->find_block("minecraft:air");
        expect(air.has_value());
        expect(tables->block_at(*air).name == "minecraft:air");
        expect(tables->block_at(*air).state_count == 1U);

        const auto sword = tables->find_item("minecraft:diamond_sword");
        expect(sword.has_value());
        if (sword.has_value()) {
            const auto item = tables->item_at(*sword);
            expect(item.name == "minecraft:diamond_sword");
            expect(item.max_stack_size == 1U);
            expect(item.max_damage == 1561U);
        }
        const auto stone = tables->find_item("minecraft:stone");
        expect(stone.has_value());
        if (stone.has_value()) {
            expect(tables->item_at(*stone).max_damage == 0U);
        }
        expect(!tables->find_item("minecraft:missing").has_value());

        const auto oak_log = tables->find_block("minecraft:oak_log");
        expect(oak_log.has_value());
        const auto &block_log = tables->block_at(*oak_log);
        expect(block_log.default_state == block_log.first_state + 1U);
        expect(tables->state_at(block_log.default_state).key ==
               "minecraft:oak_log[axis=y]");
    };

    "state lookup uses the canonical key"_test = [] {
        auto bytes = build_sample();
        auto tables = vanilla_tables::open(bytes);
        expect(tables.has_value());
        if (!tables.has_value()) {
            return;
        }
        const auto state = tables->find_state("minecraft:oak_log[axis=z]");
        expect(state.has_value());
        const auto view = tables->state_at(*state);
        expect(view.vanilla_id == 12);
        expect(view.values.size() == 1U);
        const auto properties = tables->block_properties(view.block);
        expect(properties.size() == 1U);
        expect(tables->property_name(properties[0]) == "axis");
        expect(tables->property_value(properties[0], view.values[0]) == "z");
    };

    "vanilla state id maps back to the dense state"_test = [] {
        auto bytes = build_sample();
        auto tables = vanilla_tables::open(bytes);
        expect(tables.has_value());
        if (!tables.has_value()) {
            return;
        }
        const auto state = tables->state_by_vanilla_id(11);
        expect(state.has_value());
        expect(tables->state_at(*state).key == "minecraft:oak_log[axis=y]");
        expect(!tables->state_by_vanilla_id(17).has_value());
    };

    "biome preset order is preserved"_test = [] {
        auto bytes = build_sample();
        auto tables = vanilla_tables::open(bytes);
        expect(tables.has_value());
        if (!tables.has_value()) {
            return;
        }
        const auto preset = tables->find_preset("minecraft:overworld");
        expect(preset.has_value());
        expect(tables->biome_parameter_count(*preset) == 2U);
        const auto first = tables->biome_parameter(*preset, 0);
        expect(first.biome == "minecraft:plains");
        expect(first.min[0] == -10000);
        expect(first.max[0] == 10000);
        const auto second = tables->biome_parameter(*preset, 1);
        expect(second.biome == "minecraft:ocean");
        expect(second.max[2] == -4550);
        expect(second.offset == 2500);
    };

    "registries are sorted and keep protocol ids"_test = [] {
        auto bytes = build_sample();
        auto tables = vanilla_tables::open(bytes);
        expect(tables.has_value());
        if (!tables.has_value()) {
            return;
        }
        const auto registry = tables->find_registry("minecraft:block");
        expect(registry.has_value());
        expect(tables->registry_entry_count(*registry) == 3U);
        expect(tables->registry_entry(*registry, 0).name == "minecraft:air");
        expect(tables->registry_entry(*registry, 0).protocol_id == 0);
        expect(tables->registry_entry(*registry, 2).name == "minecraft:water");
        expect(tables->registry_entry(*registry, 2).protocol_id == 2);
    };

    "building is deterministic and order independent"_test = [] {
        auto first = build_vanilla_tables(sample_spec());
        expect(first.has_value());
        vanilla_spec reversed = sample_spec();
        std::reverse(reversed.blocks.begin(), reversed.blocks.end());
        std::reverse(reversed.biome_presets.begin(),
                     reversed.biome_presets.end());
        std::reverse(reversed.registries.begin(), reversed.registries.end());
        std::reverse(reversed.items.begin(), reversed.items.end());
        auto second = build_vanilla_tables(reversed);
        expect(second.has_value());
        if (first.has_value() && second.has_value()) {
            expect(std::equal(first->begin(), first->end(), second->begin(),
                              second->end()));
        }
    };

    "reports convert into a readable container"_test = [] {
        auto bytes = build_vanilla_from_reports(reports(), 5023);
        expect(bytes.has_value());
        if (!bytes.has_value()) {
            return;
        }
        auto tables = vanilla_tables::open(*bytes);
        expect(tables.has_value());
        if (!tables.has_value()) {
            return;
        }
        expect(tables->block_count() == 2U);
        expect(tables->state_count() == 5U);
        const auto stairs = tables->find_block("minecraft:oak_stairs");
        expect(stairs.has_value());
        const auto state =
            tables->find_state("minecraft:oak_stairs[facing=north,half=top]");
        expect(state.has_value());
        expect(tables->state_at(*state).vanilla_id == 3);
        const auto preset = tables->find_preset("minecraft:overworld");
        expect(preset.has_value());
        expect(tables->biome_parameter_count(*preset) == 2U);
        // -0.45F * 10000.0F rounds to -4500.0F, then truncates to -4500.
        expect(tables->biome_parameter(*preset, 0).min[0] == -4500);
        expect(tables->biome_parameter(*preset, 0).max[1] == 0);
        const auto registry = tables->find_registry("minecraft:block");
        expect(registry.has_value());
        expect(tables->registry_entry_count(*registry) == 2U);
        expect(tables->item_count() == 2U);
        const auto sword = tables->find_item("minecraft:diamond_sword");
        expect(sword.has_value());
        if (sword.has_value()) {
            expect(tables->item_at(*sword).max_stack_size == 1U);
            expect(tables->item_at(*sword).max_damage == 1561U);
        }
        expect(tables->find_item("minecraft:stone").has_value());
        expect(tables->has_mining());
        expect(tables->tool_count() == 1U);
        const auto stone_block = tables->find_block("minecraft:stone");
        expect(stone_block.has_value());
        if (stone_block.has_value()) {
            const auto mining = tables->block_mining_at(*stone_block);
            expect(mining.hardness == 1.5F);
            expect(mining.requires_correct_tool_for_drops());
        }
        const auto pickaxe = tables->find_tool("minecraft:wooden_pickaxe");
        expect(pickaxe.has_value());
        if (pickaxe.has_value()) {
            expect(tables->tool_at(*pickaxe).tier_speed == 2.0F);
        }
    };

    "invalid specifications are rejected"_test = [] {
        auto duplicate = sample_spec();
        duplicate.blocks.push_back(air_block());
        expect(!build_vanilla_tables(duplicate).has_value());

        auto no_default = sample_spec();
        no_default.blocks[0].states[1].is_default = false;
        expect(!build_vanilla_tables(no_default).has_value());

        auto two_defaults = sample_spec();
        two_defaults.blocks[0].states[0].is_default = true;
        expect(!build_vanilla_tables(two_defaults).has_value());

        auto wrong_values = sample_spec();
        wrong_values.blocks[0].states[0].values = {"diagonal"};
        expect(!build_vanilla_tables(wrong_values).has_value());

        auto no_default_water = sample_spec();
        no_default_water.blocks[2].states[0].is_default = false;
        expect(!build_vanilla_tables(no_default_water).has_value());

        auto duplicate_item = sample_spec();
        duplicate_item.items.push_back(item_spec{.name = "minecraft:stone"});
        expect(!build_vanilla_tables(duplicate_item).has_value());

        auto empty_item = sample_spec();
        empty_item.items.push_back(item_spec{.name = ""});
        expect(!build_vanilla_tables(empty_item).has_value());

        auto oversized_stack = sample_spec();
        oversized_stack.items.push_back(
            item_spec{.name = "minecraft:stack", .max_stack_size = 100});
        expect(!build_vanilla_tables(oversized_stack).has_value());
    };

    "corrupted containers are rejected"_test = [] {
        // The digest-only case needs trailing padding after the last section,
        // so the fixture here carries no light sections.
        auto bytes = build_sample_without_light();
        auto wrong_magic = bytes;
        wrong_magic[0] = std::byte{'X'};
        expect(!vanilla_tables::open(wrong_magic).has_value());

        auto truncated = bytes;
        truncated.resize(32);
        expect(!vanilla_tables::open(truncated).has_value());

        auto flipped = bytes;
        const auto last = flipped.size() - 1;
        flipped[last] = static_cast<std::byte>(
            std::to_integer<unsigned>(flipped[last]) ^ 0xFFU);
        expect(!vanilla_tables::open(flipped).has_value());
        // Structural validation still holds, so an explicit no-verify open
        // succeeds and keeps the corruption detectable by digest only.
        expect(vanilla_tables::open(flipped, false).has_value());
    };

    "round trip exposes per-state light records and occlusion shapes"_test =
        [] {
            auto bytes = build_sample();
            auto tables = vanilla_tables::open(bytes);
            expect(tables.has_value());
            if (!tables.has_value()) {
                return;
            }
            expect(tables->has_light());
            expect(tables->light_shape_count() == 1U);
            const auto air{tables->find_state("minecraft:air")};
            const auto log{tables->find_state("minecraft:oak_log[axis=y]")};
            const auto water{tables->find_state("minecraft:water[level=0]")};
            expect(air.has_value());
            expect(log.has_value());
            expect(water.has_value());
            if (air.has_value()) {
                const auto light{tables->light_at(*air)};
                expect(light.dampening == 0);
                expect(!light.can_occlude);
            }
            if (log.has_value()) {
                const auto light{tables->light_at(*log)};
                expect(light.emission == 15);
                expect(light.dampening == 15);
            }
            if (water.has_value()) {
                const auto light{tables->light_at(*water)};
                expect(light.dampening == 1);
                expect(light.use_shape_for_light_occlusion);
                expect(light.shape != no_light_shape);
                const auto shape{tables->light_shape_at(light.shape)};
                expect(shape.face_boxes[0] == 1U);
                expect(shape.face_boxes[2] == 0U);
                const auto box{tables->light_box(light.shape, 0, 0)};
                expect(box.bounds[0] == 0.0);
                expect(box.bounds[4] == 0.5);
            }
        };

    "a container without light sections opens and reports no light data"_test =
        [] {
            auto spec = sample_spec();
            spec.light_shapes.clear();
            spec.block_lights.clear();
            auto bytes = build_vanilla_tables(spec);
            expect(bytes.has_value());
            if (!bytes.has_value()) {
                return;
            }
            auto tables = vanilla_tables::open(*bytes);
            expect(tables.has_value());
            if (!tables.has_value()) {
                return;
            }
            expect(!tables->has_light());
            expect(tables->light_shape_count() == 0U);
            const auto state{tables->find_state("minecraft:air")};
            expect(state.has_value());
            if (state.has_value()) {
                // The view answers the documented fallback for a container
                // that does not carry light data.
                const auto light{tables->light_at(*state)};
                expect(light.dampening == 15);
                expect(light.emission == 0);
                expect(light.shape == no_light_shape);
            }
        };

    "light records must cover every state exactly once"_test = [] {
        auto duplicate = sample_spec();
        duplicate.block_lights.pop_back();
        duplicate.block_lights.push_back(duplicate.block_lights.front());
        expect(!build_vanilla_tables(duplicate).has_value());

        auto missing = sample_spec();
        missing.block_lights.pop_back();
        expect(!build_vanilla_tables(missing).has_value());

        auto out_of_range = sample_spec();
        out_of_range.block_lights.front().emission = 16;
        expect(!build_vanilla_tables(out_of_range).has_value());

        auto bad_shape = sample_spec();
        bad_shape.block_lights.front().shape = 7;
        expect(!build_vanilla_tables(bad_shape).has_value());

        auto bad_box = sample_spec();
        bad_box.light_shapes.front().faces[0].front()[0] = 2.0;
        expect(!build_vanilla_tables(bad_box).has_value());
    };

    "filesystem import writes tables and a manifest"_test = [] {
        const scratch_directory scratch;
        const auto reports_dir = scratch.path / "reports";
        const auto out_dir = scratch.path / "out";
        const auto fixture = reports();
        expect(
            write_text_file(reports_dir / "blocks.json", fixture.blocks_json));
        expect(write_text_file(reports_dir / "registries.json",
                               fixture.registries_json));
        expect(write_text_file(reports_dir / "biome_parameters" / "minecraft" /
                                   "overworld.json",
                               fixture.biome_presets.at(0).second));
        expect(write_text_file(reports_dir / "block_light" / "light.json",
                               fixture.block_light_json));
        expect(write_text_file(reports_dir / "block_mining" / "mining.json",
                               fixture.block_mining_json));
        for (const auto &[name, json] : fixture.item_components) {
            const auto short_name{name.substr(
                name.find(':') == std::string::npos ? 0 : name.find(':') + 1)};
            expect(write_text_file(reports_dir / "minecraft" / "components" /
                                       "item" / (short_name + ".json"),
                                   json));
        }

        vanilla_import_options options;
        options.reports = reports_dir.string();
        options.out = out_dir.string();
        options.data_version = 5023;
        auto imported = import_vanilla_reports(options);
        expect(imported.has_value())
            << (imported ? "" : imported.error().message);
        if (!imported.has_value()) {
            return;
        }
        expect(imported->blocks == 2U);
        expect(imported->states == 5U);
        expect(imported->presets == 1U);
        expect(imported->registries == 2U);
        expect(imported->items == 2U);
        expect(imported->light_shapes == 1U);
        expect(imported->tools == 1U);

        auto tables_bytes =
            refrain::core::io::read_file((out_dir / "tables.rvtb").string());
        expect(tables_bytes.has_value());
        if (tables_bytes.has_value()) {
            expect(tables_bytes->size() == imported->tables_bytes);
            expect(refrain::data::to_hex(refrain::data::blake3(
                       *tables_bytes)) == imported->tables_digest);
            auto tables = vanilla_tables::open(*tables_bytes);
            expect(tables.has_value());
            if (tables.has_value()) {
                expect(tables->block_count() == 2U);
                expect(tables->item_count() == 2U);
                expect(tables->has_light());
                expect(tables->light_shape_count() == 1U);
                expect(tables->has_mining());
                expect(tables->tool_count() == 1U);
                const auto stairs{tables->find_state(
                    "minecraft:oak_stairs[facing=south,half=top]")};
                expect(stairs.has_value());
                if (stairs.has_value()) {
                    const auto light{tables->light_at(*stairs)};
                    expect(light.emission == 0);
                    expect(light.dampening == 1);
                    expect(light.shape == no_light_shape);
                }
                const auto default_stairs{tables->find_state(
                    "minecraft:oak_stairs[facing=north,half=bottom]")};
                expect(default_stairs.has_value());
                if (default_stairs.has_value()) {
                    const auto light{tables->light_at(*default_stairs)};
                    expect(light.shape != no_light_shape);
                    const auto shape{tables->light_shape_at(light.shape)};
                    expect(shape.face_boxes[1] == 1U);
                    const auto box{tables->light_box(light.shape, 1, 0)};
                    expect(box.bounds[4] == 0.5);
                }
                expect(tables
                           ->find_state(
                               "minecraft:oak_stairs[facing=south,half=top]")
                           .has_value());
            }
        }

        auto manifest =
            refrain::core::io::read_text((out_dir / "MANIFEST.json").string());
        expect(manifest.has_value());
        if (manifest.has_value()) {
            expect(manifest->find("refrain.vanilla-tables/1") !=
                   std::string::npos);
            expect(manifest->find("\"data_version\": 5023") !=
                   std::string::npos);
            expect(manifest->find("\"game_version\": \"26.3\"") !=
                   std::string::npos);
            expect(manifest->find("\"path\": \"blocks.json\"") !=
                   std::string::npos);
            expect(manifest->find("\"path\": \"tables.rvtb\"") !=
                   std::string::npos);
        }

        // Existing output is kept unless the caller opts into an overwrite.
        expect(!import_vanilla_reports(options).has_value());
        options.force = true;
        expect(import_vanilla_reports(options).has_value());
    };
};
