// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/feature/feature_plan.hpp>

#include <boost/ut.hpp>

#include <string>
#include <vector>

using namespace boost::ut;
using namespace refrain::worldgen::feature;

suite<"worldgen/feature-plan"> feature_plan_tests = [] {
    "the per-step order follows the reference topological sort"_test = [] {
        const std::vector<biome_feature_list> lists{
            {.biome = "a", .steps = {{"a", "b"}, {"c"}}},
            {.biome = "b", .steps = {{"b"}, {"d"}}},
        };
        const auto plan = feature_sorter::build({"a", "b"}, lists);
        expect(plan.has_value());
        if (!plan) {
            return;
        }
        expect(plan->size() == 2U);
        if (plan->size() != 2U) {
            return;
        }
        expect((*plan)[0].features.size() == 2U);
        expect((*plan)[0].features[0] == "a");
        expect((*plan)[0].features[1] == "b");
        expect((*plan)[0].index.at("a") == 0);
        expect((*plan)[0].index.at("b") == 1);
        expect((*plan)[1].features.size() == 2U);
        expect((*plan)[1].features[0] == "d");
        expect((*plan)[1].features[1] == "c");
        expect((*plan)[1].index.at("d") == 0);
        expect((*plan)[1].index.at("c") == 1);
    };

    "cycles are rejected"_test = [] {
        const std::vector<biome_feature_list> lists{
            {.biome = "a", .steps = {{"a", "b"}}},
            {.biome = "b", .steps = {{"b", "a"}}},
        };
        expect(!feature_sorter::build({"a", "b"}, lists).has_value());
    };

    "steps without features are preserved"_test = [] {
        const std::vector<biome_feature_list> lists{
            {.biome = "a", .steps = {{}, {"a"}}},
        };
        const auto plan = feature_sorter::build({"a"}, lists);
        expect(plan.has_value());
        if (plan) {
            expect(plan->size() == 2U);
            expect((*plan)[0].features.empty());
            expect((*plan)[1].features.size() == 1U);
        }
    };
};
