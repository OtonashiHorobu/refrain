// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Block-state identity shared by the chunk and the generation stages. A state
// id is dense within one `block_state_table`; the table answers the properties
// the heightmaps, features and structures test.

#pragma once

#include <refrain/worldgen/chunk/block_defaults.hpp>
#include <refrain/worldgen/core/java_compat.hpp>

#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace refrain::worldgen {

using block_state_id = std::uint16_t;
inline constexpr block_state_id no_block_state =
    std::numeric_limits<block_state_id>::max();

/// The state properties heightmap predicates distinguish. A state is air,
/// carries a fluid, or belongs to one of the two motion-blocking tags.
struct state_traits {
    bool air{true};
    bool fluid{false};
    bool water{false};
    bool blocks_motion{false};
    bool blocks_motion_no_leaves{false};
};

/// Read-only view of the block-state table a chunk belongs to. Implementations
/// own the names and the tag sets; a chunk only stores ids.
class block_state_table {
  public:
    block_state_table() = default;
    block_state_table(const block_state_table &) = delete;
    auto operator=(const block_state_table &) -> block_state_table & = delete;
    block_state_table(block_state_table &&) = default;
    auto operator=(block_state_table &&) -> block_state_table & = default;
    virtual ~block_state_table() = default;

    [[nodiscard]] virtual auto air() const -> block_state_id = 0;
    [[nodiscard]] virtual auto traits(block_state_id id) const
        -> state_traits = 0;
    [[nodiscard]] virtual auto name(block_state_id id) const
        -> std::string_view = 0;
    [[nodiscard]] virtual auto in_tag(block_state_id id,
                                      std::string_view tag) const -> bool = 0;

    /// Properties of the block's default state, sorted by name. A table
    /// without default-state data returns an empty span, so callers treat a
    /// property absent from a bare key as unknown rather than default.
    [[nodiscard]] virtual auto default_properties(block_state_id) const
        -> std::span<const block_default_property> {
        return {};
    }
};

} // namespace refrain::worldgen
