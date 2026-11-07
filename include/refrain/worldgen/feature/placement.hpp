// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Placement modifiers used by the feature stage. A modifier consumes the
// current position and emits zero or more positions for the next modifier.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/worldgen/chunk/block_state.hpp>
#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/chunk/proto_chunk.hpp>
#include <refrain/worldgen/core/java_compat.hpp>
#include <refrain/worldgen/core/rng.hpp>
#include <refrain/worldgen/provider.hpp>

#include <functional>
#include <memory>
#include <vector>

namespace refrain::data::json {
class value;
} // namespace refrain::data::json

namespace refrain::worldgen {
class block_state_registry;
}

namespace refrain::worldgen::feature {

struct placement_context {
    chunk_region *region{nullptr};
    proto_chunk *chunk{nullptr};
    const block_state_table *states{nullptr};
    provider::generation_context generation{};
    /// World-coordinate heightmap lookup; features that probe outside the
    /// centre chunk use it.
    std::function<jint(worldgen::heightmap_kind, jint, jint)> height{};
    /// Resolves a canonical state key when a feature has to synthesise a
    /// property variant (for example mushroom cap faces).
    std::function<block_state_id(std::string_view)> resolve_state{};
};

class placement_modifier {
  public:
    placement_modifier() = default;
    placement_modifier(const placement_modifier &) = delete;
    auto operator=(const placement_modifier &) -> placement_modifier & = delete;
    placement_modifier(placement_modifier &&) = default;
    auto operator=(placement_modifier &&) -> placement_modifier & = default;
    virtual ~placement_modifier() = default;

    virtual void modify(const placement_context &context,
                        worldgen_random &random, const block_pos &origin,
                        std::vector<block_pos> &out) const = 0;
};

/// Decodes one JSON placement entry.
[[nodiscard]] auto decode_placement(const data::json::value &document,
                                    block_state_registry &states)
    -> result<std::unique_ptr<placement_modifier>>;

} // namespace refrain::worldgen::feature
