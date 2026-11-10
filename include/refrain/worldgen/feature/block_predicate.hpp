// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The block predicates features, placement filters and disks evaluate against
// the generation region. Every predicate type the data pack uses is decoded;
// an unknown type is reported instead of being ignored.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/worldgen/chunk/block_state.hpp>
#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/provider.hpp>

#include <functional>
#include <memory>
#include <string_view>

namespace refrain::data::json {
class value;
} // namespace refrain::data::json

namespace refrain::worldgen::feature {

struct placement_context;

/// Everything a predicate may inspect. `biome` is empty when the caller has no
/// biome source; the biome membership predicate then fails. The generation
/// bounds resolve height anchors.
struct block_predicate_context {
    const chunk_region *region{nullptr};
    const block_state_table *states{nullptr};
    const block_pos *pos{nullptr};
    provider::generation_context generation{};
    std::function<std::string_view(const block_pos &)> biome{};
};

class block_predicate {
  public:
    block_predicate() = default;
    block_predicate(const block_predicate &) = delete;
    auto operator=(const block_predicate &) -> block_predicate & = delete;
    block_predicate(block_predicate &&) = delete;
    auto operator=(block_predicate &&) -> block_predicate & = delete;
    virtual ~block_predicate() = default;

    /// Tests the predicate with the default generation bounds and no biome
    /// source.
    [[nodiscard]] auto test(const chunk_region &region,
                            const block_state_table &states,
                            const block_pos &pos) const -> bool;

    /// Tests the predicate with the full placement environment.
    [[nodiscard]] auto test(const placement_context &context,
                            const block_pos &pos) const -> bool;

    [[nodiscard]] auto evaluate(const block_predicate_context &context) const
        -> bool {
        return test_impl(context);
    }

  protected:
    [[nodiscard]] virtual auto
    test_impl(const block_predicate_context &context) const -> bool = 0;
};

[[nodiscard]] auto decode_block_predicate(const data::json::value &document,
                                          const block_state_table &states)
    -> result<std::unique_ptr<block_predicate>>;

} // namespace refrain::worldgen::feature
