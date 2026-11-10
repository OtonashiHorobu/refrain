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
    return spec;
}

auto reports() -> vanilla_reports {
    vanilla_reports value;
    value.blocks_json = R"({
        "minecraft:stone": {
            "states": [{"id": 1, "default": true}]
        },
        "minecraft:oak_stairs": {
            "properties": {"facing": ["north", "south"], "half": ["top", "bottom"]},
            "states": [
                {"id": 20, "properties": {"facing": "north", "half": "bottom"}, "default": true},
                {"id": 21, "properties": {"facing": "south", "half": "bottom"}},
                {"id": 22, "properties": {"facing": "north", "half": "top"}},
                {"id": 23, "properties": {"facing": "south", "half": "top"}}
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
    return value;
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
        expect(tables->data_version() == 5023);

        const auto air = tables->find_block("minecraft:air");
        expect(air.has_value());
        expect(tables->block_at(*air).name == "minecraft:air");
        expect(tables->block_at(*air).state_count == 1U);

        const auto oak_log = tables->find_block("minecraft:oak_log");
        expect(oak_log.has_value());
        const auto &log = tables->block_at(*oak_log);
        expect(log.default_state == log.first_state + 1U);
        expect(tables->state_at(log.default_state).key ==
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
        expect(tables->state_at(*state).vanilla_id == 22);
        const auto preset = tables->find_preset("minecraft:overworld");
        expect(preset.has_value());
        expect(tables->biome_parameter_count(*preset) == 2U);
        // -0.45F * 10000.0F rounds to -4500.0F, then truncates to -4500.
        expect(tables->biome_parameter(*preset, 0).min[0] == -4500);
        expect(tables->biome_parameter(*preset, 0).max[1] == 0);
        const auto registry = tables->find_registry("minecraft:block");
        expect(registry.has_value());
        expect(tables->registry_entry_count(*registry) == 2U);
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
    };

    "corrupted containers are rejected"_test = [] {
        auto bytes = build_sample();
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

        vanilla_import_options options;
        options.reports = reports_dir.string();
        options.out = out_dir.string();
        options.data_version = 5023;
        auto imported = import_vanilla_reports(options);
        expect(imported.has_value()) << imported.error().message;
        if (!imported.has_value()) {
            return;
        }
        expect(imported->blocks == 2U);
        expect(imported->states == 5U);
        expect(imported->presets == 1U);
        expect(imported->registries == 2U);

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
