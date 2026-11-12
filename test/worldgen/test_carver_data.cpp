// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/io.hpp>
#include <refrain/worldgen/carver/carver_data.hpp>
#include <refrain/worldgen/carver/carver_stage.hpp>

#include "support/scratch_directory.hpp"

#include <boost/ut.hpp>

#include <filesystem>
#include <string_view>

using namespace boost::ut;
using namespace refrain::worldgen;

namespace {

constexpr std::string_view cave_document = R"({
  "type": "minecraft:cave",
  "probability": 0.15,
  "y": {"type": "minecraft:uniform", "min_inclusive": {"above_bottom": 8}, "max_inclusive": {"absolute": 180}},
  "count": {"type": "minecraft:very_biased_to_bottom", "min_inclusive": 0, "max_inclusive": 14},
  "thickness": {"type": "minecraft:trapezoid", "min": 0.0, "max": 3.0, "plateau": 1.0},
  "room_vertical_radius_multiplier": {"type": "minecraft:uniform", "min_inclusive": 0.1, "max_exclusive": 0.9},
  "horizontal_radius_multiplier": {"type": "minecraft:uniform", "min_inclusive": 0.7, "max_exclusive": 1.4},
  "vertical_radius_multiplier": {"type": "minecraft:uniform", "min_inclusive": 0.8, "max_exclusive": 1.3},
  "floor_level": {"type": "minecraft:uniform", "min_inclusive": -1.0, "max_exclusive": -0.4}
})";

auto write(std::filesystem::path path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    expect(refrain::core::io::write_text(path.string(), text).has_value());
}

} // namespace

suite<"worldgen/carver-data"> carver_data_tests = [] {
    "carvers and biome lists load from a data root"_test = [] {
        const refrain::test_support::scratch_directory scratch;
        const auto root = scratch.path();
        write(root / "minecraft/worldgen/carver/cave.json", cave_document);
        write(root / "minecraft/worldgen/biome/plains.json",
              R"({"carvers":["minecraft:cave"]})");
        write(root / "minecraft/worldgen/biome/desert.json",
              R"({"carvers":[]})");

        auto loaded = carver::load_carvers(root.string());
        expect(loaded.has_value());
        if (!loaded) {
            return;
        }
        expect(loaded->carvers.size() == 1U);
        expect(loaded->biomes.by_biome.size() == 2U);
        const auto plains = loaded->biomes.by_biome.find("minecraft:plains");
        expect(plains != loaded->biomes.by_biome.end());
        if (plains != loaded->biomes.by_biome.end()) {
            expect(plains->second.size() == 1U);
        }

        carver::carver_generator generator{
            [](jint, jint) -> std::string_view { return "minecraft:plains"; },
            loaded->biomes, 12345,
            provider::generation_context{
                .min_y = -64, .height = 384, .sea_level = 63}};
        const auto mask = generator.generate(chunk_pos{.x = 0, .z = 0});
        expect(!mask.empty());
    };

    "a missing carver reference is rejected"_test = [] {
        const refrain::test_support::scratch_directory scratch;
        const auto root = scratch.path();
        write(root / "minecraft/worldgen/carver/cave.json", cave_document);
        write(root / "minecraft/worldgen/biome/plains.json",
              R"({"carvers":["minecraft:missing"]})");
        const auto loaded = carver::load_carvers(root.string());
        expect(!loaded.has_value());
    };
};
