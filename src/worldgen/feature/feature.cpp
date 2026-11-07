// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/feature/feature.hpp>

#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/core/mth.hpp>

namespace refrain::worldgen::feature {

auto simple_block_feature::place(feature_context &context,
                                 worldgen_random &random,
                                 const block_pos &origin) const -> bool {
    const auto state = provider_->get(context.placement, random, origin);
    if (state == no_block_state) {
        return false;
    }
    (void)schedule_tick_;
    return context.placement.region->set_block_state(origin.x, origin.y,
                                                     origin.z, state);
}

auto disk_feature::place(feature_context &context, worldgen_random &random,
                         const block_pos &origin) const -> bool {
    jint top = origin.y + half_height_;
    const jint bottom = origin.y - half_height_ - 1;
    const jint radius = radius_->sample(random);
    bool placed_any = false;

    for (jint x = origin.x - radius; x <= origin.x + radius; ++x) {
        const jint xd = x - origin.x;
        for (jint z = origin.z - radius; z <= origin.z + radius; ++z) {
            const jint zd = z - origin.z;
            if (xd * xd + zd * zd > radius * radius) {
                continue;
            }
            bool placed_above = false;
            for (jint y = top; y > bottom; --y) {
                if (!target_->test(*context.placement.region,
                                   *context.placement.states,
                                   block_pos{.x = x, .y = y, .z = z})) {
                    placed_above = false;
                    continue;
                }
                const auto state =
                    provider_->get(context.placement, random,
                                   block_pos{.x = x, .y = y, .z = z});
                if (state == no_block_state) {
                    continue;
                }
                if (!context.placement.region->set_block_state(x, y, z,
                                                               state)) {
                    continue;
                }
                if (!placed_above) {
                    context.placement.chunk->mark_post_processing(x, y + 1, z);
                }
                placed_any = true;
                placed_above = true;
            }
        }
    }
    return placed_any;
}

namespace {

constexpr double pi = 3.14159265358979323846;

auto is_air_name(std::string_view name) -> bool {
    return name == "minecraft:air" || name == "minecraft:cave_air" ||
           name == "minecraft:void_air";
}

} // namespace

auto ore_feature::height_at(feature_context &context, jint x, jint z) const
    -> jint {
    if (context.placement.height) {
        return context.placement.height(
            worldgen::heightmap_kind::ocean_floor_wg, x, z);
    }
    const auto *chunk = context.placement.chunk;
    const jint local_x = x - chunk->chunk_x() * 16;
    const jint local_z = z - chunk->chunk_z() * 16;
    if (local_x < 0 || local_x > 15 || local_z < 0 || local_z > 15) {
        return chunk->min_y();
    }
    return chunk->first_available(worldgen::heightmap_kind::ocean_floor_wg,
                                  local_x, local_z);
}

auto ore_feature::adjacent_to_air(feature_context &context,
                                  const block_pos &pos) const -> bool {
    constexpr std::array<std::array<jint, 3>, 6> offsets{{
        {0, -1, 0},
        {0, 1, 0},
        {-1, 0, 0},
        {1, 0, 0},
        {0, 0, -1},
        {0, 0, 1},
    }};
    for (const auto &offset : offsets) {
        const block_pos neighbour{.x = pos.x + offset[0],
                                  .y = pos.y + offset[1],
                                  .z = pos.z + offset[2]};
        if (is_air_name(context.placement.states->name(
                context.placement.region->state_at(neighbour.x, neighbour.y,
                                                   neighbour.z)))) {
            return true;
        }
    }
    return false;
}

auto ore_feature::can_place(feature_context &context, worldgen_random &random,
                            block_state_id state, const block_pos &pos,
                            const rule_test &target) const -> bool {
    if (!target.test(state, pos, random)) {
        return false;
    }
    if (discard_chance_on_air_exposure_ <= 0.0F) {
        return true;
    }
    if (discard_chance_on_air_exposure_ >= 1.0F) {
        return !adjacent_to_air(context, pos);
    }
    return random.next_float() >= discard_chance_on_air_exposure_ ||
           !adjacent_to_air(context, pos);
}

auto ore_feature::place(feature_context &context, worldgen_random &random,
                        const block_pos &origin) const -> bool {
    const float direction = random.next_float() * static_cast<float>(pi);
    const float spread_xy = static_cast<float>(size_) / 8.0F;
    const jint max_radius = static_cast<jint>(std::ceil(
        static_cast<double>(static_cast<float>(size_) / 16.0F * 2.0F + 1.0F) /
        2.0F));
    const double x0 = origin.x + std::sin(direction) * spread_xy;
    const double x1 = origin.x - std::sin(direction) * spread_xy;
    const double z0 = origin.z + std::cos(direction) * spread_xy;
    const double z1 = origin.z - std::cos(direction) * spread_xy;
    const double y0 = origin.y + random.next_int(3) - 2;
    const double y1 = origin.y + random.next_int(3) - 2;
    const jint x_start =
        origin.x - static_cast<jint>(std::ceil(spread_xy)) - max_radius;
    const jint y_start = origin.y - 2 - max_radius;
    const jint z_start =
        origin.z - static_cast<jint>(std::ceil(spread_xy)) - max_radius;
    const jint size_xz =
        2 * (static_cast<jint>(std::ceil(spread_xy)) + max_radius);
    const jint size_y = 2 * (2 + max_radius);

    for (jint x = x_start; x <= x_start + size_xz; ++x) {
        for (jint z = z_start; z <= z_start + size_xz; ++z) {
            if (y_start <= height_at(context, x, z)) {
                return place_ore(context, random, x0, x1, z0, z1, y0, y1,
                                 x_start, y_start, z_start, size_xz, size_y);
            }
        }
    }
    return false;
}

auto ore_feature::place_ore(feature_context &context, worldgen_random &random,
                            double x0, double x1, double z0, double z1,
                            double y0, double y1, jint x_start, jint y_start,
                            jint z_start, jint size_xz, jint size_y) const
    -> bool {
    std::vector<bool> tested(static_cast<std::size_t>(size_xz) *
                                 static_cast<std::size_t>(size_y) *
                                 static_cast<std::size_t>(size_xz),
                             false);
    std::vector<double> data(static_cast<std::size_t>(size_) * 4U);
    for (jint i = 0; i < size_; ++i) {
        const float step = static_cast<float>(i) / static_cast<float>(size_);
        const double xx = mth::lerp(static_cast<double>(step), x0, x1);
        const double yy = mth::lerp(static_cast<double>(step), y0, y1);
        const double zz = mth::lerp(static_cast<double>(step), z0, z1);
        const double ss = random.next_double() * size_ / 16.0;
        const double radius =
            ((mth::sin(static_cast<double>(static_cast<float>(pi) * step)) +
              1.0F) *
                 ss +
             1.0) /
            2.0;
        data[static_cast<std::size_t>(i) * 4U] = xx;
        data[static_cast<std::size_t>(i) * 4U + 1U] = yy;
        data[static_cast<std::size_t>(i) * 4U + 2U] = zz;
        data[static_cast<std::size_t>(i) * 4U + 3U] = radius;
    }

    for (jint i1 = 0; i1 < size_ - 1; ++i1) {
        if (data[static_cast<std::size_t>(i1) * 4U + 3U] <= 0.0) {
            continue;
        }
        for (jint i2 = i1 + 1; i2 < size_; ++i2) {
            if (data[static_cast<std::size_t>(i2) * 4U + 3U] <= 0.0) {
                continue;
            }
            const double dx = data[static_cast<std::size_t>(i1) * 4U] -
                              data[static_cast<std::size_t>(i2) * 4U];
            const double dy = data[static_cast<std::size_t>(i1) * 4U + 1U] -
                              data[static_cast<std::size_t>(i2) * 4U + 1U];
            const double dz = data[static_cast<std::size_t>(i1) * 4U + 2U] -
                              data[static_cast<std::size_t>(i2) * 4U + 2U];
            const double dr = data[static_cast<std::size_t>(i1) * 4U + 3U] -
                              data[static_cast<std::size_t>(i2) * 4U + 3U];
            if (dr * dr > dx * dx + dy * dy + dz * dz) {
                if (dr > 0.0) {
                    data[static_cast<std::size_t>(i2) * 4U + 3U] = -1.0;
                } else {
                    data[static_cast<std::size_t>(i1) * 4U + 3U] = -1.0;
                }
            }
        }
    }

    jint placed = 0;
    const jint min_y = context.placement.chunk->min_y();
    const jint max_y = min_y + context.placement.chunk->height();
    for (jint i = 0; i < size_; ++i) {
        const double radius = data[static_cast<std::size_t>(i) * 4U + 3U];
        if (radius < 0.0) {
            continue;
        }
        const double xx = data[static_cast<std::size_t>(i) * 4U];
        const double yy = data[static_cast<std::size_t>(i) * 4U + 1U];
        const double zz = data[static_cast<std::size_t>(i) * 4U + 2U];
        const jint x_min = std::max(mth::floor(xx - radius), x_start);
        const jint y_min = std::max(mth::floor(yy - radius), y_start);
        const jint z_min = std::max(mth::floor(zz - radius), z_start);
        const jint x_max = std::max(mth::floor(xx + radius), x_min);
        const jint y_max = std::max(mth::floor(yy + radius), y_min);
        const jint z_max = std::max(mth::floor(zz + radius), z_min);

        for (jint x = x_min; x <= x_max; ++x) {
            const double xd = (x + 0.5 - xx) / radius;
            if (xd * xd >= 1.0) {
                continue;
            }
            for (jint y = y_min; y <= y_max; ++y) {
                const double yd = (y + 0.5 - yy) / radius;
                if (xd * xd + yd * yd >= 1.0) {
                    continue;
                }
                for (jint z = z_min; z <= z_max; ++z) {
                    const double zd = (z + 0.5 - zz) / radius;
                    if (xd * xd + yd * yd + zd * zd >= 1.0 || y < min_y ||
                        y >= max_y) {
                        continue;
                    }
                    const auto bit_index = static_cast<std::size_t>(
                        x - x_start + (y - y_start) * size_xz +
                        (z - z_start) * size_xz * size_y);
                    if (bit_index >= tested.size() || tested[bit_index]) {
                        continue;
                    }
                    tested[bit_index] = true;
                    const block_pos pos{.x = x, .y = y, .z = z};
                    const auto current =
                        context.placement.region->state_at(pos.x, pos.y, pos.z);
                    for (const auto &replacement : targets_) {
                        if (can_place(context, random, current, pos,
                                      *replacement.target)) {
                            if (context.placement.region->set_block_state(
                                    pos.x, pos.y, pos.z, replacement.state)) {
                                ++placed;
                            }
                            break;
                        }
                    }
                }
            }
        }
    }
    return placed > 0;
}

auto spring_feature::valid(feature_context &context, const block_pos &pos) const
    -> bool {
    const auto name = state_block_name(context.placement.states->name(
        context.placement.region->state_at(pos.x, pos.y, pos.z)));
    return std::find(valid_blocks_.begin(), valid_blocks_.end(), name) !=
           valid_blocks_.end();
}

auto spring_feature::place(feature_context &context, worldgen_random &,
                           const block_pos &origin) const -> bool {
    const block_pos above{.x = origin.x, .y = origin.y + 1, .z = origin.z};
    if (!valid(context, above)) {
        return false;
    }
    const block_pos below{.x = origin.x, .y = origin.y - 1, .z = origin.z};
    if (requires_block_below_ && !valid(context, below)) {
        return false;
    }
    const auto current =
        context.placement.region->state_at(origin.x, origin.y, origin.z);
    const auto current_name = context.placement.states->name(current);
    if (!is_air_block_name(current_name) && !valid(context, origin)) {
        return false;
    }
    jint rocks = 0;
    jint holes = 0;
    constexpr std::array<block_pos, 5> relative{{
        {.x = -1, .y = 0, .z = 0},
        {.x = 1, .y = 0, .z = 0},
        {.x = 0, .y = 0, .z = -1},
        {.x = 0, .y = 0, .z = 1},
        {.x = 0, .y = -1, .z = 0},
    }};
    for (const auto &offset : relative) {
        const block_pos pos{.x = origin.x + offset.x,
                            .y = origin.y + offset.y,
                            .z = origin.z + offset.z};
        if (valid(context, pos)) {
            ++rocks;
        }
        if (is_air_block_name(context.placement.states->name(
                context.placement.region->state_at(pos.x, pos.y, pos.z)))) {
            ++holes;
        }
    }
    if (rocks != rock_count_ || holes != hole_count_) {
        return false;
    }
    return context.placement.region->set_block_state(origin.x, origin.y,
                                                     origin.z, state_);
}

auto block_blob_feature::place(feature_context &context,
                               worldgen_random &random,
                               const block_pos &initial) const -> bool {
    block_pos origin = initial;
    while (origin.y > context.placement.chunk->min_y() + 3 &&
           !can_place_on_->test(
               *context.placement.region, *context.placement.states,
               block_pos{.x = origin.x, .y = origin.y - 1, .z = origin.z})) {
        --origin.y;
    }
    if (origin.y <= context.placement.chunk->min_y() + 3) {
        return false;
    }

    for (jint c = 0; c < 3; ++c) {
        const jint xr = random.next_int(2);
        const jint yr = random.next_int(2);
        const jint zr = random.next_int(2);
        const float radius = static_cast<float>(xr + yr + zr) * 0.333F + 0.5F;
        for (jint x = origin.x - xr; x <= origin.x + xr; ++x) {
            for (jint y = origin.y - yr; y <= origin.y + yr; ++y) {
                for (jint z = origin.z - zr; z <= origin.z + zr; ++z) {
                    const double dx = x - origin.x;
                    const double dy = y - origin.y;
                    const double dz = z - origin.z;
                    if (dx * dx + dy * dy + dz * dz <=
                        static_cast<double>(radius) * radius) {
                        const auto state =
                            provider_->get(context.placement, random,
                                           block_pos{.x = x, .y = y, .z = z});
                        (void)context.placement.region->set_block_state(x, y, z,
                                                                        state);
                    }
                }
            }
        }
        origin.x += -1 + random.next_int(2);
        origin.y += -random.next_int(2);
        origin.z += -1 + random.next_int(2);
    }
    return true;
}

auto offset(block_direction direction, const block_pos &pos) -> block_pos {
    switch (direction) {
    case block_direction::down:
        return {.x = pos.x, .y = pos.y - 1, .z = pos.z};
    case block_direction::up:
        return {.x = pos.x, .y = pos.y + 1, .z = pos.z};
    case block_direction::north:
        return {.x = pos.x, .y = pos.y, .z = pos.z - 1};
    case block_direction::south:
        return {.x = pos.x, .y = pos.y, .z = pos.z + 1};
    case block_direction::west:
        return {.x = pos.x - 1, .y = pos.y, .z = pos.z};
    case block_direction::east:
        return {.x = pos.x + 1, .y = pos.y, .z = pos.z};
    }
    return pos;
}

void block_column_feature::truncate(std::vector<jint> &heights,
                                    jint total_height, jint new_height,
                                    bool prioritize_tip) {
    jint remove = total_height - new_height;
    const jint step = prioritize_tip ? 1 : -1;
    jint index = prioritize_tip ? 0 : static_cast<jint>(heights.size()) - 1;
    while (index >= 0 && index < static_cast<jint>(heights.size()) &&
           remove > 0) {
        const jint from_layer =
            std::min(heights[static_cast<std::size_t>(index)], remove);
        remove -= from_layer;
        heights[static_cast<std::size_t>(index)] -= from_layer;
        index += step;
    }
}

auto block_column_feature::place(feature_context &context,
                                 worldgen_random &random,
                                 const block_pos &origin) const -> bool {
    std::vector<jint> heights(layers_.size(), 0);
    jint total_height = 0;
    for (std::size_t i = 0; i < layers_.size(); ++i) {
        heights[i] = layers_[i].height->sample(random);
        total_height += heights[i];
    }
    if (total_height == 0) {
        return false;
    }

    block_pos place_pos = origin;
    block_pos next_pos = offset(direction_, place_pos);
    for (jint y = 0; y < total_height; ++y) {
        if (!allowed_placement_->test(*context.placement.region,
                                      *context.placement.states, next_pos)) {
            truncate(heights, total_height, y, prioritize_tip_);
            break;
        }
        next_pos = offset(direction_, next_pos);
    }

    for (std::size_t i = 0; i < layers_.size(); ++i) {
        for (jint y = 0; y < heights[i]; ++y) {
            const auto state =
                layers_[i].provider->get(context.placement, random, place_pos);
            (void)context.placement.region->set_block_state(
                place_pos.x, place_pos.y, place_pos.z, state);
            place_pos = offset(direction_, place_pos);
        }
    }
    return true;
}

auto random_selector_feature::place(feature_context &context,
                                    worldgen_random &random,
                                    const block_pos &origin) const -> bool {
    for (const auto &[chance, id] : entries_) {
        if (random.next_float() < chance) {
            return context.place_by_id(id, random, origin);
        }
    }
    return context.place_by_id(default_id_, random, origin);
}

auto weighted_selector_feature::place(feature_context &context,
                                      worldgen_random &random,
                                      const block_pos &origin) const -> bool {
    jint selection = random.next_int(total_);
    for (const auto &[weight, id] : entries_) {
        selection -= weight;
        if (selection < 0) {
            return context.place_by_id(id, random, origin);
        }
    }
    return false;
}

auto simple_selector_feature::place(feature_context &context,
                                    worldgen_random &random,
                                    const block_pos &origin) const -> bool {
    const jint index = random.next_int(static_cast<jint>(entries_.size()));
    return context.place_by_id(entries_[static_cast<std::size_t>(index)],
                               random, origin);
}

auto boolean_selector_feature::place(feature_context &context,
                                     worldgen_random &random,
                                     const block_pos &origin) const -> bool {
    const bool result = random.next_boolean();
    return context.place_by_id(result ? feature_true_ : feature_false_, random,
                               origin);
}

} // namespace refrain::worldgen::feature
