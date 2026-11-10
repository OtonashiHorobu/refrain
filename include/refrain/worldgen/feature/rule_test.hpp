// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The rule-test subset ore targets use: block/tag matches, height matches and
// the boolean combinators.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/worldgen/chunk/block_state.hpp>
#include <refrain/worldgen/chunk/chunk_region.hpp>
#include <refrain/worldgen/core/java_compat.hpp>
#include <refrain/worldgen/core/rng.hpp>

#include <memory>

namespace refrain::data::json {
class value;
} // namespace refrain::data::json

namespace refrain::worldgen::feature {

class rule_test {
  public:
    rule_test() = default;
    rule_test(const rule_test &) = delete;
    auto operator=(const rule_test &) -> rule_test & = delete;
    rule_test(rule_test &&) = delete;
    auto operator=(rule_test &&) -> rule_test & = delete;
    virtual ~rule_test() = default;

    [[nodiscard]] virtual auto test(block_state_id state, const block_pos &pos,
                                    worldgen_random &random) const -> bool = 0;
};

[[nodiscard]] auto decode_rule_test(const data::json::value &document,
                                    const block_state_table &states)
    -> result<std::unique_ptr<rule_test>>;

} // namespace refrain::worldgen::feature
