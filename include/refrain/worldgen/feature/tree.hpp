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

enum class trunk_kind : std::uint8_t {
    straight,
    forking,
    fancy,
    dark_oak,
    giant,
    mega_jungle
};

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

class shelf_mushroom_decorator final : public tree_decorator {
  public:
    explicit shelf_mushroom_decorator(float probability)
        : probability_{probability} {}
    void place(const tree_decorator_context &context) const override;

  private:
    float probability_{0.0F};
};

class attached_to_logs_decorator final : public tree_decorator {
  public:
    attached_to_logs_decorator(float probability,
                               std::unique_ptr<state_provider> provider,
                               std::vector<std::string> directions)
        : probability_{probability}, provider_{std::move(provider)},
          directions_{std::move(directions)} {}
    void place(const tree_decorator_context &context) const override;

  private:
    float probability_{0.0F};
    std::unique_ptr<state_provider> provider_;
    std::vector<std::string> directions_{};
};

class attached_to_leaves_decorator final : public tree_decorator {
  public:
    attached_to_leaves_decorator(float probability, jint exclusion_radius_xz,
                                 jint exclusion_radius_y,
                                 std::unique_ptr<state_provider> provider,
                                 jint required_empty_blocks,
                                 std::vector<std::string> directions)
        : probability_{probability}, exclusion_xz_{exclusion_radius_xz},
          exclusion_y_{exclusion_radius_y}, provider_{std::move(provider)},
          required_empty_{required_empty_blocks},
          directions_{std::move(directions)} {}
    void place(const tree_decorator_context &context) const override;

  private:
    float probability_{0.0F};
    jint exclusion_xz_{0};
    jint exclusion_y_{0};
    std::unique_ptr<state_provider> provider_;
    jint required_empty_{0};
    std::vector<std::string> directions_{};
};

class cocoa_decorator final : public tree_decorator {
  public:
    explicit cocoa_decorator(float probability) : probability_{probability} {}
    void place(const tree_decorator_context &context) const override;

  private:
    float probability_{0.0F};
};

class place_on_ground_decorator final : public tree_decorator {
  public:
    place_on_ground_decorator(jint tries, jint radius, jint height,
                              std::unique_ptr<state_provider> provider)
        : tries_{tries}, radius_{radius}, height_{height},
          provider_{std::move(provider)} {}
    void place(const tree_decorator_context &context) const override;

  private:
    jint tries_{128};
    jint radius_{2};
    jint height_{1};
    std::unique_ptr<state_provider> provider_;
};

class beehive_decorator final : public tree_decorator {
  public:
    explicit beehive_decorator(float probability) : probability_{probability} {}
    void place(const tree_decorator_context &context) const override;

  private:
    float probability_{0.0F};
};

class alter_ground_decorator final : public tree_decorator {
  public:
    explicit alter_ground_decorator(std::unique_ptr<state_provider> provider)
        : provider_{std::move(provider)} {}
    void place(const tree_decorator_context &context) const override;

  private:
    void place_circle(const tree_decorator_context &context,
                      const block_pos &center) const;
    void place_block_at(const tree_decorator_context &context,
                        const block_pos &pos) const;

    std::unique_ptr<state_provider> provider_;
};

class leave_vine_decorator final : public tree_decorator {
  public:
    explicit leave_vine_decorator(float probability)
        : probability_{probability} {}
    void place(const tree_decorator_context &context) const override;

  private:
    float probability_{0.0F};
};
class fallen_tree_feature final : public feature {
  public:
    fallen_tree_feature(block_state_registry &states,
                        std::unique_ptr<state_provider> trunk_provider,
                        std::unique_ptr<provider::int_provider> log_length,
                        std::vector<std::unique_ptr<tree_decorator>> stump,
                        std::vector<std::unique_ptr<tree_decorator>> logs)
        : states_{&states}, trunk_provider_{std::move(trunk_provider)},
          log_length_{std::move(log_length)},
          stump_decorators_{std::move(stump)},
          log_decorators_{std::move(logs)} {}

    [[nodiscard]] auto place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool override;

  private:
    block_state_registry *states_{nullptr};
    std::unique_ptr<state_provider> trunk_provider_{};
    std::unique_ptr<provider::int_provider> log_length_{};
    std::vector<std::unique_ptr<tree_decorator>> stump_decorators_{};
    std::vector<std::unique_ptr<tree_decorator>> log_decorators_{};
};

enum class foliage_kind : std::uint8_t {
    blob,
    acacia,
    spruce,
    fancy,
    dark_oak,
    mega_pine,
    pine,
    jungle,
    bush
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
                 std::unique_ptr<provider::int_provider> mega_pine_crown,
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
          mega_pine_crown_{std::move(mega_pine_crown)},
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
    std::unique_ptr<provider::int_provider> mega_pine_crown_{};
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
