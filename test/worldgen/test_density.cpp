// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Density-sampler vectors generated from the real 26.3 classes with
// `tools/worldgen/vectors/run.sh <server.jar> DensityVectors`.

#include <refrain/worldgen/core/rng.hpp>
#include <refrain/worldgen/density/density.hpp>
#include <refrain/worldgen/density/density_buffer.hpp>
#include <refrain/worldgen/noise/noise.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>

namespace {

using namespace boost::ut;
using namespace refrain::worldgen;
using namespace refrain::worldgen::density;

auto bits(float value) -> std::uint32_t {
    std::uint32_t out = 0;
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

auto make_factory(jlong seed) -> std::unique_ptr<positional_random_factory> {
    return new_positional_factory(algorithm::xoroshiro, seed);
}

auto temperature_stack() -> std::shared_ptr<noise_stack> {
    normal_noise_parameters parameters;
    parameters.base_amplitude = 1.2453007926713473;
    parameters.base_octave = -10;
    parameters.octave_count = 6;
    parameters.amplitude_modifiers = {1.5, 0.0, 1.0, 0.0, 0.0, 0.0};
    const normal_noise normal{parameters};
    auto source = make_factory(12345)->from_hash_of("minecraft:temperature");
    return std::make_shared<noise_stack>(normal.create(*source));
}

auto simple_stack() -> std::shared_ptr<noise_stack> {
    normal_noise_parameters parameters;
    parameters.base_octave = 0;
    parameters.octave_count = 3;
    const normal_noise normal{parameters};
    auto source = make_factory(12345)->from_hash_of("refrain:noise_test");
    return std::make_shared<noise_stack>(normal.create(*source));
}

constexpr std::array<std::array<jint, 3>, 4> points{{
    {{0, -64, 0}},
    {{5, 0, -3}},
    {{-7, 63, 11}},
    {{100, 319, -100}},
}};

void check_graph(const char *name, const sampler &value,
                 std::array<std::uint32_t, 4> scalar,
                 std::array<std::uint32_t, 12> bulk) {
    const sampler_context context;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto &p = points[i];
        expect(bits(value.sample_value(context, p[0], p[1], p[2])) == scalar[i])
            << name << " scalar " << i;
    }

    const volume vol{3, 2, 2, -1, 60, 2, 2, 3, 1};
    buffer output{vol.size()};
    value.sample_volume(context, output, vol);
    for (std::size_t i = 0; i < bulk.size(); ++i) {
        expect(bits(output.get(i)) == bulk[i]) << name << " volume " << i;
    }
}

void check_range(const char *name, const sampler &value, std::uint32_t min_bits,
                 std::uint32_t max_bits) {
    const auto range = value.range();
    expect(bits(range.min()) == min_bits) << name << " range min";
    expect(bits(range.max()) == max_bits) << name << " range max";
}

void check_volume(const char *name, const sampler &value, const volume &vol,
                  std::span<const std::uint32_t> expected) {
    const sampler_context context;
    buffer output{vol.size()};
    value.sample_volume(context, output, vol);
    for (std::size_t i = 0; i < expected.size(); ++i) {
        expect(bits(output.get(i)) == expected[i]) << name << " sample " << i;
    }
}

} // namespace

suite<"worldgen/density-samplers"> density_sampler_tests = [] {
    "sampler graphs match the reference scalar and bulk paths"_test = [] {
        {
            auto value = add(
                density::constant(1.5F),
                mul(density::constant(2.0F), gradient_y(-64, 320, 0.0F, 1.0F)));
            check_graph("add_mul", *value,
                        std::array<std::uint32_t, 4>{0x3fc00000U, 0x3feaaaabU,
                                                     0x400a5556U, 0x405faaabU},
                        std::array<std::uint32_t, 12>{
                            0x40095556U, 0x400a5556U, 0x40095556U, 0x400a5556U,
                            0x40095556U, 0x400a5556U, 0x40095556U, 0x400a5556U,
                            0x40095556U, 0x400a5556U, 0x40095556U,
                            0x400a5556U});
        }
        {
            auto value =
                div(gradient_y(0, 128, -2.0F, 2.0F), density::constant(3.0F));
            check_graph("div_const", *value,
                        std::array<std::uint32_t, 4>{0xbf2aaaabU, 0xbf2aaaabU,
                                                     0xbc2aaaabU, 0x3f2aaaabU},
                        std::array<std::uint32_t, 12>{
                            0xbd2aaaabU, 0xbc2aaaabU, 0xbd2aaaabU, 0xbc2aaaabU,
                            0xbd2aaaabU, 0xbc2aaaabU, 0xbd2aaaabU, 0xbc2aaaabU,
                            0xbd2aaaabU, 0xbc2aaaabU, 0xbd2aaaabU,
                            0xbc2aaaabU});
        }
        {
            auto value =
                min(gradient_y(-64, 320, 0.0F, 1.0F), density::constant(0.25F));
            check_graph("min_const", *value,
                        std::array<std::uint32_t, 4>{0x00000000U, 0x3e2aaaabU,
                                                     0x3e800000U, 0x3e800000U},
                        std::array<std::uint32_t, 12>{
                            0x3e800000U, 0x3e800000U, 0x3e800000U, 0x3e800000U,
                            0x3e800000U, 0x3e800000U, 0x3e800000U, 0x3e800000U,
                            0x3e800000U, 0x3e800000U, 0x3e800000U,
                            0x3e800000U});
        }
        {
            auto value = max(
                sub(density::constant(0.5F), gradient_y(0, 128, -2.0F, 2.0F)),
                density::constant(-0.2F));
            check_graph("max_const", *value,
                        std::array<std::uint32_t, 4>{0x40200000U, 0x40200000U,
                                                     0x3f080000U, 0xbe4ccccdU},
                        std::array<std::uint32_t, 12>{
                            0x3f200000U, 0x3f080000U, 0x3f200000U, 0x3f080000U,
                            0x3f200000U, 0x3f080000U, 0x3f200000U, 0x3f080000U,
                            0x3f200000U, 0x3f080000U, 0x3f200000U,
                            0x3f080000U});
        }
        {
            auto value = squeeze(square(half_negative(add(
                gradient_y(-64, 320, 0.0F, 1.0F), density::constant(-0.5F)))));
            check_graph("squeeze_chain", *value,
                        std::array<std::uint32_t, 4>{0x3cffeaabU, 0x3c638a78U,
                                                     0x3b6ab89eU, 0x3dfc0c51U},
                        std::array<std::uint32_t, 12>{
                            0x3b80719aU, 0x3b6ab89eU, 0x3b80719aU, 0x3b6ab89eU,
                            0x3b80719aU, 0x3b6ab89eU, 0x3b80719aU, 0x3b6ab89eU,
                            0x3b80719aU, 0x3b6ab89eU, 0x3b80719aU,
                            0x3b6ab89eU});
        }
        {
            auto value = clamp(mul(gradient_y(-64, 320, 0.0F, 1.0F),
                                   gradient_y(-64, 64, 1.0F, -1.0F)),
                               -0.5F, 0.5F);
            check_graph("clamp_mul", *value,
                        std::array<std::uint32_t, 4>{0x00000000U, 0x00000000U,
                                                     0xbea6b001U, 0xbf000000U},
                        std::array<std::uint32_t, 12>{
                            0xbe9b0001U, 0xbea6b001U, 0xbe9b0001U, 0xbea6b001U,
                            0xbe9b0001U, 0xbea6b001U, 0xbe9b0001U, 0xbea6b001U,
                            0xbe9b0001U, 0xbea6b001U, 0xbe9b0001U,
                            0xbea6b001U});
        }
        {
            auto value =
                add(abs(quarter_negative(sub(gradient_y(-64, 64, 1.0F, -1.0F),
                                             density::constant(0.75F)))),
                    sign(mul(gradient_y(-64, 320, 0.0F, 1.0F),
                             density::constant(-1.0F))));
            check_graph("unary_mix", *value,
                        std::array<std::uint32_t, 4>{0x3e800000U, 0xbf500000U,
                                                     0xbf110000U, 0xbf100000U},
                        std::array<std::uint32_t, 12>{
                            0xbf140000U, 0xbf110000U, 0xbf140000U, 0xbf110000U,
                            0xbf140000U, 0xbf110000U, 0xbf140000U, 0xbf110000U,
                            0xbf140000U, 0xbf110000U, 0xbf140000U,
                            0xbf110000U});
        }
        {
            auto value = add(cube(density::constant(0.5F)),
                             sqrt(abs(gradient_y(-64, 64, 1.0F, -1.0F))));
            check_graph("cube_sqrt", *value,
                        std::array<std::uint32_t, 4>{0x3f900000U, 0x3e000000U,
                                                     0x3f8efefeU, 0x3f900000U},
                        std::array<std::uint32_t, 12>{
                            0x3f8bef7bU, 0x3f8efefeU, 0x3f8bef7bU, 0x3f8efefeU,
                            0x3f8bef7bU, 0x3f8efefeU, 0x3f8bef7bU, 0x3f8efefeU,
                            0x3f8bef7bU, 0x3f8efefeU, 0x3f8bef7bU,
                            0x3f8efefeU});
        }
        {
            auto value = interpolated(add(gradient_y(-64, 320, 0.0F, 1.0F),
                                          gradient_y(-64, 64, 1.0F, -1.0F)),
                                      4, 8);
            check_graph("interp_fn", *value,
                        std::array<std::uint32_t, 4>{0x3f800000U, 0x3e2aaaabU,
                                                     0xbf275555U, 0xbb2aaaa0U},
                        std::array<std::uint32_t, 12>{
                            0xbf1d5555U, 0xbf275554U, 0xbf1d5555U, 0xbf275554U,
                            0xbf1d5555U, 0xbf275554U, 0xbf1d5555U, 0xbf275554U,
                            0xbf1d5555U, 0xbf275554U, 0xbf1d5555U,
                            0xbf275554U});
        }
        {
            auto value =
                range_choice(gradient_y(-64, 320, 0.0F, 1.0F), 0.25F, 0.75F,
                             density::constant(1.0F), density::constant(2.0F));
            check_graph("range_const_fn", *value,
                        std::array<std::uint32_t, 4>{0x40000000U, 0x40000000U,
                                                     0x3f800000U, 0x40000000U},
                        std::array<std::uint32_t, 12>{
                            0x3f800000U, 0x3f800000U, 0x3f800000U, 0x3f800000U,
                            0x3f800000U, 0x3f800000U, 0x3f800000U, 0x3f800000U,
                            0x3f800000U, 0x3f800000U, 0x3f800000U,
                            0x3f800000U});
        }
        {
            auto value = range_choice(gradient_y(-64, 320, 0.0F, 1.0F), 0.25F,
                                      0.75F, gradient_y(0, 128, -2.0F, 2.0F),
                                      gradient_y(-64, 64, 1.0F, -1.0F));
            check_graph("range_dyn_fn", *value,
                        std::array<std::uint32_t, 4>{0x3f800000U, 0x00000000U,
                                                     0xbd000000U, 0xbf800000U},
                        std::array<std::uint32_t, 12>{
                            0xbe000000U, 0xbd000000U, 0xbe000000U, 0xbd000000U,
                            0xbe000000U, 0xbd000000U, 0xbe000000U, 0xbd000000U,
                            0xbe000000U, 0xbd000000U, 0xbe000000U,
                            0xbd000000U});
        }
        {
            auto value =
                lerp(gradient_y(-64, 63, 0.0F, 1.0F), density::constant(0.5F),
                     gradient_y(0, 128, -2.0F, 2.0F));
            check_graph("lerp_const_first_fn", *value,
                        std::array<std::uint32_t, 4>{0x3f000000U, 0xbf42850aU,
                                                     0xbd000000U, 0x40000000U},
                        std::array<std::uint32_t, 12>{
                            0xbde1c388U, 0xbd000000U, 0xbde1c388U, 0xbd000000U,
                            0xbde1c388U, 0xbd000000U, 0xbde1c388U, 0xbd000000U,
                            0xbde1c388U, 0xbd000000U, 0xbde1c388U,
                            0xbd000000U});
        }
        {
            auto value =
                lerp(gradient_y(-64, 63, 0.0F, 1.0F),
                     gradient_y(0, 128, -2.0F, 2.0F), density::constant(-0.5F));
            check_graph("lerp_const_second_fn", *value,
                        std::array<std::uint32_t, 4>{0xc0000000U, 0xbf9f3e7dU,
                                                     0xbf000000U, 0xbf000000U},
                        std::array<std::uint32_t, 12>{
                            0xbefb76eeU, 0xbf000000U, 0xbefb76eeU, 0xbf000000U,
                            0xbefb76eeU, 0xbf000000U, 0xbefb76eeU, 0xbf000000U,
                            0xbefb76eeU, 0xbf000000U, 0xbefb76eeU,
                            0xbf000000U});
        }
        {
            auto value = lerp(gradient_y(-64, 63, 0.0F, 1.0F),
                              gradient_y(0, 128, -2.0F, 2.0F),
                              gradient_y(-64, 64, 1.0F, -1.0F));
            check_graph("lerp_dyn_fn", *value,
                        std::array<std::uint32_t, 4>{0xc0000000U, 0xbf7dfbf8U,
                                                     0xbf7c0000U, 0xbf800000U},
                        std::array<std::uint32_t, 12>{
                            0xbf6b162cU, 0xbf7c0000U, 0xbf6b162cU, 0xbf7c0000U,
                            0xbf6b162cU, 0xbf7c0000U, 0xbf6b162cU, 0xbf7c0000U,
                            0xbf6b162cU, 0xbf7c0000U, 0xbf6b162cU,
                            0xbf7c0000U});
        }
        {
            auto value = density::noise(temperature_stack(), 0.25, 0.5);
            check_graph("noise_fn", *value,
                        std::array<std::uint32_t, 4>{0xbd2c9b52U, 0xbcd6ab64U,
                                                     0xbc6cc63aU, 0xbe234e3eU},
                        std::array<std::uint32_t, 12>{
                            0xbc84b07cU, 0xbc83f2beU, 0xbc8981c7U, 0xbc88beffU,
                            0xbc8e594aU, 0xbc8d9141U, 0xbc84c109U, 0xbc8400cbU,
                            0xbc899189U, 0xbc88cc09U, 0xbc8e6849U,
                            0xbc8d9d8cU});
        }
        {
            auto value = shift(temperature_stack());
            check_graph("shift_fn", *value,
                        std::array<std::uint32_t, 4>{0xbe06259fU, 0xbdd6ab64U,
                                                     0xbd8cce41U, 0xbe9cc544U},
                        std::array<std::uint32_t, 12>{
                            0xbd9baa56U, 0xbd99ef07U, 0xbda0b30aU, 0xbd9ef51bU,
                            0xbda5c0caU, 0xbda40011U, 0xbd9bdd28U, 0xbd9a1fdeU,
                            0xbda0e55fU, 0xbd9f2559U, 0xbda5f28aU,
                            0xbda42fc7U});
        }
        {
            auto value = shift_a(temperature_stack());
            check_graph("shift_a_fn", *value,
                        std::array<std::uint32_t, 4>{0xbdca9ff2U, 0xbdd6ab64U,
                                                     0xbdbc953eU, 0xbe690f68U},
                        std::array<std::uint32_t, 12>{
                            0xbdc8c4f0U, 0xbdc8c4f0U, 0xbdce0698U, 0xbdce0698U,
                            0xbdd34c3eU, 0xbdd34c3eU, 0xbdc927baU, 0xbdc927baU,
                            0xbdce692fU, 0xbdce692fU, 0xbdd3aeaaU,
                            0xbdd3aeaaU});
        }
        {
            auto value = shift_b(temperature_stack());
            check_graph("shift_b_fn", *value,
                        std::array<std::uint32_t, 4>{0xbdca9ff2U, 0xbdbe59c2U,
                                                     0xbdee54aaU, 0x3d27b4c8U},
                        std::array<std::uint32_t, 12>{
                            0xbdd0cba7U, 0xbdd0cba7U, 0xbdcefd82U, 0xbdcefd82U,
                            0xbdcd3439U, 0xbdcd3439U, 0xbdd36f7aU, 0xbdd36f7aU,
                            0xbdd1a081U, 0xbdd1a081U, 0xbdcfd62eU,
                            0xbdcfd62eU});
        }
        {
            auto value = shifted_noise_xz(shift(temperature_stack()),
                                          shift(temperature_stack()), 0.25, 0.0,
                                          simple_stack());
            check_graph("shifted_xz_fn", *value,
                        std::array<std::uint32_t, 4>{0xbe5ee3baU, 0xbe68ec09U,
                                                     0xbe2051a4U, 0xbe0f19caU},
                        std::array<std::uint32_t, 12>{
                            0xbcf4b2a0U, 0xbcf5cf79U, 0x3e122af0U, 0x3e129b25U,
                            0x3f3c0d11U, 0x3f3c8724U, 0xbe489823U, 0xbe4a200aU,
                            0x3e94538dU, 0x3e94d78eU, 0x3f783084U,
                            0x3f77f01dU});
        }
        {
            auto value = shifted_noise_xyz(
                shift(temperature_stack()), shift(temperature_stack()),
                shift(temperature_stack()), 0.25, 0.5, simple_stack());
            check_graph("shifted_xyz_fn", *value,
                        std::array<std::uint32_t, 4>{0xbe96dfdeU, 0xbd466976U,
                                                     0xbecf3261U, 0xbedbe6b3U},
                        std::array<std::uint32_t, 12>{
                            0x3dc95237U, 0x3af29bc0U, 0x3d818a1dU, 0x3f2776cfU,
                            0x3ea07d80U, 0x3e0cec68U, 0x3ee8d244U, 0xbeb7a26cU,
                            0xbdf475a5U, 0x3ef0a350U, 0x3e697cfcU,
                            0x3cccbe44U});
        }
        const auto make_basic_spline = [] {
            return spline::multipoint(gradient_y(-64, 320, 0.0F, 1.0F),
                                      {{-0.5F, 0.0F, spline::constant(1.0F)},
                                       {0.0F, 2.0F, spline::constant(0.0F)},
                                       {0.5F, -1.0F, spline::constant(2.0F)}});
        };
        const auto make_nested_spline = [] {
            const auto inner =
                spline::multipoint(gradient_y(-64, 64, 1.0F, -1.0F),
                                   {{-1.0F, 0.5F, spline::constant(0.25F)},
                                    {1.0F, 0.0F, spline::constant(-0.25F)}});
            return spline::multipoint(gradient_y(-64, 320, 0.0F, 1.0F),
                                      {{-0.25F, 0.0F, spline::constant(0.5F)},
                                       {0.25F, 0.0F, inner},
                                       {0.75F, 0.0F, inner},
                                       {1.25F, 1.0F, spline::constant(1.5F)}});
        };
        {
            auto value = spline_value(make_basic_spline());
            check_graph("spline_basic_fn", *value,
                        std::array<std::uint32_t, 4>{0x00000000U, 0x3f3425eeU,
                                                     0x3fcf0747U, 0x3fc05555U},
                        std::array<std::uint32_t, 12>{
                            0x3fca3c4cU, 0x3fcf0747U, 0x3fca3c4cU, 0x3fcf0747U,
                            0x3fca3c4cU, 0x3fcf0747U, 0x3fca3c4cU, 0x3fcf0747U,
                            0x3fca3c4cU, 0x3fcf0747U, 0x3fca3c4cU,
                            0x3fcf0747U});
            check_range("spline_basic_fn", *value, 0xbf000000U, 0x40280000U);
        }
        {
            auto value = spline_value(make_nested_spline());
            check_graph("spline_nested_fn", *value,
                        std::array<std::uint32_t, 4>{0x3e000000U, 0x3e1c71c8U,
                                                     0x3e83e420U, 0x3f4dab24U},
                        std::array<std::uint32_t, 12>{
                            0x3e8e4800U, 0x3e83e420U, 0x3e8e4800U, 0x3e83e420U,
                            0x3e8e4800U, 0x3e83e420U, 0x3e8e4800U, 0x3e83e420U,
                            0x3e8e4800U, 0x3e83e420U, 0x3e8e4800U,
                            0x3e83e420U});
            check_range("spline_nested_fn", *value, 0xbf580000U, 0x3fec0000U);
        }
        {
            auto value = spline_value(spline::constant(0.75F));
            check_graph("spline_constant_fn", *value,
                        std::array<std::uint32_t, 4>{0x3f400000U, 0x3f400000U,
                                                     0x3f400000U, 0x3f400000U},
                        std::array<std::uint32_t, 12>{
                            0x3f400000U, 0x3f400000U, 0x3f400000U, 0x3f400000U,
                            0x3f400000U, 0x3f400000U, 0x3f400000U, 0x3f400000U,
                            0x3f400000U, 0x3f400000U, 0x3f400000U,
                            0x3f400000U});
            check_range("spline_constant_fn", *value, 0x3f400000U, 0x3f400000U);
        }
        {
            auto value = blend_density(gradient_y(0, 128, -2.0F, 2.0F));
            check_graph("blend_density_fn", *value,
                        std::array<std::uint32_t, 4>{0xc0000000U, 0xc0000000U,
                                                     0xbd000000U, 0x40000000U},
                        std::array<std::uint32_t, 12>{
                            0xbe000000U, 0xbd000000U, 0xbe000000U, 0xbd000000U,
                            0xbe000000U, 0xbd000000U, 0xbe000000U, 0xbd000000U,
                            0xbe000000U, 0xbd000000U, 0xbe000000U,
                            0xbd000000U});
        }
        {
            auto value =
                find_top_surface(gradient_y(-64, 320, 0.0F, 1.0F),
                                 gradient_y(-64, 320, 16.0F, 80.0F), -32, 8);
            check_volume("find_top_fn", *value, volume{3, 1, 2, -1, 0, 2},
                         std::array<std::uint32_t, 6>{
                             0x41c00000U, 0x41c00000U, 0x41c00000U, 0x41c00000U,
                             0x41c00000U, 0x41c00000U});
            const sampler_context context;
            expect(bits(value->sample_value(context, 0, -64, 0)) ==
                   0x41c00000U);
            expect(bits(value->sample_value(context, 5, 0, -3)) == 0x41c00000U);
            expect(bits(value->sample_value(context, -7, 63, 11)) ==
                   0x41c00000U);
            expect(bits(value->sample_value(context, 100, 319, -100)) ==
                   0x41c00000U);
            check_range("find_top_fn", *value, 0xc2000000U, 0x42a00000U);
        }
    };

    "interpolated covers all three volume branches"_test = [] {
        {
            auto sampler = interpolated(add(gradient_y(-64, 320, 0.0F, 1.0F),
                                            gradient_y(-64, 64, 1.0F, -1.0F)),
                                        4, 8);
            check_volume("interp_fn.aligned", *sampler,
                         volume{3, 2, 2, -4, 56, 0, 4, 8, 4},
                         std::array<std::uint32_t, 12>{
                             0xbf100000U, 0xbf2aaaaaU, 0xbf100000U, 0xbf2aaaaaU,
                             0xbf100000U, 0xbf2aaaaaU, 0xbf100000U, 0xbf2aaaaaU,
                             0xbf100000U, 0xbf2aaaaaU, 0xbf100000U,
                             0xbf2aaaaaU});
        }
        {
            auto sampler = interpolated(add(gradient_y(-64, 320, 0.0F, 1.0F),
                                            gradient_y(-64, 64, 1.0F, -1.0F)),
                                        4, 8);
            check_volume("interp_fn.block_step", *sampler,
                         volume{5, 9, 3, -4, 56, 0},
                         std::array<std::uint32_t, 135>{
                             0xbf100000U, 0xbf135555U, 0xbf16aaaaU, 0xbf19ffffU,
                             0xbf1d5554U, 0xbf20aaa9U, 0xbf23fffeU, 0xbf275553U,
                             0xbf2aaaaaU, 0xbf100000U, 0xbf135555U, 0xbf16aaaaU,
                             0xbf19ffffU, 0xbf1d5554U, 0xbf20aaa9U, 0xbf23fffeU,
                             0xbf275553U, 0xbf2aaaaaU, 0xbf100000U, 0xbf135555U,
                             0xbf16aaaaU, 0xbf19ffffU, 0xbf1d5554U, 0xbf20aaa9U,
                             0xbf23fffeU, 0xbf275553U, 0xbf2aaaaaU, 0xbf100000U,
                             0xbf135555U, 0xbf16aaaaU, 0xbf19ffffU, 0xbf1d5554U,
                             0xbf20aaa9U, 0xbf23fffeU, 0xbf275553U, 0xbf2aaaaaU,
                             0xbf100000U, 0xbf135555U, 0xbf16aaaaU, 0xbf19ffffU,
                             0xbf1d5554U, 0xbf20aaa9U, 0xbf23fffeU, 0xbf275553U,
                             0xbf2aaaaaU, 0xbf100000U, 0xbf135555U, 0xbf16aaaaU,
                             0xbf19ffffU, 0xbf1d5554U, 0xbf20aaa9U, 0xbf23fffeU,
                             0xbf275553U, 0xbf2aaaaaU, 0xbf100000U, 0xbf135555U,
                             0xbf16aaaaU, 0xbf19ffffU, 0xbf1d5554U, 0xbf20aaa9U,
                             0xbf23fffeU, 0xbf275553U, 0xbf2aaaaaU, 0xbf100000U,
                             0xbf135555U, 0xbf16aaaaU, 0xbf19ffffU, 0xbf1d5554U,
                             0xbf20aaa9U, 0xbf23fffeU, 0xbf275553U, 0xbf2aaaaaU,
                             0xbf100000U, 0xbf135555U, 0xbf16aaaaU, 0xbf19ffffU,
                             0xbf1d5554U, 0xbf20aaa9U, 0xbf23fffeU, 0xbf275553U,
                             0xbf2aaaaaU, 0xbf100000U, 0xbf135555U, 0xbf16aaaaU,
                             0xbf19ffffU, 0xbf1d5554U, 0xbf20aaa9U, 0xbf23fffeU,
                             0xbf275553U, 0xbf2aaaaaU, 0xbf100000U, 0xbf135555U,
                             0xbf16aaaaU, 0xbf19ffffU, 0xbf1d5554U, 0xbf20aaa9U,
                             0xbf23fffeU, 0xbf275553U, 0xbf2aaaaaU, 0xbf100000U,
                             0xbf135555U, 0xbf16aaaaU, 0xbf19ffffU, 0xbf1d5554U,
                             0xbf20aaa9U, 0xbf23fffeU, 0xbf275553U, 0xbf2aaaaaU,
                             0xbf100000U, 0xbf135555U, 0xbf16aaaaU, 0xbf19ffffU,
                             0xbf1d5554U, 0xbf20aaa9U, 0xbf23fffeU, 0xbf275553U,
                             0xbf2aaaaaU, 0xbf100000U, 0xbf135555U, 0xbf16aaaaU,
                             0xbf19ffffU, 0xbf1d5554U, 0xbf20aaa9U, 0xbf23fffeU,
                             0xbf275553U, 0xbf2aaaaaU, 0xbf100000U, 0xbf135555U,
                             0xbf16aaaaU, 0xbf19ffffU, 0xbf1d5554U, 0xbf20aaa9U,
                             0xbf23fffeU, 0xbf275553U, 0xbf2aaaaaU});
        }
        {
            auto sampler = interpolated(add(gradient_y(-64, 320, 0.0F, 1.0F),
                                            gradient_y(-64, 64, 1.0F, -1.0F)),
                                        4, 8);
            check_volume("interp_fn.step12", *sampler,
                         volume{3, 2, 2, -2, 52, 1, 3, 5, 2},
                         std::array<std::uint32_t, 12>{
                             0xbf02aaaaU, 0xbf135555U, 0xbf02aaaaU, 0xbf135555U,
                             0xbf02aaaaU, 0xbf135555U, 0xbf02aaaaU, 0xbf135555U,
                             0xbf02aaaaU, 0xbf135555U, 0xbf02aaaaU,
                             0xbf135555U});
        }
    };
};
