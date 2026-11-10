// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/feature/tree.hpp>

#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/core/mth.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <numbers>
#include <optional>
#include <set>
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

struct tree_direction {
    jint dx;
    jint dy;
    jint dz;
};

/// The six block faces in the order the reference enumerates them.
constexpr std::array<tree_direction, 6> all_directions{{
    {.dx = 0, .dy = -1, .dz = 0},
    {.dx = 0, .dy = 1, .dz = 0},
    {.dx = 0, .dy = 0, .dz = -1},
    {.dx = 0, .dy = 0, .dz = 1},
    {.dx = -1, .dy = 0, .dz = 0},
    {.dx = 1, .dy = 0, .dz = 0},
}};

/// Fisher-Yates over indices, matching the reference's shuffle helper.
auto shuffled(std::size_t count, worldgen_random &random)
    -> std::vector<std::size_t> {
    std::vector<std::size_t> order(count);
    for (std::size_t i = 0; i < count; ++i) {
        order[i] = i;
    }
    for (std::size_t i = count; i > 1; --i) {
        const auto swap_to =
            static_cast<std::size_t>(random.next_int(static_cast<jint>(i)));
        std::swap(order[i - 1], order[swap_to]);
    }
    return order;
}

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

void shelf_mushroom_decorator::place(
    const tree_decorator_context &context) const {
    if (context.logs.empty() || context.random.next_float() >= probability_) {
        return;
    }
    const auto is_shelf = [&](const block_pos &pos) {
        return context.context.placement.states
            ->name(
                context.context.placement.region->state_at(pos.x, pos.y, pos.z))
            .starts_with("minecraft:shelf_mushroom");
    };
    const auto is_replaceable = [&](const block_pos &pos) {
        return context.is_air(pos);
    };
    const auto water_nearby = [&](const block_pos &pos) {
        constexpr std::array<std::array<jint, 3>, 5> checks{{
            {0, 0, 0},
            {1, 0, 0},
            {-1, 0, 0},
            {0, 0, 1},
            {0, 0, -1},
        }};
        for (const auto &offset : checks) {
            const block_pos check{.x = pos.x + offset[0],
                                  .y = pos.y + offset[1],
                                  .z = pos.z + offset[2]};
            if (context.context.placement.states
                    ->name(context.context.placement.region->state_at(
                        check.x, check.y, check.z))
                    .starts_with("minecraft:water")) {
                return true;
            }
        }
        return false;
    };
    const auto horizontal = [](jint index) {
        switch (index) {
        case 0:
            return std::array<jint, 3>{0, 0, -1};
        case 1:
            return std::array<jint, 3>{1, 0, 0};
        case 2:
            return std::array<jint, 3>{0, 0, 1};
        default:
            return std::array<jint, 3>{-1, 0, 0};
        }
    };
    const auto place_mushroom = [&](const block_pos &pos,
                                    const std::array<jint, 3> &direction) {
        const std::array<std::string_view, 4> names{"north", "east", "south",
                                                    "west"};
        jint index = 0;
        for (jint i = 0; i < 4; ++i) {
            if (horizontal(i) == direction) {
                index = i;
            }
        }
        auto key = std::string{"minecraft:shelf_mushroom[age="};
        key.append(std::to_string(context.random.next_int(2)));
        key.append(",facing=");
        key.append(names[static_cast<std::size_t>(index)]);
        key.push_back(']');
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
        context.set_block(pos, *found);
    };
    const bool fallen = context.logs.front().y == context.logs.back().y;
    if (fallen) {
        const bool x_axis = context.logs.front().x != context.logs.back().x;
        const std::array<std::array<jint, 3>, 2> directions =
            x_axis
                ? std::array<std::array<jint, 3>, 2>{{{0, 0, -1}, {0, 0, 1}}}
                : std::array<std::array<jint, 3>, 2>{{{1, 0, 0}, {-1, 0, 0}}};
        for (const auto &log : context.logs) {
            for (const auto &direction : directions) {
                if (context.random.next_float() > 0.25F) {
                    continue;
                }
                const block_pos pos{.x = log.x + direction[0],
                                    .y = log.y,
                                    .z = log.z + direction[2]};
                if (!is_replaceable(pos) || water_nearby(pos)) {
                    continue;
                }
                constexpr std::array<std::array<jint, 3>, 4> around{{
                    {1, 0, 0},
                    {-1, 0, 0},
                    {0, 0, 1},
                    {0, 0, -1},
                }};
                bool adjacent = false;
                for (const auto &offset : around) {
                    if (is_shelf(block_pos{.x = pos.x + offset[0],
                                           .y = pos.y,
                                           .z = pos.z + offset[2]})) {
                        adjacent = true;
                    }
                }
                if (!adjacent) {
                    place_mushroom(pos, direction);
                }
            }
        }
        return;
    }
    const jint tree_base = context.logs.front().y;
    const jint first = context.random.next_int(4);
    const std::array<std::array<jint, 3>, 2> directions{
        horizontal(first), horizontal((first + 1) % 4)};
    for (const auto &log : context.logs) {
        if (log.y - tree_base < 1 || log.y - tree_base > 4) {
            continue;
        }
        for (const auto &direction : directions) {
            if (context.random.next_float() > 0.25F) {
                continue;
            }
            const block_pos pos{.x = log.x + direction[0],
                                .y = log.y,
                                .z = log.z + direction[2]};
            const block_pos below{.x = pos.x, .y = pos.y - 1, .z = pos.z};
            if (is_replaceable(pos) && !water_nearby(pos) && !is_shelf(below)) {
                place_mushroom(pos, direction);
                break;
            }
        }
    }
}

void attached_to_logs_decorator::place(
    const tree_decorator_context &context) const {
    auto shuffled = context.logs;
    for (std::size_t i = shuffled.size(); i > 1; --i) {
        const auto swap_to = static_cast<std::size_t>(
            context.random.next_int(static_cast<jint>(i)));
        std::swap(shuffled[i - 1], shuffled[swap_to]);
    }
    const auto direction_vector = [](std::string_view name) {
        if (name == "down") {
            return std::array<jint, 3>{0, -1, 0};
        }
        if (name == "up") {
            return std::array<jint, 3>{0, 1, 0};
        }
        if (name == "north") {
            return std::array<jint, 3>{0, 0, -1};
        }
        if (name == "south") {
            return std::array<jint, 3>{0, 0, 1};
        }
        if (name == "west") {
            return std::array<jint, 3>{-1, 0, 0};
        }
        return std::array<jint, 3>{1, 0, 0};
    };
    for (const auto &log : shuffled) {
        if (directions_.empty()) {
            continue;
        }
        const auto direction = direction_vector(
            directions_[static_cast<std::size_t>(context.random.next_int(
                static_cast<jint>(directions_.size())))]);
        const block_pos placement{.x = log.x + direction[0],
                                  .y = log.y + direction[1],
                                  .z = log.z + direction[2]};
        if (context.random.next_float() <= probability_ &&
            context.is_air(placement)) {
            context.set_block(placement,
                              provider_->get(context.context.placement,
                                             context.random, placement));
        }
    }
}

void attached_to_leaves_decorator::place(
    const tree_decorator_context &context) const {
    auto shuffled = context.leaves;
    for (std::size_t i = shuffled.size(); i > 1; --i) {
        const auto swap_to = static_cast<std::size_t>(
            context.random.next_int(static_cast<jint>(i)));
        std::swap(shuffled[i - 1], shuffled[swap_to]);
    }
    std::set<std::array<jint, 3>> blacklist;
    const auto direction_vector = [](std::string_view name) {
        if (name == "down") {
            return std::array<jint, 3>{0, -1, 0};
        }
        if (name == "up") {
            return std::array<jint, 3>{0, 1, 0};
        }
        if (name == "north") {
            return std::array<jint, 3>{0, 0, -1};
        }
        if (name == "south") {
            return std::array<jint, 3>{0, 0, 1};
        }
        if (name == "west") {
            return std::array<jint, 3>{-1, 0, 0};
        }
        return std::array<jint, 3>{1, 0, 0};
    };
    for (const auto &leaf : shuffled) {
        if (directions_.empty()) {
            continue;
        }
        const auto direction = direction_vector(
            directions_[static_cast<std::size_t>(context.random.next_int(
                static_cast<jint>(directions_.size())))]);
        const block_pos placement{.x = leaf.x + direction[0],
                                  .y = leaf.y + direction[1],
                                  .z = leaf.z + direction[2]};
        const std::array<jint, 3> key{placement.x, placement.y, placement.z};
        if (blacklist.contains(key) ||
            context.random.next_float() >= probability_) {
            continue;
        }
        bool empty = true;
        for (jint i = 1; i <= required_empty_; ++i) {
            const block_pos pos{.x = leaf.x + direction[0] * i,
                                .y = leaf.y + direction[1] * i,
                                .z = leaf.z + direction[2] * i};
            if (!context.is_air(pos)) {
                empty = false;
                break;
            }
        }
        if (!empty) {
            continue;
        }
        for (jint x = placement.x - exclusion_xz_;
             x <= placement.x + exclusion_xz_; ++x) {
            for (jint y = placement.y - exclusion_y_;
                 y <= placement.y + exclusion_y_; ++y) {
                for (jint z = placement.z - exclusion_xz_;
                     z <= placement.z + exclusion_xz_; ++z) {
                    blacklist.insert({x, y, z});
                }
            }
        }
        context.set_block(placement, provider_->get(context.context.placement,
                                                    context.random, placement));
    }
}

void cocoa_decorator::place(const tree_decorator_context &context) const {
    if (context.logs.empty()) {
        return;
    }
    if (context.random.next_float() >= probability_) {
        return;
    }
    const jint tree_y = context.logs.front().y;
    static constexpr std::array<std::array<jint, 3>, 4> horizontal{{
        {0, 0, -1},
        {1, 0, 0},
        {0, 0, 1},
        {-1, 0, 0},
    }};
    static constexpr std::array<std::string_view, 4> names{"north", "east",
                                                           "south", "west"};
    for (const auto &log : context.logs) {
        if (log.y - tree_y > 2) {
            continue;
        }
        for (std::size_t i = 0; i < horizontal.size(); ++i) {
            if (context.random.next_float() > 0.25F) {
                continue;
            }
            const auto &step = horizontal[i];
            const block_pos pos{.x = log.x - step[0],
                                .y = log.y - step[1],
                                .z = log.z - step[2]};
            if (!context.is_air(pos)) {
                continue;
            }
            auto key = std::string{"minecraft:cocoa[age="};
            key.append(std::to_string(context.random.next_int(3)));
            key.append(",facing=");
            key.append(names[i]);
            key.push_back(']');
            auto found = context.states.find(key);
            if (!found) {
                const auto interned = context.states.intern(
                    key, state_traits{.air = false,
                                      .blocks_motion = true,
                                      .blocks_motion_no_leaves = true});
                if (!interned) {
                    continue;
                }
                found = *interned;
            }
            context.set_block(pos, *found);
        }
    }
}

void place_on_ground_decorator::place(
    const tree_decorator_context &context) const {
    if (context.logs.empty()) {
        return;
    }
    const jint min_y = context.logs.front().y;
    jint min_x = context.logs.front().x;
    jint max_x = min_x;
    jint min_z = context.logs.front().z;
    jint max_z = min_z;
    for (const auto &log : context.logs) {
        if (log.y == min_y) {
            min_x = std::min(min_x, log.x);
            max_x = std::max(max_x, log.x);
            min_z = std::min(min_z, log.z);
            max_z = std::max(max_z, log.z);
        }
    }
    min_x -= radius_;
    max_x += radius_;
    min_z -= radius_;
    max_z += radius_;
    const jint min_scan_y = min_y - height_;
    const jint max_scan_y = min_y + height_;
    for (jint i = 0; i < tries_; ++i) {
        const jint x = context.random.next_int(max_x - min_x + 1) + min_x;
        const jint y =
            context.random.next_int(max_scan_y - min_scan_y + 1) + min_scan_y;
        const jint z = context.random.next_int(max_z - min_z + 1) + min_z;
        const block_pos above{.x = x, .y = y + 1, .z = z};
        const auto above_name = context.context.placement.states->name(
            context.context.placement.region->state_at(above.x, above.y,
                                                       above.z));
        const bool above_ok =
            is_air_block_name(above_name) || above_name == "minecraft:vine";
        const auto floor_state =
            context.context.placement.region->state_at(x, y, z);
        const bool floor_ok =
            context.context.placement.states->traits(floor_state).blocks_motion;
        const jint local_x =
            x - context.context.placement.chunk->chunk_x() * 16;
        const jint local_z =
            z - context.context.placement.chunk->chunk_z() * 16;
        if (local_x < 0 || local_x > 15 || local_z < 0 || local_z > 15) {
            continue;
        }
        const jint heightmap = context.context.placement.chunk->first_available(
            heightmap_kind::motion_blocking_no_leaves, local_x, local_z);
        if (above_ok && floor_ok && heightmap <= above.y) {
            const auto state = provider_->get(context.context.placement,
                                              context.random, above);
            context.set_block(above, state);
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

namespace {

auto resolve_tree_state(const tree_decorator_context &context,
                        std::string_view key) -> block_state_id {
    if (const auto found = context.states.find(key); found.has_value()) {
        return *found;
    }
    const auto traits = context.states.infer_traits(key);
    const auto interned = context.states.intern(key, traits);
    return interned ? *interned : no_block_state;
}

} // namespace

void pale_moss_decorator::add_hanger(const tree_decorator_context &context,
                                     const block_pos &start) const {
    block_pos pos = start;
    auto top =
        resolve_tree_state(context, "minecraft:pale_hanging_moss[tip=true]");
    auto body =
        resolve_tree_state(context, "minecraft:pale_hanging_moss[tip=false]");
    while (context.is_air(block_pos{.x = pos.x, .y = pos.y - 1, .z = pos.z}) &&
           !(context.random.next_float() < 0.5F)) {
        if (body != no_block_state) {
            context.set_block(pos, body);
        }
        --pos.y;
    }
    if (top != no_block_state) {
        context.set_block(pos, top);
    }
}

void pale_moss_decorator::place(const tree_decorator_context &context) const {
    if (context.logs.empty()) {
        return;
    }
    const auto order = shuffled(context.logs.size(), context.random);
    block_pos lowest = context.logs[order.front()];
    for (const auto index : order) {
        if (context.logs[index].y < lowest.y) {
            lowest = context.logs[index];
        }
    }
    if (context.random.next_float() < ground_probability_ &&
        !patch_feature_.empty() && context.context.place_configured) {
        (void)context.context.place_configured(
            patch_feature_, context.random,
            block_pos{.x = lowest.x, .y = lowest.y + 1, .z = lowest.z});
    }
    for (const auto &log : context.logs) {
        if (context.random.next_float() < trunk_probability_) {
            const block_pos below{.x = log.x, .y = log.y - 1, .z = log.z};
            if (context.is_air(below)) {
                add_hanger(context, below);
            }
        }
    }
    for (const auto &leaf : context.leaves) {
        if (context.random.next_float() < leaves_probability_) {
            const block_pos below{.x = leaf.x, .y = leaf.y - 1, .z = leaf.z};
            if (context.is_air(below)) {
                add_hanger(context, below);
            }
        }
    }
}

void creaking_heart_decorator::place(
    const tree_decorator_context &context) const {
    if (context.logs.empty()) {
        return;
    }
    if (!(context.random.next_float() < probability_)) {
        return;
    }
    const auto order = shuffled(context.logs.size(), context.random);
    constexpr std::array<block_pos, 6> neighbours{{
        {.x = 0, .y = -1, .z = 0},
        {.x = 0, .y = 1, .z = 0},
        {.x = 0, .y = 0, .z = -1},
        {.x = 0, .y = 0, .z = 1},
        {.x = -1, .y = 0, .z = 0},
        {.x = 1, .y = 0, .z = 0},
    }};
    for (const auto index : order) {
        const auto &pos = context.logs[index];
        bool surrounded = true;
        for (const auto &offset : neighbours) {
            const auto state = context.context.placement.region->state_at(
                pos.x + offset.x, pos.y + offset.y, pos.z + offset.z);
            if (!context.states.in_tag(state, "minecraft:logs")) {
                surrounded = false;
                break;
            }
        }
        if (!surrounded) {
            continue;
        }
        const auto heart = resolve_tree_state(
            context,
            "minecraft:creaking_heart[axis=y,natural=true,state=dormant]");
        if (heart != no_block_state) {
            context.set_block(pos, heart);
        }
        return;
    }
}

auto fallen_tree_feature::place(feature_context &context,
                                worldgen_random &random,
                                const block_pos &origin) const -> bool {
    const auto valid_tree_pos = [&](const block_pos &pos) {
        const auto state =
            context.placement.region->state_at(pos.x, pos.y, pos.z);
        return is_air_block_name(context.placement.states->name(state)) ||
               context.placement.states->in_tag(
                   state, "minecraft:replaceable_by_trees");
    };
    const auto solid_below = [&](const block_pos &pos) {
        return context.placement.states
            ->traits(
                context.placement.region->state_at(pos.x, pos.y - 1, pos.z))
            .blocks_motion;
    };
    const auto may_place_on = [&](const block_pos &pos) {
        return valid_tree_pos(pos) && solid_below(pos);
    };
    const auto place_log = [&](const block_pos &pos, std::string_view axis) {
        auto state = trunk_provider_->get(context.placement, random, pos);
        if (!axis.empty()) {
            const auto key = context.placement.states->name(state);
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
            if (found) {
                state = *found;
            }
        }
        (void)context.placement.region->set_block_state(pos.x, pos.y, pos.z,
                                                        state);
        return pos;
    };
    const auto decorate =
        [&](const std::vector<block_pos> &logs,
            const std::vector<std::unique_ptr<tree_decorator>> &decorators) {
            if (decorators.empty() || logs.empty()) {
                return;
            }
            tree_decorator_context decorator_context{.context = context,
                                                     .states = *states_,
                                                     .random = random,
                                                     .logs = logs,
                                                     .leaves = {}};
            for (const auto &decorator : decorators) {
                decorator->place(decorator_context);
            }
        };

    const std::array<std::array<jint, 3>, 4> horizontal{{
        {0, 0, -1},
        {1, 0, 0},
        {0, 0, 1},
        {-1, 0, 0},
    }};
    const auto direction =
        horizontal[static_cast<std::size_t>(random.next_int(4))];
    const jint length = log_length_->sample(random) - 2;
    block_pos start{.x = origin.x + direction[0] * (2 + random.next_int(2)),
                    .y = origin.y,
                    .z = origin.z + direction[2] * (2 + random.next_int(2))};
    start.y += 1;
    for (jint i = 0; i < 6; ++i) {
        if (may_place_on(start)) {
            break;
        }
        --start.y;
    }
    const block_pos saved = start;
    bool can_place = true;
    jint gap = 0;
    for (jint i = 0; i < length; ++i) {
        if (!valid_tree_pos(start)) {
            can_place = false;
            break;
        }
        if (!solid_below(start)) {
            if (++gap > 2) {
                can_place = false;
                break;
            }
        } else {
            gap = 0;
        }
        start.x += direction[0];
        start.z += direction[2];
    }
    start = saved;
    if (!can_place) {
        return true;
    }
    std::vector<block_pos> logs;
    for (jint i = 0; i < length; ++i) {
        logs.push_back(place_log(start, direction[0] != 0 ? "x" : "z"));
        start.x += direction[0];
        start.z += direction[2];
    }
    const std::vector<block_pos> stump{place_log(origin, {})};
    decorate(stump, stump_decorators_);
    decorate(logs, log_decorators_);
    return true;
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
            } else if (foliage_ == foliage_kind::bush) {
                if (min_dx == radius && min_dz == radius &&
                    random.next_int(2) == 0) {
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

auto tree_feature::place_leaf(feature_context &context, worldgen_random &random,
                              const block_pos &pos,
                              std::vector<block_pos> &leaves,
                              bool &placed) const -> bool {
    const auto current =
        context.placement.region->state_at(pos.x, pos.y, pos.z);
    const auto current_key = context.placement.states->name(current);
    const auto persistent = state_property(current_key, "persistent");
    if (persistent.has_value() && *persistent == "true") {
        return false;
    }
    if (!valid_tree_pos(context, pos)) {
        return false;
    }
    auto state = foliage_provider_->get(context.placement, random, pos);
    const auto key = context.placement.states->name(state);
    if (state_property(key, "waterlogged").has_value()) {
        auto modified = std::string{key};
        const bool water =
            state_block_name(context.placement.states->name(
                context.placement.region->state_at(pos.x, pos.y, pos.z))) ==
            "minecraft:water";
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
    if (!context.placement.region->set_block_state(pos.x, pos.y, pos.z,
                                                   state)) {
        return false;
    }
    leaves.push_back(pos);
    placed = true;
    return true;
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
    if (foliage_ == foliage_kind::pine) {
        jint current = 0;
        for (jint yo = offset; yo >= offset - foliage_height; --yo) {
            place_leaves_row(context, random, attachment, current, yo,
                             double_trunk, leaves, placed);
            if (current >= 1 && yo == offset - foliage_height + 1) {
                --current;
            } else if (current < leaf_radius + radius_offset) {
                ++current;
            }
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
    if (foliage_ == foliage_kind::jungle) {
        const jint height =
            double_trunk ? foliage_height : 1 + random.next_int(2);
        for (jint yo = offset; yo >= offset - height; --yo) {
            const jint current = leaf_radius + radius_offset + 1 - yo;
            place_leaves_row(context, random, attachment, current, yo,
                             double_trunk, leaves, placed);
        }
        return;
    }
    if (foliage_ == foliage_kind::bush) {
        for (jint yo = offset; yo >= offset - foliage_height; --yo) {
            const jint current = leaf_radius + radius_offset - 1 - yo;
            place_leaves_row(context, random, attachment, current, yo,
                             double_trunk, leaves, placed);
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
    if (foliage_ == foliage_kind::random_spread) {
        const block_pos position{
            .x = attachment.x, .y = attachment.y + offset, .z = attachment.z};
        for (jint i = 0; i < options_.random_spread_leaf_attempts; ++i) {
            const block_pos pos{
                .x = position.x + random.next_int(leaf_radius) -
                     random.next_int(leaf_radius),
                .y = position.y + random.next_int(foliage_height) -
                     random.next_int(foliage_height),
                .z = position.z + random.next_int(leaf_radius) -
                     random.next_int(leaf_radius),
            };
            (void)place_leaf(context, random, pos, leaves, placed);
        }
        return;
    }
    if (foliage_ == foliage_kind::cherry) {
        const block_pos foliage_pos{
            .x = attachment.x, .y = attachment.y + offset, .z = attachment.z};
        const jint current_radius = leaf_radius + radius_offset - 1;
        const jint height_with_offset = foliage_height;
        std::set<block_pos> placed_leaves(leaves.begin(), leaves.end());
        const auto place_one = [&](const block_pos &pos) {
            if (place_leaf(context, random, pos, leaves, placed)) {
                placed_leaves.insert(pos);
            }
        };
        const auto skip = [&](jint dx, jint y, jint dz, jint radius) -> bool {
            if (y == -1 && (dx == radius || dz == radius) &&
                random.next_float() < options_.cherry_wide_bottom_hole_chance) {
                return true;
            }
            const bool corner = dx == radius && dz == radius;
            if (radius > 2) {
                return corner || (dx + dz > radius * 2 - 2 &&
                                  random.next_float() <
                                      options_.cherry_corner_hole_chance);
            }
            return corner &&
                   random.next_float() < options_.cherry_corner_hole_chance;
        };
        const auto row = [&](jint radius, jint y) {
            const jint extent = radius + (double_trunk ? 1 : 0);
            for (jint dx = -radius; dx <= extent; ++dx) {
                for (jint dz = -radius; dz <= extent; ++dz) {
                    const jint min_dx =
                        double_trunk ? std::min(std::abs(dx), std::abs(dx - 1))
                                     : std::abs(dx);
                    const jint min_dz =
                        double_trunk ? std::min(std::abs(dz), std::abs(dz - 1))
                                     : std::abs(dz);
                    if (skip(min_dx, y, min_dz, radius)) {
                        continue;
                    }
                    place_one(block_pos{.x = foliage_pos.x + dx,
                                        .y = foliage_pos.y + y,
                                        .z = foliage_pos.z + dz});
                }
            }
        };
        const auto extension = [&](const block_pos &pos, float chance) {
            if (std::abs(pos.x - foliage_pos.x) +
                    std::abs(pos.y - (foliage_pos.y - 1)) +
                    std::abs(pos.z - foliage_pos.z) >=
                7) {
                return false;
            }
            if (random.next_float() > chance) {
                return false;
            }
            place_one(pos);
            return true;
        };
        const auto hanging_row = [&](jint radius, jint y) {
            row(radius, y);
            const jint extra = double_trunk ? 1 : 0;
            constexpr std::array<std::pair<jint, jint>, 4> horizontal{{
                {0, -1},
                {1, 0},
                {0, 1},
                {-1, 0},
            }};
            for (std::size_t along = 0; along < horizontal.size(); ++along) {
                const std::size_t to_edge = (along + 1) % horizontal.size();
                const auto &[edge_dx, edge_dz] = horizontal[to_edge];
                const auto &[along_dx, along_dz] = horizontal[along];
                const bool positive = edge_dx > 0 || edge_dz > 0;
                const jint offset_to_edge = positive ? radius + extra : radius;
                block_pos pos{.x = foliage_pos.x,
                              .y = foliage_pos.y + y - 1,
                              .z = foliage_pos.z};
                pos.x += edge_dx * offset_to_edge;
                pos.z += edge_dz * offset_to_edge;
                pos.x -= along_dx * radius;
                pos.z -= along_dz * radius;
                jint offset_along = -radius;
                while (offset_along < radius + extra) {
                    const block_pos above{
                        .x = pos.x, .y = pos.y + 1, .z = pos.z};
                    if (placed_leaves.contains(above) &&
                        extension(pos, options_.cherry_hanging_leaves_chance)) {
                        --pos.y;
                        (void)extension(
                            pos, options_.cherry_hanging_extension_chance);
                        ++pos.y;
                    }
                    ++offset_along;
                    pos.x += along_dx;
                    pos.z += along_dz;
                }
            }
        };
        row(current_radius - 2, height_with_offset - 3);
        row(current_radius - 1, height_with_offset - 4);
        for (jint y = height_with_offset - 5; y >= 0; --y) {
            row(current_radius, y);
        }
        hanging_row(current_radius, -1);
        hanging_row(current_radius - 1, -2);
        return;
    }
    if (foliage_ == foliage_kind::poplar) {
        const block_pos foliage_pos{
            .x = attachment.x, .y = attachment.y + offset, .z = attachment.z};
        const jint current_radius = leaf_radius + radius_offset - 1;
        const bool flip = random.next_boolean();
        const jint height_with_offset = foliage_height;
        const auto partial_row = [&](jint y) {
            return height_with_offset - 1 == y || height_with_offset - 2 == y;
        };
        const auto corner_cut = [&](jint dx, jint dz, jint radius,
                                    bool partial) {
            const bool small = flip
                                   ? ((dx > 0 && dz > 0) || (dz < 0 && dx < 0))
                                   : ((dx > 0 && dz < 0) || (dz > 0 && dx < 0));
            return small ? radius - 1 : (partial ? radius + 1 : radius);
        };
        const auto within = [](jint radius, jint abs_dx, jint abs_dz, jint cut,
                               jint extra) {
            return abs_dx + abs_dz <= radius * 2 - (cut + extra);
        };
        const auto row = [&](jint radius, jint y) {
            const jint extent = radius + (double_trunk ? 1 : 0);
            for (jint dx = -radius; dx <= extent; ++dx) {
                for (jint dz = -radius; dz <= extent; ++dz) {
                    const jint abs_dx = std::abs(dx);
                    const jint abs_dz = std::abs(dz);
                    const bool partial = partial_row(y);
                    const jint cut = corner_cut(dx, dz, radius, partial);
                    const bool edge = abs_dx == radius || abs_dz == radius;
                    if (partial && edge) {
                        continue;
                    }
                    const jint additional =
                        random.next_float() <= options_.poplar_side_hole_chance
                            ? 1
                            : 0;
                    if (!within(radius, abs_dx, abs_dz, cut, additional)) {
                        continue;
                    }
                    (void)place_leaf(context, random,
                                     block_pos{.x = foliage_pos.x + dx,
                                               .y = foliage_pos.y + y,
                                               .z = foliage_pos.z + dz},
                                     leaves, placed);
                }
            }
        };
        const auto replace_with_log = [&](jint radius, jint y) {
            const jint extent = radius + (double_trunk ? 1 : 0);
            const bool partial = partial_row(y);
            for (jint dx = -radius; dx <= extent; ++dx) {
                for (jint dz = -radius; dz <= extent; ++dz) {
                    const jint abs_dx = std::abs(dx);
                    const jint abs_dz = std::abs(dz);
                    const jint cut = corner_cut(dx, dz, radius, partial);
                    const bool arm = (abs_dz == 0 && radius - abs_dx >= 4) ||
                                     (abs_dx == 0 && radius - abs_dz >= 4);
                    if (!within(radius, abs_dx, abs_dz, cut, 2) || !arm) {
                        continue;
                    }
                    const block_pos pos{.x = foliage_pos.x + dx,
                                        .y = foliage_pos.y + y,
                                        .z = foliage_pos.z + dz};
                    const auto current =
                        context.placement.region->state_at(pos.x, pos.y, pos.z);
                    const auto leaf =
                        foliage_provider_->get(context.placement, random, pos);
                    if (context.placement.states->name(current) !=
                        context.placement.states->name(leaf)) {
                        continue;
                    }
                    const auto state =
                        trunk_provider_->get(context.placement, random, pos);
                    auto modified =
                        std::string{context.placement.states->name(state)};
                    modified = with_state_property(modified, "axis",
                                                   abs_dz == 0 ? "x" : "z");
                    auto found = states_->find(modified);
                    if (!found) {
                        const auto interned = states_->intern(
                            modified, context.placement.states->traits(state));
                        if (interned) {
                            found = *interned;
                        }
                    }
                    if (found) {
                        if (context.placement.region->set_block_state(
                                pos.x, pos.y, pos.z, *found)) {
                            leaves.push_back(pos);
                            placed = true;
                        }
                    }
                }
            }
        };
        row(current_radius - 2, height_with_offset - 1);
        row(current_radius - 1, height_with_offset - 2);
        row(current_radius - 1, height_with_offset - 3);
        for (jint y = height_with_offset - 4; y >= 1; --y) {
            row(current_radius, y);
        }
        replace_with_log(current_radius, height_with_offset - 4);
        row(current_radius - 1, 0);
        row(std::clamp(current_radius - 2, 1, 2), -1);
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

auto tree_feature::place_roots(feature_context &context,
                               worldgen_random &random, const block_pos &origin,
                               const block_pos &trunk_origin) const -> bool {
    auto &region = *context.placement.region;
    const auto &states = *context.placement.states;
    const auto state_at = [&](const block_pos &pos) {
        return region.state_at(pos.x, pos.y, pos.z);
    };
    const auto valid_tree_pos = [&](const block_pos &pos) {
        const auto state = state_at(pos);
        return is_air_block_name(states.name(state)) ||
               states.in_tag(state, "minecraft:replaceable_by_trees");
    };
    const auto can_grow_through = [&](block_state_id state) {
        return !options_.mangrove_can_grow_through.empty() &&
               states.in_tag(state, options_.mangrove_can_grow_through);
    };
    const auto can_place_root = [&](const block_pos &pos) {
        return valid_tree_pos(pos) || can_grow_through(state_at(pos));
    };
    const auto is_muddy = [&](block_state_id state) {
        const auto name = states.name(state);
        return std::any_of(
            options_.mangrove_muddy_roots_in.begin(),
            options_.mangrove_muddy_roots_in.end(),
            [&](const std::string &entry) { return name == entry; });
    };
    const auto waterlogged = [&](const block_pos &pos,
                                 block_state_id state) -> block_state_id {
        const auto key = states.name(state);
        if (key.find("waterlogged=") == std::string_view::npos) {
            return state;
        }
        auto modified = std::string{key};
        modified = with_state_property(
            modified, "waterlogged",
            states.traits(state_at(pos)).water ? "true" : "false");
        if (const auto found = states_->find(modified); found.has_value()) {
            return *found;
        }
        const auto interned = states_->intern(modified, states.traits(state));
        return interned ? *interned : state;
    };
    const auto write = [&](const block_pos &pos, block_state_id state) {
        (void)region.set_block_state(pos.x, pos.y, pos.z,
                                     waterlogged(pos, state));
    };
    const auto place_root = [&](const block_pos &pos) {
        if (!can_place_root(pos)) {
            return;
        }
        if (is_muddy(state_at(pos)) && options_.muddy_roots_provider) {
            write(pos, options_.muddy_roots_provider->get(context.placement,
                                                          random, pos));
        } else {
            write(pos,
                  options_.root_provider->get(context.placement, random, pos));
        }
        if (!options_.above_root_provider) {
            return;
        }
        if (random.next_float() >= options_.above_root_chance) {
            return;
        }
        const block_pos above{.x = pos.x, .y = pos.y + 1, .z = pos.z};
        if (!states.traits(state_at(above)).air) {
            return;
        }
        write(above, options_.above_root_provider->get(context.placement,
                                                       random, above));
    };

    std::vector<block_pos> root_positions;
    block_pos column = origin;
    while (column.y < trunk_origin.y) {
        if (!can_place_root(column)) {
            return false;
        }
        ++column.y;
    }
    root_positions.push_back(block_pos{
        .x = trunk_origin.x, .y = trunk_origin.y - 1, .z = trunk_origin.z});
    constexpr std::array<std::pair<jint, jint>, 4> horizontal{{
        {0, -1},
        {1, 0},
        {0, 1},
        {-1, 0},
    }};
    const auto potential_roots = [&](const block_pos &pos, jint direction,
                                     const block_pos &root_origin) {
        std::vector<block_pos> out;
        const block_pos below{.x = pos.x, .y = pos.y - 1, .z = pos.z};
        const auto &step = horizontal[static_cast<std::size_t>(direction)];
        const block_pos next_to{
            .x = pos.x + step.first, .y = pos.y, .z = pos.z + step.second};
        const jint width = std::abs(pos.x - root_origin.x) +
                           std::abs(pos.y - root_origin.y) +
                           std::abs(pos.z - root_origin.z);
        const jint max_width = options_.mangrove_max_root_width;
        if (width > max_width - 3 && width <= max_width) {
            if (random.next_float() < options_.mangrove_random_skew_chance) {
                out.push_back(below);
                out.push_back(block_pos{
                    .x = next_to.x, .y = next_to.y - 1, .z = next_to.z});
            } else {
                out.push_back(below);
            }
        } else if (width > max_width) {
            out.push_back(below);
        } else if (random.next_float() < options_.mangrove_random_skew_chance) {
            out.push_back(below);
        } else if (random.next_boolean()) {
            out.push_back(next_to);
        } else {
            out.push_back(below);
        }
        return out;
    };
    std::function<bool(const block_pos &, jint, jint, std::vector<block_pos> &)>
        simulate;
    simulate = [&](const block_pos &pos, jint direction, jint layer,
                   std::vector<block_pos> &positions) -> bool {
        if (layer == options_.mangrove_max_root_length ||
            positions.size() >
                static_cast<std::size_t>(options_.mangrove_max_root_length)) {
            return false;
        }
        for (const auto &candidate :
             potential_roots(pos, direction, trunk_origin)) {
            if (!can_place_root(candidate)) {
                continue;
            }
            positions.push_back(candidate);
            if (!simulate(candidate, direction, layer + 1, positions)) {
                return false;
            }
        }
        return true;
    };
    for (std::size_t direction = 0; direction < horizontal.size();
         ++direction) {
        const auto &[dx, dz] = horizontal[direction];
        const block_pos pos{.x = trunk_origin.x + dx,
                            .y = trunk_origin.y,
                            .z = trunk_origin.z + dz};
        std::vector<block_pos> positions;
        if (!simulate(pos, static_cast<jint>(direction), 0, positions)) {
            return false;
        }
        root_positions.insert(root_positions.end(), positions.begin(),
                              positions.end());
        root_positions.push_back(pos);
    }
    for (const auto &root : root_positions) {
        place_root(root);
    }
    return true;
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
    } else if (foliage_ == foliage_kind::mega_pine ||
               foliage_ == foliage_kind::pine) {
        if (!pine_foliage_height_) {
            return false;
        }
        foliage_height = pine_foliage_height_->sample(random);
    } else if (foliage_ == foliage_kind::cherry ||
               foliage_ == foliage_kind::poplar ||
               foliage_ == foliage_kind::random_spread) {
        if (!options_.foliage_height) {
            return false;
        }
        foliage_height = options_.foliage_height->sample(random);
    }
    jint leaf_radius = foliage_radius_->sample(random);
    if (foliage_ == foliage_kind::pine) {
        const jint trunk_height = tree_height - foliage_height;
        leaf_radius += random.next_int(std::max(trunk_height + 1, 1));
    }
    block_pos trunk_origin = origin;
    if (options_.has_root_placer()) {
        trunk_origin.y += options_.root_trunk_offset_y->sample(random);
    }
    const jint min_y = std::min(origin.y, trunk_origin.y);
    const jint max_y = std::max(origin.y, trunk_origin.y) + tree_height + 1;
    if (min_y < context.placement.chunk->min_y() + 1 ||
        max_y > context.placement.chunk->min_y() +
                    context.placement.chunk->height() + 1) {
        return false;
    }
    const auto trunk_valid_pos = [&](const block_pos &pos) {
        if (valid_tree_pos(context, pos)) {
            return true;
        }
        return kind_ == trunk_kind::upwards_branching &&
               !options_.upwards_can_grow_through.empty() &&
               context.placement.states->in_tag(
                   context.placement.region->state_at(pos.x, pos.y, pos.z),
                   options_.upwards_can_grow_through);
    };
    const auto trunk_free = [&](const block_pos &pos) {
        return is_free(context, pos) || trunk_valid_pos(pos);
    };
    jint clipped = tree_height;
    for (jint y = 0; y <= tree_height + 1; ++y) {
        const jint radius = size_at_height(tree_height, y);
        bool blocked = false;
        for (jint x = -radius; x <= radius && !blocked; ++x) {
            for (jint z = -radius; z <= radius; ++z) {
                const block_pos pos{.x = trunk_origin.x + x,
                                    .y = trunk_origin.y + y,
                                    .z = trunk_origin.z + z};
                if (!trunk_free(pos) ||
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

    if (options_.has_root_placer() &&
        !place_roots(context, random, origin, trunk_origin)) {
        return false;
    }

    std::vector<block_pos> logs;
    if (below_trunk_provider_ && kind_ != trunk_kind::upwards_branching) {
        const block_pos below{
            .x = trunk_origin.x, .y = trunk_origin.y - 1, .z = trunk_origin.z};
        const auto state =
            below_trunk_provider_->get(context.placement, random, below);
        (void)context.placement.region->set_block_state(below.x, below.y,
                                                        below.z, state);
    }
    const auto place_log = [&](jint x, jint y, jint z) {
        const block_pos pos{.x = x, .y = y, .z = z};
        if (!trunk_valid_pos(pos)) {
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
        if (!trunk_valid_pos(pos)) {
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
    const auto place_log_with_axis = [&](const block_pos &pos,
                                         std::string_view axis) {
        if (!trunk_valid_pos(pos)) {
            return false;
        }
        const auto state = trunk_provider_->get(context.placement, random, pos);
        auto modified = std::string{context.placement.states->name(state)};
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
        if (!context.placement.region->set_block_state(pos.x, pos.y, pos.z,
                                                       *found)) {
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
    } else if (kind_ == trunk_kind::giant || kind_ == trunk_kind::mega_jungle) {
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
        if (kind_ == trunk_kind::mega_jungle) {
            // Branch height and direction are drawn in the loop order the
            // reference uses: one segment height, then one angle per limb.
            for (jint branch_height = clipped - 2 - random.next_int(4);
                 branch_height > clipped / 2;
                 branch_height -= 2 + random.next_int(4)) {
                const float angle = random.next_float() *
                                    static_cast<float>(std::numbers::pi * 2.0);
                jint branch_x = 0;
                jint branch_z = 0;
                for (jint step = 0; step < 5; ++step) {
                    branch_x = static_cast<jint>(
                        1.5F + mth::cos(angle) * static_cast<float>(step));
                    branch_z = static_cast<jint>(
                        1.5F + mth::sin(angle) * static_cast<float>(step));
                    const block_pos pos{.x = origin.x + branch_x,
                                        .y = origin.y + branch_height - 3 +
                                             step / 2,
                                        .z = origin.z + branch_z};
                    if (!is_free(context, pos)) {
                        continue;
                    }
                    const auto state =
                        trunk_provider_->get(context.placement, random, pos);
                    if (context.placement.region->set_block_state(
                            pos.x, pos.y, pos.z, state)) {
                        logs.push_back(pos);
                    }
                }
                attachments.push_back(tree_attachment{
                    .pos = block_pos{.x = origin.x + branch_x,
                                     .y = origin.y + branch_height,
                                     .z = origin.z + branch_z},
                    .radius_offset = -2});
            }
        }
    } else if (kind_ == trunk_kind::cherry) {
        if (!options_.cherry_branch_count ||
            !options_.cherry_branch_horizontal_length ||
            !options_.cherry_branch_end_offset) {
            return false;
        }
        constexpr std::array<std::pair<jint, jint>, 4> hdirs{{
            {0, -1},
            {1, 0},
            {0, 1},
            {-1, 0},
        }};
        const auto sample_uniform = [&](jint minimum, jint maximum) {
            return minimum + random.next_int(maximum - minimum + 1);
        };
        const jint start_min = options_.cherry_branch_start_min;
        const jint start_max = options_.cherry_branch_start_max;
        const jint first_offset =
            std::max(0, clipped - 1 + sample_uniform(start_min, start_max));
        const jint second_offset =
            std::max(0, clipped - 1 + sample_uniform(start_min, start_max - 1));
        jint resolved_second = second_offset;
        if (resolved_second >= first_offset) {
            ++resolved_second;
        }
        const jint branches = options_.cherry_branch_count->sample(random);
        const bool middle = branches == 3;
        const bool both = branches >= 2;
        jint trunk_height = first_offset + 1;
        if (middle) {
            trunk_height = clipped;
        } else if (both) {
            trunk_height = std::max(first_offset, resolved_second) + 1;
        }
        for (jint y = 0; y < trunk_height; ++y) {
            (void)place_log(trunk_origin.x, trunk_origin.y + y, trunk_origin.z);
        }
        if (middle) {
            attachments.push_back(tree_attachment{
                .pos = block_pos{.x = trunk_origin.x,
                                 .y = trunk_origin.y + trunk_height,
                                 .z = trunk_origin.z}});
        }
        const auto generate_branch = [&](std::size_t direction, jint offset,
                                         bool middle_continues) {
            block_pos log_pos{.x = trunk_origin.x,
                              .y = trunk_origin.y + offset,
                              .z = trunk_origin.z};
            const jint end_offset =
                clipped - 1 + options_.cherry_branch_end_offset->sample(random);
            const bool extend = middle_continues || end_offset < offset;
            const jint distance =
                options_.cherry_branch_horizontal_length->sample(random) +
                (extend ? 1 : 0);
            const auto &[dx, dz] = hdirs[direction];
            const block_pos end{.x = trunk_origin.x + dx * distance,
                                .y = trunk_origin.y + end_offset,
                                .z = trunk_origin.z + dz * distance};
            const jint horizontal_steps = extend ? 2 : 1;
            const std::string_view side_axis = dx != 0 ? "x" : "z";
            for (jint step = 0; step < horizontal_steps; ++step) {
                log_pos.x += dx;
                log_pos.z += dz;
                (void)place_log_with_axis(log_pos, side_axis);
            }
            const jint vertical = end.y > log_pos.y ? 1 : -1;
            for (;;) {
                const jint remaining = std::abs(end.x - log_pos.x) +
                                       std::abs(end.y - log_pos.y) +
                                       std::abs(end.z - log_pos.z);
                if (remaining == 0) {
                    return tree_attachment{
                        .pos =
                            block_pos{.x = end.x, .y = end.y + 1, .z = end.z}};
                }
                const float chance =
                    static_cast<float>(std::abs(end.y - log_pos.y)) /
                    static_cast<float>(remaining);
                const bool vertical_step = random.next_float() < chance;
                if (vertical_step) {
                    log_pos.y += vertical;
                } else {
                    log_pos.x += dx;
                    log_pos.z += dz;
                }
                (void)place_log_with_axis(
                    log_pos, vertical_step ? std::string_view{"y"} : side_axis);
            }
        };
        const auto direction = static_cast<std::size_t>(random.next_int(4));
        attachments.push_back(generate_branch(direction, first_offset,
                                              first_offset < trunk_height - 1));
        if (both) {
            attachments.push_back(
                generate_branch((direction + 2) % 4, resolved_second,
                                resolved_second < trunk_height - 1));
        }
    } else if (kind_ == trunk_kind::poplar) {
        if (!options_.poplar_trunk_above_branches ||
            !options_.poplar_branch_amount) {
            return false;
        }
        const jint trunk_up_to =
            clipped - options_.poplar_trunk_above_branches->sample(random);
        for (jint y = 0; y < clipped; ++y) {
            (void)place_log(trunk_origin.x, trunk_origin.y + y, trunk_origin.z);
            const auto order = shuffled(all_directions.size(), random);
            if (trunk_up_to - 1 == y) {
                const jint branches =
                    options_.poplar_branch_amount->sample(random);
                std::vector<std::size_t> directions;
                for (const auto index : order) {
                    if (all_directions[index].dy == 0) {
                        directions.push_back(index);
                    }
                }
                for (jint branch = 0;
                     branch < branches &&
                     branch < static_cast<jint>(directions.size());
                     ++branch) {
                    const auto &offset =
                        all_directions[directions[static_cast<std::size_t>(
                            branch)]];
                    (void)place_log_with_axis(
                        block_pos{.x = trunk_origin.x + offset.dx,
                                  .y = trunk_origin.y + y,
                                  .z = trunk_origin.z + offset.dz},
                        offset.dx != 0 ? std::string_view{"x"}
                                       : std::string_view{"z"});
                }
            }
        }
        attachments.push_back(
            tree_attachment{.pos = block_pos{.x = trunk_origin.x,
                                             .y = trunk_origin.y + trunk_up_to,
                                             .z = trunk_origin.z}});
    } else if (kind_ == trunk_kind::bending) {
        if (!options_.bending_bend_length) {
            return false;
        }
        constexpr std::array<std::pair<jint, jint>, 4> hdirs{{
            {0, -1},
            {1, 0},
            {0, 1},
            {-1, 0},
        }};
        const auto &[dx, dz] =
            hdirs[static_cast<std::size_t>(random.next_int(4))];
        const jint log_height = clipped - 1;
        block_pos pos = trunk_origin;
        for (jint i = 0; i <= log_height; ++i) {
            if (i + 1 >= log_height + random.next_int(2)) {
                pos.x += dx;
                pos.z += dz;
            }
            if (valid_tree_pos(context, pos)) {
                (void)place_log(pos.x, pos.y, pos.z);
            }
            if (i >= options_.bending_min_height_for_leaves) {
                attachments.push_back(tree_attachment{.pos = pos});
            }
            ++pos.y;
        }
        const jint bend = options_.bending_bend_length->sample(random);
        for (jint i = 0; i <= bend; ++i) {
            if (valid_tree_pos(context, pos)) {
                (void)place_log(pos.x, pos.y, pos.z);
            }
            attachments.push_back(tree_attachment{.pos = pos});
            pos.x += dx;
            pos.z += dz;
        }
    } else if (kind_ == trunk_kind::upwards_branching) {
        if (!options_.upwards_extra_branch_steps ||
            !options_.upwards_extra_branch_length) {
            return false;
        }
        constexpr std::array<std::pair<jint, jint>, 4> hdirs{{
            {0, -1},
            {1, 0},
            {0, 1},
            {-1, 0},
        }};
        for (jint height_pos = 0; height_pos < clipped; ++height_pos) {
            const jint current_height = trunk_origin.y + height_pos;
            if (place_log(trunk_origin.x, current_height, trunk_origin.z) &&
                height_pos < clipped - 1 &&
                random.next_float() < options_.upwards_branch_probability) {
                const auto &[dx, dz] =
                    hdirs[static_cast<std::size_t>(random.next_int(4))];
                const jint branch_length =
                    options_.upwards_extra_branch_length->sample(random);
                const jint branch_pos = std::max(
                    0,
                    branch_length -
                        options_.upwards_extra_branch_length->sample(random) -
                        1);
                jint branch_steps =
                    options_.upwards_extra_branch_steps->sample(random);
                jint height_along = current_height + branch_pos;
                jint log_x = trunk_origin.x;
                jint log_z = trunk_origin.z;
                jint index = branch_pos;
                while (index < clipped && branch_steps > 0) {
                    if (index >= 1) {
                        const jint placement_height = current_height + index;
                        log_x += dx;
                        log_z += dz;
                        height_along = placement_height;
                        if (place_log(log_x, placement_height, log_z)) {
                            height_along = placement_height + 1;
                        }
                        attachments.push_back(tree_attachment{
                            .pos = block_pos{.x = log_x,
                                             .y = placement_height,
                                             .z = log_z}});
                    }
                    ++index;
                    --branch_steps;
                }
                if (height_along - current_height > 1) {
                    const block_pos foliage_pos{
                        .x = log_x, .y = height_along, .z = log_z};
                    attachments.push_back(tree_attachment{.pos = foliage_pos});
                    attachments.push_back(
                        tree_attachment{.pos = block_pos{.x = foliage_pos.x,
                                                         .y = foliage_pos.y - 2,
                                                         .z = foliage_pos.z}});
                }
            }
            if (height_pos == clipped - 1) {
                attachments.push_back(
                    tree_attachment{.pos = block_pos{.x = trunk_origin.x,
                                                     .y = current_height + 1,
                                                     .z = trunk_origin.z}});
            }
        }
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
