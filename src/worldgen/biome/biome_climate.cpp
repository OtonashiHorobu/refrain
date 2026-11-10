// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/biome/biome_climate.hpp>

#include <refrain/core/io.hpp>
#include <refrain/data/json.hpp>

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace refrain::worldgen {

namespace {

using json = data::json::value;

auto number_member(const json &node, std::string_view key)
    -> std::optional<double> {
    const json *found = node.find(key);
    if (found == nullptr) {
        return std::nullopt;
    }
    const auto value = found->as_double();
    return value ? std::optional<double>{*value} : std::nullopt;
}

auto string_member(const json &node, std::string_view key)
    -> std::optional<std::string> {
    const json *found = node.find(key);
    if (found == nullptr) {
        return std::nullopt;
    }
    const auto value = found->as_string();
    return value ? std::optional<std::string>{std::string{*value}}
                 : std::nullopt;
}

auto bool_member(const json &node, std::string_view key)
    -> std::optional<bool> {
    const json *found = node.find(key);
    if (found == nullptr) {
        return std::nullopt;
    }
    const auto value = found->as_bool();
    return value ? std::optional<bool>{*value} : std::nullopt;
}

auto collect_biome_documents(
    std::string_view data_root,
    std::vector<std::pair<std::string, std::string>> &out) -> status {
    auto namespaces = core::io::list_directory(data_root);
    if (!namespaces) {
        return std::unexpected{namespaces.error()};
    }
    for (const auto &name : *namespaces) {
        const std::string base =
            std::string{data_root} + "/" + name + "/worldgen/biome";
        if (!core::io::is_directory(base)) {
            continue;
        }
        std::vector<std::pair<std::string, std::string>> pending;
        auto walk = [&](auto &&self, std::string_view path,
                        std::string_view prefix) -> status {
            auto entries = core::io::list_directory(path);
            if (!entries) {
                return std::unexpected{entries.error()};
            }
            for (const auto &entry : *entries) {
                const std::string child = std::string{path} + "/" + entry;
                const std::string id_path =
                    prefix.empty() ? entry : std::string{prefix} + "/" + entry;
                if (core::io::is_directory(child)) {
                    if (auto walked = self(self, child, id_path); !walked) {
                        return walked;
                    }
                } else if (entry.ends_with(".json")) {
                    auto text = core::io::read_text(child);
                    if (!text) {
                        return std::unexpected{text.error()};
                    }
                    pending.emplace_back(
                        name + ":" + id_path.substr(0, id_path.size() - 5),
                        std::move(*text));
                }
            }
            return status_ok();
        };
        if (auto walked = walk(walk, base, ""); !walked) {
            return walked;
        }
        std::sort(pending.begin(), pending.end());
        for (auto &entry : pending) {
            out.push_back(std::move(entry));
        }
    }
    return status_ok();
}

} // namespace

void biome_climates::add(std::string id, biome_climate climate) {
    climates_.insert_or_assign(std::move(id), climate);
}

auto biome_climates::get(std::string_view id) const -> const biome_climate * {
    const auto found = climates_.find(std::string{id});
    return found == climates_.end() ? nullptr : &found->second;
}

auto biome_climates::load(std::string_view data_root) -> status {
    std::vector<std::pair<std::string, std::string>> documents;
    if (auto loaded = collect_biome_documents(data_root, documents); !loaded) {
        return loaded;
    }
    for (const auto &[id, text] : documents) {
        auto parsed = json::parse(text);
        if (!parsed) {
            return std::unexpected{parsed.error()};
        }
        biome_climate climate;
        climate.has_precipitation =
            bool_member(*parsed, "has_precipitation").value_or(true);
        climate.temperature = static_cast<float>(
            number_member(*parsed, "temperature").value_or(0.5));
        climate.downfall = static_cast<float>(
            number_member(*parsed, "downfall").value_or(0.5));
        if (string_member(*parsed, "temperature_modifier").value_or("none") ==
            "frozen") {
            climate.modifier = temperature_modifier::frozen;
        }
        add(id, climate);
    }
    return status_ok();
}

biome_temperatures::biome_temperatures() {
    legacy_random_source temperature_random{1234};
    temperature_noise_ =
        std::make_unique<simplex_noise>(temperature_random, true);

    legacy_random_source frozen_random{3456};
    std::vector<noise_stack::layer> layers;
    layers.push_back({std::make_unique<simplex_noise>(frozen_random, true), 1.0,
                      0.14285715F});
    layers.push_back({std::make_unique<simplex_noise>(frozen_random, true), 0.5,
                      0.2857143F});
    layers.push_back({std::make_unique<simplex_noise>(frozen_random, true),
                      0.25, 0.5714286F});
    frozen_temperature_noise_ =
        std::make_unique<noise_stack>(std::move(layers));

    legacy_random_source info_random{2345};
    biome_info_noise_ = std::make_unique<simplex_noise>(info_random, true);
}

auto biome_temperatures::modify_temperature(const biome_climate &climate,
                                            jint x, jint z) const -> float {
    if (climate.modifier == temperature_modifier::none) {
        return climate.temperature;
    }
    const double large_variation = static_cast<double>(
        frozen_temperature_noise_->get(static_cast<double>(x) * 0.05,
                                       static_cast<double>(z) * 0.05) *
        7.0F);
    const double edge_variation = static_cast<double>(biome_info_noise_->get(
        static_cast<double>(x) * 0.2, static_cast<double>(z) * 0.2));
    const double ice_patches = large_variation + edge_variation;
    if (ice_patches < 0.3) {
        const double small_variation =
            static_cast<double>(biome_info_noise_->get(
                static_cast<double>(x) * 0.09, static_cast<double>(z) * 0.09));
        if (small_variation < 0.8) {
            return 0.2F;
        }
    }
    return climate.temperature;
}

auto biome_temperatures::temperature_at(const biome_climate &climate, jint x,
                                        jint y, jint z, jint sea_level) const
    -> float {
    const float adjusted = modify_temperature(climate, x, z);
    const jint snow_level = sea_level + 17;
    if (y > snow_level) {
        const float variation =
            temperature_noise_->get(static_cast<double>(x) / 8.0F,
                                    static_cast<double>(z) / 8.0F) *
            8.0F;
        return adjusted - (variation + static_cast<float>(y) -
                           static_cast<float>(snow_level)) *
                              0.05F / 40.0F;
    }
    return adjusted;
}

auto biome_temperatures::should_melt_iceberg(const biome_climate &climate,
                                             jint x, jint y, jint z,
                                             jint sea_level) const -> bool {
    return temperature_at(climate, x, y, z, sea_level) > 0.1F;
}

} // namespace refrain::worldgen
