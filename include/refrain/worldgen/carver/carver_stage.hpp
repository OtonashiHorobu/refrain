// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The terrain carver pass: the 17x17 source scan that fills the mask, and the
// post-surface application that decides which carved blocks are actually
// written.

#pragma once

#include <refrain/worldgen/carver/carver.hpp>
#include <refrain/worldgen/chunk/block_state.hpp>
#include <refrain/worldgen/chunk/carving_mask.hpp>
#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/terrain/terrain.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace refrain::worldgen::carver {

/// One biome's carvers in data order.
struct carver_biomes {
    std::unordered_map<std::string, std::vector<const world_carver *>>
        by_biome{};
};

class carver_generator {
  public:
    carver_generator(std::function<std::string_view(jint quart_x, jint quart_z)>
                         biome_at_quart,
                     const carver_biomes &biomes, jlong seed,
                     provider::generation_context context);

    /// Generates the carving mask for one chunk; no blocks are written.
    [[nodiscard]] auto generate(const chunk_pos &target) const -> carving_mask;

  private:
    std::function<std::string_view(jint, jint)> biome_at_quart_{};
    const carver_biomes *biomes_{nullptr};
    jlong seed_{0};
    provider::generation_context context_{};
};

struct carve_apply_context {
    terrain::aquifer *aquifer{nullptr};
    const block_state_table *states{nullptr};
    /// Resolves the top material; receives the helper position and whether
    /// the carved block carried a fluid.
    std::function<std::optional<block_state_id>(jint x, jint y, jint z,
                                                bool under_fluid)>
        top_material{};
    /// Optional carving filter; empty means no filter.
    std::function<bool(jint x, jint y, jint z)> filter{};
};

/// Applies the carving mask to a generation chunk.
void apply_carving_mask(proto_chunk &chunk, const carving_mask &mask,
                        carve_apply_context &context);

} // namespace refrain::worldgen::carver
