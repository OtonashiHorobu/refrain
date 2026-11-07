// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/feature/feature_stage.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace refrain::worldgen::feature {

namespace {

void place_placed(feature_context &context,
                  const placed_feature_runtime &placed, worldgen_random &random,
                  const block_pos &origin) {
    if (placed.configured == nullptr) {
        return;
    }
    if (placed.placement.empty()) {
        (void)placed.configured->place(context, random, origin);
        return;
    }

    std::vector<block_pos> positions{origin};
    std::vector<std::size_t> modifier_indices{0};
    std::vector<block_pos> modified;

    while (!positions.empty()) {
        const block_pos position = positions.back();
        positions.pop_back();
        const std::size_t modifier_index = modifier_indices.back();
        modifier_indices.pop_back();
        modified.clear();
        placed.placement[modifier_index]->modify(context.placement, random,
                                                 position, modified);
        const std::size_t next_index = modifier_index + 1;
        if (next_index < placed.placement.size()) {
            for (auto it = modified.rbegin(); it != modified.rend(); ++it) {
                positions.push_back(*it);
                modifier_indices.push_back(next_index);
            }
        } else {
            for (const auto &next_position : modified) {
                (void)placed.configured->place(context, random, next_position);
            }
        }
    }
}

} // namespace

void decorate(
    feature_context &context, const std::vector<step_features> &plan,
    const std::unordered_map<std::string, placed_feature_runtime> &placed,
    const std::vector<std::string> &biomes,
    const std::vector<biome_feature_list> &lists, jlong seed,
    const structure_placer &structures) {
    std::unordered_map<std::string, const biome_feature_list *> by_biome;
    for (const auto &list : lists) {
        by_biome.emplace(list.biome, &list);
    }

    context.place_by_id = [&context, &placed](std::string_view id,
                                              worldgen_random &random,
                                              const block_pos &position) {
        const auto found = placed.find(std::string{id});
        if (found == placed.end()) {
            return false;
        }
        place_placed(context, found->second, random, position);
        return true;
    };
    const block_pos origin{.x = context.placement.chunk->chunk_x() * 16,
                           .y = context.placement.chunk->min_y(),
                           .z = context.placement.chunk->chunk_z() * 16};
    worldgen_random random{std::make_unique<xoroshiro_random_source>(seed)};
    const jlong decoration_seed =
        random.set_decoration_seed(seed, origin.x, origin.z);

    for (std::size_t step = 0; step < plan.size(); ++step) {
        if (structures) {
            for (jint index = 0;; ++index) {
                random.set_feature_seed(decoration_seed, index,
                                        static_cast<jint>(step));
                if (!structures(static_cast<jint>(step), index, random)) {
                    break;
                }
            }
        }
        std::set<jint> candidates;
        for (const auto &biome : biomes) {
            const auto found = by_biome.find(biome);
            if (found == by_biome.end() ||
                step >= found->second->steps.size()) {
                continue;
            }
            for (const auto &id : found->second->steps[step]) {
                const auto index = plan[step].index.find(id);
                if (index != plan[step].index.end()) {
                    candidates.insert(index->second);
                }
            }
        }

        for (const auto index : candidates) {
            const auto &id =
                plan[step].features[static_cast<std::size_t>(index)];
            const auto found = placed.find(id);
            if (found == placed.end()) {
                continue;
            }
            random.set_feature_seed(decoration_seed, index,
                                    static_cast<jint>(step));
            place_placed(context, found->second, random, origin);
        }
    }
}

} // namespace refrain::worldgen::feature
