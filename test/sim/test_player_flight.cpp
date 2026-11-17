// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/sim/player_physics.hpp>
#include <refrain/sim/world.hpp>

#include <refrain/platform/process.hpp>
#include <refrain/world/store.hpp>

#include <boost/ut.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <numbers>
#include <string>
#include <system_error>

namespace {
using namespace boost::ut;

namespace fs = std::filesystem;
namespace storage = refrain::storage;
namespace world_ns = refrain::world;
namespace sim = refrain::sim;

[[nodiscard]] auto scratch_directory() -> fs::path {
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    auto path{fs::temp_directory_path() /
              ("refrain-player-flight-" +
               std::to_string(refrain::platform::process_id()) + "-" +
               std::to_string(stamp))};
    std::error_code error{};
    fs::create_directories(path, error);
    return path;
}

/// A column whose section zero is solid stone from y=0 to y=15, so the floor
/// every fixture player starts above is y=16.
[[nodiscard]] auto make_chunk() -> storage::chunk {
    storage::chunk chunk{};
    chunk.format_version = world_ns::chunk_format_version;
    chunk.status = storage::chunk_status::full;
    chunk.y_pos = 0;
    storage::section_record section{};
    section.y = 0;
    section.kind = storage::section_kind::single_value;
    section.bits_per_entry = 0;
    section.block_count = static_cast<std::uint16_t>(world_ns::section_volume);
    section.biome_kind = storage::section_kind::single_value;
    section.biome_bits_per_entry = 0;
    section.biome_palette.push_back("minecraft:plains");
    section.palette.push_back(storage::palette_entry{
        .key = "minecraft:stone",
        .count = static_cast<std::uint16_t>(world_ns::section_volume),
        .state = 0,
    });
    chunk.sections.push_back(std::move(section));
    return chunk;
}

[[nodiscard]] auto make_world(const fs::path &directory)
    -> std::optional<sim::world> {
    auto store{world_ns::world_store::create(directory.string())};
    if (!store) {
        return std::nullopt;
    }
    if (!store->write_chunk({0, 0}, make_chunk())) {
        return std::nullopt;
    }
    if (!store->commit()) {
        return std::nullopt;
    }
    auto opened{sim::world::open(directory.string())};
    if (!opened) {
        return std::nullopt;
    }
    if (!opened->ensure_chunk({0, 0})) {
        return std::nullopt;
    }
    return std::optional<sim::world>{std::move(*opened)};
}

auto with_world(auto &&body) {
    const auto directory{scratch_directory()};
    {
        auto level{make_world(directory)};
        expect(level.has_value()) << "world fixture failed";
        if (level) {
            body(*level);
        }
    }
    std::error_code error;
    fs::remove_all(directory, error);
}

constexpr double floor_y = 16.0;
constexpr std::uint64_t test_connection = 1;

/// A record owned by a network client: the simulation only applies validated
/// requests to it. `game_mode` selects the movement rules.
[[nodiscard]] auto add_client(sim::world &level, std::uint32_t game_mode,
                              double x = 0.5, double y = floor_y,
                              double z = 0.5,
                              std::uint64_t connection = test_connection)
    -> sim::player_state * {
    const auto added{level.add_player(sim::player_table::registration{
        .connection = connection,
        .client_driven = true,
        .name = std::string{"@client"},
        .world = std::string{"overworld"},
        .x = x,
        .y = y,
        .z = z,
        .game_mode = game_mode,
        .inventory_slots = 0,
    })};
    return added ? level.player(*added) : nullptr;
}

/// A record without a client (console or replay): fully simulated from its
/// input flags.
[[nodiscard]] auto add_simulated(sim::world &level, std::uint32_t game_mode,
                                 double x = 0.5, double y = floor_y,
                                 double z = 0.5) -> sim::player_state * {
    const auto added{level.add_player(sim::player_table::registration{
        .connection = sim::synthetic_connection,
        .name = std::string{"@console"},
        .world = std::string{"overworld"},
        .x = x,
        .y = y,
        .z = z,
        .game_mode = game_mode,
        .inventory_slots = 0,
    })};
    return added ? level.player(*added) : nullptr;
}

constexpr double client_gravity = 0.08;
constexpr double client_vertical_override = 0.6;
constexpr double client_air_drag = 0.91;
constexpr double client_vertical_drag = 0.98;
constexpr double client_fly_speed = 0.05;
constexpr double client_fly_vertical_pulse = 0.15;
constexpr double client_jump_velocity = 0.42;

/// The 26.3 client's creative-flight movement reduced to the parts that produce
/// the reported feet position: the vertical input pulse in
/// `LocalPlayer.aiStep`, `travel`'s air branch (flying speed, then the 0.6
/// vertical override) and the flight toggle that clears on landing. Collision
/// resolution is omitted: the fixtures fly over a flat floor and stop at the
/// reported floor height, and wall cases decide the request by hand.
struct flight_client {
    double x{0.5};
    double y{floor_y};
    double z{0.5};
    double vx{0.0};
    double vy{0.0};
    double vz{0.0};
    float yaw{0.0F};
    float pitch{0.0F};
    bool flying{false};
    bool on_ground{true};
    bool sprint{false};
    bool jump{false};
    bool shift{false};

    [[nodiscard]] auto request() const -> sim::player_move_request {
        return sim::player_move_request{.x = x,
                                        .y = y,
                                        .z = z,
                                        .yaw = yaw,
                                        .pitch = pitch,
                                        .on_ground = on_ground};
    }

    /// Double-tap jump while standing: the client enables flight, jumps off
    /// the ground and reports the first airborne position in the same tick.
    void take_off() {
        flying = true;
        jump = true;
        vy = client_jump_velocity;
        step();
        jump = false;
    }

    /// One client tick. While flying the reported displacement is exactly the
    /// pre-gravity vertical delta; the horizontal delta is the
    /// input-accelerated velocity.
    void step() {
        if (flying) {
            auto vertical{vy};
            if (jump) {
                vertical += client_fly_vertical_pulse;
            } else if (shift) {
                vertical -= client_fly_vertical_pulse;
            }
            const auto speed{sprint ? client_fly_speed * 2.0
                                    : client_fly_speed};
            const auto radians{static_cast<double>(yaw) *
                               (std::numbers::pi / 180.0)};
            // Forward input in world space, scaled like the client's input.
            const auto input_x{-std::sin(radians) * client_air_drag};
            const auto input_z{std::cos(radians) * client_air_drag};
            const auto delta_x{vx + input_x * speed};
            const auto delta_z{vz + input_z * speed};
            x += delta_x;
            y += vertical;
            z += delta_z;
            vx = delta_x * client_air_drag;
            vz = delta_z * client_air_drag;
            vy = vertical * client_vertical_override;
            if (y <= floor_y) {
                y = floor_y;
                on_ground = true;
                vy = 0.0;
                // The client leaves creative flight when it stands again.
                flying = false;
            } else {
                on_ground = false;
            }
            return;
        }
        // Walking/falling: input acceleration is horizontal only, gravity
        // applies to the reported displacement, and the vertical drag keeps
        // the arc bounded.
        const auto speed{sprint ? 0.13 : 0.1};
        const auto radians{static_cast<double>(yaw) *
                           (std::numbers::pi / 180.0)};
        const auto input_x{-std::sin(radians) * client_air_drag};
        const auto input_z{std::cos(radians) * client_air_drag};
        x += vx + input_x * speed;
        z += vz + input_z * speed;
        y += vy - client_gravity;
        vy = (vy - client_gravity) * client_vertical_drag;
        if (y <= floor_y) {
            y = floor_y;
            on_ground = true;
            vy = 0.0;
        } else {
            on_ground = false;
        }
    }
};

/// One simulated pair of ticks: the client reports the position it holds, the
/// server validates it and applies it, then the client advances. Mirrors the
/// runtime's order (`listener->tick()` delivers reports, then `world.tick()`
/// applies the accepted one). The flight toggle is forwarded like the runtime's
/// abilities callback does.
auto drive_tick(sim::world &level, sim::player_state &player,
                flight_client &client) -> sim::player_move_verdict {
    player.flying = client.flying;
    const auto report{client.request()};
    const auto verdict{level.submit_player_move(player, report)};
    level.tick();
    if (verdict == sim::player_move_verdict::accepted) {
        // The authoritative record must be exactly the reported position: the
        // server applies the validated target, it never predicts past it.
        expect(std::abs(player.x - report.x) < 1.0e-9)
            << "record x" << player.x << report.x;
        expect(std::abs(player.y - report.y) < 1.0e-9)
            << "record y" << player.y << report.y;
        expect(std::abs(player.z - report.z) < 1.0e-9)
            << "record z" << player.z << report.z;
    }
    client.step();
    return verdict;
}

} // namespace

suite player_flight_tests = [] {
    "sim/player/flight/creative takeoff, climb, hover and landing stay in sync"_test =
        [] {
            with_world([](sim::world &level) {
                auto *player{add_client(level, sim::game_mode_creative, 0.5,
                                        floor_y, 0.5)};
                expect(player != nullptr);
                if (player == nullptr) {
                    return;
                }
                flight_client client{};
                player->flying = true;
                client.take_off();
                auto corrections{0};
                // Climb for one second, then hover without reporting for
                // another second: a standing or hovering client sends no
                // position packet at all, so the server must keep the record
                // exactly where the last report left it.
                for (int tick{0}; tick < 20; ++tick) {
                    client.jump = tick < 10;
                    const auto verdict{drive_tick(level, *player, client)};
                    if (verdict != sim::player_move_verdict::accepted) {
                        ++corrections;
                    }
                }
                const auto hover_y{player->y};
                expect(hover_y > floor_y + 1.0) << hover_y;
                for (int tick{0}; tick < 40; ++tick) {
                    level.tick();
                }
                expect(std::abs(player->y - hover_y) < 1.0e-9) << player->y;
                // Descend and land. The client clears flight on the landing
                // tick, exactly like the vanilla client, and the server must
                // accept the reported floor position.
                for (int tick{0}; tick < 60; ++tick) {
                    client.shift = true;
                    const auto verdict{drive_tick(level, *player, client)};
                    if (verdict != sim::player_move_verdict::accepted) {
                        ++corrections;
                    }
                    client.shift = false;
                    if (client.on_ground) {
                        break;
                    }
                }
                expect(client.on_ground);
                // The landing position is reported on the tick after the
                // client touches down; deliver that report before asserting.
                const auto landing{drive_tick(level, *player, client)};
                expect(landing == sim::player_move_verdict::accepted);
                expect(std::abs(player->y - floor_y) < 1.0e-9) << player->y;
                expect(player->on_ground);
                expect(corrections == 0) << corrections;
            });
        };

    "sim/player/flight/level and diagonal sprint flight stay in sync"_test =
        [] {
            with_world([](sim::world &level) {
                auto *player{
                    add_client(level, sim::game_mode_creative, 0.5, 24.0, 0.5)};
                expect(player != nullptr);
                if (player == nullptr) {
                    return;
                }
                flight_client client{.x = 0.5,
                                     .y = 24.0,
                                     .z = 0.5,
                                     .flying = true,
                                     .on_ground = false};
                player->flying = true;
                auto corrections{0};
                for (int tick{0}; tick < 200; ++tick) {
                    // First a level run, then a diagonal sprint run: the client
                    // turns 45 degrees and holds forward plus strafe.
                    client.yaw = tick < 100 ? 0.0F : 45.0F;
                    client.sprint = tick >= 100;
                    const auto verdict{drive_tick(level, *player, client)};
                    if (verdict != sim::player_move_verdict::accepted) {
                        ++corrections;
                    }
                    static_cast<void>(tick);
                }
                expect(corrections == 0) << corrections;
                expect(player->z > 50.0) << player->z;
                expect(player->x < -10.0) << player->x;
            });
        };

    "sim/player/flight/a client-driven record is never predicted"_test = [] {
        with_world([](sim::world &level) {
            auto *player{
                add_client(level, sim::game_mode_creative, 0.5, 40.0, 0.5)};
            expect(player != nullptr);
            if (player == nullptr) {
                return;
            }
            // The client is flying and holding jump, but its report is not due
            // yet. The server must not extrapolate the flight on its own.
            player->flying = true;
            player->input = sim::player_input::jump;
            for (int tick{0}; tick < 40; ++tick) {
                level.tick();
            }
            expect(std::abs(player->y - 40.0) < 1.0e-9) << player->y;
            // A stale velocity left by a previous report must not be used
            // either: the old code carried a drift-correction delta of -3.0
            // blocks per tick into the next prediction and dived to the floor.
            player->vy = -3.0;
            for (int tick{0}; tick < 40; ++tick) {
                level.tick();
            }
            expect(std::abs(player->y - 40.0) < 1.0e-9) << player->y;
            expect(!player->on_ground);
        });
    };

    "sim/player/flight/a late report below a hover is accepted and stays put"_test =
        [] {
            with_world([](sim::world &level) {
                auto *player{
                    add_client(level, sim::game_mode_creative, 0.5, 40.0, 0.5)};
                expect(player != nullptr);
                if (player == nullptr) {
                    return;
                }
                player->flying = true;
                // The client hovers at 40.0 and reports it once.
                const auto first{level.submit_player_move(
                    *player,
                    sim::player_move_request{
                        .x = 0.5, .y = 40.0, .z = 0.5, .on_ground = false})};
                expect(first == sim::player_move_verdict::accepted);
                level.tick();
                expect(std::abs(player->y - 40.0) < 1.0e-9);
                // No reports for a while: the record must stay at the report.
                for (int tick{0}; tick < 30; ++tick) {
                    level.tick();
                }
                expect(std::abs(player->y - 40.0) < 1.0e-9) << player->y;
                // The next report lands slightly below the hover and must be
                // accepted: under the old prediction the record had already
                // drifted upwards and the acceptance drove a -3.0 fall
                // velocity that pulled it to the floor.
                const auto second{level.submit_player_move(
                    *player,
                    sim::player_move_request{
                        .x = 0.5, .y = 39.5, .z = 0.5, .on_ground = false})};
                expect(second == sim::player_move_verdict::accepted);
                level.tick();
                expect(std::abs(player->y - 39.5) < 1.0e-9) << player->y;
                for (int tick{0}; tick < 30; ++tick) {
                    level.tick();
                }
                expect(std::abs(player->y - 39.5) < 1.0e-9) << player->y;
            });
        };

    "sim/player/flight/creative flight still rejects a wall penetration"_test =
        [] {
            with_world([](sim::world &level) {
                const auto stone{*level.registry().find("minecraft:stone")};
                for (std::int16_t y{16}; y <= 18; ++y) {
                    expect(level.set_block({2, y, 0}, stone).has_value());
                }
                auto *player{
                    add_client(level, sim::game_mode_creative, 0.5, 16.0, 0.5)};
                expect(player != nullptr);
                if (player == nullptr) {
                    return;
                }
                player->flying = true;
                // Flying does not grant passage through a solid block.
                const auto verdict{level.submit_player_move(
                    *player,
                    sim::player_move_request{
                        .x = 2.5, .y = 16.0, .z = 0.5, .on_ground = false})};
                expect(verdict == sim::player_move_verdict::corrected);
                expect(!player->has_move_request);
                expect(std::abs(player->x - 0.5) < 1.0e-9) << player->x;
            });
        };

    "sim/player/flight/creative flight still rejects a teleport-sized report"_test =
        [] {
            with_world([](sim::world &level) {
                auto *player{
                    add_client(level, sim::game_mode_creative, 0.5, 40.0, 0.5)};
                expect(player != nullptr);
                if (player == nullptr) {
                    return;
                }
                player->flying = true;
                const auto verdict{level.submit_player_move(
                    *player,
                    sim::player_move_request{
                        .x = 0.5, .y = 40.0, .z = 40.0, .on_ground = false})};
                expect(verdict == sim::player_move_verdict::corrected);
                expect(!player->has_move_request);
            });
        };

    "sim/player/flight/survival cannot fly out of the walking envelope"_test =
        [] {
            with_world([](sim::world &level) {
                auto *player{
                    add_client(level, sim::game_mode_survival, 0.5, 16.0, 0.5)};
                expect(player != nullptr);
                if (player == nullptr) {
                    return;
                }
                const auto grounded{level.submit_player_move(
                    *player,
                    sim::player_move_request{
                        .x = 0.5, .y = 16.0, .z = 0.5, .on_ground = true})};
                expect(grounded == sim::player_move_verdict::accepted);
                level.tick();
                expect(player->on_ground);
                // The runtime refuses the flight toggle in survival, so the
                // record stays on the ground and the walking gates apply: one
                // report cannot climb more than a jump can reach.
                const auto climb{level.submit_player_move(
                    *player,
                    sim::player_move_request{
                        .x = 0.5, .y = 18.4, .z = 0.5, .on_ground = false})};
                expect(climb == sim::player_move_verdict::corrected);
                const auto sprint{level.submit_player_move(
                    *player,
                    sim::player_move_request{
                        .x = 4.5, .y = 16.0, .z = 0.5, .on_ground = true})};
                expect(sprint == sim::player_move_verdict::corrected);
                expect(!player->has_move_request);
            });
        };

    "sim/player/flight/a client that toggles flight off lands and walks"_test =
        [] {
            with_world([](sim::world &level) {
                auto *player{
                    add_client(level, sim::game_mode_creative, 0.5, 24.0, 0.5)};
                expect(player != nullptr);
                if (player == nullptr) {
                    return;
                }
                flight_client client{.x = 0.5,
                                     .y = 24.0,
                                     .z = 0.5,
                                     .flying = true,
                                     .on_ground = false};
                player->flying = true;
                auto corrections{0};
                // Descend to the floor; the client clears flight on landing.
                for (int tick{0}; tick < 80 && !client.on_ground; ++tick) {
                    client.shift = true;
                    const auto verdict{drive_tick(level, *player, client)};
                    if (verdict != sim::player_move_verdict::accepted) {
                        ++corrections;
                    }
                    client.shift = false;
                }
                expect(client.on_ground);
                // The client reports the flight-off flag and the floor position
                // on the tick after it touches down.
                const auto landing{drive_tick(level, *player, client)};
                expect(landing == sim::player_move_verdict::accepted);
                expect(!player->flying);
                expect(player->on_ground);
                // Walk forward for a second; the record follows the reports.
                for (int tick{0}; tick < 20; ++tick) {
                    const auto verdict{drive_tick(level, *player, client)};
                    if (verdict != sim::player_move_verdict::accepted) {
                        ++corrections;
                    }
                    static_cast<void>(tick);
                }
                expect(player->z > 1.0) << player->z;
                expect(corrections == 0) << corrections;
            });
        };

    "sim/player/flight/an accepted teleport position is adopted without prediction"_test =
        [] {
            with_world([](sim::world &level) {
                auto *player{
                    add_client(level, sim::game_mode_creative, 0.5, 24.0, 0.5)};
                expect(player != nullptr);
                if (player == nullptr) {
                    return;
                }
                level.tick();
                // The runtime's teleport-confirm handler adopts the position
                // the client acknowledged and clears the pending request. The
                // first report after that must be accepted: no predicted
                // position may exist between the teleport and the report.
                level.set_player_position(*player, 0.5, 30.0, 0.5);
                for (int tick{0}; tick < 20; ++tick) {
                    level.tick();
                }
                expect(std::abs(player->y - 30.0) < 1.0e-9) << player->y;
                const auto verdict{level.submit_player_move(
                    *player,
                    sim::player_move_request{
                        .x = 0.5, .y = 30.2, .z = 0.5, .on_ground = false})};
                expect(verdict == sim::player_move_verdict::accepted);
                level.tick();
                expect(std::abs(player->y - 30.2) < 1.0e-9) << player->y;
            });
        };

    "sim/player/flight/batched reports land on the newest position"_test = [] {
        with_world([](sim::world &level) {
            auto *player{
                add_client(level, sim::game_mode_creative, 0.5, 40.0, 0.5)};
            expect(player != nullptr);
            if (player == nullptr) {
                return;
            }
            player->flying = true;
            // A stalled server tick or a batched connection delivers several
            // reports at once: each one is validated against the record and the
            // tick applies the newest position, so the client is never pulled
            // back to an intermediate report.
            const auto first{level.submit_player_move(
                *player,
                sim::player_move_request{.x = 0.5, .y = 40.2, .z = 0.5})};
            const auto second{level.submit_player_move(
                *player,
                sim::player_move_request{.x = 0.5, .y = 40.5, .z = 0.5})};
            // Flight is exempt from the walking jump ceiling, so a batched
            // climb larger than one jump still fits under the distance gate.
            const auto third{level.submit_player_move(
                *player,
                sim::player_move_request{.x = 0.5, .y = 42.5, .z = 0.5})};
            expect(first == sim::player_move_verdict::accepted);
            expect(second == sim::player_move_verdict::accepted);
            expect(third == sim::player_move_verdict::accepted);
            level.tick();
            expect(std::abs(player->y - 42.5) < 1.0e-9) << player->y;

            // A late server tick can batch a whole second of sprint-flight
            // reports. Each report is one client tick of ~1 block, so the
            // batch as a whole covers more than the single-report distance
            // gate; each report must still be validated against the one
            // before it, and the tick applies only the newest.
            auto *batched{
                add_client(level, sim::game_mode_creative, 0.5, 40.0, 0.5, 2)};
            expect(batched != nullptr);
            if (batched == nullptr) {
                return;
            }
            batched->flying = true;
            auto accepted{0};
            for (int report{1}; report <= 9; ++report) {
                const auto verdict{level.submit_player_move(
                    *batched,
                    sim::player_move_request{
                        .x = 0.5,
                        .y = 40.0,
                        .z = 0.5 + static_cast<double>(report) * 0.95})};
                if (verdict == sim::player_move_verdict::accepted) {
                    ++accepted;
                }
            }
            expect(accepted == 9) << accepted;
            level.tick();
            expect(std::abs(batched->z - (0.5 + 9.0 * 0.95)) < 1.0e-9)
                << batched->z;
        });
    };

    "sim/player/flight/a console player is still simulated from its input"_test =
        [] {
            with_world([](sim::world &level) {
                auto *player{add_simulated(level, sim::game_mode_creative)};
                expect(player != nullptr);
                if (player == nullptr) {
                    return;
                }
                level.tick();
                player->flying = true;
                player->input = sim::player_input::jump;
                const auto start_y{player->y};
                for (int tick{0}; tick < 20; ++tick) {
                    level.tick();
                }
                expect(player->y > start_y + 1.0) << player->y;
                // A client-driven record with the same input does not move:
                // only its reports do.
                auto *client{
                    add_client(level, sim::game_mode_creative, 1.5, 16.0, 0.5)};
                expect(client != nullptr);
                if (client == nullptr) {
                    return;
                }
                level.tick();
                client->flying = true;
                client->input = sim::player_input::jump;
                const auto client_start{client->y};
                for (int tick{0}; tick < 20; ++tick) {
                    level.tick();
                }
                expect(std::abs(client->y - client_start) < 1.0e-9)
                    << client->y;
            });
        };
};
