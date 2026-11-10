// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/biome/biome_source.hpp>
#include <refrain/worldgen/density/density.hpp>
#include <refrain/worldgen/density/interval.hpp>

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace refrain::worldgen;

/// Returns the block X coordinate, so quart-to-block mapping is observable.
class x_sampler final : public density::sampler {
  public:
    [[nodiscard]] auto sample_value(const density::sampler_context &, jint x,
                                    jint, jint) const -> float override {
        return static_cast<float>(x);
    }
    void sample_volume(const density::sampler_context &, buffer &output,
                       const volume &vol) const override {
        std::size_t index = 0;
        for (jint z = 0; z < vol.size_z(); ++z) {
            for (jint x = 0; x < vol.size_x(); ++x) {
                for (jint y = 0; y < vol.size_y(); ++y) {
                    output.set(index, static_cast<float>(vol.block_x(x)));
                    ++index;
                }
            }
        }
    }
    [[nodiscard]] auto range() const -> interval override {
        return interval::infinite();
    }
};

auto make_source() -> multi_noise_biome_source {
    return multi_noise_biome_source{{
        biome_parameter{
            .parameters =
                climate::parameter_point{
                    .temperature = climate::span(-10000, 0),
                    .humidity = climate::span(-10000, 10000),
                    .continentalness = climate::span(-10000, 10000),
                    .erosion = climate::span(-10000, 10000),
                    .depth = climate::span(-10000, 10000),
                    .weirdness = climate::span(-10000, 10000),
                    .offset = 0,
                },
            .biome = "test:cold",
        },
        biome_parameter{
            .parameters =
                climate::parameter_point{
                    .temperature = climate::span(1, 10000),
                    .humidity = climate::span(-10000, 10000),
                    .continentalness = climate::span(-10000, 10000),
                    .erosion = climate::span(-10000, 10000),
                    .depth = climate::span(-10000, 10000),
                    .weirdness = climate::span(-10000, 10000),
                    .offset = 0,
                },
            .biome = "test:warm",
        },
        biome_parameter{
            .parameters =
                climate::parameter_point{
                    .temperature = climate::span(1, 10000),
                    .humidity = climate::span(5000, 10000),
                    .continentalness = climate::span(-10000, 10000),
                    .erosion = climate::span(-10000, 10000),
                    .depth = climate::span(-10000, 10000),
                    .weirdness = climate::span(-10000, 10000),
                    .offset = 0,
                },
            .biome = "test:warm",
        },
    }};
}

} // namespace

using namespace boost::ut;

suite<"worldgen/biome-source"> biome_source_tests = [] {
    "the climate sampler quantizes the block coordinates of a quart"_test = [] {
        const x_sampler x;
        const auto zero = density::constant(0.0F);
        const density::sampler_context context;
        const climate_sampler sampler{x,     *zero, x,      *zero,
                                      *zero, *zero, context};
        const auto target = sampler.sample(3, -2, 5);
        expect(target.temperature == climate::quantize(12.0F));
        expect(target.temperature == 120000);
        expect(target.humidity == 0);
        expect(target.continentalness == 120000);
        expect(target.depth == 0);

        const auto negative = sampler.sample(-4, 1, 0);
        expect(negative.temperature == -160000);
    };

    "the source interns biome keys in first-seen order"_test = [] {
        const auto source = make_source();
        expect(source.biomes().size() == 2U);
        expect(source.biomes()[0] == "test:cold");
        expect(source.biomes()[1] == "test:warm");
        expect(source.biome(0) == "test:cold");
        expect(source.biome(1) == "test:warm");
        expect(source.parameters().size() == 3U);
    };

    "the nearest parameter point selects the biome"_test = [] {
        auto source = make_source();
        const auto zero = density::constant(0.0F);
        const auto temperature = density::gradient_y(0, 100, 0.0F, 1.0F);
        const density::sampler_context context;
        // x is unused by the source, so only the temperature gradient matters.
        const climate_sampler sampler{*temperature, *zero, *zero,  *zero,
                                      *zero,        *zero, context};
        // quart y absorbs the block Y: gradient reaches 0.25 at block Y = 25.
        expect(source.noise_biome(sampler, 0, 0, 0) == 0);
        expect(source.noise_biome(sampler, 0, 55, 0) == 1);
        expect(source.noise_biome(sampler, 5, -3, 7) == 0);
        expect(source.noise_biome(sampler, -2, 60, 1) == 1);
    };

    "the chunk grid resolves the same biomes as scalar sampling"_test = [] {
        auto source = make_source();
        const auto zero = density::constant(0.0F);
        const auto temperature = density::gradient_y(-64, 320, -1.0F, 1.0F);
        const density::sampler_context context;
        const climate_sampler sampler{*temperature, *zero, *zero,  *zero,
                                      *zero,        *zero, context};

        constexpr jint min_x = -2;
        constexpr jint min_y = 0;
        constexpr jint min_z = 3;
        constexpr jint size = 4;
        biome_grid grid{source, sampler, min_x, min_y, min_z, size, size, size};
        for (jint z = 0; z < size; ++z) {
            for (jint x = 0; x < size; ++x) {
                for (jint y = 0; y < size; ++y) {
                    const auto quart_x = min_x + x;
                    const auto quart_y = min_y + y;
                    const auto quart_z = min_z + z;
                    const auto expected =
                        source.noise_biome(sampler, quart_x, quart_y, quart_z);
                    expect(grid.at(quart_x, quart_y, quart_z) == expected);
                }
            }
        }
    };
};
