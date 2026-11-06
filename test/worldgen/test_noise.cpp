// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Gradient-noise vectors generated from the real 26.3 classes with
// `tools/worldgen/vectors/run.sh <server.jar> NoiseVectors`.

#include <refrain/worldgen/noise.hpp>
#include <refrain/worldgen/rng.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

namespace {

using namespace boost::ut;
using namespace refrain::worldgen;

auto bits(float value) -> std::uint32_t {
    std::uint32_t out = 0;
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

constexpr std::array<std::array<double, 3>, 6> points_3d{{
    {0.0, 0.0, 0.0},
    {1.5, -2.25, 3.75},
    {-100.25, 64.5, 200.125},
    {12345.678, -64.5, -9876.543},
    {1.0e8, 1.0e8, 1.0e8},
    {-1.0e8, 0.5, 1.0e8},
}};

constexpr std::array<std::array<double, 2>, 4> points_2d{{
    {0.0, 0.0},
    {1.5, -2.25},
    {-100.25, 200.125},
    {1.0e8, 1.0e8},
}};

auto make_factory(jlong seed) -> std::unique_ptr<positional_random_factory> {
    return new_positional_factory(algorithm::xoroshiro, seed);
}

auto temperature_parameters() -> normal_noise_parameters {
    normal_noise_parameters parameters;
    parameters.base_amplitude = 1.2453007926713473;
    parameters.base_octave = -10;
    parameters.octave_count = 6;
    parameters.amplitude_modifiers = {1.5, 0.0, 1.0, 0.0, 0.0, 0.0};
    return parameters;
}

auto sample_3d(const noise_stack &noise) -> std::array<std::uint32_t, 6> {
    std::array<std::uint32_t, 6> out{};
    for (std::size_t i = 0; i < points_3d.size(); ++i) {
        out[i] =
            bits(noise.get(points_3d[i][0], points_3d[i][1], points_3d[i][2]));
    }
    return out;
}

auto sample_2d(const noise_stack &noise) -> std::array<std::uint32_t, 4> {
    std::array<std::uint32_t, 4> out{};
    for (std::size_t i = 0; i < points_2d.size(); ++i) {
        out[i] = bits(noise.get(points_2d[i][0], points_2d[i][1]));
    }
    return out;
}

} // namespace

suite<"worldgen/noise"> noise_tests = [] {
    "perlin noise reproduces the reference permutation and samples"_test = [] {
        legacy_random_source random{42};
        const perlin_noise noise{random};
        expect(noise.perm(0) == 70U);
        expect(noise.perm(255) == 27U);

        constexpr std::array<std::uint32_t, 6> expected3d =
            std::array<std::uint32_t, 6>{0x3d900f9cU, 0x3d380b50U, 0x3e1fecc3U,
                                         0x3efc53d4U, 0x3d900f9cU, 0xbe00e2f4U};
        constexpr std::array<std::uint32_t, 4> expected2d =
            std::array<std::uint32_t, 4>{0x3d900f9cU, 0x3e932468U, 0xbe19c4e4U,
                                         0x3d900f9cU};
        auto actual3d = std::array<std::uint32_t, 6>{};
        auto actual2d = std::array<std::uint32_t, 4>{};
        for (std::size_t i = 0; i < points_3d.size(); ++i) {
            actual3d[i] = bits(
                noise.get(points_3d[i][0], points_3d[i][1], points_3d[i][2]));
        }
        for (std::size_t i = 0; i < points_2d.size(); ++i) {
            actual2d[i] = bits(noise.get(points_2d[i][0], points_2d[i][1]));
        }
        for (std::size_t i = 0; i < expected3d.size(); ++i) {
            expect(actual3d[i] == expected3d[i]) << "3d sample " << i;
        }
        for (std::size_t i = 0; i < expected2d.size(); ++i) {
            expect(actual2d[i] == expected2d[i]) << "2d sample " << i;
        }
    };

    "temperature noise matches the overworld parameter table"_test = [] {
        auto factory = make_factory(12345);
        auto source = factory->from_hash_of("minecraft:temperature");
        const normal_noise normal{temperature_parameters()};
        expect(bits(-normal.range()) == 0xc00db004U);

        const auto stack = normal.create(*source);
        constexpr std::array<std::uint32_t, 6> expected3d =
            std::array<std::uint32_t, 6>{0xbcca9ff2U, 0xbce91fa7U, 0xbe0bc24aU,
                                         0xbd9d5df8U, 0xbe6789a1U, 0xbdb4a985U};
        constexpr std::array<std::uint32_t, 4> expected2d =
            std::array<std::uint32_t, 4>{0xbcca9ff2U, 0xbcd7081bU, 0xbdbe607aU,
                                         0x3f13774dU};
        const auto actual3d = sample_3d(stack);
        const auto actual2d = sample_2d(stack);
        for (std::size_t i = 0; i < expected3d.size(); ++i) {
            expect(actual3d[i] == expected3d[i]) << "3d sample " << i;
        }
        for (std::size_t i = 0; i < expected2d.size(); ++i) {
            expect(actual2d[i] == expected2d[i]) << "2d sample " << i;
        }
    };

    "three octaves from the origin match the reference"_test = [] {
        auto factory = make_factory(12345);
        auto source = factory->from_hash_of("refrain:noise_test");
        normal_noise_parameters parameters;
        parameters.base_octave = 0;
        parameters.octave_count = 3;
        const normal_noise normal{parameters};
        expect(bits(-normal.range()) == 0xc0000000U);

        const auto stack = normal.create(*source);
        constexpr std::array<std::uint32_t, 6> expected3d =
            std::array<std::uint32_t, 6>{0x3dad0033U, 0x3f16dc9fU, 0x3e1f0169U,
                                         0xbe3234faU, 0xbce820f8U, 0x3ebd00a4U};
        constexpr std::array<std::uint32_t, 4> expected2d =
            std::array<std::uint32_t, 4>{0x3dad0033U, 0x3f48d4b8U, 0xbf70eb0fU,
                                         0xbcf120f3U};
        const auto actual3d = sample_3d(stack);
        const auto actual2d = sample_2d(stack);
        for (std::size_t i = 0; i < expected3d.size(); ++i) {
            expect(actual3d[i] == expected3d[i]) << "3d sample " << i;
        }
        for (std::size_t i = 0; i < expected2d.size(); ++i) {
            expect(actual2d[i] == expected2d[i]) << "2d sample " << i;
        }
    };

    "parity construction matches the legacy normalisation"_test = [] {
        auto factory = make_factory(12345);
        auto source = factory->from_hash_of("refrain:parity_test");
        constexpr std::array<double, 6> amplitudes{1.5, 0.0, 1.0,
                                                   0.0, 0.0, 0.0};
        const auto normal = normal_noise::create_parity(-10, amplitudes);
        expect(bits(-normal.range()) == 0xc00db004U);

        const auto stack = normal.create(*source);
        constexpr std::array<std::uint32_t, 6> expected3d =
            std::array<std::uint32_t, 6>{0xbf52a35dU, 0xbf5022faU, 0xbebfc70eU,
                                         0x3eb3d711U, 0x3e87d416U, 0xbe7942baU};
        constexpr std::array<std::uint32_t, 4> expected2d =
            std::array<std::uint32_t, 4>{0xbf52a35dU, 0xbf535e30U, 0xbeba4532U,
                                         0x3d95c86dU};
        const auto actual3d = sample_3d(stack);
        const auto actual2d = sample_2d(stack);
        for (std::size_t i = 0; i < expected3d.size(); ++i) {
            expect(actual3d[i] == expected3d[i]) << "3d sample " << i;
        }
        for (std::size_t i = 0; i < expected2d.size(); ++i) {
            expect(actual2d[i] == expected2d[i]) << "2d sample " << i;
        }
    };

    "stack construction is deterministic for one source"_test = [] {
        auto first = make_factory(12345)->from_hash_of("refrain:noise_test");
        auto second = make_factory(12345)->from_hash_of("refrain:noise_test");
        normal_noise_parameters parameters;
        parameters.base_octave = 0;
        parameters.octave_count = 3;
        const normal_noise normal{parameters};
        const auto a = normal.create(*first);
        const auto b = normal.create(*second);
        expect(sample_3d(a) == sample_3d(b));
    };
};
