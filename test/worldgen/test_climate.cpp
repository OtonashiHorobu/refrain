// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// `Climate.ParameterList` selection. The fixture is synthetic: 25 random
// quantized parameter points and 64 random targets, plus the selections printed
// by the reference class (`ClimateVectors.java`).

#include <refrain/core/io.hpp>
#include <refrain/data/json.hpp>
#include <refrain/worldgen/biome/climate.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace boost::ut;
using namespace refrain;
using namespace refrain::worldgen;
using json = data::json::value;

auto read_fixture() -> std::unique_ptr<json> {
    const auto text = core::io::read_text(
        REFRAIN_SOURCE_DIR "/test/fixtures/worldgen/climate_vectors.json");
    if (!text) {
        return nullptr;
    }
    auto parsed = json::parse(*text);
    if (!parsed) {
        return nullptr;
    }
    return std::make_unique<json>(std::move(*parsed));
}

} // namespace

suite<"worldgen/climate"> climate_tests = [] {
    "quantized coordinates truncate toward zero"_test = [] {
        expect(climate::quantize(0.12345F) == 1234);
        expect(climate::quantize(-0.12345F) == -1234);
        expect(climate::quantize(1.0F) == 10000);
        expect(climate::unquantize(1234) == 0.1234F);
        expect(climate::unquantize(-10000) == -1.0F);
        expect(climate::quantize(std::numeric_limits<float>::quiet_NaN()) == 0);
        expect(climate::quantize(1.0E30F) ==
               std::numeric_limits<climate::coord>::max());
        expect(climate::quantize(-1.0E30F) ==
               std::numeric_limits<climate::coord>::min());
    };

    "parameter distance clamps to the interval"_test = [] {
        const auto value = climate::span(-100, 200);
        expect(value.distance(-150) == 50);
        expect(value.distance(-100) == 0);
        expect(value.distance(0) == 0);
        expect(value.distance(200) == 0);
        expect(value.distance(260) == 60);
        expect(value.span(climate::span(50, 400)) == climate::span(-100, 400));
    };

    "fitness sums squared distances in seven dimensions"_test = [] {
        const climate::parameter_point point{
            .temperature = climate::span(-100, 100),
            .humidity = climate::span(-100, 100),
            .continentalness = climate::span(-100, 100),
            .erosion = climate::span(-100, 100),
            .depth = climate::span(-100, 100),
            .weirdness = climate::span(-100, 100),
            .offset = 3,
        };
        const climate::target_point target{
            .temperature = 104,
            .humidity = -103,
            .continentalness = 0,
            .erosion = 0,
            .depth = 0,
            .weirdness = 100,
        };
        // 4^2 + 3^2 + 3^2 = 34.
        expect(point.fitness(target) == 34);
    };

    "the parameter list matches the reference selections"_test = [] {
        const auto fixture = read_fixture();
        expect(fixture != nullptr);
        if (fixture == nullptr) {
            return;
        }
        const auto *points_value = fixture->find("points");
        const auto *targets_value = fixture->find("targets");
        const auto *rtree_value = fixture->find("expected_rtree");
        const auto *brute_value = fixture->find("expected_brute");
        if (points_value == nullptr || targets_value == nullptr ||
            rtree_value == nullptr || brute_value == nullptr) {
            expect(false) << "fixture is incomplete";
            return;
        }
        const auto points = points_value->as_array();
        const auto targets = targets_value->as_array();
        const auto rtree = rtree_value->as_array();
        const auto brute = brute_value->as_array();
        expect(points.has_value() && targets.has_value() && rtree.has_value() &&
               brute.has_value());
        if (!points || !targets || !rtree || !brute) {
            return;
        }

        std::vector<climate::parameter_list::entry> entries;
        entries.reserve(points->size());
        for (std::size_t i = 0; i < points->size(); ++i) {
            const auto &point = (*points)[i];
            std::array<climate::parameter, 6> space{};
            for (std::size_t d = 0; d < space.size(); ++d) {
                const auto *min_value = point.find("min");
                const auto *max_value = point.find("max");
                if (min_value == nullptr || max_value == nullptr) {
                    expect(false) << "point is incomplete";
                    return;
                }
                const auto min = min_value->as_array();
                const auto max = max_value->as_array();
                if (!min || !max) {
                    expect(false) << "interval is not an array";
                    return;
                }
                const auto min_coord = (*min)[d].as_double();
                const auto max_coord = (*max)[d].as_double();
                if (!min_coord || !max_coord) {
                    expect(false) << "interval is not numeric";
                    return;
                }
                space[d] =
                    climate::span(static_cast<climate::coord>(*min_coord),
                                  static_cast<climate::coord>(*max_coord));
            }
            const auto *offset_value = point.find("offset");
            std::optional<double> offset;
            if (offset_value != nullptr) {
                if (const auto parsed = offset_value->as_double(); parsed) {
                    offset = *parsed;
                }
            }
            if (!offset) {
                expect(false) << "point has no offset";
                return;
            }
            entries.push_back(climate::parameter_list::entry{
                .parameters =
                    climate::parameter_point{
                        .temperature = space[0],
                        .humidity = space[1],
                        .continentalness = space[2],
                        .erosion = space[3],
                        .depth = space[4],
                        .weirdness = space[5],
                        .offset = static_cast<climate::coord>(*offset),
                    },
                .value = static_cast<std::uint16_t>(i),
            });
        }

        climate::parameter_list list{std::move(entries)};
        expect(targets->size() == rtree->size());
        expect(targets->size() == brute->size());
        for (std::size_t i = 0; i < targets->size(); ++i) {
            const auto &target_value = (*targets)[i];
            const auto target_array = target_value.as_array();
            const auto expected_rtree = (*rtree)[i].as_double();
            const auto expected_brute = (*brute)[i].as_double();
            if (!target_array || !expected_rtree || !expected_brute) {
                expect(false) << "target row is malformed";
                return;
            }
            std::array<climate::coord, 6> coordinates{};
            for (std::size_t d = 0; d < coordinates.size(); ++d) {
                const auto value = (*target_array)[d].as_double();
                if (!value) {
                    expect(false) << "target coordinate is not numeric";
                    return;
                }
                coordinates[d] = static_cast<climate::coord>(*value);
            }
            const climate::target_point target{
                .temperature = coordinates[0],
                .humidity = coordinates[1],
                .continentalness = coordinates[2],
                .erosion = coordinates[3],
                .depth = coordinates[4],
                .weirdness = coordinates[5],
            };
            const auto selected = list.find(target);
            const auto selected_brute = list.find_brute_force(target);
            if (selected != static_cast<std::uint16_t>(*expected_rtree) ||
                selected_brute != static_cast<std::uint16_t>(*expected_brute)) {
                std::fprintf(stderr, "target %zu: got %u/%u want %u/%u\n", i,
                             static_cast<unsigned>(selected),
                             static_cast<unsigned>(selected_brute),
                             static_cast<unsigned>(*expected_rtree),
                             static_cast<unsigned>(*expected_brute));
            }
            expect(selected == static_cast<std::uint16_t>(*expected_rtree));
            expect(selected_brute ==
                   static_cast<std::uint16_t>(*expected_brute));
        }
    };
};
