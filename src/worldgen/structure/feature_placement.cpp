// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/structure/feature_placement.hpp>

#include <refrain/worldgen/feature/feature_stage.hpp>
#include <refrain/worldgen/provider.hpp>

#include <utility>

namespace refrain::worldgen::structure {

namespace {

void collect_element_features(const jigsaw_element_info &element,
                              std::vector<std::string> &out) {
    if (element.kind == pool_element_kind::feature &&
        !element.feature.empty()) {
        out.push_back(element.feature);
        return;
    }
    for (const auto &child : element.elements) {
        collect_element_features(child, out);
    }
}

} // namespace

void collect_feature_ids(const jigsaw_element_info &element,
                         std::vector<std::string> &out) {
    collect_element_features(element, out);
}

void collect_feature_ids(const structure_piece_record &piece,
                         std::vector<std::string> &out) {
    if (piece.jigsaw.has_value()) {
        collect_element_features(piece.jigsaw->element, out);
    }
}

structure_feature_placer::structure_feature_placer(
    const structure_place_context &context,
    const feature::feature_data_set &data)
    : context_{&context}, data_{&data}, registry_{*context.registry} {
    feature_context_.placement.region = context.region;
    feature_context_.placement.chunk = context.chunk;
    feature_context_.placement.states = context.states;
    if (context.chunk != nullptr) {
        feature_context_.placement.generation = provider::generation_context{
            .min_y = context.chunk->min_y(),
            .height = context.chunk->height(),
            .sea_level = context.sea_level,
            .world_seed = context.seed,
        };
    }
    feature_context_.placement.height = [this](heightmap_kind kind, jint x,
                                               jint z) -> jint {
        if (context_->region != nullptr) {
            return context_->region->height_at(kind, x, z);
        }
        return context_->chunk == nullptr ? 0 : context_->chunk->min_y();
    };
    feature_context_.placement.resolve_state = context.state;
    // A structure placement context carries no biome source; features that ask
    // for weather see a non-freezing atmosphere.
    feature_context_.placement.should_freeze = [](const block_pos &) {
        return false;
    };
    feature_context_.placement.should_snow = [](const block_pos &) {
        return false;
    };
    feature_context_.templates = context.templates;
    feature_context_.registry = context.registry;
    feature_context_.place_by_id = [this](std::string_view id,
                                          worldgen_random &random,
                                          const block_pos &position) {
        return place(id, random, position);
    };
}

auto structure_feature_placer::load(std::span<const std::string> ids)
    -> status {
    if (context_->registry == nullptr) {
        return std::unexpected{
            make_error(error_code::not_initialized,
                       "feature placement needs a block state registry")};
    }
    return registry_.load_placed(*data_, ids);
}

auto structure_feature_placer::place(std::string_view id,
                                     worldgen_random &random,
                                     const block_pos &position) -> bool {
    const auto found = registry_.placed().find(std::string{id});
    if (found == registry_.placed().end()) {
        return false;
    }
    return feature::place_placed(feature_context_, found->second, random,
                                 position);
}

} // namespace refrain::worldgen::structure
