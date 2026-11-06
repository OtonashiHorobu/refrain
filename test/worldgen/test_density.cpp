// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Density-sampler vectors generated from the real 26.3 classes with
// `tools/worldgen/vectors/run.sh <server.jar> DensityVectors`.

#include <refrain/worldgen/density.hpp>
#include <refrain/worldgen/density_buffer.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstdint>
#include <cstring>

namespace {

using namespace boost::ut;
using namespace refrain::worldgen;
using namespace refrain::worldgen::density;

auto bits(float value) -> std::uint32_t {
    std::uint32_t out = 0;
    std::memcpy(&out, &value, sizeof(out));
    return out;
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
    };
};
