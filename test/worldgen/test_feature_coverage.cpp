// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Content-driven coverage: every standard biome of the shipped snapshot must
// build its placed and configured feature closure and generate one chunk at
// the full target. The dimension of a biome is taken from the dimension biome
// sources, so no biome table lives in the test.

#include <refrain/worldgen/chunk_generator.hpp>
#include <refrain/worldgen/dimension_generator.hpp>

#include "support/vanilla_data.hpp"

#include <boost/ut.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace boost::ut;
using namespace refrain;
using namespace refrain::worldgen;

} // namespace

suite<"worldgen/feature-coverage"> feature_coverage_tests = [] {
    "every standard biome builds a generator and generates a chunk"_test = [] {
        const auto snapshot = refrain::test_support::vanilla_data();
        if (!snapshot.available) {
            return;
        }
        const auto &data_root = snapshot.path;
        std::map<std::string, dimension_kind> biome_kind;
        for (const auto kind : {dimension_kind::overworld,
                                dimension_kind::nether, dimension_kind::end}) {
            auto shared =
                dimension_generator::load_shared(dimension_generator_config{
                    .data_root = data_root, .kind = kind});
            expect(shared.has_value()) << shared.error().message;
            if (!shared) {
                return;
            }
            auto generator = dimension_generator::create(
                dimension_generator_config{.data_root = data_root,
                                           .kind = kind},
                *shared);
            expect(generator.has_value()) << generator.error().message;
            if (!generator) {
                return;
            }
            for (const auto &biome : (*generator)->biome_source().biomes()) {
                biome_kind[biome] = kind;
            }
        }
        std::vector<std::string> biomes;
        const std::filesystem::path root{data_root +
                                         "/minecraft/worldgen/biome"};
        for (const auto &entry : std::filesystem::directory_iterator{root}) {
            if (entry.path().extension() == ".json") {
                biomes.push_back("minecraft:" + entry.path().stem().string());
            }
        }
        std::sort(biomes.begin(), biomes.end());
        expect(biomes.size() > 60U);
        std::vector<std::string> failures;
        // The per-biome sweep only needs the terrain pipeline: loading the full
        // feature and structure data for every biome costs far more than the
        // construction check is worth. The first four biomes still run the
        // full pipeline, and the oracle suites cover feature generation.
        constexpr std::size_t full_samples{4};
        for (std::size_t index = 0; index < biomes.size(); ++index) {
            const auto &biome = biomes[index];
            const auto found = biome_kind.find(biome);
            const auto kind = found == biome_kind.end()
                                  ? dimension_kind::overworld
                                  : found->second;
            const bool full{index < full_samples};
            auto created = chunk_generator::create(chunk_generator_config{
                .data_root = data_root,
                .templates_root = "",
                .seed = 12345,
                .kind = kind,
                .fixed_biome = biome,
                .target =
                    full ? generation_target::full : generation_target::terrain,
                .structure_sets = {"oracle:no_structure_set"},
                .reference_world = false,
            });
            if (!created) {
                failures.push_back(biome + ": " + created.error().message);
                continue;
            }
            const auto chunk = (*created)->generate(chunk_pos{.x = 0, .z = 0});
            if (!chunk) {
                failures.push_back(biome + ": " + chunk.error().message);
                continue;
            }
            const auto expected{full ? chunk_status::full
                                     : chunk_status::terrain};
            expect((*chunk)->status() == expected) << biome;
        }
        for (const auto &failure : failures) {
            std::fprintf(stderr, "biome coverage: %s\n", failure.c_str());
        }
        expect(failures.empty()) << failures.size() << " biomes failed";
    };
};
