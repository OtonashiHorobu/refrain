// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/biome/biome_view.hpp>

namespace refrain::worldgen {

void fill_chunk_biomes(proto_chunk &chunk, multi_noise_biome_source &source,
                       const climate_sampler &sampler) {
    const jint base_quart_x = chunk.chunk_x() * 4;
    const jint base_quart_z = chunk.chunk_z() * 4;
    for (jint section = 0; section < chunk.section_count(); ++section) {
        const jint min_quart_y = (chunk.min_y() + section * 16) >> 2;
        for (jint x = 0; x < 4; ++x) {
            for (jint y = 0; y < 4; ++y) {
                for (jint z = 0; z < 4; ++z) {
                    const jint quart_y = min_quart_y + y;
                    const auto biome = source.noise_biome(
                        sampler, base_quart_x + x, quart_y, base_quart_z + z);
                    chunk.set_biome(x * 4, quart_y * 4, z * 4, biome);
                }
            }
        }
    }
}

auto chunk_biome_resolver::get_noise_biome(jint quart_x, jint quart_y,
                                           jint quart_z) -> std::uint16_t {
    const auto *chunk = lookup_(quart_x >> 2, quart_z >> 2);
    if (chunk != nullptr) {
        const jint block_y = quart_y << 2;
        if (block_y >= chunk->min_y() &&
            block_y < chunk->min_y() + chunk->height()) {
            return chunk->biome_at((quart_x & 3) << 2, block_y,
                                   (quart_z & 3) << 2);
        }
    }
    return fallback_->get_noise_biome(quart_x, quart_y, quart_z);
}

} // namespace refrain::worldgen
