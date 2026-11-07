// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/feature/tree.hpp>

#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string>
#include <vector>

namespace refrain::worldgen::feature {

namespace {

struct tree_attachment {
    block_pos pos{};
    jint radius_offset{0};
    bool double_trunk{false};
};

struct tree_bounds {
    jint min_x{0};
    jint min_y{0};
    jint min_z{0};
    jint max_x{0};
    jint max_y{0};
    jint max_z{0};

    [[nodiscard]] auto contains(const block_pos &pos) const -> bool {
        return pos.x >= min_x && pos.x <= max_x && pos.y >= min_y &&
               pos.y <= max_y && pos.z >= min_z && pos.z <= max_z;
    }

    [[nodiscard]] auto index(const block_pos &pos) const -> std::size_t {
        const auto x = static_cast<std::size_t>(pos.x - min_x);
        const auto y = static_cast<std::size_t>(pos.y - min_y);
        const auto z = static_cast<std::size_t>(pos.z - min_z);
        return x + (max_x - min_x + 1) * (y + (max_y - min_y + 1) * z);
    }
};

void expand(tree_bounds &bounds, const block_pos &pos, bool &first) {
    if (first) {
        bounds = tree_bounds{.min_x = pos.x,
                             .min_y = pos.y,
                             .min_z = pos.z,
                             .max_x = pos.x,
                             .max_y = pos.y,
                             .max_z = pos.z};
        first = false;
        return;
    }
    bounds.min_x = std::min(bounds.min_x, pos.x);
    bounds.min_y = std::min(bounds.min_y, pos.y);
    bounds.min_z = std::min(bounds.min_z, pos.z);
    bounds.max_x = std::max(bounds.max_x, pos.x);
    bounds.max_y = std::max(bounds.max_y, pos.y);
    bounds.max_z = std::max(bounds.max_z, pos.z);
}

} // namespace

auto tree_feature::size_at_height(jint tree_height, jint y) const -> jint {
    if (y < size_limit_) {
        return lower_size_;
    }
    if (three_layers_) {
        return y >= tree_height - upper_limit_ ? upper_size_ : middle_size_;
    }
    return upper_size_;
}

void tree_decorator_context::set_block(const block_pos &pos,
                                       block_state_id state) const {
    (void)context.placement.region->set_block_state(pos.x, pos.y, pos.z, state);
}

auto tree_decorator_context::is_air(const block_pos &pos) const -> bool {
    return is_air_block_name(context.placement.states->name(
        context.placement.region->state_at(pos.x, pos.y, pos.z)));
}

void tree_decorator_context::place_vine(const block_pos &pos,
                                        std::string_view direction) const {
    auto key = std::string{"minecraft:vine["};
    key.append(direction);
    key.append("=true]");
    auto found = states.find(key);
    if (!found) {
        const auto interned =
            states.intern(key, state_traits{.air = false,
                                            .blocks_motion = false,
                                            .blocks_motion_no_leaves = false});
        if (!interned) {
            return;
        }
        found = *interned;
    }
    set_block(pos, *found);
}

void trunk_vine_decorator::place(const tree_decorator_context &context) const {
    for (const auto &log : context.logs) {
        if (context.random.next_int(3) > 0) {
            const block_pos pos{.x = log.x - 1, .y = log.y, .z = log.z};
            if (context.is_air(pos)) {
                context.place_vine(pos, "east");
            }
        }
        if (context.random.next_int(3) > 0) {
            const block_pos pos{.x = log.x + 1, .y = log.y, .z = log.z};
            if (context.is_air(pos)) {
                context.place_vine(pos, "west");
            }
        }
        if (context.random.next_int(3) > 0) {
            const block_pos pos{.x = log.x, .y = log.y, .z = log.z - 1};
            if (context.is_air(pos)) {
                context.place_vine(pos, "south");
            }
        }
        if (context.random.next_int(3) > 0) {
            const block_pos pos{.x = log.x, .y = log.y, .z = log.z + 1};
            if (context.is_air(pos)) {
                context.place_vine(pos, "north");
            }
        }
    }
}

void leave_vine_decorator::place(const tree_decorator_context &context) const {
    const auto add_hanging = [&](const block_pos &start,
                                 std::string_view direction) {
        context.place_vine(start, direction);
        block_pos below{.x = start.x, .y = start.y - 1, .z = start.z};
        for (jint remaining = 4; remaining > 0 && context.is_air(below);
             --remaining) {
            context.place_vine(below, direction);
            --below.y;
        }
    };
    for (const auto &leaf : context.leaves) {
        if (context.random.next_float() < probability_) {
            const block_pos pos{.x = leaf.x - 1, .y = leaf.y, .z = leaf.z};
            if (context.is_air(pos)) {
                add_hanging(pos, "east");
            }
        }
        if (context.random.next_float() < probability_) {
            const block_pos pos{.x = leaf.x + 1, .y = leaf.y, .z = leaf.z};
            if (context.is_air(pos)) {
                add_hanging(pos, "west");
            }
        }
        if (context.random.next_float() < probability_) {
            const block_pos pos{.x = leaf.x, .y = leaf.y, .z = leaf.z - 1};
            if (context.is_air(pos)) {
                add_hanging(pos, "south");
            }
        }
        if (context.random.next_float() < probability_) {
            const block_pos pos{.x = leaf.x, .y = leaf.y, .z = leaf.z + 1};
            if (context.is_air(pos)) {
                add_hanging(pos, "north");
            }
        }
    }
}

void beehive_decorator::place(const tree_decorator_context &context) const {
    if (context.logs.empty()) {
        return;
    }
    if (context.random.next_float() >= probability_) {
        return;
    }
    const jint hive_y =
        context.leaves.empty()
            ? std::min(context.logs.front().y + 1 + context.random.next_int(3),
                       context.logs.back().y)
            : std::max(context.leaves.front().y - 1,
                       context.logs.front().y + 1);
    std::vector<block_pos> placements;
    for (const auto &log : context.logs) {
        if (log.y != hive_y) {
            continue;
        }
        placements.push_back({.x = log.x + 1, .y = log.y, .z = log.z});
        placements.push_back({.x = log.x, .y = log.y, .z = log.z + 1});
        placements.push_back({.x = log.x - 1, .y = log.y, .z = log.z});
    }
    for (std::size_t i = placements.size(); i > 1; --i) {
        const auto swap_to = static_cast<std::size_t>(
            context.random.next_int(static_cast<jint>(i)));
        std::swap(placements[i - 1], placements[swap_to]);
    }
    for (const auto &position : placements) {
        const block_pos south{
            .x = position.x, .y = position.y, .z = position.z + 1};
        if (!context.is_air(position) || !context.is_air(south)) {
            continue;
        }
        auto key = std::string{"minecraft:bee_nest[facing=south]"};
        auto found = context.states.find(key);
        if (!found) {
            const auto interned = context.states.intern(
                key, state_traits{.air = false,
                                  .blocks_motion = true,
                                  .blocks_motion_no_leaves = true});
            if (!interned) {
                return;
            }
            found = *interned;
        }
        context.set_block(position, *found);
        return;
    }
}

void alter_ground_decorator::place_block_at(
    const tree_decorator_context &context, const block_pos &pos) const {
    for (jint dy = 2; dy >= -3; --dy) {
        const block_pos cursor{.x = pos.x, .y = pos.y + dy, .z = pos.z};
        const auto state = provider_->get_optional(context.context.placement,
                                                   context.random, cursor);
        if (state.has_value()) {
            context.set_block(cursor, *state);
            break;
        }
        if (!context.is_air(cursor) && dy < 0) {
            break;
        }
    }
}

void alter_ground_decorator::place_circle(const tree_decorator_context &context,
                                          const block_pos &center) const {
    for (jint dx = -2; dx <= 2; ++dx) {
        for (jint dz = -2; dz <= 2; ++dz) {
            if (std::abs(dx) != 2 || std::abs(dz) != 2) {
                place_block_at(context, block_pos{.x = center.x + dx,
                                                  .y = center.y,
                                                  .z = center.z + dz});
            }
        }
    }
}

void alter_ground_decorator::place(
    const tree_decorator_context &context) const {
    if (context.logs.empty()) {
        return;
    }
    const jint min_y = context.logs.front().y;
    for (const auto &log : context.logs) {
        if (log.y != min_y) {
            continue;
        }
        place_circle(context,
                     block_pos{.x = log.x - 1, .y = log.y, .z = log.z - 1});
        place_circle(context,
                     block_pos{.x = log.x + 2, .y = log.y, .z = log.z - 1});
        place_circle(context,
                     block_pos{.x = log.x - 1, .y = log.y, .z = log.z + 2});
        place_circle(context,
                     block_pos{.x = log.x + 2, .y = log.y, .z = log.z + 2});
        for (jint i = 0; i < 5; ++i) {
            const jint placement = context.random.next_int(64);
            const jint xx = placement % 8;
            const jint zz = placement / 8;
            if (xx == 0 || xx == 7 || zz == 0 || zz == 7) {
                place_circle(context, block_pos{.x = log.x - 3 + xx,
                                                .y = log.y,
                                                .z = log.z - 3 + zz});
            }
        }
    }
}

auto tree_feature::valid_tree_pos(feature_context &context,
                                  const block_pos &pos) const -> bool {
    const auto state = context.placement.region->state_at(pos.x, pos.y, pos.z);
    if (is_air_block_name(context.placement.states->name(state))) {
        return true;
    }
    return context.placement.states->in_tag(state,
                                            "minecraft:replaceable_by_trees");
}

auto tree_feature::is_free(feature_context &context, const block_pos &pos) const
    -> bool {
    const auto state = context.placement.region->state_at(pos.x, pos.y, pos.z);
    return valid_tree_pos(context, pos) ||
           context.placement.states->in_tag(state, "minecraft:logs");
}

auto tree_feature::is_vine(feature_context &context, const block_pos &pos) const
    -> bool {
    return state_block_name(context.placement.states->name(
               context.placement.region->state_at(pos.x, pos.y, pos.z))) ==
           "minecraft:vine";
}

void tree_feature::place_leaves_row(feature_context &context,
                                    worldgen_random &random,
                                    const block_pos &origin, jint radius,
                                    jint y, bool double_trunk,
                                    std::vector<block_pos> &leaves,
                                    bool &placed) const {
    const jint extent = radius + (double_trunk ? 1 : 0);
    for (jint dx = -radius; dx <= extent; ++dx) {
        const jint abs_dx = std::abs(dx);
        for (jint dz = -radius; dz <= extent; ++dz) {
            const jint abs_dz = std::abs(dz);
            const jint min_dx = double_trunk
                                    ? std::min(std::abs(dx), std::abs(dx - 1))
                                    : abs_dx;
            const jint min_dz = double_trunk
                                    ? std::min(std::abs(dz), std::abs(dz - 1))
                                    : abs_dz;
            bool skip = false;
            if (foliage_ == foliage_kind::dark_oak) {
                const bool corner = y == 0 && double_trunk &&
                                    (dx == -radius || dx >= radius) &&
                                    (dz == -radius || dz >= radius);
                if (corner) {
                    skip = true;
                } else if (y == -1 && !double_trunk && min_dx == radius &&
                           min_dz == radius) {
                    skip = true;
                } else if (y == 1 && min_dx + min_dz > radius * 2 - 2) {
                    skip = true;
                }
            } else if (foliage_ == foliage_kind::blob) {
                if (min_dx == radius && min_dz == radius &&
                    (random.next_int(2) == 0 || y == 0)) {
                    skip = true;
                }
            } else if (foliage_ == foliage_kind::acacia) {
                if (y == 0
                        ? (min_dx > 1 || min_dz > 1) && dx != 0 && dz != 0
                        : min_dx == radius && min_dz == radius && radius > 0) {
                    skip = true;
                }
            } else if (foliage_ == foliage_kind::fancy) {
                const float x = static_cast<float>(min_dx) + 0.5F;
                const float z = static_cast<float>(min_dz) + 0.5F;
                if (x * x + z * z >
                    static_cast<float>(radius) * static_cast<float>(radius)) {
                    skip = true;
                }
            } else if (foliage_ == foliage_kind::mega_pine) {
                if (min_dx + min_dz >= 7 ||
                    min_dx * min_dx + min_dz * min_dz > radius * radius) {
                    skip = true;
                }
            } else if (min_dx == radius && min_dz == radius && radius > 0) {
                skip = true;
            }
            if (skip) {
                continue;
            }
            const block_pos pos{
                .x = origin.x + dx, .y = origin.y + y, .z = origin.z + dz};
            const auto current =
                context.placement.region->state_at(pos.x, pos.y, pos.z);
            const auto current_key = context.placement.states->name(current);
            const auto persistent = state_property(current_key, "persistent");
            if (persistent.has_value() && *persistent == "true") {
                continue;
            }
            if (!valid_tree_pos(context, pos)) {
                continue;
            }
            auto state = foliage_provider_->get(context.placement, random, pos);
            const auto key = context.placement.states->name(state);
            if (state_property(key, "waterlogged").has_value()) {
                auto modified = std::string{key};
                const auto water =
                    state_block_name(context.placement.states->name(
                        context.placement.region->state_at(
                            pos.x, pos.y, pos.z))) == "minecraft:water";
                modified = with_state_property(modified, "waterlogged",
                                               water ? "true" : "false");
                auto found = states_->find(modified);
                if (!found) {
                    const auto interned = states_->intern(
                        modified, context.placement.states->traits(state));
                    if (interned) {
                        found = *interned;
                    }
                }
                if (found) {
                    state = *found;
                }
            }
            if (context.placement.region->set_block_state(pos.x, pos.y, pos.z,
                                                          state)) {
                leaves.push_back(pos);
                placed = true;
            }
        }
    }
}

void tree_feature::place_foliage(feature_context &context,
                                 worldgen_random &random,
                                 const block_pos &attachment, jint leaf_radius,
                                 jint radius_offset, jint foliage_height,
                                 bool double_trunk,
                                 std::vector<block_pos> &leaves,
                                 bool &placed) const {
    const jint offset = foliage_offset_->sample(random);
    if (foliage_ == foliage_kind::blob) {
        for (jint yo = offset; yo >= offset - foliage_height; --yo) {
            const jint current =
                std::max(leaf_radius + radius_offset - 1 - yo / 2, 0);
            place_leaves_row(context, random, attachment, current, yo, false,
                             leaves, placed);
        }
        return;
    }
    if (foliage_ == foliage_kind::dark_oak) {
        const block_pos pos{
            .x = attachment.x, .y = attachment.y + offset, .z = attachment.z};
        if (double_trunk) {
            place_leaves_row(context, random, pos, leaf_radius + 2, -1, true,
                             leaves, placed);
            place_leaves_row(context, random, pos, leaf_radius + 3, 0, true,
                             leaves, placed);
            place_leaves_row(context, random, pos, leaf_radius + 2, 1, true,
                             leaves, placed);
            if (random.next_boolean()) {
                place_leaves_row(context, random, pos, leaf_radius, 2, true,
                                 leaves, placed);
            }
        } else {
            place_leaves_row(context, random, pos, leaf_radius + 2, -1, false,
                             leaves, placed);
            place_leaves_row(context, random, pos, leaf_radius + 1, 0, false,
                             leaves, placed);
        }
        return;
    }
    if (foliage_ == foliage_kind::mega_pine) {
        jint previous = 0;
        for (jint yy = attachment.y - foliage_height + offset;
             yy <= attachment.y + offset; ++yy) {
            const jint yo = attachment.y - yy;
            const jint smooth = leaf_radius + radius_offset +
                                static_cast<jint>(std::floor(
                                    static_cast<float>(yo) /
                                    static_cast<float>(foliage_height) * 3.5F));
            const jint jagged = (yo > 0 && smooth == previous && (yy & 1) == 0)
                                    ? smooth + 1
                                    : smooth;
            place_leaves_row(
                context, random,
                block_pos{.x = attachment.x, .y = yy, .z = attachment.z},
                jagged, 0, double_trunk, leaves, placed);
            previous = smooth;
        }
        return;
    }
    if (foliage_ == foliage_kind::fancy) {
        for (jint yo = offset; yo >= offset - foliage_height; --yo) {
            const jint current =
                leaf_radius +
                (yo != offset && yo != offset - foliage_height ? 1 : 0);
            place_leaves_row(context, random, attachment, current, yo, false,
                             leaves, placed);
        }
        return;
    }
    if (foliage_ == foliage_kind::spruce) {
        jint current = random.next_int(2);
        jint max_radius = 1;
        jint min_radius = 0;
        for (jint yo = offset; yo >= -foliage_height; --yo) {
            place_leaves_row(context, random, attachment, current, yo, false,
                             leaves, placed);
            if (current >= max_radius) {
                current = min_radius;
                min_radius = 1;
                max_radius =
                    std::min(max_radius + 1, leaf_radius + radius_offset);
            } else {
                ++current;
            }
        }
        return;
    }
    const block_pos position{
        .x = attachment.x, .y = attachment.y + offset, .z = attachment.z};
    place_leaves_row(context, random, position, leaf_radius + radius_offset,
                     -1 - foliage_height_, false, leaves, placed);
    place_leaves_row(context, random, position, leaf_radius - 1,
                     -foliage_height_, false, leaves, placed);
    place_leaves_row(context, random, position, leaf_radius + radius_offset - 1,
                     0, false, leaves, placed);
}

void tree_feature::update_leaves(feature_context &context,
                                 const std::vector<block_pos> &logs,
                                 const std::vector<block_pos> &leaves) const {
    tree_bounds bounds{};
    bool first = true;
    for (const auto &pos : logs) {
        expand(bounds, pos, first);
    }
    for (const auto &pos : leaves) {
        expand(bounds, pos, first);
    }
    if (first) {
        return;
    }
    const auto width =
        static_cast<std::size_t>(bounds.max_x - bounds.min_x + 1);
    const auto height =
        static_cast<std::size_t>(bounds.max_y - bounds.min_y + 1);
    const auto depth =
        static_cast<std::size_t>(bounds.max_z - bounds.min_z + 1);
    std::vector<bool> shape(width * height * depth, false);
    std::array<std::vector<block_pos>, 7> to_check{};
    to_check[0] = logs;
    jint smallest = 0;
    constexpr std::array<std::array<jint, 3>, 6> offsets{{
        {0, -1, 0},
        {0, 1, 0},
        {-1, 0, 0},
        {1, 0, 0},
        {0, 0, -1},
        {0, 0, 1},
    }};
    for (;;) {
        while (smallest < 7 &&
               !to_check[static_cast<std::size_t>(smallest)].empty()) {
            const auto pos =
                to_check[static_cast<std::size_t>(smallest)].back();
            to_check[static_cast<std::size_t>(smallest)].pop_back();
            if (!bounds.contains(pos)) {
                continue;
            }
            if (smallest != 0) {
                const auto current =
                    context.placement.region->state_at(pos.x, pos.y, pos.z);
                const auto current_key =
                    context.placement.states->name(current);
                const auto previous = state_property(current_key, "distance");
                if (previous.has_value() &&
                    std::atoi(std::string{*previous}.c_str()) <= smallest) {
                    // A later bucket can still contain the same position; keep
                    // the shortest distance already written.
                    shape[bounds.index(pos)] = true;
                    continue;
                }
                auto modified = std::string{current_key};
                modified = with_state_property(modified, "distance",
                                               std::to_string(smallest));
                auto found = states_->find(modified);
                if (!found) {
                    const auto interned = states_->intern(
                        modified, context.placement.states->traits(current));
                    if (interned) {
                        found = *interned;
                    }
                }
                if (found) {
                    (void)context.placement.region->set_block_state(
                        pos.x, pos.y, pos.z, *found);
                }
            }
            shape[bounds.index(pos)] = true;
            for (const auto &offset : offsets) {
                const block_pos neighbour{.x = pos.x + offset[0],
                                          .y = pos.y + offset[1],
                                          .z = pos.z + offset[2]};
                if (!bounds.contains(neighbour) ||
                    shape[bounds.index(neighbour)]) {
                    continue;
                }
                const auto state = context.placement.region->state_at(
                    neighbour.x, neighbour.y, neighbour.z);
                const auto key = context.placement.states->name(state);
                const auto distance = state_property(key, "distance");
                if (!distance.has_value()) {
                    continue;
                }
                jint existing = std::atoi(std::string{*distance}.c_str());
                const jint candidate = std::min(existing, smallest + 1);
                if (candidate < 7) {
                    to_check[static_cast<std::size_t>(candidate)].push_back(
                        neighbour);
                    smallest = std::min(smallest, candidate);
                }
            }
        }
        if (smallest >= 7) {
            break;
        }
        ++smallest;
    }
}

auto tree_feature::place(feature_context &context, worldgen_random &random,
                         const block_pos &origin) const -> bool {
    const jint tree_height = base_height_ +
                             random.next_int(height_rand_a_ + 1) +
                             random.next_int(height_rand_b_ + 1);
    jint foliage_height = foliage_height_;
    if (foliage_ == foliage_kind::spruce) {
        if (!spruce_trunk_height_) {
            return false;
        }
        foliage_height =
            std::max(4, tree_height - spruce_trunk_height_->sample(random));
    } else if (foliage_ == foliage_kind::mega_pine) {
        if (!mega_pine_crown_) {
            return false;
        }
        foliage_height = mega_pine_crown_->sample(random);
    }
    const jint leaf_radius = foliage_radius_->sample(random);
    const jint min_y = origin.y;
    const jint max_y = origin.y + tree_height + 1;
    if (min_y < context.placement.chunk->min_y() + 1 ||
        max_y > context.placement.chunk->min_y() +
                    context.placement.chunk->height() + 1) {
        return false;
    }
    jint clipped = tree_height;
    for (jint y = 0; y <= tree_height + 1; ++y) {
        const jint radius = size_at_height(tree_height, y);
        bool blocked = false;
        for (jint x = -radius; x <= radius && !blocked; ++x) {
            for (jint z = -radius; z <= radius; ++z) {
                const block_pos pos{
                    .x = origin.x + x, .y = origin.y + y, .z = origin.z + z};
                if (!is_free(context, pos) ||
                    (!ignore_vines_ && is_vine(context, pos))) {
                    clipped = y - 2;
                    blocked = true;
                    break;
                }
            }
        }
        if (blocked) {
            break;
        }
        if (y == tree_height + 1) {
            clipped = tree_height;
        }
    }
    if (clipped < tree_height &&
        (!has_min_clipped_ || clipped < min_clipped_)) {
        return false;
    }

    std::vector<block_pos> logs;
    if (below_trunk_provider_) {
        const block_pos below{.x = origin.x, .y = origin.y - 1, .z = origin.z};
        const auto state =
            below_trunk_provider_->get(context.placement, random, below);
        (void)context.placement.region->set_block_state(below.x, below.y,
                                                        below.z, state);
    }
    const auto place_log = [&](jint x, jint y, jint z) {
        const block_pos pos{.x = x, .y = y, .z = z};
        if (!valid_tree_pos(context, pos)) {
            return false;
        }
        const auto state = trunk_provider_->get(context.placement, random, pos);
        if (!context.placement.region->set_block_state(pos.x, pos.y, pos.z,
                                                       state)) {
            return false;
        }
        logs.push_back(pos);
        return true;
    };
    const auto place_log_axis = [&](jint x, jint y, jint z, jint start_x,
                                    jint start_z) {
        const block_pos pos{.x = x, .y = y, .z = z};
        if (!valid_tree_pos(context, pos)) {
            return false;
        }
        const auto state = trunk_provider_->get(context.placement, random, pos);
        const auto key = context.placement.states->name(state);
        const jint xdiff = std::abs(x - start_x);
        const jint zdiff = std::abs(z - start_z);
        const jint maxdiff = std::max(xdiff, zdiff);
        std::string_view axis = "y";
        if (maxdiff > 0) {
            axis = xdiff == maxdiff ? "x" : "z";
        }
        auto modified = std::string{key};
        modified = with_state_property(modified, "axis", axis);
        auto found = states_->find(modified);
        if (!found) {
            const auto interned = states_->intern(
                modified, context.placement.states->traits(state));
            if (interned) {
                found = *interned;
            }
        }
        if (!found) {
            return false;
        }
        if (!context.placement.region->set_block_state(x, y, z, *found)) {
            return false;
        }
        logs.push_back(pos);
        return true;
    };
    const auto make_limb = [&](const block_pos &start, const block_pos &end,
                               bool do_place) {
        if (!do_place && start.x == end.x && start.y == end.y &&
            start.z == end.z) {
            return true;
        }
        const jint steps = std::max(
            std::abs(end.x - start.x),
            std::max(std::abs(end.y - start.y), std::abs(end.z - start.z)));
        if (steps == 0) {
            return true;
        }
        const float dx =
            static_cast<float>(end.x - start.x) / static_cast<float>(steps);
        const float dy =
            static_cast<float>(end.y - start.y) / static_cast<float>(steps);
        const float dz =
            static_cast<float>(end.z - start.z) / static_cast<float>(steps);
        for (jint i = 0; i <= steps; ++i) {
            const jint x = start.x + static_cast<jint>(std::floor(
                                         0.5F + static_cast<float>(i) * dx));
            const jint y = start.y + static_cast<jint>(std::floor(
                                         0.5F + static_cast<float>(i) * dy));
            const jint z = start.z + static_cast<jint>(std::floor(
                                         0.5F + static_cast<float>(i) * dz));
            if (do_place) {
                (void)place_log_axis(x, y, z, start.x, start.z);
            } else if (!is_free(context, block_pos{.x = x, .y = y, .z = z})) {
                return false;
            }
        }
        return true;
    };
    const auto horizontal = [&]() {
        switch (random.next_int(4)) {
        case 0:
            return std::pair<jint, jint>{0, -1};
        case 1:
            return std::pair<jint, jint>{1, 0};
        case 2:
            return std::pair<jint, jint>{0, 1};
        default:
            return std::pair<jint, jint>{-1, 0};
        }
    };

    std::vector<tree_attachment> attachments;
    if (kind_ == trunk_kind::straight) {
        for (jint y = 0; y < clipped; ++y) {
            (void)place_log(origin.x, origin.y + y, origin.z);
        }
        attachments.push_back(tree_attachment{
            .pos = block_pos{
                .x = origin.x, .y = origin.y + clipped, .z = origin.z}});
    } else if (kind_ == trunk_kind::forking) {
        const auto [lean_dx, lean_dz] = horizontal();
        const jint lean_height = clipped - random.next_int(4) - 1;
        jint lean_steps = 3 - random.next_int(3);
        jint tx = origin.x;
        jint tz = origin.z;
        bool has_lean = false;
        jint lean_top = 0;
        for (jint yo = 0; yo < clipped; ++yo) {
            const jint yy = origin.y + yo;
            if (yo >= lean_height && lean_steps > 0) {
                tx += lean_dx;
                tz += lean_dz;
                --lean_steps;
            }
            if (place_log(tx, yy, tz)) {
                has_lean = true;
                lean_top = yy + 1;
            }
        }
        if (has_lean) {
            attachments.push_back(tree_attachment{
                .pos = block_pos{.x = tx, .y = lean_top, .z = tz},
                .radius_offset = 1});
        }

        tx = origin.x;
        tz = origin.z;
        const auto [branch_dx, branch_dz] = horizontal();
        if (branch_dx != lean_dx || branch_dz != lean_dz) {
            const jint branch_pos = lean_height - random.next_int(2) - 1;
            jint branch_steps = 1 + random.next_int(3);
            bool has_branch = false;
            jint branch_top = 0;
            for (jint yo = branch_pos; yo < clipped && branch_steps > 0;
                 --branch_steps) {
                if (yo >= 1) {
                    tx += branch_dx;
                    tz += branch_dz;
                    const jint yy = origin.y + yo;
                    if (place_log(tx, yy, tz)) {
                        has_branch = true;
                        branch_top = yy + 1;
                    }
                }
                ++yo;
            }
            if (has_branch) {
                attachments.push_back(tree_attachment{
                    .pos = block_pos{.x = tx, .y = branch_top, .z = tz}});
            }
        }
    } else if (kind_ == trunk_kind::fancy) {
        const jint height = clipped + 2;
        const jint trunk_height =
            static_cast<jint>(std::floor(static_cast<double>(height) * 0.618));
        const jint trunk_top = origin.y + trunk_height;
        const jint relative_start = height - 5;
        std::vector<std::pair<block_pos, jint>> coordinates;
        coordinates.emplace_back(block_pos{.x = origin.x,
                                           .y = origin.y + relative_start,
                                           .z = origin.z},
                                 trunk_top);
        const auto trim = [&](jint local_y) {
            return static_cast<double>(local_y) >=
                   static_cast<double>(height) * 0.2;
        };
        const auto tree_shape = [&](jint y) {
            if (static_cast<float>(y) < static_cast<float>(height) * 0.3F) {
                return -1.0F;
            }
            const float radius = static_cast<float>(height) / 2.0F;
            const float adjacent = radius - static_cast<float>(y);
            float distance = static_cast<float>(std::sqrt(
                static_cast<double>(radius * radius - adjacent * adjacent)));
            if (adjacent == 0.0F) {
                distance = radius;
            } else if (std::abs(adjacent) >= radius) {
                return 0.0F;
            }
            return distance * 0.5F;
        };
        constexpr double pi = 3.14159265358979323846;
        for (jint relative = relative_start; relative >= 0; --relative) {
            const float shape = tree_shape(relative);
            if (shape < 0.0F) {
                continue;
            }
            const double radius =
                static_cast<double>(shape) *
                (static_cast<double>(random.next_float()) + 0.328);
            const double angle =
                static_cast<double>(random.next_float()) * 2.0 * pi;
            const double x = radius * std::sin(angle) + 0.5;
            const double z = radius * std::cos(angle) + 0.5;
            const block_pos check{
                .x = origin.x + static_cast<jint>(std::floor(x)),
                .y = origin.y + relative - 1,
                .z = origin.z + static_cast<jint>(std::floor(z))};
            const block_pos check_end{
                .x = check.x, .y = check.y + 5, .z = check.z};
            if (!make_limb(check, check_end, false)) {
                continue;
            }
            const jint dx = origin.x - check.x;
            const jint dz = origin.z - check.z;
            const double branch_height =
                static_cast<double>(check.y) -
                std::sqrt(static_cast<double>(dx * dx + dz * dz)) * 0.381;
            const jint branch_top =
                branch_height > static_cast<double>(trunk_top)
                    ? trunk_top
                    : static_cast<jint>(branch_height);
            const block_pos branch_base{
                .x = origin.x, .y = branch_top, .z = origin.z};
            if (make_limb(branch_base, check, false)) {
                coordinates.emplace_back(check, branch_top);
            }
        }
        (void)make_limb(origin,
                        block_pos{.x = origin.x,
                                  .y = origin.y + trunk_height,
                                  .z = origin.z},
                        true);
        for (const auto &[attachment, branch_base] : coordinates) {
            const block_pos base{
                .x = origin.x, .y = branch_base, .z = origin.z};
            if (!(base.x == attachment.x && base.y == attachment.y &&
                  base.z == attachment.z) &&
                trim(branch_base - origin.y)) {
                (void)make_limb(base, attachment, true);
            }
        }
        for (const auto &[attachment, branch_base] : coordinates) {
            if (trim(branch_base - origin.y)) {
                attachments.push_back(tree_attachment{.pos = attachment});
            }
        }
    } else if (kind_ == trunk_kind::dark_oak) {
        const auto is_air_or_leaves = [&](const block_pos &pos) {
            const auto state =
                context.placement.region->state_at(pos.x, pos.y, pos.z);
            const auto name = context.placement.states->name(state);
            return is_air_block_name(name) ||
                   context.placement.states->in_tag(state,
                                                    "minecraft:leaves") ||
                   name.ends_with("_leaves");
        };
        const auto place_dark_log = [&](jint x, jint y, jint z) {
            const block_pos pos{.x = x, .y = y, .z = z};
            if (!is_air_or_leaves(pos)) {
                return false;
            }
            const auto state =
                trunk_provider_->get(context.placement, random, pos);
            if (!context.placement.region->set_block_state(pos.x, pos.y, pos.z,
                                                           state)) {
                return false;
            }
            logs.push_back(pos);
            return true;
        };
        const std::array<std::array<jint, 2>, 4> below_offsets{{
            {0, 0},
            {1, 0},
            {0, 1},
            {1, 1},
        }};
        for (const auto &offset : below_offsets) {
            const block_pos below{.x = origin.x + offset[0],
                                  .y = origin.y - 1,
                                  .z = origin.z + offset[1]};
            if (below_trunk_provider_) {
                const auto state = below_trunk_provider_->get(context.placement,
                                                              random, below);
                (void)context.placement.region->set_block_state(
                    below.x, below.y, below.z, state);
            }
        }
        const auto [lean_dx, lean_dz] = horizontal();
        const jint lean_height = clipped - random.next_int(4);
        jint lean_steps = 2 - random.next_int(3);
        const jint ey = origin.y + clipped - 1;
        jint tx = origin.x;
        jint tz = origin.z;
        for (jint dy = 0; dy < clipped; ++dy) {
            if (dy >= lean_height && lean_steps > 0) {
                tx += lean_dx;
                tz += lean_dz;
                --lean_steps;
            }
            const jint yy = origin.y + dy;
            const block_pos pos{.x = tx, .y = yy, .z = tz};
            if (is_air_or_leaves(pos)) {
                (void)place_dark_log(tx, yy, tz);
                (void)place_dark_log(tx + 1, yy, tz);
                (void)place_dark_log(tx, yy, tz + 1);
                (void)place_dark_log(tx + 1, yy, tz + 1);
            }
        }
        attachments.push_back(tree_attachment{
            .pos = block_pos{.x = tx, .y = ey, .z = tz}, .double_trunk = true});
        for (jint ox = -1; ox <= 2; ++ox) {
            for (jint oz = -1; oz <= 2; ++oz) {
                if ((ox < 0 || ox > 1 || oz < 0 || oz > 1) &&
                    random.next_int(3) <= 0) {
                    const jint length = random.next_int(3) + 2;
                    for (jint branch_y = 0; branch_y < length; ++branch_y) {
                        (void)place_dark_log(origin.x + ox, ey - branch_y - 1,
                                             origin.z + oz);
                    }
                    attachments.push_back(tree_attachment{
                        .pos = block_pos{
                            .x = origin.x + ox, .y = ey, .z = origin.z + oz}});
                }
            }
        }
    } else {
        const std::array<std::array<jint, 2>, 4> below_offsets{{
            {0, 0},
            {1, 0},
            {0, 1},
            {1, 1},
        }};
        for (const auto &offset : below_offsets) {
            const block_pos below{.x = origin.x + offset[0],
                                  .y = origin.y - 1,
                                  .z = origin.z + offset[1]};
            if (below_trunk_provider_) {
                const auto state = below_trunk_provider_->get(context.placement,
                                                              random, below);
                (void)context.placement.region->set_block_state(
                    below.x, below.y, below.z, state);
            }
        }
        for (jint hh = 0; hh < clipped; ++hh) {
            const auto place_if_free = [&](jint x, jint y, jint z) {
                const block_pos pos{.x = x, .y = y, .z = z};
                if (!is_free(context, pos)) {
                    return;
                }
                const auto state =
                    trunk_provider_->get(context.placement, random, pos);
                if (context.placement.region->set_block_state(pos.x, pos.y,
                                                              pos.z, state)) {
                    logs.push_back(pos);
                }
            };
            place_if_free(origin.x, origin.y + hh, origin.z);
            if (hh < clipped - 1) {
                place_if_free(origin.x + 1, origin.y + hh, origin.z);
                place_if_free(origin.x + 1, origin.y + hh, origin.z + 1);
                place_if_free(origin.x, origin.y + hh, origin.z + 1);
            }
        }
        attachments.push_back(
            tree_attachment{.pos = block_pos{.x = origin.x,
                                             .y = origin.y + clipped,
                                             .z = origin.z},
                            .double_trunk = true});
    }

    std::vector<block_pos> leaves;
    bool placed = false;
    for (const auto &attachment : attachments) {
        place_foliage(context, random, attachment.pos, leaf_radius,
                      attachment.radius_offset, foliage_height,
                      attachment.double_trunk, leaves, placed);
    }
    if (logs.empty() || leaves.empty()) {
        return false;
    }
    if (!decorators_.empty()) {
        auto sorted_logs = logs;
        auto sorted_leaves = leaves;
        const auto by_y = [](const block_pos &left, const block_pos &right) {
            return left.y < right.y;
        };
        std::stable_sort(sorted_logs.begin(), sorted_logs.end(), by_y);
        std::stable_sort(sorted_leaves.begin(), sorted_leaves.end(), by_y);
        tree_decorator_context decorator_context{.context = context,
                                                 .states = *states_,
                                                 .random = random,
                                                 .logs = sorted_logs,
                                                 .leaves = sorted_leaves};
        for (const auto &decorator : decorators_) {
            decorator->place(decorator_context);
        }
    }
    update_leaves(context, logs, leaves);
    return true;
}

} // namespace refrain::worldgen::feature
