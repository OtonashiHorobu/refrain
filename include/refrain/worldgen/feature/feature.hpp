// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// A configured feature places blocks through the generation region. The
// runtime object is built from `worldgen/feature` once the stage is entered.

#pragma once

#include <refrain/worldgen/chunk/block_state.hpp>
#include <refrain/worldgen/core/rng.hpp>
#include <refrain/worldgen/feature/block_predicate.hpp>
#include <refrain/worldgen/feature/placement.hpp>

namespace refrain::worldgen::feature {

struct feature_context {
    placement_context placement{};
};

class feature {
  public:
    feature() = default;
    feature(const feature &) = delete;
    auto operator=(const feature &) -> feature & = delete;
    feature(feature &&) = delete;
    auto operator=(feature &&) -> feature & = delete;
    virtual ~feature() = default;

    [[nodiscard]] virtual auto place(feature_context &context,
                                     worldgen_random &random,
                                     const block_pos &origin) const -> bool = 0;
};

/// `SimpleBlockFeature` restricted to the plain block-state form.
class simple_block_feature final : public feature {
  public:
    simple_block_feature(block_state_id state, bool schedule_tick)
        : state_{state}, schedule_tick_{schedule_tick} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &,
                             const block_pos &origin) const -> bool override;

  private:
    block_state_id state_{no_block_state};
    bool schedule_tick_{false};
};

/// `DiskFeature`: a flat disk of one state over a target predicate.
class disk_feature final : public feature {
  public:
    disk_feature(block_state_id state, std::unique_ptr<block_predicate> target,
                 std::unique_ptr<provider::int_provider> radius,
                 jint half_height)
        : state_{state}, target_{std::move(target)}, radius_{std::move(radius)},
          half_height_{half_height} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    block_state_id state_{no_block_state};
    std::unique_ptr<block_predicate> target_{};
    std::unique_ptr<provider::int_provider> radius_{};
    jint half_height_{0};
};

} // namespace refrain::worldgen::feature
