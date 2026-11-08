// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The dimension biome sources over the shipped content snapshot: the nether
// selects from its climate parameter report, the end from its island bands and
// a fixed source returns one biome everywhere.

#include <refrain/worldgen/biome/dimension_biome_source.hpp>
#include <refrain/worldgen/density/density.hpp>

#include <boost/ut.hpp>

#include <array>
#include <memory>
#include <string>

namespace {

using namespace boost::ut;
using namespace refrain::worldgen;

auto data_root() -> std::string {
    return std::string{REFRAIN_SOURCE_DIR} + "/content/vanilla/26.3/data";
}

/// Six constant climate samplers so one test can place a target point exactly
/// on a parameter entry.
class sampled_climate {
  public:
    explicit sampled_climate(std::array<float, 6> values) {
        for (std::size_t index = 0; index < values.size(); ++index) {
            samplers_[index] = density::constant(values[index]);
        }
    }
    sampled_climate(const sampled_climate &) = delete;
    auto operator=(const sampled_climate &) -> sampled_climate & = delete;
    ~sampled_climate() = default;

    [[nodiscard]] auto sampler() -> climate_sampler {
        return climate_sampler{*samplers_[0], *samplers_[1], *samplers_[2],
                               *samplers_[3], *samplers_[4], *samplers_[5],
                               context_};
    }

  private:
    std::array<density::sampler_ptr, 6> samplers_{};
    density::sampler_context context_{};
};

} // namespace

using namespace boost::ut;

suite<"worldgen/dimension-biome-source"> dimension_biome_source_tests = [] {
    "the nether selects from the parameter report"_test = [] {
        auto source =
            dimension_biome_source::load(data_root(), dimension_kind::nether);
        expect(source.has_value()) << source.error().message;
        if (!source) {
            return;
        }
        expect(source->kind() == dimension_kind::nether);
        expect(!source->is_fixed());
        const std::array<std::string_view, 5> order{
            "minecraft:nether_wastes", "minecraft:soul_sand_valley",
            "minecraft:crimson_forest", "minecraft:warped_forest",
            "minecraft:basalt_deltas"};
        expect(source->biomes().size() == order.size());
        for (std::size_t index = 0; index < order.size(); ++index) {
            expect(source->biome(static_cast<std::uint16_t>(index)) ==
                   order[index]);
        }

        const std::array<std::pair<std::array<float, 6>, std::uint16_t>, 5>
            points{{
                {{{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F}}, 0},
                {{{0.0F, -0.5F, 0.0F, 0.0F, 0.0F, 0.0F}}, 1},
                {{{0.4F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F}}, 2},
                {{{0.0F, 0.5F, 0.0F, 0.0F, 0.0F, 0.375F}}, 3},
                {{{-0.5F, 0.0F, 0.0F, 0.0F, 0.0F, 0.175F}}, 4},
            }};
        for (const auto &[values, expected] : points) {
            sampled_climate climate{values};
            auto sampler = climate.sampler();
            const auto selected = source->noise_biome(sampler, 0, 16, 0);
            expect(selected == expected)
                << "nether point selected " << source->biome(selected);
        }
    };

    "the end selects from its island bands"_test = [] {
        auto source =
            dimension_biome_source::load(data_root(), dimension_kind::end);
        expect(source.has_value()) << source.error().message;
        if (!source) {
            return;
        }
        expect(source->kind() == dimension_kind::end);
        expect(source->biomes().size() == 5);
        expect(source->biome(0) == "minecraft:the_end");

        // The central island wins before the bands are sampled.
        sampled_climate central{{0.0F, 0.0F, 0.0F, 0.9F, 0.0F, 0.0F}};
        auto central_sampler = central.sampler();
        expect(source->noise_biome(central_sampler, 0, 16, 0) == 0);

        const std::array<std::pair<float, std::uint16_t>, 4> bands{{
            {0.5F, 1},
            {0.0F, 2},
            {-0.5F, 3},
            {-0.1F, 4},
        }};
        for (const auto &[erosion, expected] : bands) {
            sampled_climate climate{{0.0F, 0.0F, 0.0F, erosion, 0.0F, 0.0F}};
            auto sampler = climate.sampler();
            expect(source->noise_biome(sampler, 400, 16, 0) == expected)
                << "erosion " << erosion << " selected "
                << source->biome(source->noise_biome(sampler, 400, 16, 0));
        }
    };

    "a fixed source returns one biome everywhere"_test = [] {
        auto source = dimension_biome_source::fixed(dimension_kind::end,
                                                    "minecraft:the_end");
        expect(source.is_fixed());
        expect(source.kind() == dimension_kind::end);
        expect(source.biomes().size() == 1);
        expect(source.biome(0) == "minecraft:the_end");
        sampled_climate climate{{0.9F, 0.9F, 0.9F, 0.9F, 0.9F, 0.9F}};
        auto sampler = climate.sampler();
        expect(source.noise_biome(sampler, 0, 0, 0) == 0);
        expect(source.noise_biome(sampler, 137, 33, -19) == 0);
    };

    "a missing parameter report is rejected for the nether"_test = [] {
        const auto missing = dimension_biome_source::load(
            data_root() + "/missing", dimension_kind::nether);
        expect(!missing.has_value());
    };
};
