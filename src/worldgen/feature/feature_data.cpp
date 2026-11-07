// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/feature/feature_data.hpp>

#include <refrain/core/io.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace refrain::worldgen::feature {

namespace {

struct loaded_documents {
    std::unordered_map<std::string, data::json::value> documents{};

    [[nodiscard]] auto find(std::string_view id) const
        -> const data::json::value * {
        const auto found = documents.find(std::string{id});
        return found == documents.end() ? nullptr : &found->second;
    }
};

auto collect_documents(const std::string &directory, const std::string &prefix,
                       loaded_documents &out) -> status {
    const auto entries = core::io::list_directory(directory);
    if (!entries) {
        return std::unexpected{entries.error()};
    }
    std::vector<std::string> sorted{entries->begin(), entries->end()};
    std::sort(sorted.begin(), sorted.end());
    for (const auto &entry : sorted) {
        const auto path = directory + "/" + entry;
        const auto id = prefix.empty() ? entry : prefix + "/" + entry;
        if (core::io::is_directory(path)) {
            if (const auto collected = collect_documents(path, id, out);
                !collected) {
                return collected;
            }
            continue;
        }
        if (entry.size() <= 5 || entry.substr(entry.size() - 5) != ".json") {
            continue;
        }
        auto document = data::json::value::parse_file(path);
        if (!document) {
            return std::unexpected{document.error()};
        }
        out.documents.emplace(id.substr(0, id.size() - 5),
                              std::move(*document));
    }
    return {};
}

auto biome_lists(const data::json::value &document)
    -> result<std::vector<std::vector<std::string>>> {
    const auto *features = document.find("features");
    if (features == nullptr || !features->is_array()) {
        return std::vector<std::vector<std::string>>{};
    }
    std::vector<std::vector<std::string>> steps;
    for (const auto &step : *features->as_array()) {
        if (!step.is_array()) {
            return std::unexpected{make_error(
                error_code::parse_error, "biome features step is not a list")};
        }
        std::vector<std::string> ids;
        for (const auto &entry : *step.as_array()) {
            const auto id = entry.as_string();
            if (!id) {
                return std::unexpected{make_error(
                    error_code::parse_error, "biome feature is not an id")};
            }
            ids.emplace_back(*id);
        }
        steps.push_back(std::move(ids));
    }
    return steps;
}

} // namespace

auto load_feature_data(std::string_view data_root) -> result<feature_data_set> {
    feature_data_set result;
    const auto namespaces = core::io::list_directory(data_root);
    if (!namespaces) {
        return std::unexpected{namespaces.error()};
    }
    std::vector<std::string> sorted{namespaces->begin(), namespaces->end()};
    std::sort(sorted.begin(), sorted.end());

    for (const auto &name : sorted) {
        const auto base = std::string{data_root} + "/" + name;
        if (!core::io::is_directory(base)) {
            continue;
        }
        const auto worldgen = base + "/worldgen";

        loaded_documents features;
        if (core::io::is_directory(worldgen + "/feature")) {
            if (const auto collected =
                    collect_documents(worldgen + "/feature", "", features);
                !collected) {
                return std::unexpected{collected.error()};
            }
        }
        for (auto &[id, document] : features.documents) {
            result.features.emplace(
                name + ":" + id,
                configured_feature_document{.id = name + ":" + id,
                                            .document = std::move(document)});
        }

        loaded_documents providers;
        if (core::io::is_directory(worldgen + "/block_state_provider")) {
            if (const auto collected = collect_documents(
                    worldgen + "/block_state_provider", "", providers);
                !collected) {
                return std::unexpected{collected.error()};
            }
        }
        for (auto &[id, document] : providers.documents) {
            result.state_providers.emplace(
                name + ":" + id,
                state_provider_document{.id = name + ":" + id,
                                        .document = std::move(document)});
        }

        loaded_documents placed;
        if (core::io::is_directory(worldgen + "/placed_feature")) {
            if (const auto collected =
                    collect_documents(worldgen + "/placed_feature", "", placed);
                !collected) {
                return std::unexpected{collected.error()};
            }
        }
        for (auto &[id, document] : placed.documents) {
            const auto *feature = document.find("feature");
            if (feature == nullptr) {
                return std::unexpected{make_error(
                    error_code::parse_error, "placed feature has no feature")};
            }
            const auto feature_id = feature->as_string();
            if (!feature_id) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "placed feature id is not a string")};
            }
            const auto *placement = document.find("placement");
            if (placement == nullptr || !placement->is_array()) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "placed feature has no placement list")};
            }
            result.placed.emplace(name + ":" + id,
                                  placed_feature_document{
                                      .id = name + ":" + id,
                                      .feature = std::string{*feature_id},
                                      .placement = *placement,
                                  });
        }

        loaded_documents biomes;
        if (core::io::is_directory(worldgen + "/biome")) {
            if (const auto collected =
                    collect_documents(worldgen + "/biome", "", biomes);
                !collected) {
                return std::unexpected{collected.error()};
            }
        }
        for (auto &[id, document] : biomes.documents) {
            auto steps = biome_lists(document);
            if (!steps) {
                return std::unexpected{steps.error()};
            }
            result.biomes.push_back(biome_feature_list{
                .biome = name + ":" + id, .steps = std::move(*steps)});
        }
    }
    return result;
}

} // namespace refrain::worldgen::feature
