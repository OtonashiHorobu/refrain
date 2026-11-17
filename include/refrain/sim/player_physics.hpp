// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/sim/player.hpp>

#include <cstdint>

namespace refrain::sim {

class world;

/// One client-reported movement request: the absolute feet position and view
/// angles the client predicted for this tick. The server validates the request
/// against its own authoritative record and never accepts it blindly.
struct player_move_request {
    double x{0.0};
    double y{0.0};
    double z{0.0};
    float yaw{0.0F};
    float pitch{0.0F};
    /// The client reports whether it believes it stands on a block. Landing
    /// detection reads it, but the server still probes the ground itself.
    bool on_ground{false};
};

/// What the simulation did with a movement request.
enum class player_move_verdict : std::uint8_t {
    /// The request was reachable, inside the speed envelope and collision
    /// free; `tick()` will apply it.
    accepted = 0,
    /// The request was rejected. The caller sends the authoritative position
    /// back so the client resynchronises.
    corrected,
};

/// Player physics constants, as the vanilla movement model uses them. They are
/// exposed for tests that state an expected position after a fixed number of
/// ticks instead of hard-coding the derivation.
inline constexpr double player_gravity = 0.08;
inline constexpr double player_vertical_drag = 0.98;
inline constexpr double player_air_drag = 0.91;
inline constexpr double player_ground_friction = 0.6;
inline constexpr double player_walk_speed = 0.1;
inline constexpr double player_sprint_factor = 1.3;
inline constexpr double player_air_speed = 0.02;
inline constexpr double player_sprint_air_speed = 0.026;
inline constexpr double player_fly_speed = 0.05;
inline constexpr double player_jump_velocity = 0.42;
/// Vanilla's maximum upward step while walking (slabs and stairs). A full
/// block still needs a jump.
inline constexpr double player_step_height = 0.6;
/// Horizontal displacement one tick may report. Walking is 0.1 and sprinting
/// 0.13 blocks per tick; the envelope leaves room for knockback and the packet
/// batching a real client produces.
inline constexpr double player_max_horizontal_per_tick = 3.0;
/// A jump starts at 0.42 blocks per tick and the packet may batch two ticks.
inline constexpr double player_max_upward_per_tick = 1.0;
/// Height a player may gain from the last ground contact without flight,
/// fluid or a mount: vanilla's jump apex is about 1.25 blocks.
inline constexpr double player_max_jump_height = 1.5;
/// Full displacement a single request may cover. Anything larger is a
/// teleport request from a modified client, not movement.
inline constexpr double player_max_move_distance = 8.0;
/// How far a validated request may end from the collision-resolved position
/// before the server treats it as a wall penetration.
inline constexpr double player_move_tolerance = 0.25;
/// Safe fall distance: falling further than this hurts.
inline constexpr double player_safe_fall_distance = 3.0;
/// Height above the build limit below which the void hurts.
inline constexpr double player_void_depth = 64.0;
/// Eye height above the feet, used for drowning and line of sight.
inline constexpr double player_eye_height = 1.62;
/// Ticks a player is immune after taking damage.
inline constexpr std::int32_t player_hurt_immunity = 10;
/// Air supply value and the threshold below which drowning starts.
inline constexpr std::int32_t player_max_air_supply = 300;
inline constexpr std::int32_t player_drowning_air = -20;

/// Total experience points needed to reach `level` from level zero, using the
/// vanilla level curve (7, 9, ... until 16, then 5l-38, then 9l-158). Negative
/// levels are clamped to zero.
[[nodiscard]] auto experience_for_level(std::int32_t level) noexcept
    -> std::int32_t;

/// Add experience points and recompute level and progress. Negative points are
/// ignored (the vanilla call never removes experience).
void add_experience(player_state &player, std::int32_t points) noexcept;

/// Restore health without exceeding the maximum; dead records stay dead.
void heal_player(player_state &player, float amount) noexcept;

/// Apply one damage hit. Handles creative/spectator immunity, the immunity
/// window, health reduction, death and the damage-source record. `attacker` is
/// the connection of the player that dealt the hit, or zero for the
/// environment; it is recorded for the death message and the kill experience.
/// Returns true when the hit changed the player's health.
auto damage_player(world &world, player_state &player, damage_source source,
                   float amount, std::uint64_t attacker = 0) -> bool;

/// Reset a dead player at the given position: full health and food, empty
/// exhaustion and air, no fall distance and no pending movement. Experience is
/// lost on death unless the caller restores it first (the runtime decides what
/// the world's keep-inventory rule preserves).
void respawn_player(world &world, player_state &player, double x, double y,
                    double z, float yaw, float pitch) noexcept;

/// Apply a game-mode change and the ability state that follows from it.
/// Switching to spectator forces flight; switching away from creative or
/// spectator stops it. The client-facing ability broadcast stays the runtime's
/// job because it is a protocol packet, not simulation state.
void set_player_game_mode(world &world, player_state &player,
                          std::uint32_t mode) noexcept;

/// One server tick of player physics and survival state: input-driven movement
/// with collision and step-up, gravity, fluid movement, fall tracking and
/// damage, drowning, void and in-wall damage, hunger, natural regeneration and
/// starvation. Called by `world::tick()` for every record in ascending entity
/// id order.
///
/// Only records without a client are simulated this way. A client-driven
/// record (see `player_state::client_driven`) is moved exclusively by the
/// validated requests it reports: it keeps the survival half of the tick but
/// no movement is predicted between reports, because the client stops
/// reporting whenever it stands still or hovers and a prediction would drift
/// away from the body the client actually has.
void tick_player(world &world, player_state &player) noexcept;

/// Validate one client movement request against the authoritative record.
///
/// Accepted requests are stored on the record and applied by the next
/// `tick()`; a rejected request leaves the record untouched and reports that
/// the caller must send the authoritative position back. Rejection covers
/// non-finite values, a teleport-sized displacement, a target the collision
/// sweep cannot reach, and, for a body that is not flying, horizontal speed
/// above the walking/sprint envelope and upward movement no jump can produce.
/// Flight is exempt from the walking envelope because the client's flying
/// speed is not the walking speed, but it is not exempt from the sweep or the
/// teleport-sized displacement gate: a flying client still cannot report a
/// position inside a wall or across the map.
[[nodiscard]] auto submit_player_move(world &world, player_state &player,
                                      const player_move_request &request)
    -> player_move_verdict;

/// Resolve a movement delta against the world's block collisions without
/// changing the record. The returned position is the furthest collision-free
/// position along the delta, resolved on Y, then X, then Z with step-up. Tests
/// and the movement validator use it to compare a request with what the world
/// actually allows.
struct player_collision_result {
    double x{0.0};
    double y{0.0};
    double z{0.0};
    bool collided{false};
};

[[nodiscard]] auto resolve_player_move(world &world, const player_state &player,
                                       double dx, double dy, double dz)
    -> player_collision_result;

} // namespace refrain::sim
