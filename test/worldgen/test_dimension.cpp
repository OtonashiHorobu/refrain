// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/dimension.hpp>

#include <boost/ut.hpp>

namespace {
using namespace boost::ut;
using namespace refrain::worldgen;

suite<"worldgen/dimension"> dimension_tests = [] {
    "the overworld profile uses the full build height"_test = [] {
        const auto &profile = dimension_profile_for(dimension_kind::overworld);
        expect(profile.min_y == -64);
        expect(profile.height == 384);
        expect(profile.logical_height == 384);
        expect(profile.sea_level == 63);
        expect(profile.has_skylight);
        expect(!profile.has_ceiling);
        expect(profile.noise_settings == "minecraft/overworld");
    };

    "the nether profile has a ceiling and scaled coordinates"_test = [] {
        const auto &profile = dimension_profile_for(dimension_kind::nether);
        expect(profile.min_y == 0);
        expect(profile.height == 256);
        expect(profile.logical_height == 128);
        expect(profile.sea_level == 32);
        expect(profile.coordinate_scale == 8.0);
        expect(!profile.has_skylight);
        expect(profile.has_ceiling);
        expect(profile.noise_settings == "minecraft/nether");
        expect(profile.material_rule == "minecraft/nether");
    };

    "the end profile has no sea and no ceiling"_test = [] {
        const auto &profile = dimension_profile_for(dimension_kind::end);
        expect(profile.min_y == 0);
        expect(profile.height == 256);
        expect(profile.logical_height == 256);
        expect(profile.sea_level == 0);
        expect(profile.has_skylight);
        expect(!profile.has_ceiling);
        expect(profile.noise_settings == "minecraft/end");
    };

    "dimension names use the dimension ids"_test = [] {
        expect(dimension_name(dimension_kind::overworld) ==
               "minecraft:overworld");
        expect(dimension_name(dimension_kind::nether) ==
               "minecraft:the_nether");
        expect(dimension_name(dimension_kind::end) == "minecraft:the_end");
    };
};
} // namespace
