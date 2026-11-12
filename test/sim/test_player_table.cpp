// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/checksum.hpp>
#include <refrain/sim/player.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/store.hpp>

#include <boost/ut.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;

namespace data = refrain::data;
namespace fs = std::filesystem;
namespace sim = refrain::sim;

[[nodiscard]] auto scratch_directory() -> fs::path {
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    auto path{fs::temp_directory_path() /
              ("refrain-player-table-" + std::to_string(stamp))};
    std::error_code error{};
    fs::create_directories(path, error);
    return path;
}

/// A store with no chunks is enough for the player table, the player tick and
/// the state hash's player section.
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
                                   std::string name, double x, double y,
                                   double z, std::uint32_t game_mode = 0)
    -> sim::entity_id {
    auto added{world.add_player(sim::player_table::registration{
        .connection = connection,
        .name = std::move(name),
        .world = "overworld",
        .x = x,
        .y = y,
        .z = z,
        .game_mode = game_mode,
    })};
    return added ? *added : 0;
}

} // namespace

suite player_table_tests = [] {
    "sim/players are addressed by entity id in ascending order"_test = [] {
        sim::player_table table{};
        const auto bob{table.add(sim::player_table::registration{
            .connection = 2, .name = "Bob", .world = "overworld"})};
        const auto alice{table.add(sim::player_table::registration{
            .connection = 1, .name = "Alice", .world = "overworld"})};
        expect(bob.has_value());
        expect(alice.has_value());
        if (!bob || !alice) {
            return;
        }
        expect(*bob == 1U);
        expect(*alice == 2U);

        std::vector<sim::entity_id> order{};
        for (const auto &[id, value] : table.records()) {
            static_cast<void>(value);
            order.push_back(id);
        }
        expect(order == std::vector<sim::entity_id>{*bob, *alice});
        expect(table.find(*alice)->name == "Alice");
        expect(table.find_by_connection(2)->name == "Bob");
        expect(table.find_by_name("Alice")->id == *alice);
        expect(table.first()->id == *bob);

        // A connection owns at most one player, and a name is required.
        expect(!table
                    .add(sim::player_table::registration{.connection = 2,
                                                         .name = "Dup"})
                    .has_value());
        expect(!table
                    .add(sim::player_table::registration{.connection = 3,
                                                         .name = ""})
                    .has_value());
        expect(table.size() == 2U);
        expect(table.remove(1));
        expect(!table.remove(1));
        expect(table.size() == 1U);
        expect(table.find(*alice) == nullptr);
    };

    "sim/players and spawned entities share one entity id sequence"_test = [] {
        const auto directory{scratch_directory()};
        auto world{open_empty_world(directory)};
        expect(world.has_value());
        if (!world) {
            std::error_code error;
            fs::remove_all(directory, error);
            return;
        }
        expect(register_player(*world, 1, "Alice", 0.0, 64.0, 0.0) == 1U);
        const auto pig{world->spawn_entity("minecraft:pig", 0.5, 64.0, 0.5)};
        expect(pig.has_value());
        if (pig) {
            expect(*pig == 2U);
        }
        expect(register_player(*world, 2, "Bob", 0.0, 64.0, 0.0) == 3U);
        expect(world->player(1) != nullptr);
        expect(world->player(3) != nullptr);
        // Entity 2 is the pig, not a player.
        expect(world->player(2) == nullptr);
        expect(world->entity(*pig) != nullptr);
        std::error_code error;
        fs::remove_all(directory, error);
    };

    "sim/state hash covers every player record in entity id order"_test = [] {
        const auto directory{scratch_directory()};
        auto world{open_empty_world(directory)};
        expect(world.has_value());
        if (!world) {
            std::error_code error;
            fs::remove_all(directory, error);
            return;
        }
        expect(register_player(*world, 1, "Alice", 1.0, 64.0, 2.0) == 1U);
        expect(register_player(*world, 2, "Bob", 10.0, 64.0, 20.0) == 2U);
        auto *alice{world->player(1)};
        auto *bob{world->player(2)};
        expect(alice != nullptr);
        expect(bob != nullptr);
        if (alice == nullptr || bob == nullptr) {
            return;
        }
        const auto base{data::to_hex(world->state_hash())};

        // Changing any field of the second record must change the hash; a hash
        // that covers only the first player would miss every one of these.
        bob->x = 11.0;
        const auto moved_hash{data::to_hex(world->state_hash())};
        expect(moved_hash != base);
        bob->game_mode = 1;
        expect(data::to_hex(world->state_hash()) != moved_hash);
        const auto mode_hash{data::to_hex(world->state_hash())};
        expect(world->add_item(*bob, "minecraft:stone", 5).has_value());
        expect(data::to_hex(world->state_hash()) != mode_hash);
        const auto inventory_hash{data::to_hex(world->state_hash())};
        alice->yaw = 90.0F;
        expect(data::to_hex(world->state_hash()) != inventory_hash);
        const auto rotation_hash{data::to_hex(world->state_hash())};

        // The other record is untouched by a mutation of the first.
        expect(bob->yaw == 0.0F);
        expect(rotation_hash != inventory_hash);
        std::error_code error;
        fs::remove_all(directory, error);
    };

    "sim/the same player records hash the same in two runs"_test = [] {
        const auto first_directory{scratch_directory()};
        const auto second_directory{scratch_directory()};
        auto first{open_empty_world(first_directory)};
        auto second{open_empty_world(second_directory)};
        expect(first.has_value());
        expect(second.has_value());
        if (!first || !second) {
            std::error_code error;
            fs::remove_all(first_directory, error);
            fs::remove_all(second_directory, error);
            return;
        }
        for (auto *world : {&*first, &*second}) {
            expect(register_player(*world, 1, "Alice", 1.0, 64.0, 2.0) == 1U);
            expect(register_player(*world, 2, "Bob", 10.0, 64.0, 20.0) == 2U);
            expect(world->add_item(*world->player(2), "minecraft:dirt", 3)
                       .has_value());
            world->tick();
        }
        expect(data::to_hex(first->state_hash()) ==
               data::to_hex(second->state_hash()));
        std::error_code error;
        fs::remove_all(first_directory, error);
        fs::remove_all(second_directory, error);
    };

    "sim/tick applies gravity to every player record"_test = [] {
        const auto directory{scratch_directory()};
        auto world{open_empty_world(directory)};
        expect(world.has_value());
        if (!world) {
            std::error_code error;
            fs::remove_all(directory, error);
            return;
        }
        expect(register_player(*world, 1, "Alice", 0.5, 30.0, 0.5) == 1U);
        expect(register_player(*world, 2, "Bob", 0.5, 40.0, 0.5) == 2U);
        auto *alice{world->player(1)};
        auto *bob{world->player(2)};
        expect(alice != nullptr);
        expect(bob != nullptr);
        if (alice == nullptr || bob == nullptr) {
            return;
        }
        for (int tick{0}; tick < 3; ++tick) {
            world->tick();
        }
        expect(alice->y < 30.0);
        expect(bob->y < 40.0);
        expect(alice->y != 30.0);
        expect(bob->y != 40.0);
        // Moving one record leaves the other untouched.
        const auto bob_before{bob->y};
        expect(world->move_player(*alice, 1.0, 0.0, 0.0));
        expect(alice->x > 0.5);
        expect(bob->y == bob_before);
        std::error_code error;
        fs::remove_all(directory, error);
    };
};
