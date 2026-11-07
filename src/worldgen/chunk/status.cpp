// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/chunk/status.hpp>

namespace refrain::worldgen {

auto status_name(chunk_status status) -> std::string_view {
    switch (status) {
    case chunk_status::empty:
        return "empty";
    case chunk_status::structure_starts:
        return "structure_starts";
    case chunk_status::structure_references:
        return "structure_references";
    case chunk_status::biomes:
        return "biomes";
    case chunk_status::terrain:
        return "terrain";
    case chunk_status::features:
        return "features";
    case chunk_status::initialize_light:
        return "initialize_light";
    case chunk_status::light:
        return "light";
    case chunk_status::spawn:
        return "spawn";
    case chunk_status::full:
        return "full";
    }
    return "unknown";
}

auto status_at_least(chunk_status value, chunk_status minimum) -> bool {
    return static_cast<std::uint8_t>(value) >=
           static_cast<std::uint8_t>(minimum);
}

} // namespace refrain::worldgen
