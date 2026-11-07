// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/biome_source.hpp>

#include <algorithm>
#include <utility>

namespace refrain::worldgen {

namespace {

/// Interning keeps the returned ids dense and in first-seen order.
auto intern(std::vector<biome_parameter> &parameters,
            std::vector<std::string> &biomes)
    -> std::vector<climate::parameter_list::entry> {
    std::vector<climate::parameter_list::entry> entries;
    entries.reserve(parameters.size());
    for (auto &parameter : parameters) {
        auto found = std::find(biomes.begin(), biomes.end(), parameter.biome);
        std::uint16_t id = 0;
        if (found == biomes.end()) {
            id = static_cast<std::uint16_t>(biomes.size());
            biomes.push_back(std::move(parameter.biome));
        } else {
            id = static_cast<std::uint16_t>(
                std::distance(biomes.begin(), found));
        }
        entries.push_back(climate::parameter_list::entry{
            .parameters = parameter.parameters,
            .value = id,
        });
    }
    return entries;
}

} // namespace

climate_sampler::climate_sampler(const density::sampler &temperature,
                                 const density::sampler &humidity,
                                 const density::sampler &continentalness,
                                 const density::sampler &erosion,
                                 const density::sampler &depth,
                                 const density::sampler &weirdness,
                                 const density::sampler_context &context)
    : samplers_{&temperature, &humidity, &continentalness,
                &erosion,     &depth,    &weirdness},
      context_{&context} {}

auto climate_sampler::sample(jint quart_x, jint quart_y, jint quart_z) const
    -> climate::target_point {
    const jint block_x = quart_x * 4;
    const jint block_y = quart_y * 4;
    const jint block_z = quart_z * 4;
    return climate::target_point{
        .temperature = climate::quantize(
            samplers_[0]->sample_value(*context_, block_x, block_y, block_z)),
        .humidity = climate::quantize(
            samplers_[1]->sample_value(*context_, block_x, block_y, block_z)),
        .continentalness = climate::quantize(
            samplers_[2]->sample_value(*context_, block_x, block_y, block_z)),
        .erosion = climate::quantize(
            samplers_[3]->sample_value(*context_, block_x, block_y, block_z)),
        .depth = climate::quantize(
            samplers_[4]->sample_value(*context_, block_x, block_y, block_z)),
        .weirdness = climate::quantize(
            samplers_[5]->sample_value(*context_, block_x, block_y, block_z)),
    };
}

multi_noise_biome_source::multi_noise_biome_source(
    std::vector<biome_parameter> parameters)
    : biomes_{}, parameters_{intern(parameters, biomes_)} {}

auto multi_noise_biome_source::noise_biome(const climate_sampler &sampler,
                                           jint quart_x, jint quart_y,
                                           jint quart_z) -> std::uint16_t {
    return biome_at(sampler.sample(quart_x, quart_y, quart_z));
}

biome_grid::biome_grid(multi_noise_biome_source &source,
                       const climate_sampler &sampler, jint min_quart_x,
                       jint min_quart_y, jint min_quart_z, jint size_x,
                       jint size_y, jint size_z)
    : source_{&source}, min_quart_x_{min_quart_x}, min_quart_y_{min_quart_y},
      min_quart_z_{min_quart_z}, size_x_{size_x}, size_y_{size_y} {
    const volume block_volume{size_x,
                              size_y,
                              size_z,
                              min_quart_x * 4,
                              min_quart_y * 4,
                              min_quart_z * 4,
                              4,
                              4,
                              4};
    values_.reserve(sampler.samplers().size());
    for (const auto *function : sampler.samplers()) {
        buffer values{block_volume.size()};
        function->sample_volume(sampler.context(), values, block_volume);
        values_.push_back(std::move(values));
    }
}

auto biome_grid::at(jint quart_x, jint quart_y, jint quart_z) -> std::uint16_t {
    const auto x = static_cast<std::size_t>(quart_x - min_quart_x_);
    const auto y = static_cast<std::size_t>(quart_y - min_quart_y_);
    const auto z = static_cast<std::size_t>(quart_z - min_quart_z_);
    const std::size_t index = y + (x + z * static_cast<std::size_t>(size_x_)) *
                                      static_cast<std::size_t>(size_y_);
    const climate::target_point target{
        .temperature = climate::quantize(values_[0].get(index)),
        .humidity = climate::quantize(values_[1].get(index)),
        .continentalness = climate::quantize(values_[2].get(index)),
        .erosion = climate::quantize(values_[3].get(index)),
        .depth = climate::quantize(values_[4].get(index)),
        .weirdness = climate::quantize(values_[5].get(index)),
    };
    return source_->biome_at(target);
}

} // namespace refrain::worldgen
