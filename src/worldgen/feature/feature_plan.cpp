// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/feature/feature_plan.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace refrain::worldgen::feature {

namespace {

struct feature_data {
    jint index{0};
    jint step{0};
    std::string id{};

    friend auto operator<(const feature_data &left, const feature_data &right)
        -> bool {
        // The reference orders its feature graph by the global feature index
        // first and the step second; the depth-first walk and every edge set
        // inherit that order, so it decides which feature wins an overlap and
        // which stream index each placed feature receives.
        if (left.index != right.index) {
            return left.index < right.index;
        }
        return left.step < right.step;
    }
};

auto depth_first_search(
    const std::map<feature_data, std::set<feature_data>> &edges,
    std::set<feature_data> &discovered, std::set<feature_data> &visiting,
    std::vector<feature_data> &out, const feature_data &current) -> bool {
    if (discovered.contains(current)) {
        return false;
    }
    if (visiting.contains(current)) {
        return true;
    }
    visiting.insert(current);
    if (const auto found = edges.find(current); found != edges.end()) {
        for (const auto &next : found->second) {
            if (depth_first_search(edges, discovered, visiting, out, next)) {
                return true;
            }
        }
    }
    visiting.erase(current);
    discovered.insert(current);
    out.push_back(current);
    return false;
}

} // namespace

auto feature_sorter::build(const std::vector<std::string> &possible_biomes,
                           const std::vector<biome_feature_list> &lists)
    -> result<std::vector<step_features>> {
    std::unordered_map<std::string, const biome_feature_list *> by_biome;
    for (const auto &list : lists) {
        by_biome.emplace(list.biome, &list);
    }

    std::unordered_map<std::string, jint> feature_index;
    jint next_index = 0;
    std::map<feature_data, std::set<feature_data>> edges;
    jint max_step = 0;

    for (const auto &biome : possible_biomes) {
        const auto found = by_biome.find(biome);
        if (found == by_biome.end()) {
            continue;
        }
        const auto &steps = found->second->steps;
        max_step = std::max(max_step, static_cast<jint>(steps.size()));

        std::vector<feature_data> features;
        for (std::size_t step = 0; step < steps.size(); ++step) {
            for (const auto &id : steps[step]) {
                jint index = 0;
                if (const auto existing = feature_index.find(id);
                    existing != feature_index.end()) {
                    index = existing->second;
                } else {
                    index = next_index++;
                    feature_index.emplace(id, index);
                }
                features.push_back(feature_data{
                    .index = index,
                    .step = static_cast<jint>(step),
                    .id = id,
                });
            }
        }
        for (std::size_t i = 0; i < features.size(); ++i) {
            auto &out = edges[features[i]];
            if (i + 1 < features.size()) {
                out.insert(features[i + 1]);
            }
        }
    }

    std::set<feature_data> discovered;
    std::set<feature_data> visiting;
    std::vector<feature_data> sorted;
    for (const auto &[key, value] : edges) {
        (void)value;
        if (depth_first_search(edges, discovered, visiting, sorted, key)) {
            return std::unexpected{make_error(error_code::invalid_argument,
                                              "feature order cycle found")};
        }
    }
    std::reverse(sorted.begin(), sorted.end());

    std::vector<step_features> result(static_cast<std::size_t>(max_step));
    for (const auto &feature : sorted) {
        auto &step = result[static_cast<std::size_t>(feature.step)];
        step.index.emplace(feature.id, static_cast<jint>(step.features.size()));
        step.features.push_back(feature.id);
    }
    return result;
}

} // namespace refrain::worldgen::feature
