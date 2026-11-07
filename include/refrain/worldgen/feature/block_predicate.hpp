// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The block predicates features and disks evaluate against the generation
// region. Only the predicates the current feature slice uses are decoded.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/worldgen/chunk/block_state.hpp>
#include <refrain/worldgen/chunk/chunk_region.hpp>

#include <memory>

namespace refrain::data::json {
class value;
} // namespace refrain::data::json

namespace refrain::worldgen::feature {

class block_predicate {
  public:
    block_predicate() = default;
    block_predicate(const block_predicate &) = delete;
    auto operator=(const block_predicate &) -> block_predicate & = delete;
    block_predicate(block_predicate &&) = delete;
    auto operator=(block_predicate &&) -> block_predicate & = delete;
    virtual ~block_predicate() = default;

    [[nodiscard]] virtual auto test(const chunk_region &region,
                                    const block_state_table &states,
                                    const block_pos &pos) const -> bool = 0;
};

[[nodiscard]] auto decode_block_predicate(const data::json::value &document,
                                          const block_state_table &states)
    -> result<std::unique_ptr<block_predicate>>;

} // namespace refrain::worldgen::feature
