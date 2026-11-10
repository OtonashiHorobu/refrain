// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Block-state providers: the value a feature writes. The first slice supports
// direct states and weighted lists; other provider types are added as the
// owning feature families need them.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/worldgen/chunk/block_state.hpp>
#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/core/rng.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace refrain::data::json {
class value;
} // namespace refrain::data::json

namespace refrain::worldgen {
class block_state_registry;
}

namespace refrain::worldgen::feature {

struct placement_context;

class state_provider {
  public:
    state_provider() = default;
    state_provider(const state_provider &) = delete;
    auto operator=(const state_provider &) -> state_provider & = delete;
    state_provider(state_provider &&) = default;
    auto operator=(state_provider &&) -> state_provider & = default;
    virtual ~state_provider() = default;

    [[nodiscard]] virtual auto get(const placement_context &context,
                                   worldgen_random &random,
                                   const block_pos &pos) const
        -> block_state_id = 0;
    [[nodiscard]] virtual auto get_optional(const placement_context &context,
                                            worldgen_random &random,
                                            const block_pos &pos) const
        -> std::optional<block_state_id> {
        return get(context, random, pos);
    }
};

[[nodiscard]] auto simple_state(block_state_id state)
    -> std::unique_ptr<state_provider>;

using state_provider_resolver =
    std::function<const state_provider *(std::string_view)>;

[[nodiscard]] auto
decode_state_provider(const data::json::value &document,
                      block_state_registry &states,
                      const state_provider_resolver &resolve = {})
    -> result<std::unique_ptr<state_provider>>;

} // namespace refrain::worldgen::feature
