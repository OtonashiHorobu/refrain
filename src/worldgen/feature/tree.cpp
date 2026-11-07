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
                                    jint y, std::vector<block_pos> &leaves,
                                    bool &placed) const {
    for (jint dx = -radius; dx <= radius; ++dx) {
        const jint abs_dx = std::abs(dx);
        for (jint dz = -radius; dz <= radius; ++dz) {
            const jint abs_dz = std::abs(dz);
            if (foliage_ == foliage_kind::blob) {
                if (abs_dx == radius && abs_dz == radius &&
                    (random.next_int(2) == 0 || y == 0)) {
                    continue;
                }
            } else if (foliage_ == foliage_kind::acacia) {
                if (y == 0
                        ? (abs_dx > 1 || abs_dz > 1) && dx != 0 && dz != 0
                        : abs_dx == radius && abs_dz == radius && radius > 0) {
                    continue;
                }
            } else if (abs_dx == radius && abs_dz == radius && radius > 0) {
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
                                 std::vector<block_pos> &leaves,
                                 bool &placed) const {
    const jint offset = foliage_offset_->sample(random);
    if (foliage_ == foliage_kind::blob) {
        for (jint yo = offset; yo >= offset - foliage_height; --yo) {
            const jint current =
                std::max(leaf_radius + radius_offset - 1 - yo / 2, 0);
            place_leaves_row(context, random, attachment, current, yo, leaves,
                             placed);
        }
        return;
    }
    if (foliage_ == foliage_kind::spruce) {
        jint current = random.next_int(2);
        jint max_radius = 1;
        jint min_radius = 0;
        for (jint yo = offset; yo >= -foliage_height; --yo) {
            place_leaves_row(context, random, attachment, current, yo, leaves,
                             placed);
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
                     -1 - foliage_height_, leaves, placed);
    place_leaves_row(context, random, position, leaf_radius - 1,
                     -foliage_height_, leaves, placed);
    place_leaves_row(context, random, position, leaf_radius + radius_offset - 1,
                     0, leaves, placed);
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
        const jint radius = y < size_limit_ ? lower_size_ : upper_size_;
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

    std::vector<std::pair<block_pos, jint>> attachments;
    if (kind_ == trunk_kind::straight) {
        for (jint y = 0; y < clipped; ++y) {
            (void)place_log(origin.x, origin.y + y, origin.z);
        }
        attachments.emplace_back(
            block_pos{.x = origin.x, .y = origin.y + clipped, .z = origin.z},
            0);
    } else {
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
            attachments.emplace_back(block_pos{.x = tx, .y = lean_top, .z = tz},
                                     1);
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
                attachments.emplace_back(
                    block_pos{.x = tx, .y = branch_top, .z = tz}, 0);
            }
        }
    }

    std::vector<block_pos> leaves;
    bool placed = false;
    for (const auto &[attachment, radius_offset] : attachments) {
        place_foliage(context, random, attachment, leaf_radius, radius_offset,
                      foliage_height, leaves, placed);
    }
    if (logs.empty() || leaves.empty()) {
        return false;
    }
    update_leaves(context, logs, leaves);
    return true;
}

} // namespace refrain::worldgen::feature
