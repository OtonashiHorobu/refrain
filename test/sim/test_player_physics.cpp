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
              ("refrain-player-physics-" +
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

/// Register the synthetic player and return its record. The console player is
/// the console/replay driver; tests use it for deterministic fixed-input runs.
[[nodiscard]] auto
add_player(sim::world &level, double x = 0.5, double y = 16.0, double z = 0.5,
           std::uint64_t connection = sim::synthetic_connection,
           std::string name = "@test") -> sim::player_state * {
    const auto added{level.add_player(sim::player_table::registration{
        .connection = connection,
        .name = std::move(name),
        .world = std::string{"overworld"},
        .x = x,
        .y = y,
        .z = z,
        .game_mode = sim::game_mode_survival,
    })};
    return added ? level.player(*added) : nullptr;
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

} // namespace

suite player_physics_tests = [] {
    "sim/player/gravity lands on the floor and reports ground"_test = [] {
        with_world([](sim::world &level) {
            auto *player{add_player(level, 0.5, 18.0, 0.5)};
            expect(player != nullptr);
            if (player == nullptr) {
                return;
            }
            for (int tick{0}; tick < 80; ++tick) {
                level.tick();
            }
            expect(std::abs(player->y - 16.0) < 1.0e-3) << player->y;
            expect(player->on_ground);
            expect(player->health == 20.0F);
        });
    };

    "sim/player/fixed input moves a deterministic distance"_test = [] {
        with_world([](sim::world &level) {
            auto *player{add_player(level)};
            expect(player != nullptr);
            if (player == nullptr) {
                return;
            }
            player->input = sim::player_input::forward;
            for (int tick{0}; tick < 20; ++tick) {
                level.tick();
            }
            const auto walking_z{player->z};
            expect(walking_z > 2.0) << walking_z;
            expect(walking_z < 6.0) << walking_z;
            expect(std::abs(player->x - 0.5) < 1.0e-9) << player->x;
            // Sprinting covers more ground over the same number of ticks.
            auto *second{add_player(level, 0.5, 16.0, 0.5, 1, "@sprint")};
            expect(second != nullptr);
            if (second == nullptr) {
                return;
            }
            second->input =
                sim::player_input::forward | sim::player_input::sprint;
            for (int tick{0}; tick < 20; ++tick) {
                level.tick();
            }
            expect(second->z > walking_z);
        });
    };

    "sim/player/collision stops at a wall and jumps over a step"_test = [] {
        with_world([](sim::world &level) {
            const auto stone{*level.registry().find("minecraft:stone")};
            for (std::int16_t y{16}; y <= 18; ++y) {
                expect(level.set_block({2, y, 0}, stone).has_value());
            }
            auto *player{add_player(level)};
            expect(player != nullptr);
            if (player == nullptr) {
                return;
            }
            // Yaw -90 degrees turns forward input into +X.
            player->yaw = -90.0F;
            player->input = sim::player_input::forward;
            for (int tick{0}; tick < 60; ++tick) {
                level.tick();
            }
            expect(player->x <= 1.7 + 1.0e-6) << player->x;
            expect(player->on_ground);

            // A jump cannot clear a two-block wall: the apex is about one
            // block and the wall is three blocks tall from the floor.
            player->input =
                sim::player_input::forward | sim::player_input::jump;
            for (int tick{0}; tick < 60; ++tick) {
                level.tick();
            }
            expect(player->y < 18.0) << player->y;
            expect(player->x <= 1.7 + 1.0e-6) << player->x;
        });
    };

    "sim/player/step-up walks onto a bottom slab"_test = [] {
        with_world([](sim::world &level) {
            const auto slab{level.registry().intern(
                "minecraft:oak_slab[type=bottom,waterlogged=false]")};
            for (std::int32_t x{1}; x <= 3; ++x) {
                expect(
                    level.set_block({static_cast<std::int32_t>(x), 16, 0}, slab)
                        .has_value());
            }
            auto *player{add_player(level)};
            expect(player != nullptr);
            if (player == nullptr) {
                return;
            }
            level.tick();
            expect(player->on_ground);
            // Yaw -90 degrees turns forward input into +X; the slab top is
            // half a block up, inside the player step height.
            player->yaw = -90.0F;
            player->input = sim::player_input::forward;
            for (int tick{0}; tick < 12; ++tick) {
                level.tick();
            }
            expect(player->x > 1.0) << player->x;
            expect(std::abs(player->y - 16.5) < 1.0e-3) << player->y;
            expect(player->on_ground);
        });
    };

    "sim/player/jump apex stays below two blocks and lands again"_test = [] {
        with_world([](sim::world &level) {
            auto *player{add_player(level)};
            expect(player != nullptr);
            if (player == nullptr) {
                return;
            }
            level.tick();
            expect(player->on_ground);
            player->input = sim::player_input::jump;
            auto apex{player->y};
            for (int tick{0}; tick < 40; ++tick) {
                level.tick();
                apex = std::max(apex, player->y);
                if (tick == 1) {
                    player->input = sim::player_input::none;
                }
            }
            expect(apex > 16.8) << apex;
            expect(apex < 18.0) << apex;
            expect(std::abs(player->y - 16.0) < 1.0e-3) << player->y;
            expect(player->on_ground);
        });
    };

    "sim/player/falling hurts and kills"_test = [] {
        with_world([](sim::world &level) {
            auto *player{add_player(level, 0.5, 60.0, 0.5)};
            expect(player != nullptr);
            if (player == nullptr) {
                return;
            }
            for (int tick{0}; tick < 200 && !player->dead; ++tick) {
                level.tick();
            }
            for (int tick{0}; tick < 2; ++tick) {
                level.tick();
            }
            expect(player->dead);
            expect(player->health == 0.0F);
            expect(player->last_damage == sim::damage_source::fall);
            expect(player->death_time > 0);
        });
    };

    "sim/player/short fall is harmless"_test = [] {
        with_world([](sim::world &level) {
            auto *player{add_player(level, 0.5, 18.0, 0.5)};
            expect(player != nullptr);
            if (player == nullptr) {
                return;
            }
            for (int tick{0}; tick < 80; ++tick) {
                level.tick();
            }
            expect(!player->dead);
            expect(player->health == 20.0F);
        });
    };

    "sim/player/water catches the fall and resets fall distance"_test = [] {
        with_world([](sim::world &level) {
            const auto water{level.registry().intern("minecraft:water")};
            for (std::int16_t y{16}; y <= 18; ++y) {
                expect(level.set_block({0, y, 0}, water).has_value());
            }
            auto *player{add_player(level, 0.5, 25.0, 0.5)};
            expect(player != nullptr);
            if (player == nullptr) {
                return;
            }
            for (int tick{0}; tick < 120; ++tick) {
                level.tick();
            }
            expect(!player->dead);
            expect(player->health == 20.0F);
            expect(player->y < 20.0) << player->y;
            expect(player->fall_distance == 0.0);
        });
    };

    "sim/player/submit rejects a teleport request"_test = [] {
        with_world([](sim::world &level) {
            auto *player{add_player(level)};
            expect(player != nullptr);
            if (player == nullptr) {
                return;
            }
            level.tick();
            const auto verdict{level.submit_player_move(
                *player,
                sim::player_move_request{
                    .x = 50.5, .y = 16.0, .z = 0.5, .on_ground = true})};
            expect(verdict == sim::player_move_verdict::corrected);
            expect(!player->has_move_request);
            expect(std::abs(player->x - 0.5) < 1.0e-9);
        });
    };

    "sim/player/submit rejects a wall penetration"_test = [] {
        with_world([](sim::world &level) {
            const auto stone{*level.registry().find("minecraft:stone")};
            for (std::int16_t y{16}; y <= 18; ++y) {
                expect(level.set_block({1, y, 0}, stone).has_value());
            }
            auto *player{add_player(level)};
            expect(player != nullptr);
            if (player == nullptr) {
                return;
            }
            level.tick();
            // The block at x=1 occupies the whole cell; a request landing
            // inside it is not a reachable position.
            const auto verdict{level.submit_player_move(
                *player,
                sim::player_move_request{
                    .x = 1.5, .y = 16.0, .z = 0.5, .on_ground = true})};
            expect(verdict == sim::player_move_verdict::corrected);
            expect(!player->has_move_request);
        });
    };

    "sim/player/submit rejects a two-block jump"_test = [] {
        with_world([](sim::world &level) {
            auto *player{add_player(level)};
            expect(player != nullptr);
            if (player == nullptr) {
                return;
            }
            level.tick();
            expect(player->on_ground);
            const auto verdict{level.submit_player_move(
                *player,
                sim::player_move_request{
                    .x = 0.5, .y = 18.4, .z = 0.5, .on_ground = false})};
            expect(verdict == sim::player_move_verdict::corrected);
            expect(std::abs(player->y - 16.0) < 1.0e-9);
        });
    };

    "sim/player/submit accepts a walking step and applies it on tick"_test =
        [] {
            with_world([](sim::world &level) {
                auto *player{add_player(level)};
                expect(player != nullptr);
                if (player == nullptr) {
                    return;
                }
                level.tick();
                const auto verdict{level.submit_player_move(
                    *player, sim::player_move_request{.x = 0.7,
                                                      .y = 16.0,
                                                      .z = 0.5,
                                                      .yaw = 90.0F,
                                                      .on_ground = true})};
                expect(verdict == sim::player_move_verdict::accepted);
                expect(player->has_move_request);
                expect(player->yaw == 90.0F);
                level.tick();
                expect(std::abs(player->x - 0.7) < 1.0e-9) << player->x;
                expect(!player->has_move_request);
            });
        };

    "sim/player/fall damage is applied on landing after a validated fall"_test =
        [] {
            with_world([](sim::world &level) {
                auto *player{add_player(level, 0.5, 40.0, 0.5)};
                expect(player != nullptr);
                if (player == nullptr) {
                    return;
                }
                // The client reports its falling position every tick; the
                // server accepts each step and turns the accumulated distance
                // into damage when the player lands.
                auto y{40.0};
                for (int tick{0}; tick < 120 && !player->dead; ++tick) {
                    y = std::max(16.0, y - 0.5);
                    const auto verdict{level.submit_player_move(
                        *player,
                        sim::player_move_request{.x = 0.5,
                                                 .y = y,
                                                 .z = 0.5,
                                                 .on_ground = y <= 16.0})};
                    expect(verdict == sim::player_move_verdict::accepted)
                        << tick;
                    level.tick();
                }
                expect(player->dead);
                expect(player->last_damage == sim::damage_source::fall);
            });
        };

    "sim/player/drowning drains air and hurts"_test = [] {
        with_world([](sim::world &level) {
            const auto water{level.registry().intern("minecraft:water")};
            for (std::int16_t y{16}; y <= 20; ++y) {
                expect(level.set_block({0, y, 0}, water).has_value());
            }
            auto *player{add_player(level, 0.5, 16.0, 0.5)};
            expect(player != nullptr);
            if (player == nullptr) {
                return;
            }
            // The eye is under water from the first tick on; a nearly empty
            // supply reaches the drowning threshold within the run.
            player->air_supply = 1;
            for (int tick{0}; tick < 60 && player->health == 20.0F; ++tick) {
                level.tick();
            }
            expect(player->health < 20.0F) << player->health;
            expect(player->last_damage == sim::damage_source::drown);
        });
    };

    "sim/player/the void hurts and bypasses the immunity window"_test = [] {
        with_world([](sim::world &level) {
            auto *player{add_player(level)};
            expect(player != nullptr);
            if (player == nullptr) {
                return;
            }
            level.set_player_position(
                *player, 0.5,
                static_cast<double>(sim::world::build_min_y()) -
                    2.0 * sim::player_void_depth,
                0.5);
            for (int tick{0}; tick < 20 && !player->dead; ++tick) {
                level.tick();
            }
            expect(player->dead);
            expect(player->last_damage == sim::damage_source::out_of_world);
        });
    };

    "sim/player/an empty hunger bar starves the player"_test = [] {
        with_world([](sim::world &level) {
            auto *player{add_player(level)};
            expect(player != nullptr);
            if (player == nullptr) {
                return;
            }
            player->food_level = 0;
            player->saturation = 0.0F;
            player->exhaustion = 0.0F;
            player->health = 15.0F;
            for (int tick{0}; tick < 160 && player->health == 15.0F; ++tick) {
                level.tick();
            }
            expect(player->health < 15.0F) << player->health;
            expect(player->last_damage == sim::damage_source::starve);
        });
    };

    "sim/player/hunger drains with exhaustion and regenerates health"_test =
        [] {
            with_world([](sim::world &level) {
                auto *player{add_player(level)};
                expect(player != nullptr);
                if (player == nullptr) {
                    return;
                }
                level.tick();
                expect(player->food_level == 20);
                player->exhaustion = 5.0F;
                level.tick();
                // Saturation absorbs the first reduction; food drops only
                // once saturation is empty.
                expect(player->saturation == 4.0F) << player->saturation;
                expect(player->food_level == 20) << player->food_level;
                player->saturation = 0.0F;
                player->exhaustion = 5.0F;
                level.tick();
                expect(player->food_level == 19) << player->food_level;
                // Missing health regenerates while saturation and food are
                // high; the first band heals after ten ticks.
                player->health = 15.0F;
                player->saturation = 5.0F;
                player->food_level = 20;
                for (int tick{0}; tick < 10; ++tick) {
                    level.tick();
                }
                expect(player->health > 15.0F) << player->health;
            });
        };

    "sim/player/creative ignores damage and can fly"_test = [] {
        with_world([](sim::world &level) {
            auto *player{add_player(level)};
            expect(player != nullptr);
            if (player == nullptr) {
                return;
            }
            sim::set_player_game_mode(level, *player, sim::game_mode_creative);
            expect(!sim::damage_player(level, *player, sim::damage_source::fall,
                                       10.0F));
            expect(player->health == 20.0F);
            player->flying = true;
            player->input = sim::player_input::jump;
            const auto start_y{player->y};
            for (int tick{0}; tick < 20; ++tick) {
                level.tick();
            }
            expect(player->y > start_y + 1.0) << player->y;
            expect(!player->on_ground);
        });
    };

    "sim/player/spectator ignores collision and damage"_test = [] {
        with_world([](sim::world &level) {
            const auto stone{*level.registry().find("minecraft:stone")};
            for (std::int16_t y{16}; y <= 18; ++y) {
                expect(level.set_block({1, y, 0}, stone).has_value());
            }
            auto *player{add_player(level)};
            expect(player != nullptr);
            if (player == nullptr) {
                return;
            }
            sim::set_player_game_mode(level, *player, sim::game_mode_spectator);
            const auto verdict{level.submit_player_move(
                *player,
                sim::player_move_request{
                    .x = 1.5, .y = 16.0, .z = 0.5, .on_ground = false})};
            expect(verdict == sim::player_move_verdict::accepted);
            level.tick();
            expect(std::abs(player->x - 1.5) < 1.0e-9) << player->x;
            expect(!sim::damage_player(level, *player, sim::damage_source::fall,
                                       10.0F));
            expect(!player->dead);
        });
    };

    "sim/player/death and respawn restore the survival state"_test = [] {
        with_world([](sim::world &level) {
            auto *player{add_player(level)};
            expect(player != nullptr);
            if (player == nullptr) {
                return;
            }
            for (int tick{0}; tick < 30; ++tick) {
                level.tick();
            }
            expect(sim::damage_player(
                level, *player, sim::damage_source::player_attack, 25.0F));
            expect(player->dead);
            expect(player->health == 0.0F);
            for (int tick{0}; tick < 20; ++tick) {
                level.tick();
            }
            expect(player->death_time == 20) << player->death_time;
            level.respawn_player(*player, 0.5, 16.0, 0.5, 90.0F, 0.0F);
            expect(!player->dead);
            expect(player->health == 20.0F);
            expect(player->food_level == 20);
            expect(player->saturation == 5.0F);
            expect(player->yaw == 90.0F);
            expect(
                level.damage_player(*player, sim::damage_source::fall, 25.0F));
            expect(player->dead);
        });
    };

    "sim/player/game mode change updates the rules immediately"_test = [] {
        with_world([](sim::world &level) {
            auto *player{add_player(level)};
            expect(player != nullptr);
            if (player == nullptr) {
                return;
            }
            expect(sim::damage_player(
                level, *player, sim::damage_source::player_attack, 10.0F));
            expect(player->health == 10.0F);
            sim::set_player_game_mode(level, *player, sim::game_mode_creative);
            expect(!sim::damage_player(
                level, *player, sim::damage_source::player_attack, 10.0F));
            expect(player->health == 10.0F);
            sim::set_player_game_mode(level, *player, sim::game_mode_survival);
            expect(!player->flying);
            expect(sim::damage_player(level, *player,
                                      sim::damage_source::player_attack, 1.0F));
            expect(player->health == 9.0F);
        });
    };

    "sim/player/experience curve matches the vanilla level thresholds"_test =
        [] {
            sim::player_state player{};
            sim::add_experience(player, 7);
            expect(player.experience_level == 1) << player.experience_level;
            expect(player.experience_progress == 0.0F);
            sim::add_experience(player, 9);
            expect(player.experience_level == 2) << player.experience_level;
            expect(player.total_experience == 16);
            expect(player.experience_progress == 0.0F);
            sim::add_experience(player, 1);
            expect(player.experience_level == 2);
            expect(player.experience_progress > 0.0F);
            expect(sim::experience_for_level(16) == 352);
            expect(sim::experience_for_level(31) == 1507);
        };

    "sim/player/fixed input produces the same position twice"_test = [] {
        with_world([](sim::world &level) {
            auto *player{add_player(level)};
            expect(player != nullptr);
            if (player == nullptr) {
                return;
            }
            const auto run = [&level, player] {
                player->input =
                    sim::player_input::forward | sim::player_input::jump;
                for (int tick{0}; tick < 40; ++tick) {
                    level.tick();
                }
                return std::array<double, 3>{player->x, player->y, player->z};
            };
            const auto first{run()};
            // Reset every field the movement reads, then replay the same
            // input; the simulation has no wall-clock or RNG dependency.
            player->x = 0.5;
            player->y = 16.0;
            player->z = 0.5;
            player->vx = 0.0;
            player->vy = 0.0;
            player->vz = 0.0;
            player->yaw = 0.0F;
            player->pitch = 0.0F;
            player->on_ground = false;
            player->fall_distance = 0.0;
            player->ground_y = 16.0;
            player->dead = false;
            player->input = sim::player_input::none;
            const auto second{run()};
            expect(first[0] == second[0]) << first[0] << " " << second[0];
            expect(first[1] == second[1]) << first[1] << " " << second[1];
            expect(first[2] == second[2]) << first[2] << " " << second[2];
        });
    };
};
