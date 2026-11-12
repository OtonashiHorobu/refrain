// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/io.hpp>
#include <refrain/worldgen/feature/feature_data.hpp>

#include "support/scratch_directory.hpp"

#include <boost/ut.hpp>

#include <filesystem>
#include <string_view>

using namespace boost::ut;
using namespace refrain::worldgen::feature;

namespace {

auto write(std::filesystem::path path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    expect(refrain::core::io::write_text(path.string(), text).has_value());
}

} // namespace

suite<"worldgen/feature-data"> feature_data_tests = [] {
    "feature documents, placements and biome lists load"_test = [] {
        const refrain::test_support::scratch_directory scratch;
        const auto root = scratch.path();
        write(
            root / "minecraft/worldgen/feature/simple_block.json",
            R"({"type":"minecraft:simple_block","to_place":{"type":"minecraft:simple_state_provider","state":{"Name":"minecraft:stone"}}})");
        write(root / "minecraft/worldgen/feature/tree/oak.json",
              R"({"type":"minecraft:tree"})");
        write(
            root / "minecraft/worldgen/placed_feature/stone.json",
            R"({"feature":"minecraft:simple_block","placement":[{"type":"minecraft:count","count":3}]})");
        write(root / "minecraft/worldgen/biome/plains.json",
              R"({"features":[[],["minecraft:stone"]]})");

        auto data = load_feature_data(root.string());
        expect(data.has_value());
        if (!data) {
            return;
        }
        expect(data->features.size() == 2U);
        expect(data->features.contains("minecraft:simple_block"));
        expect(data->features.contains("minecraft:tree/oak"));
        expect(data->placed.size() == 1U);
        const auto placed = data->placed.find("minecraft:stone");
        expect(placed != data->placed.end());
        if (placed != data->placed.end()) {
            expect(placed->second.feature == "minecraft:simple_block");
            expect(placed->second.placement.is_array());
        }
        expect(data->biomes.size() == 1U);
        if (!data->biomes.empty()) {
            expect(data->biomes[0].biome == "minecraft:plains");
            expect(data->biomes[0].steps.size() == 2U);
            expect(data->biomes[0].steps[1].size() == 1U);
            expect(data->biomes[0].steps[1][0] == "minecraft:stone");
        }
    };

    "a placed feature without placement is rejected"_test = [] {
        const refrain::test_support::scratch_directory scratch;
        const auto root = scratch.path();
        write(root / "minecraft/worldgen/placed_feature/bad.json",
              R"({"feature":"minecraft:simple_block"})");
        expect(!load_feature_data(root.string()).has_value());
    };
};
