// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/json.hpp>
#include <refrain/worldgen/provider.hpp>

#include <boost/ut.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string_view>

namespace {

using refrain::data::json::value;
using refrain::worldgen::legacy_random_source;
using refrain::worldgen::worldgen_random;
using refrain::worldgen::provider::float_provider;
using refrain::worldgen::provider::generation_context;
using refrain::worldgen::provider::height_provider;
using refrain::worldgen::provider::int_provider;

auto parse(std::string_view text) -> value {
    auto parsed = value::parse(text);
    if (!parsed) {
        return value{};
    }
    return std::move(*parsed);
}

auto pool() -> worldgen_random {
    return worldgen_random{std::make_unique<legacy_random_source>(12345)};
}

} // namespace

using namespace boost::ut;
using namespace refrain::worldgen::provider;

suite<"worldgen/provider"> provider_tests = [] {
    "integer providers decode inline constants and ranges"_test = [] {
        auto random = pool();
        const auto constant = int_provider::decode(parse("7"));
        expect(constant.has_value());
        if (constant) {
            expect((*constant)->sample(random) == 7);
        }

        const auto uniform = int_provider::decode(parse(
            R"({"type":"minecraft:uniform","min_inclusive":2,"max_inclusive":5})"));
        expect(uniform.has_value());
        if (uniform) {
            for (int i = 0; i < 200; ++i) {
                const auto value = (*uniform)->sample(random);
                expect(value >= 2 && value <= 5);
            }
        }

        const auto biased = int_provider::decode(parse(
            R"({"type":"minecraft:biased_to_bottom","min_inclusive":0,"max_inclusive":10})"));
        expect(biased.has_value());
        if (biased) {
            expect((*biased)->sample(random) >= 0);
            expect((*biased)->sample(random) <= 10);
        }

        const auto very_biased = int_provider::decode(parse(
            R"({"type":"minecraft:very_biased_to_bottom","min_inclusive":0,"max_inclusive":14})"));
        expect(very_biased.has_value());

        const auto trapezoid = int_provider::decode(parse(
            R"({"type":"minecraft:trapezoid","min":-2,"max":2,"plateau":0})"));
        expect(trapezoid.has_value());
        if (trapezoid) {
            bool negative = false;
            bool positive = false;
            for (int i = 0; i < 100; ++i) {
                const auto value = (*trapezoid)->sample(random);
                expect(value >= -2 && value <= 2);
                negative = negative || value < 0;
                positive = positive || value > 0;
            }
            expect(negative);
            expect(positive);
        }
    };

    "weighted lists select their entries"_test = [] {
        auto random = pool();
        const auto provider = int_provider::decode(parse(
            R"({"type":"minecraft:weighted_list","distribution":[{"data":7,"weight":3},{"data":9,"weight":1}]})"));
        expect(provider.has_value());
        if (!provider) {
            return;
        }
        for (int i = 0; i < 100; ++i) {
            const auto value = (*provider)->sample(random);
            expect(value == 7 || value == 9);
        }
    };

    "float providers decode constants, ranges and trapezoids"_test = [] {
        auto random = pool();
        const auto constant = float_provider::decode(parse("1.5"));
        expect(constant.has_value());
        if (constant) {
            expect((*constant)->sample(random) == 1.5F);
        }
        const auto uniform = float_provider::decode(parse(
            R"({"type":"minecraft:uniform","min_inclusive":0.5,"max_exclusive":1.5})"));
        expect(uniform.has_value());
        if (uniform) {
            for (int i = 0; i < 100; ++i) {
                const auto value = (*uniform)->sample(random);
                expect(value >= 0.5F && value < 1.5F);
            }
        }
        const auto trapezoid = float_provider::decode(parse(
            R"({"type":"minecraft:trapezoid","min":-1.0,"max":1.0,"plateau":0.0})"));
        expect(trapezoid.has_value());
        if (trapezoid) {
            const auto value = (*trapezoid)->sample(random);
            expect(value >= -1.0F && value <= 1.0F);
        }
    };

    "height providers resolve anchors"_test = [] {
        auto random = pool();
        const generation_context context{
            .min_y = -64, .height = 384, .sea_level = 63};

        const auto constant =
            height_provider::decode(parse(R"({"absolute":10})"));
        expect(constant.has_value());
        if (constant) {
            expect((*constant)->sample(random, context) == 10);
        }
        const auto above =
            height_provider::decode(parse(R"({"above_bottom":8})"));
        expect(above.has_value());
        if (above) {
            expect((*above)->sample(random, context) == -56);
        }
        const auto sea =
            height_provider::decode(parse(R"({"relative_to_sea_level":5})"));
        expect(sea.has_value());
        if (sea) {
            expect((*sea)->sample(random, context) == 68);
        }

        const auto uniform = height_provider::decode(parse(
            R"({"type":"minecraft:uniform","min_inclusive":{"above_bottom":0},"max_inclusive":{"absolute":5}})"));
        expect(uniform.has_value());
        if (uniform) {
            for (int i = 0; i < 50; ++i) {
                const auto value = (*uniform)->sample(random, context);
                expect(value >= -64 && value <= 5);
            }
        }
        const auto very_biased = height_provider::decode(parse(
            R"({"type":"minecraft:very_biased_to_bottom","min_inclusive":{"above_bottom":8},"max_inclusive":{"absolute":180}})"));
        expect(very_biased.has_value());
        if (very_biased) {
            expect((*very_biased)->sample(random, context) >= -56);
        }
    };

    "malformed providers are rejected"_test = [] {
        auto normal = float_provider::decode(parse(
            R"({"type":"minecraft:clamped_normal","mean":0.1,"deviation":0.3,"min":0.1,"max":0.9})"));
        expect(normal.has_value());
        if (normal) {
            auto random = pool();
            const float expected[] = {0.1F, 0.2765309F, 0.38464144F, 0.1F,
                                      0.1F};
            for (const auto value : expected) {
                expect(std::abs((*normal)->sample(random) - value) < 1e-6F);
            }
        }
        expect(!int_provider::decode(parse("{}")).has_value());
        expect(
            !int_provider::decode(
                 parse(
                     R"({"type":"minecraft:uniform","min_inclusive":5,"max_inclusive":1})"))
                 .has_value());
        expect(
            !float_provider::decode(
                 parse(
                     R"({"type":"minecraft:uniform","min_inclusive":1.0,"max_exclusive":1.0})"))
                 .has_value());
        expect(
            !height_provider::decode(
                 parse(
                     R"({"type":"minecraft:clamped_normal","mean":0.0,"deviation":1.0,"min_inclusive":0,"max_inclusive":5})"))
                 .has_value());
        expect(!int_provider::decode(parse(R"({"type":"minecraft:unknown"})"))
                    .has_value());
    };
};
