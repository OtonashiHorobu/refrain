// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/console_commands.hpp>

#include <refrain/core/log.hpp>
#include <refrain/core/time.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/server/console_service.hpp>
#include <refrain/sim/player.hpp>
#include <refrain/sim/replay.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/store.hpp>

#include <boost/ut.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using namespace boost::ut;

namespace data = refrain::data;
namespace fs = std::filesystem;
namespace sim = refrain::sim;
namespace server = refrain::server;

[[nodiscard]] auto scratch_directory() -> fs::path {
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    auto path{fs::temp_directory_path() /
              ("refrain-console-player-" + std::to_string(stamp))};
    std::error_code error{};
    fs::create_directories(path, error);
    return path;
}

[[nodiscard]] auto read_text(const fs::path &path) -> std::string {
    std::ifstream in{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{in},
                       std::istreambuf_iterator<char>{}};
}

[[nodiscard]] auto open_empty_world(const fs::path &directory)
    -> std::optional<sim::world> {
    auto store{refrain::world::world_store::create(directory.string())};
    if (!store) {
        return std::nullopt;
    }
    if (!store->commit()) {
        return std::nullopt;
    }
    auto opened{sim::world::open(directory.string())};
    if (!opened) {
        return std::nullopt;
    }
    return std::optional<sim::world>{std::move(*opened)};
}

[[nodiscard]] auto register_player(sim::world &world, std::uint64_t connection,
                                   std::string_view name, double x, double y,
                                   double z, std::uint32_t game_mode = 1)
    -> sim::entity_id {
    auto added{world.add_player(sim::player_table::registration{
        .connection = connection,
        .name = std::string{name},
        .world = "overworld",
        .x = x,
        .y = y,
        .z = z,
        .game_mode = game_mode,
    })};
    return added ? *added : 0;
}

/// Two clients plus the synthetic console player, registered in the same order
/// the server uses so the console default stays the synthetic record.
[[nodiscard]] auto make_world_with_players(const fs::path &directory)
    -> std::optional<sim::world> {
    auto world{open_empty_world(directory)};
    if (!world) {
        return std::nullopt;
    }
    expect(register_player(*world, sim::synthetic_connection, "@console", 0.5,
                           64.0, 0.5) == 1U);
    expect(register_player(*world, 1, "Alice", 1.5, 64.0, 0.5) == 2U);
    expect(register_player(*world, 2, "Bob", 10.5, 64.0, 0.5) == 3U);
    return world;
}

[[nodiscard]] auto find_player(sim::world &world, std::string_view name)
    -> sim::player_state * {
    return world.players().find_by_name(name);
}

} // namespace

suite console_player_tests = [] {
    "server/pos selects the named player"_test = [] {
        const auto directory{scratch_directory()};
        auto world{make_world_with_players(directory)};
        expect(world.has_value());
        if (!world) {
            std::error_code error;
            fs::remove_all(directory, error);
            return;
        }
        auto *console_player{find_player(*world, "@console")};
        auto *alice{find_player(*world, "Alice")};
        auto *bob{find_player(*world, "Bob")};
        expect(console_player != nullptr);
        expect(alice != nullptr);
        expect(bob != nullptr);
        if (console_player == nullptr || alice == nullptr || bob == nullptr) {
            return;
        }

        server::tick_metrics metrics{};
        server::console_commands commands{
            server::console_dependencies{.world = &*world}, metrics};
        const refrain::core::tick_info tick{};

        // Capture the report through the logger's file sink: the command still
        // writes its line through the normal server logger.
        const auto log_path = directory / "console.log";
        refrain::log::shutdown();
        refrain::log::config cfg{};
        cfg.console_level = refrain::log::level::off;
        cfg.file_path = log_path.string();
        cfg.file_level = refrain::log::level::info;
        cfg.enable_colors = false;
        expect(static_cast<bool>(refrain::log::init(cfg)));
        commands.execute("pos Bob", tick);
        commands.execute("pos", tick);
        commands.execute("pos Carol", tick);
        refrain::log::shutdown();
        const auto text{read_text(log_path)};
        expect(text.find("Bob x=10.5 y=64.0 z=0.5") != std::string::npos)
            << text;
        // Without a name the command reports the first player by entity id.
        expect(text.find("@console x=0.5 y=64.0 z=0.5") != std::string::npos)
            << text;
        expect(text.find("no player") != std::string::npos) << text;
        // Reporting never moves the record.
        expect(bob->x == 10.5) << bob->x;
        expect(console_player->x == 0.5) << console_player->x;
        std::error_code error;
        fs::remove_all(directory, error);
    };

    "server/walk moves only the named player"_test = [] {
        const auto directory{scratch_directory()};
        auto world{make_world_with_players(directory)};
        expect(world.has_value());
        if (!world) {
            std::error_code error;
            fs::remove_all(directory, error);
            return;
        }
        server::tick_metrics metrics{};
        server::console_commands commands{
            server::console_dependencies{.world = &*world}, metrics};
        const refrain::core::tick_info tick{};
        auto *console_player{find_player(*world, "@console")};
        auto *alice{find_player(*world, "Alice")};
        auto *bob{find_player(*world, "Bob")};
        expect(console_player != nullptr);
        expect(alice != nullptr);
        expect(bob != nullptr);
        if (console_player == nullptr || alice == nullptr || bob == nullptr) {
            return;
        }

        commands.execute("walk Alice 2 0", tick);
        expect(alice->x == 3.5) << alice->x;
        expect(alice->z == 0.5) << alice->z;
        expect(bob->x == 10.5) << bob->x;
        expect(console_player->x == 0.5) << console_player->x;

        // Without a name the command addresses the first player by entity id,
        // which is the synthetic record the replay driver owns.
        commands.execute("walk 1 0", tick);
        expect(console_player->x == 1.5) << console_player->x;
        expect(alice->x == 3.5) << alice->x;

        commands.execute("move Bob 1 2 3", tick);
        expect(bob->x == 11.5) << bob->x;
        expect(bob->y == 66.0) << bob->y;
        expect(bob->z == 3.5) << bob->z;
        expect(alice->x == 3.5) << alice->x;

        commands.execute("give Bob minecraft:dirt 5", tick);
        expect(bob->inventory.size() == 1U);
        if (!bob->inventory.empty()) {
            expect(bob->inventory.front().count == 5U);
            expect(bob->inventory.front().item == "minecraft:dirt");
        }
        expect(alice->inventory.empty());

        // A name that matches no record leaves every record untouched.
        commands.execute("walk Carol 4 4", tick);
        expect(alice->x == 3.5) << alice->x;
        expect(bob->x == 11.5) << bob->x;
        std::error_code error;
        fs::remove_all(directory, error);
    };

    "server/a replay drives the synthetic player and hashes deterministically"_test =
        [] {
            const auto first_directory{scratch_directory()};
            const auto second_directory{scratch_directory()};
            const std::vector<sim::replay_step> steps{
                {.tick = 0, .command = "walk 2 0"},
                {.tick = 1, .command = "give minecraft:dirt 5"},
                {.tick = 2, .command = "jump"},
                {.tick = 3, .command = "hash"},
                {.tick = 4, .command = "move 1 0 0"},
                {.tick = 5, .command = "hash"},
            };
            struct replay_result {
                data::digest32 hash{};
                double console_x{0.0};
                std::uint32_t console_items{0};
                double alice_x{0.0};
            };
            const auto run =
                [&steps](
                    const fs::path &directory) -> std::optional<replay_result> {
                auto world{make_world_with_players(directory)};
                if (!world) {
                    return std::nullopt;
                }
                server::tick_metrics metrics{};
                server::console_commands commands{
                    server::console_dependencies{.world = &*world}, metrics};
                server::console_service console{
                    [&commands](std::string_view line,
                                const refrain::core::tick_info &tick) {
                        commands.execute(line, tick);
                    }};
                console.load_replay(steps);
                std::atomic<bool> stop{false};
                replay_result result{};
                for (std::int64_t ticks_run{0}; ticks_run <= 6; ++ticks_run) {
                    console.tick(refrain::core::tick_info{.index = ticks_run},
                                 ticks_run, stop);
                    if (ticks_run == 5) {
                        result.hash = world->state_hash();
                    }
                    world->tick();
                }
                const auto *console_player{find_player(*world, "@console")};
                const auto *alice{find_player(*world, "Alice")};
                if (console_player == nullptr || alice == nullptr) {
                    return std::nullopt;
                }
                result.console_x = console_player->x;
                result.console_items =
                    world->item_count(*console_player, "minecraft:dirt");
                result.alice_x = alice->x;
                return result;
            };

            const auto first{run(first_directory)};
            const auto second{run(second_directory)};
            expect(first.has_value());
            expect(second.has_value());
            if (!first || !second) {
                std::error_code error;
                fs::remove_all(first_directory, error);
                fs::remove_all(second_directory, error);
                return;
            }
            expect(first->console_x == second->console_x);
            expect(first->console_items == second->console_items);
            expect(first->alice_x == second->alice_x);
            // The script names no player, so it must keep addressing the
            // synthetic record even with two clients registered.
            expect(first->console_x == 3.5) << first->console_x;
            expect(first->console_items == 5U) << first->console_items;
            expect(first->alice_x == 1.5) << first->alice_x;
            expect(data::to_hex(first->hash) == data::to_hex(second->hash));
            std::error_code error;
            fs::remove_all(first_directory, error);
            fs::remove_all(second_directory, error);
        };
};
