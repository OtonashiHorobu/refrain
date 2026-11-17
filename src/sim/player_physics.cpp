// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/sim/player_physics.hpp>

#include <refrain/sim/world.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>

namespace refrain::sim {
namespace {

constexpr float max_health_points = 20.0F;

[[nodiscard]] auto is_survival(const player_state &player) noexcept -> bool {
    return player.game_mode == game_mode_survival ||
           player.game_mode == game_mode_adventure;
}

[[nodiscard]] auto is_creative(const player_state &player) noexcept -> bool {
    return player.game_mode == game_mode_creative;
}

[[nodiscard]] auto is_spectator(const player_state &player) noexcept -> bool {
    return player.game_mode == game_mode_spectator;
}

[[nodiscard]] auto horizontal_distance(double dx, double dz) noexcept
    -> double {
    return std::sqrt(dx * dx + dz * dz);
}

/// Block state at the player's feet plus `offset_y`, or air outside the
/// loaded columns.
[[nodiscard]] auto block_at_height(const world &level,
                                   const player_state &player,
                                   double offset_y) noexcept -> block_state_id {
    return level.block_at(
        block_pos{static_cast<std::int32_t>(std::floor(player.x)),
                  static_cast<std::int16_t>(std::floor(player.y + offset_y)),
                  static_cast<std::int32_t>(std::floor(player.z))});
}

[[nodiscard]] auto in_fluid(const world &level, const player_state &player,
                            world::fluid_kind kind) noexcept -> bool {
    for (const auto offset : {0.1, 0.9, player_eye_height}) {
        if (level.block_fluid(block_at_height(level, player, offset)) == kind) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] auto on_climbable(const world &level,
                                const player_state &player) noexcept -> bool {
    return level.block_climbable(block_at_height(level, player, 0.1)) ||
           level.block_climbable(block_at_height(level, player, 0.9));
}

/// Movement intent from the held keys, in world space. The vector matches the
/// vanilla input transform: strafe on X and forward on Z, rotated by the body
/// yaw, so a fixed input and yaw always produce the same displacement.
struct move_intent {
    double x{0.0};
    double z{0.0};
    double length{0.0};
};

[[nodiscard]] auto intent_of(const player_state &player) noexcept
    -> move_intent {
    const auto left{has_input(player.input, player_input::left)};
    const auto right{has_input(player.input, player_input::right)};
    const auto forward{has_input(player.input, player_input::forward)};
    const auto backward{has_input(player.input, player_input::backward)};
    auto strafe{0.0};
    auto advance{0.0};
    if (left != right) {
        strafe = left ? 1.0 : -1.0;
    }
    if (forward != backward) {
        advance = forward ? 1.0 : -1.0;
    }
    move_intent intent{};
    intent.length = std::sqrt(strafe * strafe + advance * advance);
    if (intent.length == 0.0) {
        return intent;
    }
    const auto radians{static_cast<double>(player.yaw) *
                       (std::numbers::pi / 180.0)};
    const auto sine{std::sin(radians)};
    const auto cosine{std::cos(radians)};
    intent.x = (strafe * cosine - advance * sine) / intent.length;
    intent.z = (advance * cosine + strafe * sine) / intent.length;
    return intent;
}

[[nodiscard]] auto sprinting(const player_state &player) noexcept -> bool {
    return has_input(player.input, player_input::sprint);
}

/// Apply one damage hit. The caller has already rejected immune and dead
/// records.
auto apply_damage(player_state &player, damage_source source, float amount,
                  std::uint64_t attacker) -> bool {
    if (amount <= 0.0F) {
        return false;
    }
    player.health = std::max(0.0F, player.health - amount);
    player.last_damage = source;
    player.last_attacker = attacker;
    player.exhaustion = std::min(player.exhaustion + 0.1F, 40.0F);
    if (player.health <= 0.0F) {
        player.health = 0.0F;
        player.dead = true;
        player.death_time = 0;
        player.fall_distance = 0.0;
        player.has_move_request = false;
        player.input = player_input::none;
        player.flying = false;
        // The experience drops as orbs in vanilla. The entity system that owns
        // item and orb entities does not exist yet, so the levels are lost here
        // and the runtime awards the kill experience to the attacker.
        // TODO: spawn experience orbs instead of discarding the points, and
        // drop the inventory as item entities, once entities exist.
        player.total_experience = 0;
        player.experience_level = 0;
        player.experience_progress = 0.0F;
    }
    return true;
}

/// One `FoodData.tick` equivalent.
void tick_food(world &level, player_state &player) noexcept {
    const auto difficulty{level.difficulty()};
    if (player.exhaustion > 4.0F) {
        player.exhaustion -= 4.0F;
        if (player.saturation > 0.0F) {
            player.saturation = std::max(player.saturation - 1.0F, 0.0F);
        } else if (difficulty != 0) {
            player.food_level = std::max(player.food_level - 1, 0);
        }
    }
    const auto hurt{player.health < max_health_points};
    if (player.saturation > 0.0F && hurt && player.food_level >= 20) {
        ++player.food_timer;
        if (player.food_timer >= 10) {
            const auto spent{std::min(player.saturation, 6.0F)};
            heal_player(player, spent / 6.0F);
            player.exhaustion = std::min(player.exhaustion + spent, 40.0F);
            player.food_timer = 0;
        }
    } else if (player.food_level >= 18 && hurt) {
        ++player.food_timer;
        if (player.food_timer >= 80) {
            heal_player(player, 1.0F);
            player.exhaustion = std::min(player.exhaustion + 6.0F, 40.0F);
            player.food_timer = 0;
        }
    } else if (player.food_level <= 0) {
        ++player.food_timer;
        if (player.food_timer >= 80) {
            const auto starves{difficulty == 3 ||
                               (difficulty == 2 && player.health > 1.0F) ||
                               (difficulty == 1 && player.health > 10.0F)};
            if (starves) {
                static_cast<void>(
                    damage_player(level, player, damage_source::starve, 1.0F));
            }
            player.food_timer = 0;
        }
    } else {
        player.food_timer = 0;
    }
}

/// Peaceful difficulty regenerates health, saturation and food on fixed
/// tick intervals, keyed off the record's age rather than wall time.
void tick_peaceful(player_state &player) noexcept {
    if (player.age % 20 == 0) {
        heal_player(player, 1.0F);
        if (player.saturation < 20.0F) {
            player.saturation += 1.0F;
        }
    }
    if (player.age % 10 == 0 && player.food_level < 20) {
        ++player.food_level;
    }
}

/// The grounded probe: the collision sweep reports `collided` for any contact,
/// so a tiny downward sweep answers whether a support is under the feet.
[[nodiscard]] auto grounded(const world &level,
                            const player_state &player) noexcept -> bool {
    return level.resolve_player_move(player, 0.0, -1.0e-4, 0.0).collided;
}

void apply_predicted_move(world &level, player_state &player) noexcept {
    const auto resolved{
        level.resolve_player_move(player, player.vx, player.vy, player.vz)};
    player.x = resolved.x;
    player.y = resolved.y;
    player.z = resolved.z;
}

} // namespace

auto experience_for_level(std::int32_t level) noexcept -> std::int32_t {
    if (level <= 0) {
        return 0;
    }
    if (level <= 16) {
        return level * level + 6 * level;
    }
    if (level <= 31) {
        return static_cast<std::int32_t>(2.5 * level * level - 40.5 * level +
                                         360.0);
    }
    return static_cast<std::int32_t>(4.5 * level * level - 162.5 * level +
                                     2220.0);
}

void add_experience(player_state &player, std::int32_t points) noexcept {
    if (points <= 0) {
        return;
    }
    player.total_experience = static_cast<std::int32_t>(std::min<std::int64_t>(
        static_cast<std::int64_t>(player.total_experience) + points,
        std::numeric_limits<std::int32_t>::max()));
    auto level{player.experience_level};
    while (level < 21863 &&
           player.total_experience >= experience_for_level(level + 1)) {
        ++level;
    }
    player.experience_level = level;
    const auto base{experience_for_level(level)};
    const auto next{experience_for_level(level + 1)};
    const auto span{next - base};
    player.experience_progress =
        span > 0
            ? std::clamp(static_cast<float>(player.total_experience - base) /
                             static_cast<float>(span),
                         0.0F, 1.0F)
            : 1.0F;
}

void heal_player(player_state &player, float amount) noexcept {
    if (player.dead || amount <= 0.0F) {
        return;
    }
    player.health = std::min(max_health_points, player.health + amount);
}

auto damage_player(world & /*level*/, player_state &player,
                   damage_source source, float amount, std::uint64_t attacker)
    -> bool {
    if (player.dead || amount <= 0.0F) {
        return false;
    }
    if (is_creative(player) || is_spectator(player)) {
        return false;
    }
    // TODO: armour, enchantment protection and the absorption pool do not
    // reduce damage yet; the equipment slots and attributes that carry them
    // are not modelled, so every hit lands at full strength.
    const auto bypasses_immunity{source == damage_source::out_of_world};
    if (!bypasses_immunity && player.invulnerable_time > 0) {
        if (amount <= player.last_hurt) {
            return false;
        }
        const auto extra{amount - player.last_hurt};
        player.last_hurt = amount;
        return apply_damage(player, source, extra, attacker);
    }
    if (!bypasses_immunity) {
        player.invulnerable_time = player_hurt_immunity;
        player.last_hurt = amount;
    }
    return apply_damage(player, source, amount, attacker);
}

void respawn_player(world & /*level*/, player_state &player, double x, double y,
                    double z, float yaw, float pitch) noexcept {
    player.x = x;
    player.y = y;
    player.z = z;
    player.vx = 0.0;
    player.vy = 0.0;
    player.vz = 0.0;
    player.yaw = yaw;
    player.pitch = pitch;
    player.health = max_health_points;
    player.food_level = 20;
    player.saturation = 5.0F;
    player.exhaustion = 0.0F;
    player.food_timer = 0;
    player.air_supply = player_max_air_supply;
    player.fall_distance = 0.0;
    player.invulnerable_time = 0;
    player.last_hurt = 0.0F;
    player.dead = false;
    player.death_time = 0;
    player.on_ground = false;
    player.ground_y = y;
    player.has_move_request = false;
    player.input = player_input::none;
    player.flying = false;
    player.last_damage = damage_source::generic;
    player.last_attacker = 0;
}

void set_player_game_mode(world & /*level*/, player_state &player,
                          std::uint32_t mode) noexcept {
    player.game_mode = std::min<std::uint32_t>(mode, 3U);
    if (player.game_mode == game_mode_spectator) {
        player.flying = true;
    } else if (player.game_mode != game_mode_creative) {
        player.flying = false;
    }
    if (player.game_mode == game_mode_survival ||
        player.game_mode == game_mode_adventure) {
        player.invulnerable_time = 0;
    }
}

void tick_player(world &level, player_state &player) noexcept {
    if (player.dead) {
        ++player.death_time;
        return;
    }
    ++player.age;
    if (player.invulnerable_time > 0) {
        --player.invulnerable_time;
    }
    if (is_spectator(player)) {
        // Spectators fly through everything: no collision, gravity or
        // survival effects. A validated client request is applied verbatim;
        // the record still tracks the position for tracking range and
        // respawn.
        if (player.has_move_request) {
            player.x = player.move_x;
            player.y = player.move_y;
            player.z = player.move_z;
        }
        player.has_move_request = false;
        player.on_ground = false;
        player.fall_distance = 0.0;
        player.air_supply = player_max_air_supply;
        player.ground_y = player.y;
        return;
    }

    const auto water{in_fluid(level, player, world::fluid_kind::water)};
    const auto lava{!water && in_fluid(level, player, world::fluid_kind::lava)};
    const auto climbing{on_climbable(level, player)};
    const auto flying{player.flying && is_creative(player)};
    const auto start_x{player.x};
    const auto start_y{player.y};
    const auto start_z{player.z};
    const auto was_on_ground{player.on_ground};
    const auto intent{intent_of(player)};
    const auto wants_jump{has_input(player.input, player_input::jump)};
    const auto wants_shift{has_input(player.input, player_input::shift)};
    const auto sprint{sprinting(player)};

    // A record owned by a network client is only moved by that client's
    // validated requests. The client stops reporting while it hovers or stands
    // still, and any server-side prediction would then walk the record away
    // from the body the client actually has: the next report would look like a
    // correction, its displacement would be read as a fall velocity, and the
    // record would dive away from the client until the distance gate rejects
    // it. The client owns the position; the server owns the verdict. Console
    // and replay records have no client and are simulated from their input.
    if (!player.client_driven) {
        if (flying) {
            const auto speed{player_fly_speed * (sprint ? 2.0 : 1.0)};
            player.vx += intent.x * speed;
            player.vz += intent.z * speed;
            if (wants_jump) {
                player.vy += speed;
            } else if (wants_shift) {
                player.vy -= speed;
            }
            apply_predicted_move(level, player);
            player.vx *= player_air_drag;
            player.vy *= player_air_drag;
            player.vz *= player_air_drag;
            player.on_ground = false;
            player.fall_distance = 0.0;
        } else if (water || lava) {
            const auto gravity{player_gravity / (lava ? 4.0 : 16.0)};
            player.vx += intent.x * 0.02;
            player.vz += intent.z * 0.02;
            if (wants_jump) {
                player.vy += 0.04;
            }
            player.vy -= gravity;
            apply_predicted_move(level, player);
            const auto drag{lava ? 0.5 : 0.8};
            player.vx *= drag;
            player.vz *= drag;
            player.vy *= 0.8;
            player.on_ground = false;
            player.fall_distance = 0.0;
            player.air_supply =
                std::min(player.air_supply, player_max_air_supply);
        } else if (was_on_ground) {
            const auto below{level.block_at(
                block_pos{static_cast<std::int32_t>(std::floor(player.x)),
                          static_cast<std::int16_t>(std::floor(player.y - 0.2)),
                          static_cast<std::int32_t>(std::floor(player.z))})};
            const auto friction{level.block_friction(below)};
            player.vx += intent.x * player_walk_speed *
                         (sprint ? player_sprint_factor : 1.0);
            player.vz += intent.z * player_walk_speed *
                         (sprint ? player_sprint_factor : 1.0);
            if (wants_jump) {
                player.vy = player_jump_velocity;
                if (sprint) {
                    const auto radians{static_cast<double>(player.yaw) *
                                       (std::numbers::pi / 180.0)};
                    player.vx -= std::sin(radians) * 0.2;
                    player.vz += std::cos(radians) * 0.2;
                }
                player.exhaustion = std::min(
                    player.exhaustion + (sprint ? 0.2F : 0.05F), 40.0F);
            } else {
                player.vy -= player_gravity;
            }
            apply_predicted_move(level, player);
            player.vx *= friction * player_air_drag;
            player.vz *= friction * player_air_drag;
            player.vy *= player_vertical_drag;
            player.on_ground = grounded(level, player);
            if (player.on_ground && player.vy < 0.0) {
                player.vy = 0.0;
            }
        } else {
            const auto speed{sprint ? player_sprint_air_speed
                                    : player_air_speed};
            player.vx += intent.x * speed;
            player.vz += intent.z * speed;
            player.vy -= player_gravity;
            if (wants_jump && climbing) {
                player.vy = 0.2;
            }
            apply_predicted_move(level, player);
            player.vx *= player_air_drag;
            player.vz *= player_air_drag;
            player.vy *= player_vertical_drag;
            player.on_ground = grounded(level, player);
            if (player.on_ground && player.vy < 0.0) {
                player.vy = 0.0;
            }
        }
    }

    // A validated client request is authoritative for the final position of
    // the tick. For a client-driven record it is the only thing that moves the
    // body; for a simulated record the prediction above fills ticks without a
    // request, and the request still decides where the body ends up. Either
    // way fall distance and landing come from the displacement the server
    // observed, never from the prediction.
    if (player.has_move_request) {
        player.x = player.move_x;
        player.y = player.move_y;
        player.z = player.move_z;
        // Recorded vertical displacement of the report. A simulated record
        // also seeds the next prediction with it, so it is bounded by the jump
        // and terminal-fall speeds; a client-driven record never predicts from
        // it, because the client is the only source of its motion.
        player.vy =
            std::clamp(player.move_y - start_y, -3.92, player_jump_velocity);
        player.on_ground = player.move_on_ground || grounded(level, player);
        player.has_move_request = false;
    }

    // Fall distance accumulates the downward displacement of this tick while
    // the player is neither flying nor held by a fluid or a climbable block.
    const auto moved_y{player.y - start_y};
    if (flying || water || lava || climbing) {
        player.fall_distance = 0.0;
    } else if (moved_y < 0.0) {
        player.fall_distance -= moved_y;
    }
    if (player.on_ground) {
        if (player.fall_distance > 0.0) {
            const auto power{static_cast<int>(std::floor(
                player.fall_distance + 1.0e-6 - player_safe_fall_distance))};
            if (power > 0) {
                static_cast<void>(damage_player(level, player,
                                                damage_source::fall,
                                                static_cast<float>(power)));
            }
            player.fall_distance = 0.0;
        }
        player.ground_y = player.y;
    } else {
        player.ground_y = std::min(player.ground_y, player.y);
    }

    // Sprinting on the ground costs food: 0.1 exhaustion per block. Plain
    // walking costs nothing, matching the vanilla movement statistics.
    if (player.on_ground && sprint && is_survival(player)) {
        const auto moved{
            horizontal_distance(player.x - start_x, player.z - start_z)};
        player.exhaustion = std::min(
            player.exhaustion + static_cast<float>(moved) * 0.1F, 40.0F);
    }

    // Drowning: the eye in water drains the air supply, and a fully drained
    // supply deals two damage per second.
    if (is_survival(player) &&
        level.block_fluid(block_at_height(level, player, player_eye_height)) ==
            world::fluid_kind::water) {
        --player.air_supply;
        if (player.air_supply <= player_drowning_air) {
            player.air_supply = 0;
            static_cast<void>(
                damage_player(level, player, damage_source::drown, 2.0F));
        }
    } else if (player.air_supply < player_max_air_supply) {
        player.air_supply =
            std::min(player.air_supply + 4, player_max_air_supply);
    }

    // The void hurts below the build floor; the damage bypasses the immunity
    // window so a player outside the world dies.
    if (player.y <
        static_cast<double>(world::build_min_y()) - player_void_depth) {
        static_cast<void>(
            damage_player(level, player, damage_source::out_of_world, 4.0F));
    }

    // Suffocation: an AABB overlapping a solid block hurts until the player
    // moves out.
    if (is_survival(player) && level.player_overlaps_solid(player)) {
        static_cast<void>(
            damage_player(level, player, damage_source::in_wall, 1.0F));
    }

    tick_food(level, player);
    if (level.difficulty() == 0) {
        tick_peaceful(player);
    }

    // Keep the record inside the addressable world. A client cannot move
    // outside the build bounds through a validated request, but console
    // commands and future portals can place it there.
    player.x = std::clamp(player.x, -3.0e7, 3.0e7);
    player.z = std::clamp(player.z, -3.0e7, 3.0e7);
}

} // namespace refrain::sim
