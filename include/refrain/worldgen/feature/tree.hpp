// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The straight-trunk / blob-foliage tree variant. This is the first slice of
// the tree family; other trunk, foliage, root and decorator types are decoded
// as they gain an implementation.

#pragma once

#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/feature/feature.hpp>

#include <string>
#include <vector>

namespace refrain::worldgen::feature {

enum class trunk_kind : std::uint8_t { straight, forking };
enum class foliage_kind : std::uint8_t { blob, acacia, spruce };

class tree_feature final : public feature {
  public:
    tree_feature(block_state_registry &states, trunk_kind kind,
                 foliage_kind foliage,
                 std::unique_ptr<state_provider> trunk_provider,
                 std::unique_ptr<state_provider> foliage_provider,
                 std::unique_ptr<state_provider> below_trunk_provider,
                 jint base_height, jint height_rand_a, jint height_rand_b,
                 std::unique_ptr<provider::int_provider> foliage_radius,
                 std::unique_ptr<provider::int_provider> foliage_offset,
                 std::unique_ptr<provider::int_provider> spruce_trunk_height,
                 jint foliage_height, jint size_limit, jint lower_size,
                 jint upper_size, bool has_min_clipped, jint min_clipped,
                 bool ignore_vines)
        : states_{&states}, kind_{kind}, foliage_{foliage},
          trunk_provider_{std::move(trunk_provider)},
          foliage_provider_{std::move(foliage_provider)},
          below_trunk_provider_{std::move(below_trunk_provider)},
          base_height_{base_height}, height_rand_a_{height_rand_a},
          height_rand_b_{height_rand_b},
          foliage_radius_{std::move(foliage_radius)},
          foliage_offset_{std::move(foliage_offset)},
          spruce_trunk_height_{std::move(spruce_trunk_height)},
          foliage_height_{foliage_height}, size_limit_{size_limit},
          lower_size_{lower_size}, upper_size_{upper_size},
          has_min_clipped_{has_min_clipped}, min_clipped_{min_clipped},
          ignore_vines_{ignore_vines} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    [[nodiscard]] auto is_free(feature_context &context,
                               const block_pos &pos) const -> bool;
    [[nodiscard]] auto valid_tree_pos(feature_context &context,
                                      const block_pos &pos) const -> bool;
    [[nodiscard]] auto is_vine(feature_context &context,
                               const block_pos &pos) const -> bool;
    void place_foliage(feature_context &context, worldgen_random &random,
                       const block_pos &attachment, jint leaf_radius,
                       jint radius_offset, jint foliage_height,
                       std::vector<block_pos> &leaves, bool &placed) const;
    void place_leaves_row(feature_context &context, worldgen_random &random,
                          const block_pos &origin, jint radius, jint y,
                          std::vector<block_pos> &leaves, bool &placed) const;
    void update_leaves(feature_context &context,
                       const std::vector<block_pos> &logs,
                       const std::vector<block_pos> &leaves) const;

    block_state_registry *states_{nullptr};
    trunk_kind kind_{trunk_kind::straight};
    foliage_kind foliage_{foliage_kind::blob};
    std::unique_ptr<state_provider> trunk_provider_{};
    std::unique_ptr<state_provider> foliage_provider_{};
    std::unique_ptr<state_provider> below_trunk_provider_{};
    jint base_height_{0};
    jint height_rand_a_{0};
    jint height_rand_b_{0};
    std::unique_ptr<provider::int_provider> foliage_radius_{};
    std::unique_ptr<provider::int_provider> foliage_offset_{};
    std::unique_ptr<provider::int_provider> spruce_trunk_height_{};
    jint foliage_height_{0};
    jint size_limit_{1};
    jint lower_size_{0};
    jint upper_size_{1};
    bool has_min_clipped_{false};
    jint min_clipped_{0};
    bool ignore_vines_{false};
};

} // namespace refrain::worldgen::feature
