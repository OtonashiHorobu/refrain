// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Typed read-only views over RWF native aux objects:
//
//   global/data/<namespace>/<name>.dat   media=nbt (uncompressed NBT binary)
//   players/<uuid>/advancements|stats    media=json (byte-exact)
//
// The bytes remain the source of truth; these views parse on read and never
// rewrite.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/world/object_store.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::world {

/// `global/data/<namespace>/<name>.dat`; rejects unsafe components.
[[nodiscard]] auto saved_data_path(std::string_view namespace_name,
                                   std::string_view name)
    -> result<std::string>;

/// Read one NBT object (media=nbt) and parse its root compound.
[[nodiscard]] auto read_nbt_object(object_store &objects, std::string_view path,
                                   std::size_t max_bytes = 64ULL << 20U)
    -> result<data::nbt::value>;

/// Write one NBT object as uncompressed NBT binary (batched by default).
[[nodiscard]] auto write_nbt_object(object_store &objects,
                                    std::string_view path,
                                    const data::nbt::value &value,
                                    bool durable = false) -> status;

struct world_gen_settings {
    std::int32_t data_version{0};
    std::int64_t seed{0};
    bool has_seed{false};
    bool generate_structures{false};
};

/// `world_gen_settings.dat` (`{DataVersion, data:{seed,generate_structures}}`).
[[nodiscard]] auto read_world_gen_settings(object_store &objects)
    -> result<world_gen_settings>;

/// `game_rules.dat` (`{data:{<rule>: string}}`), values kept verbatim.
[[nodiscard]] auto read_game_rules(object_store &objects)
    -> result<std::map<std::string, std::string>>;

/// Player JSON bytes (`advancements` or `stats`), byte-exact.
[[nodiscard]] auto read_player_json(object_store &objects,
                                    std::string_view uuid,
                                    std::string_view kind)
    -> result<std::string>;

/// UUIDs that have at least one advancements/stats object, sorted.
[[nodiscard]] auto list_player_views(object_store &objects)
    -> result<std::vector<std::string>>;

} // namespace refrain::world
