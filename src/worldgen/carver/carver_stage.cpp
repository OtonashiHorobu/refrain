// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/carver/carver_stage.hpp>

#include <string>
#include <utility>

namespace refrain::worldgen::carver {

carver_generator::carver_generator(
    std::function<std::string_view(jint, jint)> biome_at_quart,
    const carver_biomes &biomes, jlong seed,
    provider::generation_context context)
    : biome_at_quart_{std::move(biome_at_quart)}, biomes_{&biomes}, seed_{seed},
      context_{context} {}

auto carver_generator::generate(const chunk_pos &target) const -> carving_mask {
    const jint max_y = context_.min_y + context_.height - 1 - 7;
    carving_mask mask{context_.min_y + 1, max_y};
    worldgen_random random{std::make_unique<legacy_random_source>(0)};

    for (jint dx = -8; dx <= 8; ++dx) {
        for (jint dz = -8; dz <= 8; ++dz) {
            const chunk_pos source{.x = target.x + dx, .z = target.z + dz};
            const auto biome = biome_at_quart_(source.x * 4, source.z * 4);
            const auto found = biomes_->by_biome.find(std::string{biome});
            if (found == biomes_->by_biome.end()) {
                continue;
            }
            jint index = 0;
            for (const auto *carver : found->second) {
                random.set_large_feature_seed(seed_ + index, source.x,
                                              source.z);
                if (carver->is_start_chunk(random)) {
                    carver->carve(context_, random, target, source, mask);
                }
                ++index;
            }
        }
    }
    return mask;
}

void apply_carving_mask(proto_chunk &chunk, const carving_mask &mask,
                        carve_apply_context &context) {
    const jint chunk_min_x = chunk.chunk_x() * 16;
    const jint chunk_min_z = chunk.chunk_z() * 16;
    const auto &states = *context.states;
    const block_state_id grass =
        context.states->name(0) == "minecraft:grass_block" ? 0 : no_block_state;
    (void)grass;

    mask.visit([&](jint x, jint z, jint bottom, jint top) {
        bool has_grass = false;
        for (jint y = top; y >= bottom; --y) {
            if (context.filter && !context.filter(x, y, z)) {
                continue;
            }
            const auto current = chunk.state_at(x, y, z);
            if (states.in_tag(current, "minecraft:uncarvable")) {
                continue;
            }
            const auto current_name = states.name(current);
            if (current_name == "minecraft:grass_block" ||
                current_name == "minecraft:mycelium") {
                has_grass = true;
            }
            auto carved = context.aquifer->compute_substance(
                chunk_min_x + x, y, chunk_min_z + z, 0.0);
            if (!carved.has_value()) {
                continue;
            }
            chunk.set_state(x, y, z, *carved);
            if (context.aquifer->should_schedule_fluid_update() &&
                states.traits(*carved).fluid) {
                chunk.mark_post_processing(x, y, z);
            }
            if (!has_grass || y - 1 < chunk.min_y()) {
                continue;
            }
            if (states.name(chunk.state_at(x, y - 1, z)) != "minecraft:dirt") {
                continue;
            }
            if (!context.top_material) {
                continue;
            }
            const auto top_state =
                context.top_material(chunk_min_x + x, y - 1, chunk_min_z + z,
                                     states.traits(*carved).fluid);
            if (!top_state.has_value()) {
                continue;
            }
            chunk.set_state(x, y - 1, z, *top_state);
            if (states.traits(*top_state).fluid) {
                chunk.mark_post_processing(x, y - 1, z);
            }
        }
    });
}

} // namespace refrain::worldgen::carver
