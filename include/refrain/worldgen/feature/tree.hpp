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

enum class trunk_kind : std::uint8_t { straight, forking, fancy, dark_oak };

class tree_feature;

struct tree_decorator_context {
    feature_context &context;
    block_state_registry &states;
    worldgen_random &random;
    const std::vector<block_pos> &logs;
    const std::vector<block_pos> &leaves;

    void set_block(const block_pos &pos, block_state_id state) const;
    [[nodiscard]] auto is_air(const block_pos &pos) const -> bool;
    void place_vine(const block_pos &pos, std::string_view direction) const;
};

class tree_decorator {
  public:
    tree_decorator() = default;
    tree_decorator(const tree_decorator &) = delete;
    auto operator=(const tree_decorator &) -> tree_decorator & = delete;
    tree_decorator(tree_decorator &&) = delete;
    auto operator=(tree_decorator &&) -> tree_decorator & = delete;
    virtual ~tree_decorator() = default;
    virtual void place(const tree_decorator_context &context) const = 0;
};

class trunk_vine_decorator final : public tree_decorator {
  public:
    void place(const tree_decorator_context &context) const override;
};

class leave_vine_decorator final : public tree_decorator {
  public:
    explicit leave_vine_decorator(float probability)
        : probability_{probability} {}
    void place(const tree_decorator_context &context) const override;

  private:
    float probability_{0.0F};
};
enum class foliage_kind : std::uint8_t {
    blob,
    acacia,
    spruce,
    fancy,
    dark_oak
};

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
                 jint upper_size, bool three_layers, jint upper_limit,
                 jint middle_size, bool has_min_clipped, jint min_clipped,
                 bool ignore_vines,
                 std::vector<std::unique_ptr<tree_decorator>> decorators = {})
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
          three_layers_{three_layers}, upper_limit_{upper_limit},
          middle_size_{middle_size}, has_min_clipped_{has_min_clipped},
          min_clipped_{min_clipped}, ignore_vines_{ignore_vines},
          decorators_{std::move(decorators)} {}

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
                       bool double_trunk, std::vector<block_pos> &leaves,
                       bool &placed) const;
    void place_leaves_row(feature_context &context, worldgen_random &random,
                          const block_pos &origin, jint radius, jint y,
                          bool double_trunk, std::vector<block_pos> &leaves,
                          bool &placed) const;
    [[nodiscard]] auto size_at_height(jint tree_height, jint y) const -> jint;

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
    bool three_layers_{false};
    jint upper_limit_{1};
    jint middle_size_{1};
    bool has_min_clipped_{false};
    jint min_clipped_{0};
    bool ignore_vines_{false};
    std::vector<std::unique_ptr<tree_decorator>> decorators_{};
};

} // namespace refrain::worldgen::feature
