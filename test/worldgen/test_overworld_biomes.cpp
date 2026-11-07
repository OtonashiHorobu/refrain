// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The overworld parameter table is generated in code by
// `OverworldBiomeBuilder`. The fingerprint covers every entry (biome key and
// all quantized intervals) in order; the selection indices come from the same
// synthetic targets querying the reference `Climate.ParameterList`. Both are
// printed by `BiomeVectors.java`.

#include <refrain/worldgen/biome/climate.hpp>
#include <refrain/worldgen/biome/overworld_biome_builder.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;
using namespace refrain::worldgen;

constexpr std::size_t expected_count = 7594;
constexpr std::uint64_t expected_fingerprint = 0x3ca050df6d1a6e32ULL;

constexpr std::array<std::uint16_t, 256> expected_indices{
    7590, 1384, 12,   10,   15,   3,    2,    3476, 6916, 6,    6,    5108,
    7591, 7591, 18,   3896, 7590, 2566, 4,    3306, 18,   20,   2673, 1848,
    2170, 6,    3309, 5728, 5430, 7200, 6,    1594, 21,   5732, 3308, 3838,
    1578, 2910, 10,   5,    7591, 11,   7590, 2588, 6926, 1924, 6,    2442,
    4,    20,   1430, 3280, 5544, 6,    3931, 4044, 7,    4,    7591, 4334,
    7590, 7590, 7591, 3399, 7590, 6,    7590, 1366, 7591, 21,   122,  7591,
    4430, 9,    2716, 7592, 5,    14,   7590, 5965, 4174, 4672, 19,   7591,
    3918, 7591, 8,    4038, 8,    1832, 3404, 14,   18,   14,   10,   3895,
    8,    19,   10,   7590, 4,    19,   10,   1697, 5936, 7591, 10,   14,
    20,   3828, 746,  18,   10,   3610, 16,   6538, 7591, 4,    6134, 2600,
    6054, 6146, 7590, 5358, 2,    7590, 14,   3802, 15,   16,   8,    4314,
    6784, 18,   14,   7590, 1172, 3716, 2941, 18,   7590, 860,  3838, 7591,
    5156, 906,  7590, 7590, 7591, 7591, 6,    18,   7591, 5196, 1270, 7592,
    3830, 6,    7,    7590, 3306, 7,    5712, 4116, 1028, 10,   7590, 5238,
    1022, 14,   3,    2580, 7591, 6928, 5832, 6,    4561, 12,   7590, 602,
    1136, 7446, 6818, 3304, 7591, 6325, 2,    5144, 5904, 308,  3,    7591,
    1732, 3426, 3836, 7591, 6,    20,   7591, 6890, 18,   5176, 7591, 3234,
    1376, 16,   2,    10,   7591, 3762, 5624, 2592, 2,    7590, 18,   1009,
    4075, 2,    3310, 5656, 7591, 4700, 3306, 16,   5202, 18,   2622, 3,
    2,    4,    4802, 1446, 7591, 7592, 3426, 4178, 4,    3918, 18,   7590,
    18,   5442, 7591, 1178, 7590, 996,  4256, 5534, 10,   6068, 16,   1916,
    3782, 7591, 2,    2587,
};

auto fnv1a(std::uint64_t hash, std::string_view text) -> std::uint64_t {
    for (const char character : text) {
        hash ^=
            static_cast<std::uint64_t>(static_cast<unsigned char>(character));
        hash *= 0x100000001B3ULL;
    }
    return hash;
}

auto fingerprint() -> std::uint64_t {
    std::uint64_t hash = 0xCBF29CE484222325ULL;
    for (const auto &entry : overworld_parameters()) {
        const auto &point = entry.parameters;
        std::string line = entry.biome;
        for (const auto &interval :
             {point.temperature, point.humidity, point.continentalness,
              point.erosion, point.depth, point.weirdness}) {
            line += '|';
            line += std::to_string(interval.min);
            line += ',';
            line += std::to_string(interval.max);
        }
        line += '|';
        line += std::to_string(point.offset);
        line += '\n';
        hash = fnv1a(hash, line);
    }
    return hash;
}

struct lcg {
    std::uint64_t state{0x5DEECE66DULL};

    auto next() -> std::uint64_t {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return state;
    }
    auto coord() -> jlong {
        return static_cast<jlong>((next() >> 33U) % 20001ULL) - 10000;
    }
};

} // namespace

suite<"worldgen/overworld-biomes"> overworld_biome_tests = [] {
    "the generated parameter table matches the reference fingerprint"_test =
        [] {
            const auto parameters = overworld_parameters();
            expect(parameters.size() == expected_count);
            expect(fingerprint() == expected_fingerprint);
        };

    "the reference selections reproduce on synthetic targets"_test = [] {
        const auto parameters = overworld_parameters();
        std::vector<climate::parameter_list::entry> entries;
        entries.reserve(parameters.size());
        for (std::size_t i = 0; i < parameters.size(); ++i) {
            entries.push_back(climate::parameter_list::entry{
                .parameters = parameters[i].parameters,
                .value = static_cast<std::uint16_t>(i),
            });
        }
        climate::parameter_list list{std::move(entries)};

        lcg random;
        std::size_t mismatches = 0;
        for (std::size_t i = 0; i < expected_indices.size(); ++i) {
            const climate::target_point target{
                .temperature = random.coord(),
                .humidity = random.coord(),
                .continentalness = random.coord(),
                .erosion = random.coord(),
                .depth = random.coord(),
                .weirdness = random.coord(),
            };
            const auto selected = list.find_index(target);
            if (selected != expected_indices[i]) {
                if (mismatches < 8) {
                    std::fprintf(stderr, "target %zu: got %zu want %u\n", i,
                                 selected,
                                 static_cast<unsigned>(expected_indices[i]));
                }
                ++mismatches;
            }
        }
        expect(mismatches == 0) << "selection mismatches: " << mismatches;
    };
};
