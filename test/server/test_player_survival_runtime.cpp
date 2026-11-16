// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/platform/process.hpp>
#include <refrain/server/server_runtime.hpp>
#include <refrain/server/world_port.hpp>
#include <refrain/sim/player_physics.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/create.hpp>
#include <refrain/world/store.hpp>

#include <boost/ut.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace sim = refrain::sim;
namespace storage = refrain::storage;
namespace world_ns = refrain::world;

[[nodiscard]] auto scratch_directory() -> std::filesystem::path {
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    auto path{std::filesystem::temp_directory_path() /
              ("refrain-survival-" +
               std::to_string(refrain::platform::process_id()) + "-" +
               std::to_string(stamp))};
    std::error_code error{};
    std::filesystem::create_directories(path, error);
    return path;
}

/// Stone from y=0 to y=15; the level spawn is y=64, so a player falls 48
/// blocks to the floor unless a console command moves it first.
[[nodiscard]] auto flat_chunk() -> storage::chunk {
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

/// Create a world the runtime can open: a level record, one generated column
/// and the store index committed.
[[nodiscard]] auto create_flat_world(const std::filesystem::path &root)
    -> bool {
    if (!world_ns::create_world(root.string(), 0, "world")) {
        return false;
    }
    auto store{world_ns::world_store::open(root.string())};
    if (!store) {
        return false;
    }
    if (!store->write_chunk({0, 0}, flat_chunk()) || !store->commit()) {
        return false;
    }
    return true;
}

/// Handles that select the direct port. The runtime never routes a call
/// through the vtables on that path, so non-null dummy pointers carry the
/// selection and the provider ids name the first-party implementations.
struct direct_handles {
    int dummy{0};
    refrain::server::server_service_handles handles{
        .world =
            reinterpret_cast<const refrain_services_world_vtable *>(&dummy),
        .world_self = &dummy,
        .sim = reinterpret_cast<const refrain_services_sim_vtable *>(&dummy),
        .sim_self = &dummy,
        .storage =
            reinterpret_cast<const refrain_services_storage_vtable *>(&dummy),
        .storage_self = &dummy,
        .world_provider = "refrain.world.vanilla",
        .sim_provider = "refrain.sim.vanilla",
        .storage_provider = "refrain.storage.rwf",
    };
};

/// Run the server runtime over a fresh flat world with `commands` executed on
/// the first tick, then return the simulation record of the console player.
struct runtime_result {
    int exit_code{1};
    bool have_record{false};
    sim::player_state record{};
};

[[nodiscard]] auto run_runtime(const std::filesystem::path &root,
                               std::string_view commands, std::int64_t ticks)
    -> runtime_result {
    runtime_result result{};
    const auto world_path{root / "world"};
    if (!create_flat_world(world_path)) {
        return result;
    }
    const auto command_file{root / "commands.txt"};
    {
        std::ofstream stream{command_file, std::ios::binary | std::ios::trunc};
        stream << commands;
    }
    direct_handles handles{};
    auto port{refrain::server::make_world_port(handles.handles)};
    if (!port) {
        return result;
    }
    refrain::server::server_run_options options{};
    options.world_path = world_path.string();
    options.ticks = ticks;
    options.command_file = command_file.string();
    result.exit_code =
        refrain::server::run_server(options, handles.handles, *port);
    auto *sim_level{port->sim_world()};
    if (sim_level == nullptr) {
        return result;
    }
    if (const auto *record{sim_level->players().first()}; record != nullptr) {
        result.have_record = true;
        result.record = *record;
    }
    return result;
}

} // namespace

suite player_survival_runtime_tests = [] {
    "runtime/falling hurts and lands the console player"_test = [] {
        const auto directory{scratch_directory()};
        // Spawn is y=64; the relative move puts the player at y=34, an
        // 18-block fall onto the stone floor, which costs 15 health. The run
        // stops right after the landing so natural regeneration cannot heal
        // the damage back.
        const auto result{run_runtime(directory,
                                      "gamemode survival\n"
                                      "move 0 -30 0\n",
                                      26)};
        expect(result.exit_code == 0) << result.exit_code;
        expect(result.have_record);
        if (result.have_record) {
            expect(result.record.game_mode == sim::game_mode_survival);
            expect(result.record.dead == false);
            expect(result.record.on_ground);
            expect(std::abs(result.record.health - 5.0F) < 1.0e-3)
                << result.record.health;
            expect(result.record.last_damage == sim::damage_source::fall);
        }
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    };

    "runtime/gamemode creative makes the player immune immediately"_test = [] {
        const auto directory{scratch_directory()};
        const auto result{run_runtime(directory,
                                      "gamemode creative\n"
                                      "damage 100\n",
                                      20)};
        expect(result.exit_code == 0) << result.exit_code;
        expect(result.have_record);
        if (result.have_record) {
            expect(result.record.game_mode == sim::game_mode_creative);
            expect(!result.record.dead);
            expect(result.record.health == 20.0F);
        }
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    };

    "runtime/damage kills and the death state is kept for the respawn"_test =
        [] {
            const auto directory{scratch_directory()};
            const auto result{run_runtime(directory,
                                          "gamemode survival\n"
                                          "food 10\n"
                                          "xp 7\n"
                                          "damage 100\n",
                                          5)};
            expect(result.exit_code == 0) << result.exit_code;
            expect(result.have_record);
            if (result.have_record) {
                expect(result.record.dead);
                expect(result.record.health == 0.0F);
                expect(result.record.food_level == 10);
                // The death loses the level (vanilla drops it as orbs).
                expect(result.record.total_experience == 0);
                expect(result.record.death_time > 0);
            }
            std::error_code error;
            std::filesystem::remove_all(directory, error);
        };

    "runtime/experience and food commands update the survival record"_test =
        [] {
            const auto directory{scratch_directory()};
            const auto result{run_runtime(directory,
                                          "gamemode survival\n"
                                          "xp 16\n"
                                          "food 12\n",
                                          5)};
            expect(result.exit_code == 0) << result.exit_code;
            expect(result.have_record);
            if (result.have_record) {
                expect(result.record.experience_level == 2)
                    << result.record.experience_level;
                expect(result.record.total_experience == 16);
                expect(result.record.food_level == 12);
            }
            std::error_code error;
            std::filesystem::remove_all(directory, error);
        };
};
