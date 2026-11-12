// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/biome/overworld_biome_builder.hpp>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::worldgen {

namespace {

using climate::parameter;

[[nodiscard]] auto qspan(float min, float max) -> parameter {
    return climate::span(climate::quantize(min), climate::quantize(max));
}

[[nodiscard]] auto qpoint(float value) -> parameter {
    return climate::point(climate::quantize(value));
}

const parameter full_range = qspan(-1.0F, 1.0F);

const std::array<parameter, 5> temperatures{
    qspan(-1.0F, -0.45F), qspan(-0.45F, -0.15F), qspan(-0.15F, 0.2F),
    qspan(0.2F, 0.55F), qspan(0.55F, 1.0F)};

const std::array<parameter, 5> humidities{
    qspan(-1.0F, -0.35F), qspan(-0.35F, -0.1F), qspan(-0.1F, 0.1F),
    qspan(0.1F, 0.3F), qspan(0.3F, 1.0F)};

const std::array<parameter, 7> erosions{
    qspan(-1.0F, -0.78F),   qspan(-0.78F, -0.375F), qspan(-0.375F, -0.2225F),
    qspan(-0.2225F, 0.05F), qspan(0.05F, 0.45F),    qspan(0.45F, 0.55F),
    qspan(0.55F, 1.0F)};

const parameter frozen_range = temperatures[0];
const parameter unfrozen_range = temperatures[1].span(temperatures[4]);

const parameter mushroom_fields_continentalness = qspan(-1.2F, -1.05F);
const parameter deep_ocean_continentalness = qspan(-1.05F, -0.455F);
const parameter ocean_continentalness = qspan(-0.455F, -0.19F);
const parameter coast_continentalness = qspan(-0.19F, -0.11F);
const parameter inland_continentalness = qspan(-0.11F, 0.55F);
const parameter near_inland_continentalness = qspan(-0.11F, 0.03F);
const parameter mid_inland_continentalness = qspan(0.03F, 0.3F);
const parameter far_inland_continentalness = qspan(0.3F, 1.0F);

using table5 = std::array<std::array<const char *, 5>, 5>;

const table5 oceans{{
    {"minecraft:deep_frozen_ocean", "minecraft:deep_cold_ocean",
     "minecraft:deep_ocean", "minecraft:deep_lukewarm_ocean",
     "minecraft:warm_ocean"},
    {"minecraft:frozen_ocean", "minecraft:cold_ocean", "minecraft:ocean",
     "minecraft:lukewarm_ocean", "minecraft:warm_ocean"},
}};

const table5 middle_biomes{{
    {"minecraft:snowy_plains", "minecraft:snowy_plains",
     "minecraft:snowy_plains", "minecraft:snowy_taiga", "minecraft:taiga"},
    {"minecraft:plains", "minecraft:plains", "minecraft:forest",
     "minecraft:taiga", "minecraft:old_growth_spruce_taiga"},
    {"minecraft:flower_forest", "minecraft:plains", "minecraft:forest",
     "minecraft:birch_forest", "minecraft:dark_forest"},
    {"minecraft:savanna", "minecraft:savanna", "minecraft:forest",
     "minecraft:jungle", "minecraft:jungle"},
    {"minecraft:desert", "minecraft:desert", "minecraft:desert",
     "minecraft:desert", "minecraft:desert"},
}};

const table5 middle_biomes_variant{{
    {"minecraft:ice_spikes", nullptr, "minecraft:snowy_taiga", nullptr,
     nullptr},
    {"minecraft:dappled_forest", nullptr, nullptr, nullptr,
     "minecraft:old_growth_pine_taiga"},
    {"minecraft:sunflower_plains", nullptr, nullptr,
     "minecraft:old_growth_birch_forest", nullptr},
    {nullptr, nullptr, "minecraft:plains", "minecraft:sparse_jungle",
     "minecraft:bamboo_jungle"},
    {nullptr, nullptr, nullptr, nullptr, nullptr},
}};

const table5 plateau_biomes{{
    {"minecraft:snowy_plains", "minecraft:snowy_plains",
     "minecraft:snowy_plains", "minecraft:snowy_taiga",
     "minecraft:snowy_taiga"},
    {"minecraft:meadow", "minecraft:meadow", "minecraft:forest",
     "minecraft:taiga", "minecraft:old_growth_spruce_taiga"},
    {"minecraft:meadow", "minecraft:meadow", "minecraft:meadow",
     "minecraft:meadow", "minecraft:pale_garden"},
    {"minecraft:savanna_plateau", "minecraft:savanna_plateau",
     "minecraft:forest", "minecraft:forest", "minecraft:jungle"},
    {"minecraft:badlands", "minecraft:badlands", "minecraft:badlands",
     "minecraft:wooded_badlands", "minecraft:wooded_badlands"},
}};

const table5 plateau_biomes_variant{{
    {"minecraft:ice_spikes", nullptr, nullptr, nullptr, nullptr},
    {"minecraft:cherry_grove", nullptr, "minecraft:meadow", "minecraft:meadow",
     "minecraft:old_growth_pine_taiga"},
    {"minecraft:cherry_grove", "minecraft:cherry_grove", "minecraft:forest",
     "minecraft:birch_forest", nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr},
    {"minecraft:eroded_badlands", "minecraft:eroded_badlands", nullptr, nullptr,
     nullptr},
}};

const table5 shattered_biomes{{
    {"minecraft:windswept_gravelly_hills", "minecraft:windswept_gravelly_hills",
     "minecraft:windswept_hills", "minecraft:windswept_forest",
     "minecraft:windswept_forest"},
    {"minecraft:windswept_gravelly_hills", "minecraft:windswept_gravelly_hills",
     "minecraft:windswept_hills", "minecraft:windswept_forest",
     "minecraft:windswept_forest"},
    {"minecraft:windswept_hills", "minecraft:windswept_hills",
     "minecraft:windswept_hills", "minecraft:windswept_forest",
     "minecraft:windswept_forest"},
    {nullptr, nullptr, nullptr, nullptr, nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr},
}};

struct state {
    std::vector<biome_parameter> parameters;
};

void push(state &out, parameter temperature, parameter humidity,
          parameter continentalness, parameter erosion, parameter depth,
          parameter weirdness, float offset, const char *biome) {
    out.parameters.push_back(biome_parameter{
        .parameters =
            climate::parameter_point{
                .temperature = temperature,
                .humidity = humidity,
                .continentalness = continentalness,
                .erosion = erosion,
                .depth = depth,
                .weirdness = weirdness,
                .offset = climate::quantize(offset),
            },
        .biome = biome,
    });
}

/// `addSurfaceBiome`: two points, with depth exactly 0 and 1.
void add_surface(state &out, parameter temperature, parameter humidity,
                 parameter continentalness, parameter erosion,
                 parameter weirdness, float offset, const char *biome) {
    push(out, temperature, humidity, continentalness, erosion, qpoint(0.0F),
         weirdness, offset, biome);
    push(out, temperature, humidity, continentalness, erosion, qpoint(1.0F),
         weirdness, offset, biome);
}

/// `addUndergroundBiome`: depth in [0.2, 0.9].
void add_underground(state &out, parameter temperature, parameter humidity,
                     parameter continentalness, parameter erosion,
                     parameter weirdness, float offset, const char *biome) {
    push(out, temperature, humidity, continentalness, erosion,
         qspan(0.2F, 0.9F), weirdness, offset, biome);
}

/// `addBottomBiome`: depth exactly 1.1.
void add_bottom(state &out, parameter temperature, parameter humidity,
                parameter continentalness, parameter erosion,
                parameter weirdness, float offset, const char *biome) {
    push(out, temperature, humidity, continentalness, erosion, qpoint(1.1F),
         weirdness, offset, biome);
}

[[nodiscard]] auto pick_badlands(int humidity, const parameter &weirdness)
    -> const char * {
    if (humidity < 2) {
        return weirdness.max < 0 ? "minecraft:badlands"
                                 : "minecraft:eroded_badlands";
    }
    return humidity < 3 ? "minecraft:badlands" : "minecraft:wooded_badlands";
}

[[nodiscard]] auto pick_beach(int temperature) -> const char * {
    if (temperature == 0) {
        return "minecraft:snowy_beach";
    }
    return temperature == 4 ? "minecraft:desert" : "minecraft:beach";
}

[[nodiscard]] auto pick_middle(int temperature, int humidity,
                               const parameter &weirdness) -> const char * {
    if (weirdness.max < 0) {
        return middle_biomes[static_cast<std::size_t>(temperature)]
                            [static_cast<std::size_t>(humidity)];
    }
    const char *variant =
        middle_biomes_variant[static_cast<std::size_t>(temperature)]
                             [static_cast<std::size_t>(humidity)];
    return variant != nullptr
               ? variant
               : middle_biomes[static_cast<std::size_t>(temperature)]
                              [static_cast<std::size_t>(humidity)];
}

[[nodiscard]] auto pick_plateau(int temperature, int humidity,
                                const parameter &weirdness) -> const char * {
    if (weirdness.max >= 0) {
        const char *variant =
            plateau_biomes_variant[static_cast<std::size_t>(temperature)]
                                  [static_cast<std::size_t>(humidity)];
        if (variant != nullptr) {
            return variant;
        }
    }
    return plateau_biomes[static_cast<std::size_t>(temperature)]
                         [static_cast<std::size_t>(humidity)];
}

[[nodiscard]] auto pick_peak(int temperature, int humidity,
                             const parameter &weirdness) -> const char * {
    if (temperature <= 2) {
        return weirdness.max < 0 ? "minecraft:jagged_peaks"
                                 : "minecraft:frozen_peaks";
    }
    return temperature == 3 ? "minecraft:stony_peaks"
                            : pick_badlands(humidity, weirdness);
}

[[nodiscard]] auto pick_slope(int temperature, int humidity,
                              const parameter &weirdness) -> const char * {
    if (temperature >= 3) {
        return pick_plateau(temperature, humidity, weirdness);
    }
    return humidity <= 1 ? "minecraft:snowy_slopes" : "minecraft:grove";
}

[[nodiscard]] auto pick_shattered(int temperature, int humidity,
                                  const parameter &weirdness) -> const char * {
    const char *biome = shattered_biomes[static_cast<std::size_t>(temperature)]
                                        [static_cast<std::size_t>(humidity)];
    return biome != nullptr ? biome
                            : pick_middle(temperature, humidity, weirdness);
}

[[nodiscard]] auto pick_middle_or_badlands_if_hot(int temperature, int humidity,
                                                  const parameter &weirdness)
    -> const char * {
    return temperature == 4 ? pick_badlands(humidity, weirdness)
                            : pick_middle(temperature, humidity, weirdness);
}

[[nodiscard]] auto pick_middle_or_badlands_if_hot_or_slope_if_cold(
    int temperature, int humidity, const parameter &weirdness) -> const char * {
    return temperature == 0 ? pick_slope(temperature, humidity, weirdness)
                            : pick_middle_or_badlands_if_hot(
                                  temperature, humidity, weirdness);
}

[[nodiscard]] auto maybe_pick_windswept_savanna(int temperature, int humidity,
                                                const parameter &weirdness,
                                                const char *underlying) -> const
    char * {
    return temperature > 1 && humidity < 4 && weirdness.max >= 0
               ? "minecraft:windswept_savanna"
               : underlying;
}

[[nodiscard]] auto pick_shattered_coast(int temperature, int humidity,
                                        const parameter &weirdness) -> const
    char * {
    const char *beach_or_middle =
        weirdness.max >= 0 ? pick_middle(temperature, humidity, weirdness)
                           : pick_beach(temperature);
    return maybe_pick_windswept_savanna(temperature, humidity, weirdness,
                                        beach_or_middle);
}

} // namespace

namespace {

const parameter coast_to_far =
    coast_continentalness.span(far_inland_continentalness);
const parameter coast_to_near =
    coast_continentalness.span(near_inland_continentalness);
const parameter mid_to_far =
    mid_inland_continentalness.span(far_inland_continentalness);
const parameter near_to_far =
    near_inland_continentalness.span(far_inland_continentalness);
const parameter near_to_mid =
    near_inland_continentalness.span(mid_inland_continentalness);

const parameter erosion_0_to_1 = erosions[0].span(erosions[1]);
const parameter erosion_0_to_2 = erosions[0].span(erosions[2]);
const parameter erosion_2_to_3 = erosions[2].span(erosions[3]);
const parameter erosion_3_to_4 = erosions[3].span(erosions[4]);
const parameter erosion_2_to_5 = erosions[2].span(erosions[5]);
const parameter erosion_5_to_6 = erosions[5].span(erosions[6]);

void add_off_coast(state &out) {
    add_surface(out, full_range, full_range, mushroom_fields_continentalness,
                full_range, full_range, 0.0F, "minecraft:mushroom_fields");
    for (int temperature = 0; temperature < 5; ++temperature) {
        add_surface(out, temperatures[static_cast<std::size_t>(temperature)],
                    full_range, deep_ocean_continentalness, full_range,
                    full_range, 0.0F,
                    oceans[0][static_cast<std::size_t>(temperature)]);
        add_surface(out, temperatures[static_cast<std::size_t>(temperature)],
                    full_range, ocean_continentalness, full_range, full_range,
                    0.0F, oceans[1][static_cast<std::size_t>(temperature)]);
    }
}

void add_peaks(state &out, const parameter &weirdness) {
    for (int t = 0; t < 5; ++t) {
        for (int h = 0; h < 5; ++h) {
            const auto &temperature = temperatures[static_cast<std::size_t>(t)];
            const auto &humidity = humidities[static_cast<std::size_t>(h)];
            const char *middle = pick_middle(t, h, weirdness);
            const char *middle_or_badlands =
                pick_middle_or_badlands_if_hot(t, h, weirdness);
            const char *middle_or_badlands_or_slope =
                pick_middle_or_badlands_if_hot_or_slope_if_cold(t, h,
                                                                weirdness);
            const char *plateau = pick_plateau(t, h, weirdness);
            const char *shattered = pick_shattered(t, h, weirdness);
            const char *shattered_or_savanna =
                maybe_pick_windswept_savanna(t, h, weirdness, shattered);
            const char *peak = pick_peak(t, h, weirdness);
            add_surface(out, temperature, humidity, coast_to_far, erosions[0],
                        weirdness, 0.0F, peak);
            add_surface(out, temperature, humidity, coast_to_near, erosions[1],
                        weirdness, 0.0F, middle_or_badlands_or_slope);
            add_surface(out, temperature, humidity, mid_to_far, erosions[1],
                        weirdness, 0.0F, peak);
            add_surface(out, temperature, humidity, coast_to_near,
                        erosion_2_to_3, weirdness, 0.0F, middle);
            add_surface(out, temperature, humidity, mid_to_far, erosions[2],
                        weirdness, 0.0F, plateau);
            add_surface(out, temperature, humidity, mid_inland_continentalness,
                        erosions[3], weirdness, 0.0F, middle_or_badlands);
            add_surface(out, temperature, humidity, far_inland_continentalness,
                        erosions[3], weirdness, 0.0F, plateau);
            add_surface(out, temperature, humidity, coast_to_far, erosions[4],
                        weirdness, 0.0F, middle);
            add_surface(out, temperature, humidity, coast_to_near, erosions[5],
                        weirdness, 0.0F, shattered_or_savanna);
            add_surface(out, temperature, humidity, mid_to_far, erosions[5],
                        weirdness, 0.0F, shattered);
            add_surface(out, temperature, humidity, coast_to_far, erosions[6],
                        weirdness, 0.0F, middle);
        }
    }
}

void add_high_slice(state &out, const parameter &weirdness) {
    for (int t = 0; t < 5; ++t) {
        for (int h = 0; h < 5; ++h) {
            const auto &temperature = temperatures[static_cast<std::size_t>(t)];
            const auto &humidity = humidities[static_cast<std::size_t>(h)];
            const char *middle = pick_middle(t, h, weirdness);
            const char *middle_or_badlands =
                pick_middle_or_badlands_if_hot(t, h, weirdness);
            const char *middle_or_badlands_or_slope =
                pick_middle_or_badlands_if_hot_or_slope_if_cold(t, h,
                                                                weirdness);
            const char *plateau = pick_plateau(t, h, weirdness);
            const char *shattered = pick_shattered(t, h, weirdness);
            const char *middle_or_savanna =
                maybe_pick_windswept_savanna(t, h, weirdness, middle);
            const char *slope = pick_slope(t, h, weirdness);
            const char *peak = pick_peak(t, h, weirdness);
            add_surface(out, temperature, humidity, coast_continentalness,
                        erosion_0_to_1, weirdness, 0.0F, middle);
            add_surface(out, temperature, humidity, near_inland_continentalness,
                        erosions[0], weirdness, 0.0F, slope);
            add_surface(out, temperature, humidity, mid_to_far, erosions[0],
                        weirdness, 0.0F, peak);
            add_surface(out, temperature, humidity, near_inland_continentalness,
                        erosions[1], weirdness, 0.0F,
                        middle_or_badlands_or_slope);
            add_surface(out, temperature, humidity, mid_to_far, erosions[1],
                        weirdness, 0.0F, slope);
            add_surface(out, temperature, humidity, coast_to_near,
                        erosion_2_to_3, weirdness, 0.0F, middle);
            add_surface(out, temperature, humidity, mid_to_far, erosions[2],
                        weirdness, 0.0F, plateau);
            add_surface(out, temperature, humidity, mid_inland_continentalness,
                        erosions[3], weirdness, 0.0F, middle_or_badlands);
            add_surface(out, temperature, humidity, far_inland_continentalness,
                        erosions[3], weirdness, 0.0F, plateau);
            add_surface(out, temperature, humidity, coast_to_far, erosions[4],
                        weirdness, 0.0F, middle);
            add_surface(out, temperature, humidity, coast_to_near, erosions[5],
                        weirdness, 0.0F, middle_or_savanna);
            add_surface(out, temperature, humidity, mid_to_far, erosions[5],
                        weirdness, 0.0F, shattered);
            add_surface(out, temperature, humidity, coast_to_far, erosions[6],
                        weirdness, 0.0F, middle);
        }
    }
}

void add_mid_slice(state &out, const parameter &weirdness) {
    add_surface(out, full_range, full_range, coast_continentalness,
                erosion_0_to_2, weirdness, 0.0F, "minecraft:stony_shore");
    add_surface(out, temperatures[1].span(temperatures[2]), full_range,
                near_to_far, erosions[6], weirdness, 0.0F, "minecraft:swamp");
    add_surface(out, temperatures[3].span(temperatures[4]), full_range,
                near_to_far, erosions[6], weirdness, 0.0F,
                "minecraft:mangrove_swamp");

    for (int t = 0; t < 5; ++t) {
        for (int h = 0; h < 5; ++h) {
            const auto &temperature = temperatures[static_cast<std::size_t>(t)];
            const auto &humidity = humidities[static_cast<std::size_t>(h)];
            const char *middle = pick_middle(t, h, weirdness);
            const char *middle_or_badlands =
                pick_middle_or_badlands_if_hot(t, h, weirdness);
            const char *middle_or_badlands_or_slope =
                pick_middle_or_badlands_if_hot_or_slope_if_cold(t, h,
                                                                weirdness);
            const char *shattered = pick_shattered(t, h, weirdness);
            const char *plateau = pick_plateau(t, h, weirdness);
            const char *beach = pick_beach(t);
            const char *middle_or_savanna =
                maybe_pick_windswept_savanna(t, h, weirdness, middle);
            const char *shattered_coast = pick_shattered_coast(t, h, weirdness);
            const char *slope = pick_slope(t, h, weirdness);
            add_surface(out, temperature, humidity, near_to_far, erosions[0],
                        weirdness, 0.0F, slope);
            add_surface(out, temperature, humidity, near_to_mid, erosions[1],
                        weirdness, 0.0F, middle_or_badlands_or_slope);
            add_surface(out, temperature, humidity, far_inland_continentalness,
                        erosions[1], weirdness, 0.0F, t == 0 ? slope : plateau);
            add_surface(out, temperature, humidity, near_inland_continentalness,
                        erosions[2], weirdness, 0.0F, middle);
            add_surface(out, temperature, humidity, mid_inland_continentalness,
                        erosions[2], weirdness, 0.0F, middle_or_badlands);
            add_surface(out, temperature, humidity, far_inland_continentalness,
                        erosions[2], weirdness, 0.0F, plateau);
            add_surface(out, temperature, humidity, coast_to_near, erosions[3],
                        weirdness, 0.0F, middle);
            add_surface(out, temperature, humidity, mid_to_far, erosions[3],
                        weirdness, 0.0F, middle_or_badlands);
            if (weirdness.max < 0) {
                add_surface(out, temperature, humidity, coast_continentalness,
                            erosions[4], weirdness, 0.0F, beach);
                add_surface(out, temperature, humidity, near_to_far,
                            erosions[4], weirdness, 0.0F, middle);
            } else {
                add_surface(out, temperature, humidity, coast_to_far,
                            erosions[4], weirdness, 0.0F, middle);
            }
            add_surface(out, temperature, humidity, coast_continentalness,
                        erosions[5], weirdness, 0.0F, shattered_coast);
            add_surface(out, temperature, humidity, near_inland_continentalness,
                        erosions[5], weirdness, 0.0F, middle_or_savanna);
            add_surface(out, temperature, humidity, mid_to_far, erosions[5],
                        weirdness, 0.0F, shattered);
            if (weirdness.max < 0) {
                add_surface(out, temperature, humidity, coast_continentalness,
                            erosions[6], weirdness, 0.0F, beach);
            } else {
                add_surface(out, temperature, humidity, coast_continentalness,
                            erosions[6], weirdness, 0.0F, middle);
            }
            if (t == 0) {
                add_surface(out, temperature, humidity, near_to_far,
                            erosions[6], weirdness, 0.0F, middle);
            }
        }
    }
}

void add_low_slice(state &out, const parameter &weirdness) {
    add_surface(out, full_range, full_range, coast_continentalness,
                erosion_0_to_2, weirdness, 0.0F, "minecraft:stony_shore");
    add_surface(out, temperatures[1].span(temperatures[2]), full_range,
                near_to_far, erosions[6], weirdness, 0.0F, "minecraft:swamp");
    add_surface(out, temperatures[3].span(temperatures[4]), full_range,
                near_to_far, erosions[6], weirdness, 0.0F,
                "minecraft:mangrove_swamp");

    for (int t = 0; t < 5; ++t) {
        for (int h = 0; h < 5; ++h) {
            const auto &temperature = temperatures[static_cast<std::size_t>(t)];
            const auto &humidity = humidities[static_cast<std::size_t>(h)];
            const char *middle = pick_middle(t, h, weirdness);
            const char *middle_or_badlands =
                pick_middle_or_badlands_if_hot(t, h, weirdness);
            const char *middle_or_badlands_or_slope =
                pick_middle_or_badlands_if_hot_or_slope_if_cold(t, h,
                                                                weirdness);
            const char *beach = pick_beach(t);
            const char *middle_or_savanna =
                maybe_pick_windswept_savanna(t, h, weirdness, middle);
            const char *shattered_coast = pick_shattered_coast(t, h, weirdness);
            add_surface(out, temperature, humidity, near_inland_continentalness,
                        erosion_0_to_1, weirdness, 0.0F, middle_or_badlands);
            add_surface(out, temperature, humidity, mid_to_far, erosion_0_to_1,
                        weirdness, 0.0F, middle_or_badlands_or_slope);
            add_surface(out, temperature, humidity, near_inland_continentalness,
                        erosion_2_to_3, weirdness, 0.0F, middle);
            add_surface(out, temperature, humidity, mid_to_far, erosion_2_to_3,
                        weirdness, 0.0F, middle_or_badlands);
            add_surface(out, temperature, humidity, coast_continentalness,
                        erosion_3_to_4, weirdness, 0.0F, beach);
            add_surface(out, temperature, humidity, near_to_far, erosions[4],
                        weirdness, 0.0F, middle);
            add_surface(out, temperature, humidity, coast_continentalness,
                        erosions[5], weirdness, 0.0F, shattered_coast);
            add_surface(out, temperature, humidity, near_inland_continentalness,
                        erosions[5], weirdness, 0.0F, middle_or_savanna);
            add_surface(out, temperature, humidity, mid_to_far, erosions[5],
                        weirdness, 0.0F, middle);
            add_surface(out, temperature, humidity, coast_continentalness,
                        erosions[6], weirdness, 0.0F, beach);
            if (t == 0) {
                add_surface(out, temperature, humidity, near_to_far,
                            erosions[6], weirdness, 0.0F, middle);
            }
        }
    }
}

void add_valleys(state &out, const parameter &weirdness) {
    const bool low = weirdness.max < 0;
    add_surface(out, frozen_range, full_range, coast_continentalness,
                erosion_0_to_1, weirdness, 0.0F,
                low ? "minecraft:stony_shore" : "minecraft:frozen_river");
    add_surface(out, unfrozen_range, full_range, coast_continentalness,
                erosion_0_to_1, weirdness, 0.0F,
                low ? "minecraft:stony_shore" : "minecraft:river");
    add_surface(out, frozen_range, full_range, near_inland_continentalness,
                erosion_0_to_1, weirdness, 0.0F, "minecraft:frozen_river");
    add_surface(out, unfrozen_range, full_range, near_inland_continentalness,
                erosion_0_to_1, weirdness, 0.0F, "minecraft:river");
    add_surface(out, frozen_range, full_range, coast_to_far, erosion_2_to_5,
                weirdness, 0.0F, "minecraft:frozen_river");
    add_surface(out, unfrozen_range, full_range, coast_to_far, erosion_2_to_5,
                weirdness, 0.0F, "minecraft:river");
    add_surface(out, frozen_range, full_range, coast_continentalness,
                erosions[6], weirdness, 0.0F, "minecraft:frozen_river");
    add_surface(out, unfrozen_range, full_range, coast_continentalness,
                erosions[6], weirdness, 0.0F, "minecraft:river");
    add_surface(out, temperatures[1].span(temperatures[2]), full_range,
                inland_continentalness.span(far_inland_continentalness),
                erosions[6], weirdness, 0.0F, "minecraft:swamp");
    add_surface(out, temperatures[3].span(temperatures[4]), full_range,
                inland_continentalness.span(far_inland_continentalness),
                erosions[6], weirdness, 0.0F, "minecraft:mangrove_swamp");
    add_surface(out, frozen_range, full_range,
                inland_continentalness.span(far_inland_continentalness),
                erosions[6], weirdness, 0.0F, "minecraft:frozen_river");

    for (int t = 0; t < 5; ++t) {
        for (int h = 0; h < 5; ++h) {
            add_surface(out, temperatures[static_cast<std::size_t>(t)],
                        humidities[static_cast<std::size_t>(h)], mid_to_far,
                        erosion_0_to_1, weirdness, 0.0F,
                        pick_middle_or_badlands_if_hot(t, h, weirdness));
        }
    }
}

void add_underground(state &out) {
    add_underground(out, full_range, full_range, qspan(0.8F, 1.0F), full_range,
                    full_range, 0.0F, "minecraft:dripstone_caves");
    add_underground(out, full_range, qspan(0.7F, 1.0F), full_range, full_range,
                    full_range, 0.0F, "minecraft:lush_caves");
    add_underground(out, full_range, full_range,
                    coast_continentalness.span(inland_continentalness),
                    erosion_5_to_6, qspan(-1.1F, -0.85F), 0.0F,
                    "minecraft:sulfur_caves");
    add_bottom(out, full_range, full_range, full_range, erosion_0_to_1,
               full_range, 0.0F, "minecraft:deep_dark");
}

void add_inland(state &out) {
    add_mid_slice(out, qspan(-1.0F, -0.93333334F));
    add_high_slice(out, qspan(-0.93333334F, -0.7666667F));
    add_peaks(out, qspan(-0.7666667F, -0.56666666F));
    add_high_slice(out, qspan(-0.56666666F, -0.4F));
    add_mid_slice(out, qspan(-0.4F, -0.26666668F));
    add_low_slice(out, qspan(-0.26666668F, -0.05F));
    add_valleys(out, qspan(-0.05F, 0.05F));
    add_low_slice(out, qspan(0.05F, 0.26666668F));
    add_mid_slice(out, qspan(0.26666668F, 0.4F));
    add_high_slice(out, qspan(0.4F, 0.56666666F));
    add_peaks(out, qspan(0.56666666F, 0.7666667F));
    add_high_slice(out, qspan(0.7666667F, 0.93333334F));
    add_mid_slice(out, qspan(0.93333334F, 1.0F));
}

} // namespace

// TODO: this builder duplicates the overworld climate table that already ships
// in content/vanilla/26.3/reports/biome_parameters/overworld.json and is kept
// alive only by a test; the data-driven loader must become the only source.
auto overworld_parameters() -> std::vector<biome_parameter> {
    state out;
    out.parameters.reserve(8192);
    add_off_coast(out);
    add_inland(out);
    add_underground(out);
    return std::move(out.parameters);
}

} // namespace refrain::worldgen
