// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The biome view the generation stages run against. Biomes are chosen once per
// chunk cell during the `biomes` status and later read back from the chunk;
// re-running the multi-noise search for every block would give a different
// answer whenever two parameter points are equidistant, because the reference
// search keeps the previous result as its starting candidate.

#pragma once

#include <refrain/worldgen/biome/biome_manager.hpp>
#include <refrain/worldgen/biome/biome_source.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>

#include <functional>

namespace refrain::worldgen {

/// `ChunkGenerator.doCreateBiomes` and `ChunkAccess.fillBiomesFromNoise`:
/// fills the chunk's 4x4x4 quart biome cells in the reference order (sections
/// bottom to top, then x, y, z). `source` carries the search cache, so one
/// source must be shared by everything that queries biomes for the same chunk.
void fill_chunk_biomes(proto_chunk &chunk, multi_noise_biome_source &source,
                       const climate_sampler &sampler);

/// `LevelReader.getNoiseBiome`: reads a quart biome from a loaded generation
/// chunk and falls back to the uncached noise resolver outside it. The biome
/// manager used by surface material rules reads through this view.
class chunk_biome_resolver final : public biome_resolver {
  public:
    using chunk_lookup =
        std::function<const proto_chunk *(jint chunk_x, jint chunk_z)>;

    chunk_biome_resolver(chunk_lookup lookup, biome_resolver &fallback)
        : lookup_{std::move(lookup)}, fallback_{&fallback} {}

    [[nodiscard]] auto get_noise_biome(jint quart_x, jint quart_y, jint quart_z)
        -> std::uint16_t override;

  private:
    chunk_lookup lookup_{};
    biome_resolver *fallback_{nullptr};
};

} // namespace refrain::worldgen
