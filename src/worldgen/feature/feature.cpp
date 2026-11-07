// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/feature/feature.hpp>

#include <refrain/worldgen/chunk/block_state_codec.hpp>
#include <refrain/worldgen/core/mth.hpp>
#include <refrain/worldgen/noise/noise.hpp>

namespace refrain::worldgen::feature {

namespace {

auto block_name(const feature_context &context, block_state_id state)
    -> std::string_view {
    return state_block_name(context.placement.states->name(state));
}

struct direction_offset {
    jint dx;
    jint dy;
    jint dz;
};

constexpr std::array<std::pair<jint, jint>, 4> horizontal_neighbors{{
    {0, -1},
    {0, 1},
    {-1, 0},
    {1, 0},
}};

constexpr std::array<direction_offset, 6> all_directions{{
    {.dx = 0, .dy = -1, .dz = 0},
    {.dx = 0, .dy = 1, .dz = 0},
    {.dx = 0, .dy = 0, .dz = -1},
    {.dx = 0, .dy = 0, .dz = 1},
    {.dx = -1, .dy = 0, .dz = 0},
    {.dx = 1, .dy = 0, .dz = 0},
}};

/// `Feature.markAboveForPostProcessing`: mark up to two non-air blocks above a
/// placement, stopping at the first air block.
void mark_above_for_post_processing(const feature_context &context,
                                    const block_pos &pos) {
    for (jint offset = 1; offset <= 2; ++offset) {
        const block_pos above{.x = pos.x, .y = pos.y + offset, .z = pos.z};
        const auto state =
            context.placement.region->state_at(above.x, above.y, above.z);
        if (context.placement.states->traits(state).air) {
            return;
        }
        (void)context.placement.region->mark_post_processing(above.x, above.y,
                                                             above.z);
    }
}

} // namespace

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
                    mark_above_for_post_processing(
                        context, block_pos{.x = x, .y = y, .z = z});
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

auto scattered_ore_feature::adjacent_to_air(feature_context &context,
                                            const block_pos &pos) const
    -> bool {
    for (const auto &direction : all_directions) {
        const auto state = context.placement.region->state_at(
            pos.x + direction.dx, pos.y + direction.dy, pos.z + direction.dz);
        if (context.placement.states->traits(state).air) {
            return true;
        }
    }
    return false;
}

auto scattered_ore_feature::can_place(feature_context &context,
                                      worldgen_random &random,
                                      block_state_id state,
                                      const block_pos &pos,
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

auto scattered_ore_feature::place(feature_context &context,
                                  worldgen_random &random,
                                  const block_pos &origin) const -> bool {
    const jint tries = random.next_int(size_ + 1);
    for (jint i = 0; i < tries; ++i) {
        const jint distance = std::min(i, 7);
        const auto spread = [&]() {
            return static_cast<jint>(
                std::lround((random.next_float() - random.next_float()) *
                            static_cast<float>(distance)));
        };
        const block_pos pos{.x = origin.x + spread(),
                            .y = origin.y + spread(),
                            .z = origin.z + spread()};
        const auto state =
            context.placement.region->state_at(pos.x, pos.y, pos.z);
        for (const auto &target : targets_) {
            if (can_place(context, random, state, pos, *target.target)) {
                (void)context.placement.region->set_block_state(
                    pos.x, pos.y, pos.z, target.state);
                break;
            }
        }
    }
    return true;
}

auto random_neighbor_spread_feature::accepted(const block_state_table &states,
                                              block_state_id state) const
    -> bool {
    const auto name = state_block_name(states.name(state));
    for (const auto &entry : accepted_neighbors_) {
        if (!entry.empty() && entry.front() == '#') {
            if (states.in_tag(state, entry.substr(1))) {
                return true;
            }
        } else if (name == entry) {
            return true;
        }
    }
    return false;
}

auto random_neighbor_spread_feature::place(feature_context &context,
                                           worldgen_random &random,
                                           const block_pos &origin) const
    -> bool {
    const auto write = [&](const block_pos &pos) {
        const auto state = block_->get(context.placement, random, pos);
        if (state == no_block_state) {
            return;
        }
        (void)context.placement.region->set_block_state(pos.x, pos.y, pos.z,
                                                        state);
    };
    write(origin);
    const jint attempts = attempts_->sample(random);
    for (jint i = 0; i < attempts; ++i) {
        const block_pos pos{
            .x = origin.x + xz_offset_->sample(random),
            .y = origin.y + y_offset_->sample(random),
            .z = origin.z + xz_offset_->sample(random),
        };
        if (!can_replace_->test(*context.placement.region,
                                *context.placement.states, pos)) {
            continue;
        }
        jint neighbours = 0;
        for (const auto &direction : all_directions) {
            const auto state = context.placement.region->state_at(
                pos.x + direction.dx, pos.y + direction.dy,
                pos.z + direction.dz);
            if (accepted(*context.placement.states, state)) {
                ++neighbours;
            }
            if (neighbours > 1) {
                break;
            }
        }
        if (neighbours == 1) {
            write(pos);
        }
    }
    return true;
}

auto bamboo_feature::place(feature_context &context, worldgen_random &random,
                           const block_pos &origin) const -> bool {
    const auto air = context.placement.states->air();
    if (context.placement.region->state_at(origin.x, origin.y, origin.z) !=
        air) {
        return false;
    }
    if (!context.placement.states->in_tag(context.placement.region->state_at(
                                              origin.x, origin.y - 1, origin.z),
                                          "minecraft:supports_bamboo")) {
        return false;
    }
    const auto surface_height = [&](jint x, jint z) {
        if (context.placement.height) {
            return context.placement.height(heightmap_kind::world_surface, x,
                                            z);
        }
        const auto *chunk = context.placement.chunk;
        const jint local_x = x - chunk->chunk_x() * 16;
        const jint local_z = z - chunk->chunk_z() * 16;
        if (local_x < 0 || local_x > 15 || local_z < 0 || local_z > 15) {
            return chunk->min_y();
        }
        return chunk->first_available(heightmap_kind::world_surface, local_x,
                                      local_z);
    };
    const jint height = random.next_int(12) + 5;
    if (random.next_float() < probability_) {
        const jint radius = random.next_int(4) + 1;
        for (jint x = origin.x - radius; x <= origin.x + radius; ++x) {
            for (jint z = origin.z - radius; z <= origin.z + radius; ++z) {
                const jint dx = x - origin.x;
                const jint dz = z - origin.z;
                if (dx * dx + dz * dz > radius * radius) {
                    continue;
                }
                const jint y = surface_height(x, z) - 1;
                if (context.placement.states->in_tag(
                        context.placement.region->state_at(x, y, z),
                        "minecraft:beneath_bamboo_podzol_replaceable")) {
                    (void)context.placement.region->set_block_state(x, y, z,
                                                                    podzol_);
                }
            }
        }
    }
    block_pos pos = origin;
    for (jint i = 0; i < height && context.placement.region->state_at(
                                       pos.x, pos.y, pos.z) == air;
         ++i) {
        (void)context.placement.region->set_block_state(pos.x, pos.y, pos.z,
                                                        trunk_);
        ++pos.y;
    }
    if (pos.y - origin.y >= 3) {
        (void)context.placement.region->set_block_state(pos.x, pos.y, pos.z,
                                                        final_large_);
        --pos.y;
        (void)context.placement.region->set_block_state(pos.x, pos.y, pos.z,
                                                        top_large_);
        --pos.y;
        (void)context.placement.region->set_block_state(pos.x, pos.y, pos.z,
                                                        top_small_);
    }
    return true;
}

auto vines_feature::place(feature_context &context, worldgen_random &random,
                          const block_pos &origin) const -> bool {
    (void)random;
    if (context.placement.region->state_at(origin.x, origin.y, origin.z) !=
        context.placement.states->air()) {
        return false;
    }
    for (std::size_t i = 0; i < all_directions.size(); ++i) {
        const auto &direction = all_directions[i];
        if (direction.dy == -1) {
            continue;
        }
        const auto neighbor = context.placement.region->state_at(
            origin.x + direction.dx, origin.y + direction.dy,
            origin.z + direction.dz);
        if (!context.placement.states->traits(neighbor).blocks_motion) {
            continue;
        }
        // all_directions order is down, up, north, south, west, east; the
        // state array uses Direction.get3DDataValue order.
        static constexpr std::array<std::size_t, 6> state_index{0, 1, 2,
                                                                3, 4, 5};
        return context.placement.region->set_block_state(
            origin.x, origin.y, origin.z, states_[state_index[i]]);
    }
    return false;
}

auto huge_fungus_feature::replaceable(feature_context &context,
                                      const block_pos &pos,
                                      bool check_plants) const -> bool {
    const auto state = context.placement.region->state_at(pos.x, pos.y, pos.z);
    if (context.placement.states->traits(state).air) {
        return true;
    }
    return check_plants && replaceable_->test(*context.placement.region,
                                              *context.placement.states, pos);
}

void huge_fungus_feature::place_stem(feature_context &context,
                                     worldgen_random &random,
                                     const block_pos &origin, jint total_height,
                                     bool huge) const {
    const jint radius = huge ? 1 : 0;
    for (jint dx = -radius; dx <= radius; ++dx) {
        for (jint dz = -radius; dz <= radius; ++dz) {
            const bool corner =
                huge && std::abs(dx) == radius && std::abs(dz) == radius;
            for (jint dy = 0; dy < total_height; ++dy) {
                const block_pos pos{
                    .x = origin.x + dx, .y = origin.y + dy, .z = origin.z + dz};
                if (!replaceable(context, pos, true)) {
                    continue;
                }
                if (planted_) {
                    (void)context.placement.region->set_block_state(
                        pos.x, pos.y, pos.z, stem_);
                } else if (corner) {
                    if (random.next_float() < 0.1F) {
                        (void)context.placement.region->set_block_state(
                            pos.x, pos.y, pos.z, stem_);
                    }
                } else {
                    (void)context.placement.region->set_block_state(
                        pos.x, pos.y, pos.z, stem_);
                }
            }
        }
    }
}

void huge_fungus_feature::place_hat_drop(feature_context &context,
                                         worldgen_random &random,
                                         const block_pos &pos,
                                         bool vines) const {
    const auto below =
        context.placement.region->state_at(pos.x, pos.y - 1, pos.z);
    if (block_name(context, below) == block_name(context, hat_)) {
        (void)context.placement.region->set_block_state(pos.x, pos.y, pos.z,
                                                        hat_);
        return;
    }
    if (random.next_float() < 0.15F) {
        (void)context.placement.region->set_block_state(pos.x, pos.y, pos.z,
                                                        hat_);
        if (vines && random.next_int(11) == 0) {
            try_weeping_vines(context, random, pos);
        }
    }
}

void huge_fungus_feature::place_hat_block(feature_context &context,
                                          worldgen_random &random,
                                          const block_pos &pos,
                                          float decor_chance, float hat_chance,
                                          float vines_chance) const {
    if (random.next_float() < decor_chance) {
        (void)context.placement.region->set_block_state(pos.x, pos.y, pos.z,
                                                        decor_);
        return;
    }
    if (random.next_float() < hat_chance) {
        (void)context.placement.region->set_block_state(pos.x, pos.y, pos.z,
                                                        hat_);
        if (random.next_float() < vines_chance) {
            try_weeping_vines(context, random, pos);
        }
    }
}

void huge_fungus_feature::try_weeping_vines(feature_context &context,
                                            worldgen_random &random,
                                            const block_pos &pos) const {
    const block_pos start{.x = pos.x, .y = pos.y - 1, .z = pos.z};
    if (context.placement.region->state_at(start.x, start.y, start.z) !=
        context.placement.states->air()) {
        return;
    }
    jint goal = 1 + random.next_int(5);
    if (random.next_int(7) == 0) {
        goal *= 2;
    }
    block_pos cursor = start;
    for (jint height = 0; height <= goal; ++height) {
        if (context.placement.region->state_at(cursor.x, cursor.y, cursor.z) ==
            context.placement.states->air()) {
            const bool at_bottom =
                height == goal || context.placement.region->state_at(
                                      cursor.x, cursor.y - 1, cursor.z) !=
                                      context.placement.states->air();
            if (at_bottom) {
                const auto age = random.next_int(3);
                (void)context.placement.region->set_block_state(
                    cursor.x, cursor.y, cursor.z,
                    weeping_heads_[static_cast<std::size_t>(age)]);
                break;
            }
            (void)context.placement.region->set_block_state(
                cursor.x, cursor.y, cursor.z, weeping_plant_);
        }
        --cursor.y;
    }
}

void huge_fungus_feature::place_hat(feature_context &context,
                                    worldgen_random &random,
                                    const block_pos &origin, jint total_height,
                                    bool huge) const {
    const bool vines = place_vines_;
    const jint hat_height =
        std::min(random.next_int(1 + total_height / 3) + 5, total_height);
    const jint hat_start = total_height - hat_height;
    for (jint dy = hat_start; dy <= total_height; ++dy) {
        jint radius = dy < total_height - random.next_int(3) ? 2 : 1;
        if (hat_height > 8 && dy < hat_start + 4) {
            radius = 3;
        }
        if (huge) {
            ++radius;
        }
        for (jint dx = -radius; dx <= radius; ++dx) {
            for (jint dz = -radius; dz <= radius; ++dz) {
                const bool edge_x = dx == -radius || dx == radius;
                const bool edge_z = dz == -radius || dz == radius;
                const bool inside = !edge_x && !edge_z && dy != total_height;
                const bool corner = edge_x && edge_z;
                const bool hat_bottom = dy < hat_start + 3;
                const block_pos pos{
                    .x = origin.x + dx, .y = origin.y + dy, .z = origin.z + dz};
                if (!replaceable(context, pos, false)) {
                    continue;
                }
                if (hat_bottom) {
                    if (!inside) {
                        place_hat_drop(context, random, pos, vines);
                    }
                } else if (inside) {
                    place_hat_block(context, random, pos, 0.1F, 0.2F,
                                    vines ? 0.1F : 0.0F);
                } else if (corner) {
                    place_hat_block(context, random, pos, 0.01F, 0.7F,
                                    vines ? 0.083F : 0.0F);
                } else {
                    place_hat_block(context, random, pos, 0.0005F, 0.98F,
                                    vines ? 0.07F : 0.0F);
                }
            }
        }
    }
}

auto huge_fungus_feature::place(feature_context &context,
                                worldgen_random &random,
                                const block_pos &origin) const -> bool {
    const auto below =
        context.placement.region->state_at(origin.x, origin.y - 1, origin.z);
    if (block_name(context, below) != block_name(context, base_)) {
        return false;
    }
    jint total = 4 + random.next_int(10);
    if (random.next_int(12) == 0) {
        total *= 2;
    }
    if (!planted_ &&
        origin.y + total + 1 >= context.placement.chunk->min_y() +
                                    context.placement.chunk->height()) {
        return false;
    }
    const bool huge = !planted_ && random.next_float() < 0.06F;
    (void)context.placement.region->set_block_state(
        origin.x, origin.y, origin.z, context.placement.states->air());
    place_stem(context, random, origin, total, huge);
    place_hat(context, random, origin, total, huge);
    return true;
}

auto huge_mushroom_feature::radius_for_height(jint height, jint y) const
    -> jint {
    if (red_) {
        if (y < height && y >= height - 3) {
            return foliage_radius_;
        }
        return y == height ? foliage_radius_ : 0;
    }
    return y <= 3 ? 0 : foliage_radius_;
}

void huge_mushroom_feature::place_mushroom_block(feature_context &context,
                                                 const block_pos &pos,
                                                 block_state_id state) const {
    const auto current =
        context.placement.region->state_at(pos.x, pos.y, pos.z);
    if (context.placement.states->traits(current).air ||
        context.placement.states->in_tag(
            current, "minecraft:replaceable_by_mushrooms")) {
        (void)context.placement.region->set_block_state(pos.x, pos.y, pos.z,
                                                        state);
    }
}

auto huge_mushroom_feature::place(feature_context &context,
                                  worldgen_random &random,
                                  const block_pos &origin) const -> bool {
    jint height = random.next_int(3) + 4;
    if (random.next_int(12) == 0) {
        height *= 2;
    }
    const jint min_y = context.placement.chunk->min_y();
    const jint max_y = min_y + context.placement.chunk->height() - 1;
    if (origin.y < min_y + 1 || origin.y + height + 1 > max_y) {
        return false;
    }
    if (!can_place_on_->test(
            *context.placement.region, *context.placement.states,
            block_pos{.x = origin.x, .y = origin.y - 1, .z = origin.z})) {
        return false;
    }
    for (jint dy = 0; dy <= height; ++dy) {
        const jint radius = radius_for_height(height, dy);
        for (jint dx = -radius; dx <= radius; ++dx) {
            for (jint dz = -radius; dz <= radius; ++dz) {
                const auto state = context.placement.region->state_at(
                    origin.x + dx, origin.y + dy, origin.z + dz);
                if (!context.placement.states->traits(state).air &&
                    !context.placement.states->in_tag(state,
                                                      "minecraft:leaves")) {
                    return false;
                }
            }
        }
    }
    make_cap(context, random, origin, height);
    place_trunk(context, random, origin, height);
    return true;
}

void huge_mushroom_feature::make_cap(feature_context &context,
                                     worldgen_random &random,
                                     const block_pos &origin,
                                     jint height) const {
    if (red_) {
        for (jint dy = height - 3; dy <= height; ++dy) {
            const jint radius =
                dy < height ? foliage_radius_ : foliage_radius_ - 1;
            const jint center = foliage_radius_ - 2;
            for (jint dx = -radius; dx <= radius; ++dx) {
                for (jint dz = -radius; dz <= radius; ++dz) {
                    const bool edge_x = dx == -radius || dx == radius;
                    const bool edge_z = dz == -radius || dz == radius;
                    if (dy < height && edge_x == edge_z) {
                        continue;
                    }
                    const auto pos = block_pos{.x = origin.x + dx,
                                               .y = origin.y + dy,
                                               .z = origin.z + dz};
                    auto state = cap_->get(context.placement, random, origin);
                    if (state == no_block_state) {
                        continue;
                    }
                    auto key = context.placement.states->name(state);
                    auto variant = with_state_property(
                        key, "up", dy >= height - 1 ? "true" : "false");
                    variant = with_state_property(
                        variant, "west", dx < -center ? "true" : "false");
                    variant = with_state_property(
                        variant, "east", dx > center ? "true" : "false");
                    variant = with_state_property(
                        variant, "north", dz < -center ? "true" : "false");
                    variant = with_state_property(
                        variant, "south", dz > center ? "true" : "false");
                    if (context.placement.resolve_state) {
                        const auto resolved =
                            context.placement.resolve_state(variant);
                        if (resolved != no_block_state) {
                            state = resolved;
                        }
                    }
                    place_mushroom_block(context, pos, state);
                }
            }
        }
        return;
    }
    for (jint dx = -foliage_radius_; dx <= foliage_radius_; ++dx) {
        for (jint dz = -foliage_radius_; dz <= foliage_radius_; ++dz) {
            const bool min_x = dx == -foliage_radius_;
            const bool max_x = dx == foliage_radius_;
            const bool min_z = dz == -foliage_radius_;
            const bool max_z = dz == foliage_radius_;
            const bool x_edge = min_x || max_x;
            const bool z_edge = min_z || max_z;
            if (x_edge && z_edge) {
                continue;
            }
            const auto pos = block_pos{
                .x = origin.x + dx, .y = origin.y + height, .z = origin.z + dz};
            auto state = cap_->get(context.placement, random, origin);
            if (state == no_block_state) {
                continue;
            }
            auto key = context.placement.states->name(state);
            auto variant = with_state_property(
                key, "west",
                (min_x || (z_edge && dx == 1 - foliage_radius_)) ? "true"
                                                                 : "false");
            variant = with_state_property(
                variant, "east",
                (max_x || (z_edge && dx == foliage_radius_ - 1)) ? "true"
                                                                 : "false");
            variant = with_state_property(
                variant, "north",
                (min_z || (x_edge && dz == 1 - foliage_radius_)) ? "true"
                                                                 : "false");
            variant = with_state_property(
                variant, "south",
                (max_z || (x_edge && dz == foliage_radius_ - 1)) ? "true"
                                                                 : "false");
            if (context.placement.resolve_state) {
                const auto resolved = context.placement.resolve_state(variant);
                if (resolved != no_block_state) {
                    state = resolved;
                }
            }
            place_mushroom_block(context, pos, state);
        }
    }
}

void huge_mushroom_feature::place_trunk(feature_context &context,
                                        worldgen_random &random,
                                        const block_pos &origin,
                                        jint height) const {
    for (jint dy = 0; dy < height; ++dy) {
        const auto state = stem_->get(context.placement, random, origin);
        if (state == no_block_state) {
            continue;
        }
        place_mushroom_block(
            context,
            block_pos{.x = origin.x, .y = origin.y + dy, .z = origin.z}, state);
    }
}

auto iceberg_feature::is_iceberg(block_state_id state) const -> bool {
    return state == packed_ice_ || state == snow_block_ || state == blue_ice_;
}

auto iceberg_feature::height_radius_round(worldgen_random &random, jint y,
                                          jint height, jint width) const
    -> jint {
    const float k = 3.5F - random.next_float();
    float scale = (1.0F - static_cast<float>(static_cast<double>(y * y) /
                                             static_cast<double>(height) /
                                             static_cast<double>(k))) *
                  static_cast<float>(width);
    if (height > 15 + random.next_int(5)) {
        const jint temp = y < 3 + random.next_int(6) ? y / 2 : y;
        scale = (1.0F - static_cast<float>(temp) /
                            (static_cast<float>(height) * k * 0.4F)) *
                static_cast<float>(width);
    }
    return static_cast<jint>(std::ceil(static_cast<double>(scale / 2.0F)));
}

auto iceberg_feature::height_radius_ellipse(jint y, jint height,
                                            jint width) const -> jint {
    float scale = (1.0F - static_cast<float>(static_cast<double>(y * y) /
                                             static_cast<double>(height))) *
                  static_cast<float>(width);
    return static_cast<jint>(std::ceil(static_cast<double>(scale / 2.0F)));
}

auto iceberg_feature::height_radius_steep(worldgen_random &random, jint y,
                                          jint height, jint width) const
    -> jint {
    const float k = 1.0F + random.next_float() / 2.0F;
    const float scale =
        (1.0F - static_cast<float>(y) / (static_cast<float>(height) * k)) *
        static_cast<float>(width);
    return static_cast<jint>(std::ceil(static_cast<double>(scale / 2.0F)));
}

auto iceberg_feature::signed_distance_circle(worldgen_random &random, jint x,
                                             jint z, jint radius) -> double {
    const float offset = 10.0F * std::clamp(random.next_float(), 0.2F, 0.8F) /
                         static_cast<float>(radius);
    return static_cast<double>(offset) + static_cast<double>(x * x) +
           static_cast<double>(z * z) -
           static_cast<double>(radius) * static_cast<double>(radius);
}

auto iceberg_feature::signed_distance_ellipse(jint x, jint z, jint a, jint c,
                                              double angle) -> double {
    const double cos_angle = std::cos(angle);
    const double sin_angle = std::sin(angle);
    const double u = (x * cos_angle - z * sin_angle) / static_cast<double>(a);
    const double v = (x * sin_angle + z * cos_angle) / static_cast<double>(c);
    return u * u + v * v - 1.0;
}

void iceberg_feature::set_iceberg_block(feature_context &context,
                                        worldgen_random &random,
                                        const block_pos &pos, jint h_diff,
                                        jint height, bool ellipse,
                                        bool snow_on_top) const {
    const auto current =
        context.placement.region->state_at(pos.x, pos.y, pos.z);
    if (current != context.placement.states->air() && current != snow_block_ &&
        current != ice_ && current != water_) {
        return;
    }
    const bool randomness = !ellipse || random.next_double() > 0.05;
    const jint divisor = ellipse ? 3 : 2;
    if (snow_on_top && current != water_ &&
        static_cast<double>(h_diff) <= static_cast<double>(random.next_int(
                                           std::max(1, height / divisor))) +
                                           static_cast<double>(height) * 0.6 &&
        randomness) {
        (void)context.placement.region->set_block_state(pos.x, pos.y, pos.z,
                                                        snow_block_);
        return;
    }
    (void)context.placement.region->set_block_state(pos.x, pos.y, pos.z,
                                                    state_);
}

void iceberg_feature::generate_block(feature_context &context,
                                     worldgen_random &random,
                                     const block_pos &origin, jint height,
                                     jint x, jint y, jint z, jint radius,
                                     jint a, bool ellipse, jint ellipse_c,
                                     double angle, bool snow_on_top) const {
    const double distance =
        ellipse ? signed_distance_ellipse(x, z, a,
                                          y > 0 && height - y <= 3
                                              ? ellipse_c - (4 - (height - y))
                                              : ellipse_c,
                                          angle)
                : signed_distance_circle(random, x, z, radius);
    if (distance >= 0.0) {
        return;
    }
    const block_pos pos{
        .x = origin.x + x, .y = origin.y + y, .z = origin.z + z};
    const double compare = ellipse ? -0.5 : -6 - random.next_int(3);
    if (distance > compare && random.next_double() > 0.9) {
        return;
    }
    set_iceberg_block(context, random, pos, height - y, height, ellipse,
                      snow_on_top);
}

void iceberg_feature::smooth(feature_context &context, const block_pos &origin,
                             jint width, jint height, bool ellipse,
                             jint ellipse_a) const {
    const jint a = ellipse ? ellipse_a : width / 2;
    for (jint x = -a; x <= a; ++x) {
        for (jint z = -a; z <= a; ++z) {
            for (jint y = 0; y <= height; ++y) {
                const block_pos pos{
                    .x = origin.x + x, .y = origin.y + y, .z = origin.z + z};
                const auto current =
                    context.placement.region->state_at(pos.x, pos.y, pos.z);
                if (!is_iceberg(current) && current != snow_layer_) {
                    continue;
                }
                const auto below =
                    context.placement.region->state_at(pos.x, pos.y - 1, pos.z);
                if (below == context.placement.states->air()) {
                    (void)context.placement.region->set_block_state(
                        pos.x, pos.y, pos.z, context.placement.states->air());
                    (void)context.placement.region->set_block_state(
                        pos.x, pos.y + 1, pos.z,
                        context.placement.states->air());
                    continue;
                }
                if (!is_iceberg(current)) {
                    continue;
                }
                jint counter = 0;
                for (const auto &offset : horizontal_neighbors) {
                    if (!is_iceberg(context.placement.region->state_at(
                            pos.x + offset.first, pos.y,
                            pos.z + offset.second))) {
                        ++counter;
                    }
                }
                if (counter >= 3) {
                    (void)context.placement.region->set_block_state(
                        pos.x, pos.y, pos.z, context.placement.states->air());
                }
            }
        }
    }
}

void iceberg_feature::carve(feature_context &context, jint radius, jint y,
                            const block_pos &origin, bool underwater,
                            double angle, const block_pos &local_origin,
                            jint ellipse_a, jint ellipse_c) const {
    const jint a = radius + 1 + ellipse_a / 3;
    const jint c = std::min(radius - 3, 3) + ellipse_c / 2 - 1;
    for (jint x = -a; x < a; ++x) {
        for (jint z = -a; z < a; ++z) {
            const double distance = signed_distance_ellipse(
                x - local_origin.x, z - local_origin.z, a, c, angle);
            if (distance >= 0.0) {
                continue;
            }
            const block_pos pos{
                .x = origin.x + x, .y = origin.y + y, .z = origin.z + z};
            const auto current =
                context.placement.region->state_at(pos.x, pos.y, pos.z);
            if (!is_iceberg(current) && current != snow_block_) {
                continue;
            }
            if (underwater) {
                (void)context.placement.region->set_block_state(pos.x, pos.y,
                                                                pos.z, water_);
            } else {
                (void)context.placement.region->set_block_state(
                    pos.x, pos.y, pos.z, context.placement.states->air());
                if (context.placement.region->state_at(pos.x, pos.y + 1,
                                                       pos.z) == snow_layer_) {
                    (void)context.placement.region->set_block_state(
                        pos.x, pos.y + 1, pos.z,
                        context.placement.states->air());
                }
            }
        }
    }
}

void iceberg_feature::generate_cutout(feature_context &context,
                                      worldgen_random &random, jint width,
                                      jint height, const block_pos &origin,
                                      bool ellipse, jint ellipse_a,
                                      double angle, jint ellipse_c) const {
    const jint sign_x = random.next_boolean() ? -1 : 1;
    const jint sign_z = random.next_boolean() ? -1 : 1;
    jint x_off = random.next_int(std::max(width / 2 - 2, 1));
    if (random.next_boolean()) {
        x_off =
            width / 2 + 1 - random.next_int(std::max(width - width / 2 - 1, 1));
    }
    jint z_off = random.next_int(std::max(width / 2 - 2, 1));
    if (random.next_boolean()) {
        z_off =
            width / 2 + 1 - random.next_int(std::max(width - width / 2 - 1, 1));
    }
    if (ellipse) {
        x_off = z_off = random.next_int(std::max(ellipse_a - 5, 1));
    }
    const block_pos local{sign_x * x_off, 0, sign_z * z_off};
    const double cut_angle =
        ellipse ? angle + pi / 2.0 : random.next_double() * 2.0 * pi;
    for (jint y = 0; y < height - 3; ++y) {
        const jint radius = height_radius_round(random, y, height, width);
        carve(context, radius, y, origin, false, cut_angle, local, ellipse_a,
              ellipse_c);
    }
    for (jint y = -1; y > -height + random.next_int(5); --y) {
        const jint radius = height_radius_steep(random, -y, height, width);
        carve(context, radius, y, origin, true, cut_angle, local, ellipse_a,
              ellipse_c);
    }
}

auto iceberg_feature::place(feature_context &context, worldgen_random &random,
                            const block_pos &origin_in) const -> bool {
    const block_pos origin{.x = origin_in.x,
                           .y = context.placement.generation.sea_level,
                           .z = origin_in.z};
    const bool snow_on_top = random.next_double() > 0.7;
    const double angle = random.next_double() * 2.0 * pi;
    const jint ellipse_a = 11 - random.next_int(5);
    const jint ellipse_c = 3 + random.next_int(3);
    const bool ellipse = random.next_double() > 0.7;
    jint over_water =
        ellipse ? random.next_int(6) + 6 : random.next_int(15) + 3;
    if (!ellipse && random.next_double() > 0.9) {
        over_water += random.next_int(19) + 7;
    }
    const jint under_water = std::min(over_water + random.next_int(11), 18);
    const jint width =
        std::min(over_water + random.next_int(7) - random.next_int(5), 11);
    const jint a = ellipse ? ellipse_a : 11;
    for (jint x = -a; x < a; ++x) {
        for (jint z = -a; z < a; ++z) {
            for (jint y = 0; y < over_water; ++y) {
                const jint radius =
                    ellipse ? height_radius_ellipse(y, over_water, width)
                            : height_radius_round(random, y, over_water, width);
                if (ellipse || x < radius) {
                    generate_block(context, random, origin, over_water, x, y, z,
                                   radius, a, ellipse, ellipse_c, angle,
                                   snow_on_top);
                }
            }
        }
    }
    smooth(context, origin, width, over_water, ellipse, ellipse_a);
    for (jint x = -a; x < a; ++x) {
        for (jint z = -a; z < a; ++z) {
            for (jint y = -1; y > -under_water; --y) {
                const jint new_a =
                    ellipse ? static_cast<jint>(std::ceil(
                                  static_cast<double>(a) *
                                  (1.0 - static_cast<double>(y * y) /
                                             (static_cast<double>(under_water) *
                                              8.0))))
                            : a;
                const jint radius =
                    height_radius_steep(random, -y, under_water, width);
                if (x < radius) {
                    generate_block(context, random, origin, under_water, x, y,
                                   z, radius, new_a, ellipse, ellipse_c, angle,
                                   snow_on_top);
                }
            }
        }
    }
    const bool cutout =
        ellipse ? random.next_double() > 0.1 : random.next_double() > 0.7;
    if (cutout) {
        generate_cutout(context, random, width, over_water, origin, ellipse,
                        ellipse_a, angle, ellipse_c);
    }
    return true;
}

auto geode_feature::can_replace(feature_context &context,
                                const block_pos &pos) const -> bool {
    if (parameters_.cannot_replace_tag.empty()) {
        return true;
    }
    return !context.placement.states->in_tag(
        context.placement.region->state_at(pos.x, pos.y, pos.z),
        parameters_.cannot_replace_tag);
}

void geode_feature::safe_set(feature_context &context, const block_pos &pos,
                             block_state_id state) const {
    if (can_replace(context, pos)) {
        (void)context.placement.region->set_block_state(pos.x, pos.y, pos.z,
                                                        state);
    }
}

auto geode_feature::place(feature_context &context, worldgen_random &random,
                          const block_pos &origin) const -> bool {
    std::vector<std::pair<block_pos, jint>> points;
    const jint num_points = parameters_.distribution_min +
                            random.next_int(parameters_.distribution_max -
                                            parameters_.distribution_min + 1);
    normal_noise noise_parameters =
        normal_noise::create_parity(-4, std::array<double, 1>{1.0});
    legacy_random_source noise_random{context.placement.generation.world_seed};
    const auto noise = noise_parameters.create(noise_random);
    std::vector<block_pos> crack_points;
    const double crack_adjustment =
        static_cast<double>(num_points) /
        static_cast<double>(parameters_.outer_wall_max);
    const double inner_air = 1.0 / std::sqrt(parameters_.filling_threshold);
    const double innermost_layer =
        1.0 / std::sqrt(parameters_.inner_threshold + crack_adjustment);
    const double inner_crust =
        1.0 / std::sqrt(parameters_.middle_threshold + crack_adjustment);
    const double outer_crust =
        1.0 / std::sqrt(parameters_.outer_threshold + crack_adjustment);
    const double crack_size =
        1.0 /
        std::sqrt(parameters_.base_crack_size + random.next_double() / 2.0 +
                  (num_points > 3 ? crack_adjustment : 0.0));
    const bool should_crack =
        random.next_float() < static_cast<float>(parameters_.crack_chance);
    jint invalid = 0;
    const auto air = context.placement.states->air();
    for (jint i = 0; i < num_points; ++i) {
        const jint x =
            parameters_.outer_wall_provider
                ? parameters_.outer_wall_provider->sample(random)
                : parameters_.outer_wall_min +
                      random.next_int(parameters_.outer_wall_max -
                                      parameters_.outer_wall_min + 1);
        const jint y =
            parameters_.outer_wall_provider
                ? parameters_.outer_wall_provider->sample(random)
                : parameters_.outer_wall_min +
                      random.next_int(parameters_.outer_wall_max -
                                      parameters_.outer_wall_min + 1);
        const jint z =
            parameters_.outer_wall_provider
                ? parameters_.outer_wall_provider->sample(random)
                : parameters_.outer_wall_min +
                      random.next_int(parameters_.outer_wall_max -
                                      parameters_.outer_wall_min + 1);
        const block_pos pos{
            .x = origin.x + x, .y = origin.y + y, .z = origin.z + z};
        const auto state =
            context.placement.region->state_at(pos.x, pos.y, pos.z);
        if (state == air || context.placement.states->in_tag(
                                state, parameters_.invalid_blocks_tag)) {
            if (++invalid > parameters_.invalid_blocks_threshold) {
                return false;
            }
        }
        points.emplace_back(
            pos, parameters_.point_offset_provider
                     ? parameters_.point_offset_provider->sample(random)
                     : parameters_.point_offset_min +
                           random.next_int(parameters_.point_offset_max -
                                           parameters_.point_offset_min + 1));
    }
    if (should_crack) {
        const jint index = random.next_int(4);
        const jint offset = num_points * 2 + 1;
        if (index == 0) {
            crack_points.push_back({origin.x + offset, origin.y + 7, origin.z});
            crack_points.push_back({origin.x + offset, origin.y + 5, origin.z});
            crack_points.push_back({origin.x + offset, origin.y + 1, origin.z});
        } else if (index == 1) {
            crack_points.push_back({origin.x, origin.y + 7, origin.z + offset});
            crack_points.push_back({origin.x, origin.y + 5, origin.z + offset});
            crack_points.push_back({origin.x, origin.y + 1, origin.z + offset});
        } else if (index == 2) {
            crack_points.push_back(
                {origin.x + offset, origin.y + 7, origin.z + offset});
            crack_points.push_back(
                {origin.x + offset, origin.y + 5, origin.z + offset});
            crack_points.push_back(
                {origin.x + offset, origin.y + 1, origin.z + offset});
        } else {
            crack_points.push_back({origin.x, origin.y + 7, origin.z});
            crack_points.push_back({origin.x, origin.y + 5, origin.z});
            crack_points.push_back({origin.x, origin.y + 1, origin.z});
        }
    }
    std::vector<block_pos> potential;
    const auto distance_sqr = [](const block_pos &a, const block_pos &b) {
        const double dx = static_cast<double>(a.x - b.x);
        const double dy = static_cast<double>(a.y - b.y);
        const double dz = static_cast<double>(a.z - b.z);
        return dx * dx + dy * dy + dz * dz;
    };
    for (jint z = origin.z + parameters_.min_gen_offset;
         z <= origin.z + parameters_.max_gen_offset; ++z) {
        for (jint y = origin.y + parameters_.min_gen_offset;
             y <= origin.y + parameters_.max_gen_offset; ++y) {
            for (jint x = origin.x + parameters_.min_gen_offset;
                 x <= origin.x + parameters_.max_gen_offset; ++x) {
                const block_pos pos{.x = x, .y = y, .z = z};
                const double noise_offset =
                    static_cast<double>(noise.get(static_cast<double>(x),
                                                  static_cast<double>(y),
                                                  static_cast<double>(z))) *
                    parameters_.noise_multiplier;
                double shell = 0.0;
                for (const auto &[point, offset] : points) {
                    shell += 1.0 / std::sqrt(distance_sqr(pos, point) +
                                             static_cast<double>(offset)) +
                             noise_offset;
                }
                if (shell < outer_crust) {
                    continue;
                }
                if (shell >= inner_air) {
                    const auto state = parameters_.filling->get(
                        context.placement, random, pos);
                    safe_set(context, pos, state);
                    continue;
                }
                double crack = 0.0;
                for (const auto &point : crack_points) {
                    crack +=
                        1.0 / std::sqrt(distance_sqr(pos, point) +
                                        static_cast<double>(
                                            parameters_.crack_point_offset)) +
                        noise_offset;
                }
                if (should_crack && crack >= crack_size) {
                    safe_set(context, pos, air);
                    for (const auto &direction : all_directions) {
                        const auto neighbor =
                            context.placement.region->state_at(
                                pos.x + direction.dx, pos.y + direction.dy,
                                pos.z + direction.dz);
                        (void)neighbor;
                    }
                    continue;
                }
                if (shell >= innermost_layer) {
                    const bool alternate =
                        random.next_float() <
                        static_cast<float>(
                            parameters_.use_alternate_layer0_chance);
                    const auto state =
                        alternate ? parameters_.alternate_inner->get(
                                        context.placement, random, pos)
                                  : parameters_.inner->get(context.placement,
                                                           random, pos);
                    safe_set(context, pos, state);
                    if ((!parameters_.placements_require_layer0_alternate ||
                         alternate) &&
                        random.next_float() <
                            static_cast<float>(
                                parameters_.use_potential_placements_chance)) {
                        potential.push_back(pos);
                    }
                } else if (shell >= inner_crust) {
                    safe_set(context, pos,
                             parameters_.middle->get(context.placement, random,
                                                     pos));
                } else if (shell >= outer_crust) {
                    safe_set(
                        context, pos,
                        parameters_.outer->get(context.placement, random, pos));
                }
            }
        }
    }
    for (const auto &crystal : potential) {
        if (parameters_.inner_placements.empty()) {
            continue;
        }
        const auto base =
            parameters_
                .inner_placements[static_cast<std::size_t>(random.next_int(
                    static_cast<jint>(parameters_.inner_placements.size())))];
        auto state = base;
        bool placed = false;
        for (const auto &direction : all_directions) {
            auto key = context.placement.states->name(state);
            if (state_property(key, "facing").has_value()) {
                std::string_view name = "north";
                if (direction.dy < 0) {
                    name = "down";
                } else if (direction.dy > 0) {
                    name = "up";
                } else if (direction.dz < 0) {
                    name = "north";
                } else if (direction.dz > 0) {
                    name = "south";
                } else if (direction.dx < 0) {
                    name = "west";
                } else {
                    name = "east";
                }
                key = with_state_property(key, "facing", name);
            }
            if (state_property(key, "waterlogged").has_value()) {
                const auto below = context.placement.region->state_at(
                    crystal.x + direction.dx, crystal.y + direction.dy,
                    crystal.z + direction.dz);
                key = with_state_property(
                    key, "waterlogged",
                    context.placement.states->traits(below).water ? "true"
                                                                  : "false");
            }
            if (context.placement.resolve_state) {
                state = context.placement.resolve_state(key);
            }
            const auto place = block_pos{.x = crystal.x + direction.dx,
                                         .y = crystal.y + direction.dy,
                                         .z = crystal.z + direction.dz};
            const auto place_state =
                context.placement.region->state_at(place.x, place.y, place.z);
            if (context.placement.states->traits(place_state).air ||
                context.placement.states->traits(place_state).water) {
                safe_set(context, place, state);
                placed = true;
                break;
            }
        }
        (void)placed;
    }
    return true;
}

auto freeze_top_layer_feature::place(feature_context &context,
                                     worldgen_random &random,
                                     const block_pos &origin) const -> bool {
    (void)random;
    for (jint dx = 0; dx < 16; ++dx) {
        for (jint dz = 0; dz < 16; ++dz) {
            const jint x = origin.x + dx;
            const jint z = origin.z + dz;
            jint y = 0;
            if (context.placement.height) {
                y = context.placement.height(heightmap_kind::motion_blocking, x,
                                             z);
            } else {
                const auto *chunk = context.placement.chunk;
                const jint local_x = x - chunk->chunk_x() * 16;
                const jint local_z = z - chunk->chunk_z() * 16;
                if (local_x < 0 || local_x > 15 || local_z < 0 ||
                    local_z > 15) {
                    continue;
                }
                y = chunk->first_available(heightmap_kind::motion_blocking,
                                           local_x, local_z);
            }
            const block_pos top{.x = x, .y = y, .z = z};
            const block_pos below{.x = x, .y = y - 1, .z = z};
            if (context.placement.should_freeze &&
                context.placement.should_freeze(below)) {
                (void)context.placement.region->set_block_state(
                    below.x, below.y, below.z, ice_);
            }
            if (!context.placement.should_snow ||
                !context.placement.should_snow(top)) {
                continue;
            }
            (void)context.placement.region->set_block_state(top.x, top.y, top.z,
                                                            snow_);
            const auto below_state =
                context.placement.region->state_at(below.x, below.y, below.z);
            auto key = context.placement.states->name(below_state);
            if (state_property(key, "snowy").has_value() &&
                context.placement.resolve_state) {
                const auto variant = with_state_property(key, "snowy", "true");
                const auto resolved = context.placement.resolve_state(variant);
                if (resolved != no_block_state) {
                    (void)context.placement.region->set_block_state(
                        below.x, below.y, below.z, resolved);
                }
            }
        }
    }
    return true;
}

auto lake_feature::place(feature_context &context, worldgen_random &random,
                         const block_pos &initial) const -> bool {
    if (initial.y <= context.placement.chunk->min_y() + 4) {
        return false;
    }
    const block_pos origin{
        .x = initial.x - 8, .y = initial.y - 4, .z = initial.z - 8};
    std::array<bool, 2048> grid{};
    const jint spots = random.next_int(4) + 4;
    for (jint spot = 0; spot < spots; ++spot) {
        const double xr = random.next_double() * 6.0 + 3.0;
        const double yr = random.next_double() * 4.0 + 2.0;
        const double zr = random.next_double() * 6.0 + 3.0;
        const double xp =
            random.next_double() * (16.0 - xr - 2.0) + 1.0 + xr / 2.0;
        const double yp =
            random.next_double() * (8.0 - yr - 4.0) + 2.0 + yr / 2.0;
        const double zp =
            random.next_double() * (16.0 - zr - 2.0) + 1.0 + zr / 2.0;
        for (jint xx = 1; xx < 15; ++xx) {
            for (jint zz = 1; zz < 15; ++zz) {
                for (jint yy = 1; yy < 7; ++yy) {
                    const double xd = (xx - xp) / (xr / 2.0);
                    const double yd = (yy - yp) / (yr / 2.0);
                    const double zd = (zz - zp) / (zr / 2.0);
                    if (xd * xd + yd * yd + zd * zd < 1.0) {
                        grid[static_cast<std::size_t>((xx * 16 + zz) * 8 +
                                                      yy)] = true;
                    }
                }
            }
        }
    }

    const auto fluid = fluid_->get(context.placement, random, origin);
    const auto &states = *context.placement.states;
    auto &region = *context.placement.region;
    const auto boundary = [&grid](jint xx, jint zz, jint yy) {
        const auto at = [&grid](jint x, jint z, jint y) {
            return grid[static_cast<std::size_t>((x * 16 + z) * 8 + y)];
        };
        return !at(xx, zz, yy) && ((xx < 15 && at(xx + 1, zz, yy)) ||
                                   (xx > 0 && at(xx - 1, zz, yy)) ||
                                   (zz < 15 && at(xx, zz + 1, yy)) ||
                                   (zz > 0 && at(xx, zz - 1, yy)) ||
                                   (yy < 7 && at(xx, zz, yy + 1)) ||
                                   (yy > 0 && at(xx, zz, yy - 1)));
    };

    for (jint xx = 0; xx < 16; ++xx) {
        for (jint zz = 0; zz < 16; ++zz) {
            for (jint yy = 0; yy < 8; ++yy) {
                if (!boundary(xx, zz, yy)) {
                    continue;
                }
                const block_pos pos{
                    .x = origin.x + xx, .y = origin.y + yy, .z = origin.z + zz};
                const auto state = region.state_at(pos.x, pos.y, pos.z);
                const auto traits = states.traits(state);
                if (yy >= 4 && traits.fluid) {
                    return false;
                }
                if (yy < 4 && !traits.blocks_motion && state != fluid) {
                    return false;
                }
                if (!can_place_feature_->test(region, states, pos)) {
                    return false;
                }
            }
        }
    }

    for (jint xx = 0; xx < 16; ++xx) {
        for (jint zz = 0; zz < 16; ++zz) {
            for (jint yy = 0; yy < 8; ++yy) {
                if (!grid[static_cast<std::size_t>((xx * 16 + zz) * 8 + yy)]) {
                    continue;
                }
                const block_pos pos{
                    .x = origin.x + xx, .y = origin.y + yy, .z = origin.z + zz};
                if (!can_replace_with_air_or_fluid_->test(region, states,
                                                          pos)) {
                    continue;
                }
                const bool place_air = yy >= 4;
                (void)region.set_block_state(pos.x, pos.y, pos.z,
                                             place_air ? cave_air_ : fluid);
                if (place_air) {
                    mark_above_for_post_processing(context, pos);
                }
            }
        }
    }

    const auto barrier = barrier_->get(context.placement, random, origin);
    if (!states.traits(barrier).air) {
        for (jint xx = 0; xx < 16; ++xx) {
            for (jint zz = 0; zz < 16; ++zz) {
                for (jint yy = 0; yy < 8; ++yy) {
                    if (!boundary(xx, zz, yy)) {
                        continue;
                    }
                    if (yy >= 4 && random.next_int(2) == 0) {
                        continue;
                    }
                    const block_pos pos{.x = origin.x + xx,
                                        .y = origin.y + yy,
                                        .z = origin.z + zz};
                    if (!states.traits(region.state_at(pos.x, pos.y, pos.z))
                             .blocks_motion) {
                        continue;
                    }
                    if (!can_replace_with_barrier_->test(region, states, pos)) {
                        continue;
                    }
                    (void)region.set_block_state(pos.x, pos.y, pos.z, barrier);
                    mark_above_for_post_processing(context, pos);
                }
            }
        }
    }

    if (states.traits(fluid).water) {
        for (jint xx = 0; xx < 16; ++xx) {
            for (jint zz = 0; zz < 16; ++zz) {
                const block_pos pos{
                    .x = origin.x + xx, .y = origin.y + 4, .z = origin.z + zz};
                if (context.placement.should_freeze &&
                    context.placement.should_freeze(pos) &&
                    can_replace_with_air_or_fluid_->test(region, states, pos)) {
                    (void)region.set_block_state(pos.x, pos.y, pos.z, ice_);
                }
            }
        }
    }
    return true;
}

auto delta_feature::is_clear(feature_context &context,
                             const block_pos &pos) const -> bool {
    const auto state = context.placement.region->state_at(pos.x, pos.y, pos.z);
    const auto name = block_name(context, state);
    if (name == block_name(context, contents_) || name == "minecraft:bedrock" ||
        name == "minecraft:nether_bricks" ||
        name == "minecraft:nether_brick_fence" ||
        name == "minecraft:nether_brick_stairs" ||
        name == "minecraft:nether_wart" || name == "minecraft:chest" ||
        name == "minecraft:spawner") {
        return false;
    }
    for (const auto &direction : all_directions) {
        const bool air = context.placement.states
                             ->traits(context.placement.region->state_at(
                                 pos.x + direction.dx, pos.y + direction.dy,
                                 pos.z + direction.dz))
                             .air;
        if ((air && direction.dy != 1) || (!air && direction.dy == 1)) {
            return false;
        }
    }
    return true;
}

void delta_feature::write(feature_context &context, const block_pos &pos,
                          block_state_id state) const {
    (void)context.placement.region->set_block_state(pos.x, pos.y, pos.z, state);
}

auto delta_feature::place(feature_context &context, worldgen_random &random,
                          const block_pos &origin) const -> bool {
    bool any = false;
    const bool spawn_rim = random.next_double() < 0.9;
    const jint rim_x = spawn_rim ? rim_size_->sample(random) : 0;
    const jint rim_z = spawn_rim ? rim_size_->sample(random) : 0;
    const bool has_rim = spawn_rim && rim_x != 0 && rim_z != 0;
    const jint radius_x = size_->sample(random);
    const jint radius_z = size_->sample(random);
    const jint radius_limit = std::max(radius_x, radius_z);
    for (jint depth = 0; depth <= radius_limit; ++depth) {
        const jint max_x = std::min(radius_x, depth);
        for (jint x = -max_x; x <= max_x; ++x) {
            const jint z = depth - std::abs(x);
            if (z < 0 || z > radius_z) {
                continue;
            }
            const std::array<jint, 2> z_values{z, -z};
            for (const jint dz : z_values) {
                if (dz != z && z == 0) {
                    continue;
                }
                const block_pos pos{
                    .x = origin.x + x, .y = origin.y, .z = origin.z + dz};
                if (std::abs(x) + std::abs(dz) > radius_limit) {
                    return any;
                }
                if (!is_clear(context, pos)) {
                    continue;
                }
                if (has_rim) {
                    any = true;
                    write(context, pos, rim_);
                }
                const block_pos offset{
                    .x = pos.x + rim_x, .y = pos.y, .z = pos.z + rim_z};
                if (is_clear(context, offset)) {
                    any = true;
                    write(context, offset, contents_);
                }
            }
        }
    }
    return any;
}

auto large_dripstone_feature::empty_or_water(feature_context &context,
                                             const block_pos &pos) const
    -> bool {
    const auto traits = context.placement.states->traits(
        context.placement.region->state_at(pos.x, pos.y, pos.z));
    return traits.air || traits.water;
}

auto large_dripstone_feature::empty_or_water_or_lava(feature_context &context,
                                                     const block_pos &pos) const
    -> bool {
    const auto traits = context.placement.states->traits(
        context.placement.region->state_at(pos.x, pos.y, pos.z));
    return traits.air || traits.water || (traits.fluid && !traits.water);
}

auto large_dripstone_feature::is_base_or_lava(feature_context &context,
                                              const block_pos &pos) const
    -> bool {
    const auto state = context.placement.region->state_at(pos.x, pos.y, pos.z);
    const auto name = block_name(context, state);
    return name == "minecraft:dripstone_block" ||
           context.placement.states->in_tag(state, replaceable_tag_) ||
           name == "minecraft:lava";
}

auto large_dripstone_feature::scan(feature_context &context,
                                   const block_pos &origin) const
    -> std::optional<column_range> {
    if (!empty_or_water(context, origin)) {
        return std::nullopt;
    }
    const auto scan_direction = [&](jint direction) -> std::optional<jint> {
        jint y = origin.y;
        for (jint i = 1; i < floor_to_ceiling_range_; ++i) {
            if (!empty_or_water(
                    context, block_pos{.x = origin.x, .y = y, .z = origin.z})) {
                break;
            }
            y += direction;
        }
        if (is_base_or_lava(context,
                            block_pos{.x = origin.x, .y = y, .z = origin.z})) {
            return y;
        }
        return std::nullopt;
    };
    const auto ceiling = scan_direction(1);
    const auto floor = scan_direction(-1);
    if (!ceiling || !floor) {
        return std::nullopt;
    }
    return column_range{.floor = *floor, .ceiling = *ceiling};
}

auto large_dripstone_feature::height(const dripstone &stone) const -> jint {
    return height_at(stone, 0.0F);
}

auto large_dripstone_feature::height_at(const dripstone &stone,
                                        float radius) const -> jint {
    double x = static_cast<double>(radius);
    if (x < stone.bluntness) {
        x = stone.bluntness;
    }
    const double cutoff = 0.384;
    const double r = x / static_cast<double>(stone.radius) * 0.384;
    const double part1 = 0.75 * std::pow(r, 1.3333333333333333);
    const double part2 = std::pow(r, 0.6666666666666666);
    const double part3 = 0.3333333333333333 * std::log(r);
    double relative = stone.scale * (part1 - part2 - part3);
    relative = std::max(relative, 0.0);
    return static_cast<jint>(relative / cutoff *
                             static_cast<double>(stone.radius));
}

auto large_dripstone_feature::embedded(feature_context &context,
                                       const block_pos &center,
                                       jint radius) const -> bool {
    if (empty_or_water_or_lava(context, center)) {
        return false;
    }
    const float increment = 6.0F / static_cast<float>(radius);
    for (float angle = 0.0F; angle < static_cast<float>(pi * 2.0);
         angle += increment) {
        const auto dx =
            static_cast<jint>(mth::cos(angle) * static_cast<float>(radius));
        const auto dz =
            static_cast<jint>(mth::sin(angle) * static_cast<float>(radius));
        if (empty_or_water_or_lava(context, block_pos{.x = center.x + dx,
                                                      .y = center.y,
                                                      .z = center.z + dz})) {
            return false;
        }
    }
    return true;
}

auto large_dripstone_feature::move_back(feature_context &context,
                                        dripstone &stone,
                                        const wind_offsetter &wind) const
    -> bool {
    while (stone.radius > 1) {
        block_pos root = stone.root;
        const jint tries = std::min(10, height(stone));
        for (jint i = 0; i < tries; ++i) {
            if (block_name(context, context.placement.region->state_at(
                                        root.x, root.y, root.z)) ==
                "minecraft:lava") {
                return false;
            }
            if (embedded(context, wind.offset(root), stone.radius)) {
                stone.root = root;
                return true;
            }
            root.y += stone.pointing_up ? -1 : 1;
        }
        stone.radius /= 2;
    }
    return false;
}

auto large_dripstone_feature::wind_offsetter::offset(const block_pos &pos) const
    -> block_pos {
    if (!active) {
        return pos;
    }
    const jint dy = origin_y - pos.y;
    const auto dx = static_cast<jint>(std::floor(x * static_cast<double>(dy)));
    const auto dz = static_cast<jint>(std::floor(z * static_cast<double>(dy)));
    return {.x = pos.x + std::clamp(dx, -max_offset, max_offset),
            .y = pos.y,
            .z = pos.z + std::clamp(dz, -max_offset, max_offset)};
}

void large_dripstone_feature::place_blocks(feature_context &context,
                                           worldgen_random &random,
                                           const dripstone &stone,
                                           const wind_offsetter &wind) const {
    for (jint dx = -stone.radius; dx <= stone.radius; ++dx) {
        for (jint dz = -stone.radius; dz <= stone.radius; ++dz) {
            const float current =
                mth::sqrt(static_cast<float>(dx * dx + dz * dz));
            if (current > static_cast<float>(stone.radius)) {
                continue;
            }
            jint column_height = height_at(stone, current);
            if (column_height <= 0) {
                continue;
            }
            if (random.next_float() < 0.2F) {
                const float factor = 0.8F + random.next_float() * (1.0F - 0.8F);
                column_height = static_cast<jint>(
                    static_cast<float>(column_height) * factor);
            }
            block_pos pos{.x = stone.root.x + dx,
                          .y = stone.root.y,
                          .z = stone.root.z + dz};
            bool out_of_stone = false;
            const jint max_y =
                stone.pointing_up
                    ? (context.placement.height
                           ? context.placement.height(
                                 heightmap_kind::world_surface_wg, pos.x, pos.z)
                           : context.placement.chunk->min_y() +
                                 context.placement.chunk->height())
                    : std::numeric_limits<jint>::max();
            for (jint i = 0; i < column_height && pos.y < max_y; ++i) {
                const auto adjusted = wind.offset(pos);
                if (empty_or_water_or_lava(context, adjusted)) {
                    out_of_stone = true;
                    const auto state = context.placement.resolve_state
                                           ? context.placement.resolve_state(
                                                 "minecraft:dripstone_block")
                                           : no_block_state;
                    (void)context.placement.region->set_block_state(
                        adjusted.x, adjusted.y, adjusted.z, state);
                } else if (out_of_stone &&
                           context.placement.states->in_tag(
                               context.placement.region->state_at(
                                   adjusted.x, adjusted.y, adjusted.z),
                               "minecraft:base_stone_overworld")) {
                    break;
                }
                pos.y += stone.pointing_up ? 1 : -1;
            }
        }
    }
}

auto large_dripstone_feature::place(feature_context &context,
                                    worldgen_random &random,
                                    const block_pos &origin) const -> bool {
    if (!empty_or_water(context, origin)) {
        return false;
    }
    const auto column = scan(context, origin);
    if (!column || column->ceiling - column->floor - 1 < 4) {
        return false;
    }
    const jint column_height = column->ceiling - column->floor - 1;
    const jint max_radius =
        std::clamp(static_cast<jint>(static_cast<float>(column_height) *
                                     radius_to_height_ratio_),
                   radius_min_, radius_max_);
    const jint radius =
        radius_min_ + random.next_int(max_radius - radius_min_ + 1);
    auto make = [&](const block_pos &root, bool up,
                    const provider::float_provider &bluntness) {
        return dripstone{
            .root = root,
            .pointing_up = up,
            .radius = radius,
            .bluntness = static_cast<double>(bluntness.sample(random)),
            .scale = static_cast<double>(height_scale_->sample(random)),
        };
    };
    dripstone stalactite =
        make(block_pos{.x = origin.x, .y = column->ceiling - 1, .z = origin.z},
             false, *stalactite_bluntness_);
    dripstone stalagmite =
        make(block_pos{.x = origin.x, .y = column->floor + 1, .z = origin.z},
             true, *stalagmite_bluntness_);
    wind_offsetter wind;
    if (radius >= min_radius_for_wind_ &&
        stalactite.bluntness >= static_cast<double>(min_bluntness_for_wind_) &&
        stalagmite.bluntness >= static_cast<double>(min_bluntness_for_wind_)) {
        wind.active = true;
        wind.origin_y = origin.y;
        wind.max_offset = 16 - radius;
        const float speed = wind_speed_->sample(random);
        const float direction = random.next_float() * static_cast<float>(pi);
        wind.x = static_cast<double>(mth::cos(direction)) *
                 static_cast<double>(speed);
        wind.z = static_cast<double>(mth::sin(direction)) *
                 static_cast<double>(speed);
    }
    const bool stalactite_ok = move_back(context, stalactite, wind);
    const bool stalagmite_ok = move_back(context, stalagmite, wind);
    if (stalactite_ok) {
        place_blocks(context, random, stalactite, wind);
    }
    if (stalagmite_ok) {
        place_blocks(context, random, stalagmite, wind);
    }
    return true;
}

auto sequence_feature::place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool {
    for (const auto &id : features_) {
        if (!context.place_by_id(id, random, origin)) {
            return false;
        }
    }
    return true;
}

auto replace_blobs_feature::place(feature_context &context,
                                  worldgen_random &random,
                                  const block_pos &origin) const -> bool {
    const jint max_y = context.placement.chunk->min_y() +
                       context.placement.chunk->height() - 1;
    block_pos cursor{
        .x = origin.x,
        .y = std::clamp(origin.y, context.placement.chunk->min_y() + 1, max_y),
        .z = origin.z};
    const auto target_name =
        state_block_name(context.placement.states->name(target_));
    while (cursor.y > context.placement.chunk->min_y() + 1) {
        const auto state =
            context.placement.region->state_at(cursor.x, cursor.y, cursor.z);
        if (state_block_name(context.placement.states->name(state)) ==
            target_name) {
            break;
        }
        --cursor.y;
    }
    if (cursor.y <= context.placement.chunk->min_y() + 1) {
        return false;
    }
    const jint radius_x = radius_->sample(random);
    const jint radius_y = radius_->sample(random);
    const jint radius_z = radius_->sample(random);
    const jint maximum = std::max(radius_x, std::max(radius_y, radius_z));
    bool replaced = false;
    for (jint y = cursor.y - radius_y; y <= cursor.y + radius_y; ++y) {
        for (jint z = cursor.z - radius_z; z <= cursor.z + radius_z; ++z) {
            for (jint x = cursor.x - radius_x; x <= cursor.x + radius_x; ++x) {
                if (std::abs(x - cursor.x) + std::abs(y - cursor.y) +
                        std::abs(z - cursor.z) >
                    maximum) {
                    continue;
                }
                const auto state = context.placement.region->state_at(x, y, z);
                if (state_block_name(context.placement.states->name(state)) !=
                    target_name) {
                    continue;
                }
                if (context.placement.region->set_block_state(x, y, z,
                                                              replacement_)) {
                    replaced = true;
                }
            }
        }
    }
    return replaced;
}

auto blue_ice_feature::place(feature_context &context, worldgen_random &random,
                             const block_pos &origin) const -> bool {
    if (origin.y > context.placement.generation.sea_level - 1) {
        return false;
    }
    const auto is = [&](jint x, jint y, jint z, block_state_id state) {
        return context.placement.region->state_at(x, y, z) == state;
    };
    if (!is(origin.x, origin.y, origin.z, water_) &&
        !is(origin.x, origin.y - 1, origin.z, water_)) {
        return false;
    }
    bool packed = false;
    for (const auto &direction : all_directions) {
        if (direction.dy == -1) {
            continue;
        }
        if (is(origin.x + direction.dx, origin.y + direction.dy,
               origin.z + direction.dz, packed_ice_)) {
            packed = true;
            break;
        }
    }
    if (!packed) {
        return false;
    }
    (void)context.placement.region->set_block_state(origin.x, origin.y,
                                                    origin.z, blue_ice_);
    for (jint i = 0; i < 200; ++i) {
        jint y_offset = random.next_int(5) - random.next_int(6);
        jint xz = 3;
        if (y_offset < 2) {
            xz += y_offset / 2;
        }
        if (xz < 1) {
            continue;
        }
        const block_pos place{
            .x = origin.x + random.next_int(xz) - random.next_int(xz),
            .y = origin.y + y_offset,
            .z = origin.z + random.next_int(xz) - random.next_int(xz),
        };
        const auto state =
            context.placement.region->state_at(place.x, place.y, place.z);
        if (state != air_ && state != water_ && state != packed_ice_ &&
            state != ice_) {
            continue;
        }
        for (const auto &direction : all_directions) {
            if (is(place.x + direction.dx, place.y + direction.dy,
                   place.z + direction.dz, blue_ice_)) {
                (void)context.placement.region->set_block_state(
                    place.x, place.y, place.z, blue_ice_);
                break;
            }
        }
    }
    return true;
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

auto block_pile_feature::may_place_on(feature_context &context,
                                      const block_pos &pos,
                                      worldgen_random &random) const -> bool {
    const block_pos below{.x = pos.x, .y = pos.y - 1, .z = pos.z};
    const auto state =
        context.placement.region->state_at(below.x, below.y, below.z);
    if (context.placement.states->name(state) == "minecraft:dirt_path") {
        return random.next_boolean();
    }
    return context.placement.states->traits(state).blocks_motion;
}

void block_pile_feature::try_place(feature_context &context,
                                   const block_pos &pos,
                                   worldgen_random &random) const {
    const auto name = context.placement.states->name(
        context.placement.region->state_at(pos.x, pos.y, pos.z));
    if (!is_air_block_name(name) || !may_place_on(context, pos, random)) {
        return;
    }
    const auto state = provider_->get(context.placement, random, pos);
    (void)context.placement.region->set_block_state(pos.x, pos.y, pos.z, state);
}

auto block_pile_feature::place(feature_context &context,
                               worldgen_random &random,
                               const block_pos &origin) const -> bool {
    if (origin.y < context.placement.chunk->min_y() + 5) {
        return false;
    }
    const jint xr = 2 + random.next_int(2);
    const jint zr = 2 + random.next_int(2);
    for (jint z = origin.z - zr; z <= origin.z + zr; ++z) {
        const jint zd = origin.z - z;
        for (jint y = origin.y; y <= origin.y + 1; ++y) {
            for (jint x = origin.x - xr; x <= origin.x + xr; ++x) {
                const jint xd = origin.x - x;
                const float first = random.next_float();
                const float second = random.next_float();
                if (xd * xd + zd * zd <= first * 10.0F - second * 6.0F) {
                    try_place(context, block_pos{.x = x, .y = y, .z = z},
                              random);
                } else if (random.next_float() < 0.031F) {
                    try_place(context, block_pos{.x = x, .y = y, .z = z},
                              random);
                }
            }
        }
    }
    return true;
}

auto overlay_feature::place(feature_context &context, worldgen_random &random,
                            const block_pos &origin) const -> bool {
    bool placed = false;
    for (const auto &id : features_) {
        placed = context.place_by_id(id, random, origin) || placed;
    }
    return placed;
}

} // namespace refrain::worldgen::feature
