// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/biome/biome_view.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/density/density.hpp>

#include <boost/ut.hpp>

#include <cstdint>
#include <functional>
#include <vector>

namespace {

using namespace refrain::worldgen;

/// Returns the block-local X coordinate shifted into the climate range, so the
/// four quart columns of one chunk select two different parameter points.
class local_x_sampler final : public density::sampler {
  public:
    [[nodiscard]] auto sample_value(const density::sampler_context &, jint x,
                                    jint, jint) const -> float override {
        return static_cast<float>(x & 15) - 8.0F;
    }
    void sample_volume(const density::sampler_context &, buffer &,
                       const volume &) const override {}
    [[nodiscard]] auto range() const -> interval override {
        return interval::infinite();
    }
};

auto two_biome_source() -> multi_noise_biome_source {
    const climate::parameter all = climate::span(-10000, 10000);
    return multi_noise_biome_source{{
        biome_parameter{
            .parameters =
                climate::parameter_point{
                    .temperature = climate::span(-10000, 0),
                    .humidity = all,
                    .continentalness = all,
                    .erosion = all,
                    .depth = all,
                    .weirdness = all,
                    .offset = 0,
                },
            .biome = "test:cold",
        },
        biome_parameter{
            .parameters =
                climate::parameter_point{
                    .temperature = climate::span(1, 10000),
                    .humidity = all,
                    .continentalness = all,
                    .erosion = all,
                    .depth = all,
                    .weirdness = all,
                    .offset = 0,
                },
            .biome = "test:warm",
        },
    }};
}

class fixed_resolver final : public biome_resolver {
  public:
    explicit fixed_resolver(std::uint16_t biome) : biome_{biome} {}
    [[nodiscard]] auto get_noise_biome(jint, jint, jint)
        -> std::uint16_t override {
        return biome_;
    }

  private:
    std::uint16_t biome_{0};
};

} // namespace

using namespace boost::ut;

suite<"worldgen/biome-view"> biome_view_tests = [] {
    "filling a chunk stores a biome in every quart cell"_test = [] {
        block_state_registry registry;
        proto_chunk chunk{registry, 2, 3, -64, 64};
        auto source = two_biome_source();
        const local_x_sampler function;
        const auto zero = density::constant(0.0F);
        const density::sampler_context context;
        const climate_sampler sampler{function, *zero, *zero,  *zero,
                                      *zero,    *zero, context};

        fill_chunk_biomes(chunk, source, sampler);

        for (jint y = -64; y < 0; y += 4) {
            for (jint z = 0; z < 16; z += 4) {
                for (jint x = 0; x < 16; x += 4) {
                    // Quart cells 0..2 sample a negative temperature and cell
                    // 3 a positive one.
                    const std::uint16_t want = x < 12 ? 0 : 1;
                    expect(chunk.biome_at(x, y, z) == want)
                        << "quart cell (" << x << "," << y << "," << z
                        << ") has biome " << chunk.biome_at(x, y, z);
                }
            }
        }
    };

    "the chunk view reads stored cells and falls back outside the chunk"_test =
        [] {
            block_state_registry registry;
            proto_chunk chunk{registry, 0, 0, -64, 384};
            chunk.set_biome(0, -64, 0, 5);
            chunk.set_biome(12, 316, 12, 6);
            fixed_resolver fallback{9};
            chunk_biome_resolver view{
                [&chunk](jint chunk_x, jint chunk_z) -> const proto_chunk * {
                    return chunk_x == 0 && chunk_z == 0 ? &chunk : nullptr;
                },
                fallback};

            expect(view.get_noise_biome(0, -16, 0) == 5);
            expect(view.get_noise_biome(3, 79, 3) == 6);
            // A neighbor chunk is not loaded, so the uncached resolver answers.
            expect(view.get_noise_biome(4, -16, 0) == 9);
            // Outside the chunk's build height the chunk has no cell either.
            expect(view.get_noise_biome(0, 80, 0) == 9);
        };
};
