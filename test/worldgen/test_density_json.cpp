// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The documents below are synthetic: they exercise every decoder path but are
// not copied from the game.

#include <refrain/worldgen/density.hpp>
#include <refrain/worldgen/density_buffer.hpp>
#include <refrain/worldgen/density_json.hpp>
#include <refrain/worldgen/noise.hpp>
#include <refrain/worldgen/rng.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>

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

auto simple_stack(const char *name) -> std::shared_ptr<noise_stack> {
    normal_noise_parameters parameters;
    parameters.base_octave = 0;
    parameters.octave_count = 3;
    const normal_noise normal{parameters};
    auto source = make_factory(12345)->from_hash_of(name);
    return std::make_shared<noise_stack>(normal.create(*source));
}

constexpr std::array<std::array<jint, 3>, 4> points{{
    {{0, -64, 0}},
    {{5, 0, -3}},
    {{-7, 63, 11}},
    {{100, 319, -100}},
}};

void check_same(const char *name, const sampler &decoded,
                const sampler &expected) {
    const sampler_context context;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto &p = points[i];
        expect(bits(decoded.sample_value(context, p[0], p[1], p[2])) ==
               bits(expected.sample_value(context, p[0], p[1], p[2])))
            << name << " scalar " << i;
    }
    const volume vol{3, 2, 2, -1, 60, 2, 2, 3, 1};
    buffer decoded_buffer{vol.size()};
    buffer expected_buffer{vol.size()};
    decoded.sample_volume(context, decoded_buffer, vol);
    expected.sample_volume(context, expected_buffer, vol);
    for (std::size_t i = 0; i < decoded_buffer.size(); ++i) {
        expect(bits(decoded_buffer.get(i)) == bits(expected_buffer.get(i)))
            << name << " volume " << i;
    }
    expect(bits(decoded.range().min()) == bits(expected.range().min()))
        << name << " range min";
    expect(bits(decoded.range().max()) == bits(expected.range().max()))
        << name << " range max";
}

} // namespace

suite<"worldgen/density-json"> density_json_tests = [] {
    "references and arithmetic decode to the same graph"_test = [] {
        density_documents documents;
        documents.seed = 12345;
        documents.functions.emplace_back(
            "test:b",
            R"({"type":"minecraft:gradient","axis":"y","from_coordinate":-64,"to_coordinate":320,"from_value":0.0,"to_value":1.0})");
        documents.functions.emplace_back(
            "test:a",
            R"({"type":"minecraft:add","left":1.5,"right":{"type":"minecraft:mul","left":2.0,"right":"test:b"}})");
        auto decoded = decode_density(R"("test:a")", documents);
        expect(decoded.has_value());
        if (!decoded) {
            return;
        }
        auto expected =
            add(density::constant(1.5F),
                mul(density::constant(2.0F), gradient_y(-64, 320, 0.0F, 1.0F)));
        check_same("arithmetic", **decoded, *expected);
    };

    "clamp, interpolated and range_choice decode"_test = [] {
        density_documents documents;
        documents.seed = 12345;
        auto decoded = decode_density(
            R"({"type":"minecraft:clamp","min":-0.5,"max":0.5,"input":{"type":"minecraft:interpolated","cell_size_xz":4,"cell_size_y":8,"input":{"type":"minecraft:range_choice","input":{"type":"minecraft:gradient","axis":"y","from_coordinate":-64,"to_coordinate":320,"from_value":0.0,"to_value":1.0},"min_inclusive":0.25,"max_exclusive":0.75,"when_in_range":1.0,"when_out_of_range":2.0}}})",
            documents);
        expect(decoded.has_value());
        if (!decoded) {
            return;
        }
        auto expected = clamp(
            interpolated(range_choice(gradient_y(-64, 320, 0.0F, 1.0F), 0.25F,
                                      0.75F, density::constant(1.0F),
                                      density::constant(2.0F)),
                         4, 8),
            -0.5F, 0.5F);
        check_same("clamp-chain", **decoded, *expected);
    };

    "noise with shift references decodes"_test = [] {
        density_documents documents;
        documents.seed = 12345;
        documents.noises.emplace_back("test:noise",
                                      R"({"base_octave":0,"octave_count":3})");
        documents.noises.emplace_back("test:shift_noise",
                                      R"({"base_octave":0,"octave_count":3})");
        documents.functions.emplace_back(
            "test:shift",
            R"({"type":"minecraft:shift","noise":"test:shift_noise"})");
        auto decoded = decode_density(
            R"({"type":"minecraft:noise","noise":"test:noise","xz_scale":0.25,"y_scale":0.5,"shift_x":"test:shift","shift_z":"test:shift"})",
            documents);
        expect(decoded.has_value());
        if (!decoded) {
            return;
        }
        auto noise_stack_value = simple_stack("test:noise");
        auto shift_stack_value = simple_stack("test:shift_noise");
        auto expected =
            shifted_noise_xz(shift(shift_stack_value), shift(shift_stack_value),
                             0.25, 0.5, noise_stack_value);
        check_same("shifted-noise", **decoded, *expected);
    };

    "old blended noise decodes"_test = [] {
        density_documents documents;
        documents.seed = 12345;
        auto decoded = decode_density(
            R"({"type":"minecraft:old_blended_noise","xz_scale":0.25,"y_scale":0.125,"xz_factor":80.0,"y_factor":160.0,"smear_scale_multiplier":8.0})",
            documents);
        expect(decoded.has_value());
        if (!decoded) {
            return;
        }
        const blended_noise_parameters parameters;
        auto source = make_factory(12345)->from_hash_of("minecraft:terrain");
        auto expected = blended_noise(parameters, *source);
        check_same("old-blended", **decoded, *expected);
    };

    "spline decodes"_test = [] {
        density_documents documents;
        documents.seed = 12345;
        auto decoded = decode_density(
            R"({"type":"minecraft:spline","spline":{"coordinate":{"type":"minecraft:gradient","axis":"y","from_coordinate":-64,"to_coordinate":320,"from_value":0.0,"to_value":1.0},"points":[{"location":-0.5,"derivative":0.0,"value":1.0},{"location":0.0,"derivative":2.0,"value":0.0},{"location":0.5,"derivative":-1.0,"value":2.0}]}})",
            documents);
        expect(decoded.has_value());
        if (!decoded) {
            return;
        }
        auto expected = spline_value(
            spline::multipoint(gradient_y(-64, 320, 0.0F, 1.0F),
                               {{-0.5F, 0.0F, spline::constant(1.0F)},
                                {0.0F, 2.0F, spline::constant(0.0F)},
                                {0.5F, -1.0F, spline::constant(2.0F)}}));
        check_same("spline", **decoded, *expected);
    };

    "find top surface decodes"_test = [] {
        density_documents documents;
        documents.seed = 12345;
        auto decoded = decode_density(
            R"({"type":"minecraft:find_top_surface","lower_bound":-32,"cell_height":8,"density":{"type":"minecraft:gradient","axis":"y","from_coordinate":-64,"to_coordinate":320,"from_value":0.0,"to_value":1.0},"upper_bound":{"type":"minecraft:gradient","axis":"y","from_coordinate":-64,"to_coordinate":320,"from_value":16.0,"to_value":80.0}})",
            documents);
        expect(decoded.has_value());
        if (!decoded) {
            return;
        }
        auto expected =
            find_top_surface(gradient_y(-64, 320, 0.0F, 1.0F),
                             gradient_y(-64, 320, 16.0F, 80.0F), -32, 8);
        const sampler_context context;
        const volume vol{3, 1, 2, -1, 0, 2};
        buffer decoded_buffer{vol.size()};
        buffer expected_buffer{vol.size()};
        (*decoded)->sample_volume(context, decoded_buffer, vol);
        expected->sample_volume(context, expected_buffer, vol);
        for (std::size_t i = 0; i < decoded_buffer.size(); ++i) {
            expect(bits(decoded_buffer.get(i)) == bits(expected_buffer.get(i)))
                << "top surface " << i;
        }
        expect(bits((*decoded)->range().min()) ==
               bits(expected->range().min()));
        expect(bits((*decoded)->range().max()) ==
               bits(expected->range().max()));
    };
};
