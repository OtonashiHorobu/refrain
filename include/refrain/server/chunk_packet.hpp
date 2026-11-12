// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/data/vanilla.hpp>
#include <refrain/net/registry.hpp>
#include <refrain/server/world_port.hpp>

#include <cstdint>
#include <vector>

namespace refrain::server {

/// Inputs shared by every chunk packet encoded for one world. The source is
/// used only on the thread that owns the simulation; the server builds the
/// spawn batch before the tick loop starts.
struct chunk_packet_source {
    /// World the encoder reads block data and the block-key registry from.
    /// A port whose backend cannot expose chunk payloads fails `chunk_load`.
    world_port *world{nullptr};
    const data::vanilla_tables *tables{nullptr};
    const net::registry_catalog *registries{nullptr};
    std::int32_t min_section_y{-4};
    std::int32_t max_section_y{19};
};

/// Encode one `level_chunk_with_light` packet body (packet id included).
[[nodiscard]] auto encode_full_chunk(const chunk_packet_source &source,
                                     sim::chunk_pos pos)
    -> result<std::vector<std::byte>>;

} // namespace refrain::server
