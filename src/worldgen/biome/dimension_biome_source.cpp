// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/biome/dimension_biome_source.hpp>

#include <refrain/worldgen/biome/biome_parameters.hpp>

#include <utility>

namespace refrain::worldgen {

namespace {

/// The fixed source stores its possible biomes in the order a preset lists
/// them; one biome is enough here.
auto fixed_biomes(std::string biome) -> std::vector<std::string> {
    std::vector<std::string> biomes;
    biomes.push_back(std::move(biome));
    return biomes;
}

} // namespace

dimension_biome_source::dimension_biome_source(dimension_kind kind,
                                               multi_noise_biome_source source)
    : kind_{kind}, source_{std::move(source)} {}

dimension_biome_source::dimension_biome_source(dimension_kind kind,
                                               the_end_biome_source source)
    : kind_{kind}, source_{std::move(source)} {}

dimension_biome_source::dimension_biome_source(dimension_kind kind,
                                               std::vector<std::string> biome)
    : kind_{kind}, source_{std::move(biome)} {}

auto dimension_biome_source::load(std::string_view data_root,
                                  dimension_kind kind)
    -> result<dimension_biome_source> {
    if (kind == dimension_kind::end) {
        return dimension_biome_source{kind, the_end_biome_source{}};
    }
    auto parameters = load_biome_parameters(data_root, kind);
    if (!parameters) {
        return std::unexpected{parameters.error()};
    }
    return dimension_biome_source{
        kind, multi_noise_biome_source{std::move(*parameters)}};
}

auto dimension_biome_source::fixed(dimension_kind kind, std::string biome)
    -> dimension_biome_source {
    return dimension_biome_source{kind, fixed_biomes(std::move(biome))};
}

auto dimension_biome_source::noise_biome(const climate_sampler &sampler,
                                         jint quart_x, jint quart_y,
                                         jint quart_z) -> std::uint16_t {
    if (auto *source = std::get_if<multi_noise_biome_source>(&source_)) {
        return source->noise_biome(sampler, quart_x, quart_y, quart_z);
    }
    if (const auto *source = std::get_if<the_end_biome_source>(&source_)) {
        return source->noise_biome(sampler, quart_x, quart_y, quart_z);
    }
    // The fixed source has a single biome, so every cell selects id zero.
    return std::uint16_t{0};
}

void dimension_biome_source::reset_search() {
    if (auto *source = std::get_if<multi_noise_biome_source>(&source_)) {
        source->reset_search();
    }
}

auto dimension_biome_source::biome(std::uint16_t id) const -> std::string_view {
    if (const auto *source = std::get_if<multi_noise_biome_source>(&source_)) {
        return source->biome(id);
    }
    if (const auto *source = std::get_if<the_end_biome_source>(&source_)) {
        return source->biome(id);
    }
    const auto &biomes = std::get<std::vector<std::string>>(source_);
    return id < biomes.size() ? biomes[id] : std::string_view{};
}

auto dimension_biome_source::biomes() const
    -> const std::vector<std::string> & {
    if (const auto *source = std::get_if<multi_noise_biome_source>(&source_)) {
        return source->biomes();
    }
    if (const auto *source = std::get_if<the_end_biome_source>(&source_)) {
        return source->biomes();
    }
    return std::get<std::vector<std::string>>(source_);
}

auto dimension_biome_source::is_fixed() const noexcept -> bool {
    return std::holds_alternative<std::vector<std::string>>(source_);
}

} // namespace refrain::worldgen
