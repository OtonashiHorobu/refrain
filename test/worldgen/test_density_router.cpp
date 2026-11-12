// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Synthetic datapack: the documents below are written for this test only.

#include <refrain/core/io.hpp>
#include <refrain/worldgen/density/density.hpp>
#include <refrain/worldgen/density/density_router.hpp>
#include <refrain/worldgen/noise/noise.hpp>

#include "support/scratch_directory.hpp"

#include <boost/ut.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

namespace {

using namespace boost::ut;
using namespace refrain::worldgen;
using namespace refrain::worldgen::density;

auto bits(float value) -> std::uint32_t {
    std::uint32_t out = 0;
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

using scratch_directory = refrain::test_support::scratch_directory;

auto write_text_file(const std::filesystem::path &path, std::string_view text)
    -> bool {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    return refrain::core::io::write_text(path.string(), text).has_value();
}

auto make_settings_json(std::string_view final_density) -> std::string {
    return std::string{"{"
                       "\"noise\":{\"min_y\":-64,\"height\":384},"
                       "\"default_block\":\"minecraft:stone\","
                       "\"default_fluid\":\"minecraft:water\","
                       "\"material_rule\":\"minecraft:overworld\","
                       "\"sea_level\":63,"

                       "\"noise_router\":{"
                       "\"temperature\":\"testns:temperature\","
                       "\"vegetation\":\"testns:temperature\","
                       "\"continents\":\"testns:temperature\","
                       "\"erosion\":\"testns:temperature\","
                       "\"depth\":\"testns:temperature\","
                       "\"ridges\":\"testns:temperature\","
                       "\"final_density\":\""} +
           std::string{final_density} +
           "\","
           "\"chunk_surface_level\":\"testns:temperature\""
           "}}";
}

} // namespace

suite<"worldgen/density-router"> density_router_tests = [] {
    "noise settings load and assemble the router"_test = [] {
        const scratch_directory scratch;
        const auto root = scratch.path() / "data";
        expect(write_text_file(
            root / "testns" / "worldgen" / "density_function" /
                "temperature.json",
            R"({"type":"minecraft:gradient","axis":"y","from_coordinate":-64,"to_coordinate":320,"from_value":0.0,"to_value":1.0})"));
        expect(write_text_file(
            root / "testns" / "worldgen" / "density_function" / "final.json",
            R"({"type":"minecraft:noise","noise":"testns:noise","xz_scale":0.25,"y_scale":0.5})"));
        expect(write_text_file(root / "testns" / "worldgen" / "noise" /
                                   "noise.json",
                               R"({"base_octave":0,"octave_count":3})"));
        expect(write_text_file(root / "testns" / "worldgen" / "noise_settings" /
                                   "settings.json",
                               make_settings_json("testns:final")));

        auto settings = load_noise_settings(root.string(), "testns/settings",
                                            algorithm::xoroshiro, 12345);
        if (!settings.has_value()) {
            std::fprintf(stderr, "loader error: %s\n",
                         settings.error().message.c_str());
        }
        expect(settings.has_value()) << settings.error().message;
        if (!settings) {
            return;
        }
        expect(settings->default_block == "minecraft:stone");
        expect(settings->default_fluid == "minecraft:water");
        expect(settings->sea_level == 63);
        expect(!settings->aquifers);

        const sampler_context context;
        expect(bits(settings->router.temperature->sample_value(context, 0, 64,
                                                               0)) ==
               bits(gradient_y(-64, 320, 0.0F, 1.0F)
                        ->sample_value(context, 0, 64, 0)));

        normal_noise_parameters parameters;
        parameters.base_octave = 0;
        parameters.octave_count = 3;
        const normal_noise normal{parameters};
        auto source = new_positional_factory(algorithm::xoroshiro, 12345)
                          ->from_hash_of("testns:noise");
        auto expected_stack = normal.create(*source);
        const auto expected = density::noise(
            std::make_shared<noise_stack>(std::move(expected_stack)), 0.25,
            0.5);
        for (const auto &point : std::array<std::array<jint, 3>, 4>{{
                 {{0, 0, 0}},
                 {{5, 40, -3}},
                 {{-7, 63, 11}},
                 {{100, -64, -100}},
             }}) {
            expect(bits(settings->router.final_density->sample_value(
                       context, point[0], point[1], point[2])) ==
                   bits(expected->sample_value(context, point[0], point[1],
                                               point[2])))
                << "final density at " << point[0] << "," << point[1] << ","
                << point[2];
        }
    };

    "the vanilla overworld router decodes when pointed at a datapack"_test =
        [] {
            const char *root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
            if (root == nullptr) {
                return;
            }
            auto settings = load_noise_settings(root, "minecraft/overworld",
                                                algorithm::xoroshiro, 12345);
            if (!settings.has_value()) {
                std::fprintf(stderr, "vanilla loader error: %s\n",
                             settings.error().message.c_str());
            }
            expect(settings.has_value()) << settings.error().message;
            if (!settings) {
                return;
            }
            expect(settings->default_block == "minecraft:stone");
            expect(settings->default_fluid == "minecraft:water");
            expect(settings->sea_level == 63);

            const sampler_context context;
            const volume vol{16, 16, 16, 0, -64, 0};
            buffer values{vol.size()};
            settings->router.final_density->sample_volume(context, values, vol);
            for (std::size_t i = 0; i < values.size(); ++i) {
                expect(std::isfinite(values.get(i)));
            }

            if (settings->aquifers) {
                auto factory =
                    new_positional_factory(algorithm::xoroshiro, 12345);
                const volume chunk_vol{16, 384, 16, 0, -64, 0};
                const terrain::fluid_picker picker{
                    .sea_level = 63, .default_fluid = 2, .lava = 3, .air = 0};
                const terrain::block_ids blocks{.air = 0,
                                                .default_block = 1,
                                                .default_fluid = 2,
                                                .lava = 3};
                const sampler_context fill_context;
                terrain::noise_aquifer aquifer{
                    settings->aquifer, *factory, chunk_vol, picker, blocks,
                    fill_context};
                const terrain::fill_options options{.min_y = -64,
                                                    .height = 384,
                                                    .sea_level = 63,
                                                    .blocks = blocks};
                const auto chunk = terrain::fill_chunk(
                    options, *settings->router.final_density, fill_context, 0,
                    0, aquifer);
                std::size_t stone = 0;
                std::size_t air = 0;
                for (const auto state : chunk.states) {
                    stone += state == 1 ? 1U : 0U;
                    air += state == 0 ? 1U : 0U;
                }
                expect(stone > 1000U);
                expect(air > 1000U);
                expect(stone + air <= chunk.states.size());
            }
        };

    "a router without final_density is rejected"_test = [] {
        const scratch_directory scratch;
        const auto root = scratch.path() / "data";
        expect(write_text_file(root / "testns" / "worldgen" /
                                   "density_function" / "temperature.json",
                               R"({"type":"minecraft:constant","value":0.0})"));
        expect(write_text_file(
            root / "testns" / "worldgen" / "noise_settings" / "settings.json",
            R"({"noise_router":{"temperature":"testns:temperature"}})"));
        expect(!load_noise_settings(root.string(), "testns/settings",
                                    algorithm::xoroshiro, 1)
                    .has_value());
    };
};
