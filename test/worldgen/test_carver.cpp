// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/json.hpp>
#include <refrain/worldgen/carver/carver.hpp>

#include <boost/ut.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string_view>

using namespace boost::ut;
using namespace refrain::worldgen;
using refrain::worldgen::carver::canyon_carver;
using refrain::worldgen::carver::carve_ellipsoid;
using refrain::worldgen::carver::cave_carver;
using refrain::worldgen::carver::decode_carver;
using refrain::worldgen::carver::skip_checker;
using refrain::worldgen::carver::world_carver;

namespace {

auto parse(std::string_view text) -> refrain::data::json::value {
    auto parsed = refrain::data::json::value::parse(text);
    return parsed ? std::move(*parsed) : refrain::data::json::value{};
}

auto cave_json() -> refrain::data::json::value {
    return parse(R"({
      "type": "minecraft:cave",
      "probability": 0.15,
      "y": {"type": "minecraft:uniform", "min_inclusive": {"above_bottom": 8}, "max_inclusive": {"absolute": 180}},
      "count": {"type": "minecraft:very_biased_to_bottom", "min_inclusive": 0, "max_inclusive": 14},
      "thickness": {"type": "minecraft:trapezoid", "min": 0.0, "max": 3.0, "plateau": 1.0},
      "weird_thickness_bias": true,
      "room_vertical_radius_multiplier": {"type": "minecraft:uniform", "min_inclusive": 0.1, "max_exclusive": 0.9},
      "horizontal_radius_multiplier": {"type": "minecraft:uniform", "min_inclusive": 0.7, "max_exclusive": 1.4},
      "vertical_radius_multiplier": {"type": "minecraft:uniform", "min_inclusive": 0.8, "max_exclusive": 1.3},
      "floor_level": {"type": "minecraft:uniform", "min_inclusive": -1.0, "max_exclusive": -0.4}
    })");
}

auto canyon_json() -> refrain::data::json::value {
    return parse(R"({
      "type": "minecraft:canyon",
      "probability": 0.01,
      "y": {"type": "minecraft:uniform", "min_inclusive": {"absolute": 10}, "max_inclusive": {"absolute": 67}},
      "vertical_rotation": {"type": "minecraft:uniform", "min_inclusive": -0.125, "max_exclusive": 0.125},
      "shape": {
        "distance_factor": {"type": "minecraft:uniform", "min_inclusive": 0.75, "max_exclusive": 1.0},
        "thickness": {"type": "minecraft:trapezoid", "min": 0.0, "max": 6.0, "plateau": 2.0},
        "width_smoothness": 3,
        "horizontal_radius_factor": {"type": "minecraft:uniform", "min_inclusive": 0.75, "max_exclusive": 1.0},
        "vertical_radius_default_factor": 1.0,
        "vertical_radius_center_factor": 0.0,
        "y_scale": 3.0
      }
    })");
}

auto make_random(jlong seed = 12345) -> worldgen_random {
    return worldgen_random{std::make_unique<legacy_random_source>(seed)};
}

} // namespace

suite<"worldgen/carver"> carver_tests = [] {
    "the ellipsoid carver matches the normalized test"_test = [] {
        carving_mask mask{-10, 10};
        const chunk_pos target{.x = 0, .z = 0};
        const skip_checker skip = [](double xd, double yd, double zd, jint) {
            return xd * xd + yd * yd + zd * zd >= 1.0;
        };
        constexpr double x = 8.0;
        constexpr double y = 0.0;
        constexpr double z = 8.0;
        constexpr double hr = 3.5;
        constexpr double vr = 2.5;
        carve_ellipsoid(target, x, y, z, hr, vr, mask, skip);

        std::size_t expected = 0;
        for (jint bx = 0; bx < 16; ++bx) {
            for (jint bz = 0; bz < 16; ++bz) {
                for (jint by = mask.min_y() + 1; by <= mask.max_y(); ++by) {
                    const double xd = (bx + 0.5 - x) / hr;
                    const double yd = (by - 0.5 - y) / vr;
                    const double zd = (bz + 0.5 - z) / hr;
                    if (!skip(xd, yd, zd, by)) {
                        ++expected;
                    }
                }
            }
        }
        expect(mask.count() == expected);
        expect(mask.count() > 0U);
    };

    "cave and canyon configurations decode and carve"_test = [] {
        const auto cave = decode_carver(cave_json());
        expect(cave.has_value());
        if (cave) {
            auto random = make_random();
            bool started = false;
            for (int i = 0; i < 200; ++i) {
                started = started || (*cave)->is_start_chunk(random);
            }
            expect(started);

            carving_mask mask{-63, 312};
            auto carving_random = make_random(7);
            expect((*cave)->carve(provider::generation_context{.min_y = -64,
                                                               .height = 384,
                                                               .sea_level = 63},
                                  carving_random, chunk_pos{.x = 0, .z = 0},
                                  chunk_pos{.x = 0, .z = 0}, mask));
            expect(!mask.empty());
        }

        const auto canyon = decode_carver(canyon_json());
        expect(canyon.has_value());
        if (canyon) {
            carving_mask mask{-63, 312};
            auto random = make_random(11);
            expect((*canyon)->carve(
                provider::generation_context{
                    .min_y = -64, .height = 384, .sea_level = 63},
                random, chunk_pos{.x = 0, .z = 0}, chunk_pos{.x = 0, .z = 0},
                mask));
            expect(!mask.empty());
        }
    };

    "carving is deterministic for the same seed"_test = [] {
        const auto cave = decode_carver(cave_json());
        expect(cave.has_value());
        if (!cave) {
            return;
        }
        carving_mask first_mask{-63, 312};
        carving_mask second_mask{-63, 312};
        auto first_random = make_random(999);
        auto second_random = make_random(999);
        const provider::generation_context context{
            .min_y = -64, .height = 384, .sea_level = 63};
        expect((*cave)->carve(context, first_random, chunk_pos{.x = 0, .z = 0},
                              chunk_pos{.x = 0, .z = 0}, first_mask));
        expect((*cave)->carve(context, second_random, chunk_pos{.x = 0, .z = 0},
                              chunk_pos{.x = 0, .z = 0}, second_mask));
        expect(first_mask.count() == second_mask.count());
        std::size_t same = 0;
        for (jint bx = 0; bx < 16; ++bx) {
            for (jint bz = 0; bz < 16; ++bz) {
                for (jint by = -63; by <= 312; ++by) {
                    same += first_mask.test(bx, by, bz) ==
                            second_mask.test(bx, by, bz);
                }
            }
        }
        expect(same == 16U * 16U * 376U);
    };

    "unknown carvers are rejected"_test = [] {
        expect(!decode_carver(parse(R"({"type":"minecraft:unknown"})"))
                    .has_value());
        expect(
            !decode_carver(parse(R"({"type":"minecraft:cave"})")).has_value());
    };
};
