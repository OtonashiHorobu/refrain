// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The world and player command batch against a borrowed in-process world:
// block writes, region edits, inventory, teleport, chat and persistence, plus
// the permission declarations and the bridge's explicit denial.

#include <refrain/command/command.hpp>
#include <refrain/contracts/services/permissions.hpp>
#include <refrain/net/server.hpp>
#include <refrain/server/command_bridge.hpp>
#include <refrain/sim/item.hpp>
#include <refrain/sim/player.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/model.hpp>
#include <refrain/world/store.hpp>

#include <support/permission_policy_provider.hpp>

#include <boost/ut.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace command = refrain::command;
namespace fs = std::filesystem;
namespace net = refrain::net;
namespace server = refrain::server;
namespace sim = refrain::sim;
namespace test_support = refrain::test_support;
namespace world_ns = refrain::world;

constexpr std::uint64_t alice_connection{1};
constexpr std::uint64_t bob_connection{2};
constexpr std::uint64_t alice_key{11};
constexpr std::uint64_t bob_key{12};
constexpr std::string_view alice_uuid{"11111111-1111-1111-1111-111111111111"};
constexpr std::string_view bob_uuid{"22222222-2222-2222-2222-222222222222"};

[[nodiscard]] auto scratch_directory(std::string_view name) -> fs::path {
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    auto path{fs::temp_directory_path() /
              ("refrain-world-command-" + std::to_string(stamp) + "-" +
               std::string{name})};
    std::error_code error{};
    fs::create_directories(path, error);
    return path;
}

/// One full section of stone, so every command has a known non-air block and a
/// known empty one above it.
[[nodiscard]] auto flat_chunk() -> world_ns::chunk {
    world_ns::chunk chunk{};
    chunk.format_version = world_ns::chunk_format_version;
    chunk.status = refrain::storage::chunk_status::full;
    chunk.y_pos = 0;
    refrain::storage::section_record section{};
    section.y = 0;
    section.kind = refrain::storage::section_kind::single_value;
    section.bits_per_entry = 0;
    section.block_count = static_cast<std::uint16_t>(world_ns::section_volume);
    section.biome_kind = refrain::storage::section_kind::single_value;
    section.biome_bits_per_entry = 0;
    section.biome_palette.push_back("minecraft:plains");
    section.palette.push_back(refrain::storage::palette_entry{
        .key = "minecraft:stone",
        .count = static_cast<std::uint16_t>(world_ns::section_volume),
        .state = 0,
    });
    chunk.sections.push_back(std::move(section));
    return chunk;
}

[[nodiscard]] auto open_flat_world(const fs::path &directory)
    -> std::optional<sim::world> {
    auto store{world_ns::world_store::create(directory.string())};
    if (!store) {
        return std::nullopt;
    }
    for (const world_ns::chunk_pos pos :
         {world_ns::chunk_pos{0, 0}, world_ns::chunk_pos{1, 0}}) {
        auto column{flat_chunk()};
        if (!store->write_chunk(pos, column)) {
            return std::nullopt;
        }
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

void install_items(sim::world &level) {
    sim::item_table items{};
    items.insert("minecraft:stone", {.max_stack_size = 64, .max_damage = 0});
    items.insert("minecraft:diamond_block",
                 {.max_stack_size = 64, .max_damage = 0});
    items.insert("minecraft:gold_block",
                 {.max_stack_size = 64, .max_damage = 0});
    items.insert("minecraft:glass", {.max_stack_size = 64, .max_damage = 0});
    items.insert("minecraft:apple", {.max_stack_size = 64, .max_damage = 0});
    items.insert("minecraft:potion", {.max_stack_size = 1, .max_damage = 0});
    level.set_item_table(std::move(items));
}

void register_players(sim::world &level) {
    expect(static_cast<bool>(level.add_player(sim::player_table::registration{
        .connection = alice_connection,
        .name = "Alice",
        .world = "minecraft:overworld",
        .x = 5.5,
        .y = 64.0,
        .z = 5.5,
        .game_mode = sim::game_mode_creative,
        .inventory_slots = 36,
    })));
    expect(static_cast<bool>(level.add_player(sim::player_table::registration{
        .connection = bob_connection,
        .name = "Bob",
        .world = "minecraft:overworld",
        .x = 0.5,
        .y = 64.0,
        .z = 0.5,
        .game_mode = sim::game_mode_survival,
        .inventory_slots = 36,
    })));
    if (auto *alice{level.players().find_by_name("Alice")}; alice != nullptr) {
        alice->yaw = 90.0F;
        alice->pitch = 10.0F;
    }
    if (auto *bob{level.players().find_by_name("Bob")}; bob != nullptr) {
        bob->yaw = 0.0F;
        bob->pitch = 0.0F;
    }
}

/// One world plus the command environment the runtime installs over it. The
/// world is only borrowed by the port, so it is declared before the port and
/// outlives it.
struct fixture {
    fs::path directory{};
    std::optional<sim::world> level{};
    std::unique_ptr<server::world_port> port{};
    std::shared_ptr<server::command_environment> environment{};
    net::command_bridge bridge{};
    std::vector<std::string> replies{};

    explicit fixture(
        std::string_view name,
        test_support::permission_policy_provider *provider = nullptr) {
        directory = scratch_directory(name);
        level = open_flat_world(directory);
        if (!level) {
            return;
        }
        install_items(*level);
        register_players(*level);
        port = server::make_borrowed_world_port(*level);
        // Adopt the two fixture columns so a failed command can be inspected
        // without a successful command having loaded the column first.
        for (const world_ns::chunk_pos pos :
             {world_ns::chunk_pos{0, 0}, world_ns::chunk_pos{1, 0}}) {
            expect(static_cast<bool>(port->chunk_load(pos)));
        }
        environment = std::make_shared<server::command_environment>();
        environment->world = port.get();
        if (provider != nullptr) {
            server::bind_permissions(environment, provider->vtable(),
                                     provider->self());
        }
        bridge = server::make_command_bridge(environment);
    }

    fixture(const fixture &) = delete;
    auto operator=(const fixture &) -> fixture & = delete;
    ~fixture() {
        std::error_code error{};
        fs::remove_all(directory, error);
    }

    [[nodiscard]] auto ok() const -> bool { return level.has_value(); }

    void remember(std::uint64_t key, std::uint64_t connection,
                  std::string_view uuid) {
        environment->remember_key(key, connection, std::string{uuid},
                                  "minecraft:overworld");
    }

    /// Run one command through the dispatcher as one of the fixture players.
    [[nodiscard]] auto
    execute(std::string_view input,
            command::permission_level ladder = command::level::admin,
            std::uint64_t key = alice_key,
            std::uint64_t connection = alice_connection,
            std::string_view name = "Alice")
        -> refrain::result<command::execution> {
        remember(key, connection, key == bob_key ? bob_uuid : alice_uuid);
        replies.clear();
        command::player_source source{
            std::string{name}, key, ladder,
            [this](std::string_view message) { replies.emplace_back(message); },
            [] -> std::vector<std::string> { return {"Alice", "Bob"}; }};
        return environment->dispatcher.execute(input, source);
    }

    [[nodiscard]] auto identity(std::uint64_t connection, std::string_view name,
                                std::string_view uuid_text) const
        -> net::player_identity {
        auto parsed{net::uuid::parse(uuid_text)};
        expect(static_cast<bool>(parsed));
        return net::player_identity{
            .connection = connection,
            .id = parsed ? *parsed : net::uuid{},
            .name = name,
            .world = "minecraft:overworld",
        };
    }

    [[nodiscard]] auto block_key_at(std::int32_t x, std::int32_t y,
                                    std::int32_t z) const -> std::string {
        return std::string{port->block_key(port->block_at(sim::block_pos{
            .x = x, .y = static_cast<std::int16_t>(y), .z = z}))};
    }

    [[nodiscard]] auto player(std::string_view name) -> sim::player_state * {
        return level->players().find_by_name(name);
    }

    [[nodiscard]] auto replies_contain(std::string_view needle) const -> bool {
        for (const auto &reply : replies) {
            if (reply.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

template <typename TTree>
[[nodiscard]] auto tree_has_literal(const TTree &tree, std::string_view name)
    -> bool {
    for (const auto &node : tree.nodes) {
        if (node.name == name) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] auto suggestion_texts(const net::suggestion_result &result)
    -> std::vector<std::string> {
    std::vector<std::string> out{};
    out.reserve(result.entries.size());
    for (const auto &entry : result.entries) {
        out.push_back(entry.text);
    }
    return out;
}

[[nodiscard]] auto contains(const std::vector<std::string> &values,
                            std::string_view needle) -> bool {
    for (const auto &value : values) {
        if (value == needle) {
            return true;
        }
    }
    return false;
}

constexpr std::string_view granted_policy_text{R"(
[defaults]
default = "deny"
nodes = ["refrain.command.help", "refrain.command.list", "refrain.command.me", "refrain.command.msg"]

[grants]
"player:11111111-1111-1111-1111-111111111111" = ["refrain.command.setblock"]
)"};

} // namespace

const suite command_world_block_tests = [] {
    "commands/setblock replaces a block, keeps an empty one and refuses a full one"_test =
        [] {
            fixture world{"setblock"};
            expect(world.ok());
            if (!world.ok()) {
                return;
            }

            const auto placed{
                world.execute("setblock 2 0 2 minecraft:diamond_block")};
            expect(static_cast<bool>(placed))
                << (placed ? "" : placed.error().message);
            if (placed) {
                expect(placed->success);
            }
            expect(world.block_key_at(2, 0, 2) == "minecraft:diamond_block");
            expect(world.replies_contain("Placed minecraft:diamond_block at 2, "
                                         "0, 2"))
                << world.replies.front();

            const auto kept{
                world.execute("setblock 2 0 2 minecraft:glass keep")};
            expect(!kept);
            if (!kept) {
                expect(kept.error().message.find("not empty") !=
                       std::string::npos)
                    << kept.error().message;
            }
            expect(world.block_key_at(2, 0, 2) == "minecraft:diamond_block");

            const auto empty{
                world.execute("setblock 3 64 3 minecraft:glass keep")};
            expect(static_cast<bool>(empty));
            expect(world.block_key_at(3, 64, 3) == "minecraft:glass");
        };

    "commands/setblock rejects invalid states, bounds, chunks and local positions"_test =
        [] {
            fixture world{"setblock-errors"};
            expect(world.ok());
            if (!world.ok()) {
                return;
            }

            const auto bad_state{
                world.execute("setblock 1 0 1 minecraft:stone[")};
            expect(!bad_state);
            if (!bad_state) {
                expect(bad_state.error().message.find(
                           "not a valid block state") != std::string::npos)
                    << bad_state.error().message;
            }

            const auto out_of_world{
                world.execute("setblock 1 1024 1 minecraft:stone")};
            expect(!out_of_world);
            if (!out_of_world) {
                expect(out_of_world.error().message.find("build height") !=
                       std::string::npos)
                    << out_of_world.error().message;
            }

            const auto unloaded{
                world.execute("setblock 1000 0 1000 minecraft:stone")};
            expect(!unloaded);
            if (!unloaded) {
                expect(unloaded.error().message.find("not loaded") !=
                       std::string::npos)
                    << unloaded.error().message;
            }

            const auto local{world.execute("setblock ^ ^ ^ minecraft:stone")};
            expect(!local);
            if (!local) {
                expect(local.error().message.find("cannot mix") !=
                       std::string::npos)
                    << local.error().message;
            }

            const auto missing{world.execute("setblock 0 0 0")};
            expect(!missing);
            if (!missing) {
                expect(missing.error().message.find("expected argument") !=
                       std::string::npos)
                    << missing.error().message;
            }
        };

    "commands/setblock resolves relative coordinates from the source"_test =
        [] {
            fixture world{"setblock-relative"};
            expect(world.ok());
            if (!world.ok()) {
                return;
            }

            // Alice stands at (5.5, 64.0, 5.5), so `~ ~ ~` is the block (5, 64,
            // 5).
            const auto placed{
                world.execute("setblock ~ ~ ~ minecraft:gold_block")};
            expect(static_cast<bool>(placed))
                << (placed ? "" : placed.error().message);
            expect(world.block_key_at(5, 64, 5) == "minecraft:gold_block");

            const auto offset{
                world.execute("setblock ~1.9 ~ ~-0.9 minecraft:glass")};
            expect(static_cast<bool>(offset))
                << (offset ? "" : offset.error().message);
            expect(world.block_key_at(7, 64, 4) == "minecraft:glass");
        };

    "commands/fill replaces, keeps, hollows and outlines a region"_test = [] {
        fixture world{"fill"};
        expect(world.ok());
        if (!world.ok()) {
            return;
        }

        const auto replaced{
            world.execute("fill 0 0 0 2 0 2 minecraft:gold_block")};
        expect(static_cast<bool>(replaced))
            << (replaced ? "" : replaced.error().message);
        if (replaced) {
            expect(replaced->value == 9);
        }
        expect(world.replies_contain("Filled 9 blocks"))
            << world.replies.front();
        for (std::int32_t x{0}; x <= 2; ++x) {
            for (std::int32_t z{0}; z <= 2; ++z) {
                expect(world.block_key_at(x, 0, z) == "minecraft:gold_block")
                    << x << "," << z;
            }
        }

        // `keep` fills only the empty positions of the region; a second run
        // has nothing left to change and fails like the reference.
        const auto kept{
            world.execute("fill 0 20 0 1 20 1 minecraft:glass keep")};
        expect(static_cast<bool>(kept));
        if (kept) {
            expect(kept->value == 4);
        }
        const auto again{
            world.execute("fill 0 20 0 1 20 1 minecraft:glass keep")};
        expect(!again);
        if (!again) {
            expect(again.error().message.find("No blocks were filled") !=
                   std::string::npos)
                << again.error().message;
        }

        const auto hollow{
            world.execute("fill 0 30 0 2 32 2 minecraft:glass hollow")};
        expect(static_cast<bool>(hollow))
            << (hollow ? "" : hollow.error().message);
        if (hollow) {
            expect(hollow->value == 26);
        }
        expect(world.block_key_at(0, 30, 0) == "minecraft:glass");
        expect(world.block_key_at(2, 32, 2) == "minecraft:glass");
        expect(world.block_key_at(1, 31, 1) == "minecraft:air");

        const auto outline{
            world.execute("fill 5 30 5 7 32 7 minecraft:gold_block outline")};
        expect(static_cast<bool>(outline))
            << (outline ? "" : outline.error().message);
        if (outline) {
            expect(outline->value == 26);
        }
        expect(world.block_key_at(5, 30, 5) == "minecraft:gold_block");
        expect(world.block_key_at(6, 31, 6) == "minecraft:air");
    };

    "commands/fill rejects oversized, out-of-world and unloaded regions"_test =
        [] {
            fixture world{"fill-errors"};
            expect(world.ok());
            if (!world.ok()) {
                return;
            }

            const auto too_big{
                world.execute("fill 0 0 0 32 32 32 minecraft:stone")};
            expect(!too_big);
            if (!too_big) {
                expect(too_big.error().message.find("Too many blocks") !=
                       std::string::npos)
                    << too_big.error().message;
                expect(too_big.error().message.find("35937") !=
                       std::string::npos)
                    << too_big.error().message;
            }
            // A rejected region must not be partially applied.
            expect(world.block_key_at(0, 0, 0) == "minecraft:stone");
            expect(world.block_key_at(31, 0, 0) == "minecraft:stone");

            const auto too_high{
                world.execute("fill 0 1023 0 0 1024 0 minecraft:stone")};
            expect(!too_high);
            if (!too_high) {
                expect(too_high.error().message.find("build height") !=
                       std::string::npos)
                    << too_high.error().message;
            }

            const auto unloaded{
                world.execute("fill 1000 0 1000 1001 0 1001 minecraft:stone")};
            expect(!unloaded);
            if (!unloaded) {
                expect(unloaded.error().message.find("not loaded") !=
                       std::string::npos)
                    << unloaded.error().message;
            }
        };

    "commands/clone copies, masks air and snapshots an overlapping source"_test =
        [] {
            fixture world{"clone"};
            expect(world.ok());
            if (!world.ok()) {
                return;
            }

            const auto prepared{
                world.execute("setblock 0 0 0 minecraft:gold_block")};
            expect(static_cast<bool>(prepared));

            const auto cloned{world.execute("clone 0 0 0 1 0 0 5 0 0")};
            expect(static_cast<bool>(cloned))
                << (cloned ? "" : cloned.error().message);
            expect(world.block_key_at(5, 0, 0) == "minecraft:gold_block");
            expect(world.block_key_at(6, 0, 0) == "minecraft:stone");

            // Masked copies the non-air blocks only, so the destination air
            // slot keeps its previous state.
            expect(static_cast<bool>(
                world.execute("fill 20 16 10 21 16 10 minecraft:stone")));
            expect(static_cast<bool>(
                world.execute("setblock 10 16 10 minecraft:gold_block")));
            const auto masked{
                world.execute("clone 10 16 10 11 16 10 20 16 10 masked")};
            expect(static_cast<bool>(masked))
                << (masked ? "" : masked.error().message);
            expect(world.block_key_at(20, 16, 10) == "minecraft:gold_block");
            expect(world.block_key_at(21, 16, 10) == "minecraft:stone");

            // Replace copies everything, so the source air clears the
            // destination.
            const auto replaced{
                world.execute("clone 10 16 10 11 16 10 20 16 10 replace")};
            expect(static_cast<bool>(replaced))
                << (replaced ? "" : replaced.error().message);
            expect(world.block_key_at(21, 16, 10) == "minecraft:air");

            // An overlapping clone reads the source once, so the copied gold
            // block lands in the destination instead of being read back after
            // the first write.
            expect(static_cast<bool>(
                world.execute("fill 0 40 0 3 40 0 minecraft:stone")));
            expect(static_cast<bool>(
                world.execute("setblock 0 40 0 minecraft:gold_block")));
            const auto overlap{world.execute("clone 0 40 0 3 40 0 1 40 0")};
            expect(static_cast<bool>(overlap))
                << (overlap ? "" : overlap.error().message);
            expect(world.block_key_at(1, 40, 0) == "minecraft:gold_block");
            expect(world.block_key_at(2, 40, 0) == "minecraft:stone");
            expect(world.block_key_at(0, 40, 0) == "minecraft:gold_block");
        };

    "commands/clone rejects an oversized source area"_test = [] {
        fixture world{"clone-errors"};
        expect(world.ok());
        if (!world.ok()) {
            return;
        }
        const auto too_big{world.execute("clone 0 0 0 32 32 32 0 60 0")};
        expect(!too_big);
        if (!too_big) {
            expect(too_big.error().message.find("Too many blocks") !=
                   std::string::npos)
                << too_big.error().message;
        }
    };
};

const suite command_world_player_tests = [] {
    "commands/give writes the inventory and fails closed on bad input"_test =
        [] {
            fixture world{"give"};
            expect(world.ok());
            if (!world.ok()) {
                return;
            }

            const auto given{world.execute("give Alice minecraft:apple 5")};
            expect(static_cast<bool>(given))
                << (given ? "" : given.error().message);
            if (given) {
                expect(given->value == 5);
            }
            expect(world.replies_contain("Gave 5 minecraft:apple to Alice"))
                << world.replies.front();
            const auto *alice{world.player("Alice")};
            expect(alice != nullptr);
            if (alice != nullptr) {
                expect(world.level->item_count(*alice, "minecraft:apple") ==
                       5U);
            }

            // A bare id takes the default namespace and a missing count is one.
            const auto one{world.execute("give Bob stone")};
            expect(static_cast<bool>(one)) << (one ? "" : one.error().message);
            const auto *bob{world.player("Bob")};
            expect(bob != nullptr);
            if (bob != nullptr) {
                expect(world.level->item_count(*bob, "minecraft:stone") == 1U);
            }

            const auto unknown{
                world.execute("give Alice minecraft:not_a_real_item")};
            expect(!unknown);
            if (!unknown) {
                expect(unknown.error().message.find("Unknown item") !=
                       std::string::npos)
                    << unknown.error().message;
            }

            const auto too_many{
                world.execute("give Alice minecraft:apple 6401")};
            expect(!too_many);
            if (!too_many) {
                expect(too_many.error().message.find(
                           "Cannot give more than 6400") != std::string::npos)
                    << too_many.error().message;
            }

            // One-stack items still allow 100 stacks.
            const auto potion{world.execute("give Alice minecraft:potion 101")};
            expect(!potion);
            if (!potion) {
                expect(potion.error().message.find(
                           "Cannot give more than 100") != std::string::npos)
                    << potion.error().message;
            }

            const auto zero{world.execute("give Alice minecraft:apple 0")};
            expect(!zero);
            if (!zero) {
                expect(zero.error().message.find("below the minimum") !=
                       std::string::npos)
                    << zero.error().message;
            }

            const auto nobody{world.execute("give Carol minecraft:apple")};
            expect(!nobody);
            if (!nobody) {
                expect(nobody.error().message.find(
                           "No player is named 'Carol'") != std::string::npos)
                    << nobody.error().message;
            }
        };

    "commands/tp moves a player and keeps the reference coordinate forms"_test =
        [] {
            fixture world{"tp"};
            expect(world.ok());
            if (!world.ok()) {
                return;
            }

            // Absolute integer X/Z name the block centre; Y does not.
            const auto absolute{world.execute("tp 10 64 20")};
            expect(static_cast<bool>(absolute))
                << (absolute ? "" : absolute.error().message);
            auto *alice{world.player("Alice")};
            expect(alice != nullptr);
            if (alice == nullptr) {
                return;
            }
            expect(alice->x == 10.5 && alice->y == 64.0 && alice->z == 20.5);
            expect(world.replies_contain("Teleported Alice to 10.5, 64, 20.5"))
                << world.replies.front();

            expect(static_cast<bool>(world.execute("tp 10.25 65 20.75")));
            expect(alice->x == 10.25 && alice->y == 65.0 && alice->z == 20.75);

            expect(static_cast<bool>(world.execute("tp ~1 ~ ~-1")));
            expect(alice->x == 11.25 && alice->y == 65.0 && alice->z == 19.75);

            // Bob's rotation is zero, so his local forwards axis is +Z and
            // `^ ^ ^2` moves him two blocks south.
            const auto local{world.execute("tp ^ ^ ^2", command::level::admin,
                                           bob_key, bob_connection, "Bob")};
            expect(static_cast<bool>(local))
                << (local ? "" : local.error().message);
            auto *bob{world.player("Bob")};
            expect(bob != nullptr);
            if (bob != nullptr) {
                expect(std::abs(bob->x - 0.5) < 1.0e-9 && bob->y == 64.0 &&
                       bob->z == 2.5)
                    << bob->x << "," << bob->z;
            }

            // A player destination moves the source and adopts its rotation.
            bob->yaw = 180.0F;
            bob->pitch = 45.0F;
            const auto to_player{world.execute("tp Bob", command::level::admin,
                                               alice_key, alice_connection,
                                               "Alice")};
            expect(static_cast<bool>(to_player))
                << (to_player ? "" : to_player.error().message);
            expect(std::abs(alice->x - 0.5) < 1.0e-9 &&
                   std::abs(alice->z - 2.5) < 1.0e-9)
                << alice->x << "," << alice->z;
            expect(alice->yaw == 180.0F && alice->pitch == 45.0F);

            // A named target destination moves the target to the other player.
            const auto target_to_player{
                world.execute("tp Alice Bob", command::level::admin, bob_key,
                              bob_connection, "Bob")};
            expect(static_cast<bool>(target_to_player))
                << (target_to_player ? "" : target_to_player.error().message);

            // The target-first position form moves the target.
            const auto target_to_position{
                world.execute("tp Alice 1 2 3", command::level::admin, bob_key,
                              bob_connection, "Bob")};
            expect(static_cast<bool>(target_to_position))
                << (target_to_position ? ""
                                       : target_to_position.error().message);
            expect(alice->x == 1.5 && alice->y == 2.0 && alice->z == 3.5);
        };

    "commands/tp resolves local offsets in the source rotation"_test = [] {
        fixture world{"tp-local-rotation"};
        expect(world.ok());
        if (!world.ok()) {
            return;
        }
        auto *alice{world.player("Alice")};
        expect(alice != nullptr);
        if (alice == nullptr) {
            return;
        }
        // Alice faces west (yaw 90): the reference's local forwards axis is
        // -X, so one block forwards lands at x = -0.5.
        alice->x = 0.5;
        alice->y = 64.0;
        alice->z = 0.5;
        alice->yaw = 90.0F;
        alice->pitch = 0.0F;
        const auto moved{world.execute("tp ^ ^ ^1")};
        expect(static_cast<bool>(moved))
            << (moved ? "" : moved.error().message);
        expect(std::abs(alice->x + 0.5) < 1.0e-9) << alice->x;
        expect(std::abs(alice->z - 0.5) < 1.0e-9) << alice->z;
        expect(std::abs(alice->y - 64.0) < 1.0e-9) << alice->y;
    };

    "commands/tp rejects an unknown target and an out-of-world destination"_test =
        [] {
            fixture world{"tp-errors"};
            expect(world.ok());
            if (!world.ok()) {
                return;
            }

            const auto nobody{world.execute("tp Carol")};
            expect(!nobody);
            if (!nobody) {
                expect(nobody.error().message.find(
                           "No player is named 'Carol'") != std::string::npos)
                    << nobody.error().message;
            }

            const auto outside{world.execute("tp 30000000 64 0")};
            expect(!outside);
            if (!outside) {
                expect(outside.error().message.find("outside the world") !=
                       std::string::npos)
                    << outside.error().message;
            }

            const auto tall{world.execute("tp 0 20000000 0")};
            expect(!tall);
            if (!tall) {
                expect(tall.error().message.find("outside the world") !=
                       std::string::npos)
                    << tall.error().message;
            }

            // Two tokens parse as the target-first form with a second player
            // name, exactly like the reference's argument tree; the failure is
            // the missing player rather than a syntax error.
            const auto two_words{world.execute("tp 1 2")};
            expect(!two_words);
            if (!two_words) {
                expect(two_words.error().message.find(
                           "No player is named '1'") != std::string::npos)
                    << two_words.error().message;
            }
        };

    "commands/tp needs an entity for the no-target form"_test = [] {
        fixture world{"tp-console"};
        expect(world.ok());
        if (!world.ok()) {
            return;
        }
        world.replies.clear();
        command::console_source console{command::level::owner,
                                        [&world](std::string_view message) {
                                            world.replies.emplace_back(message);
                                        }};
        const auto moved{
            world.environment->dispatcher.execute("tp 1 2 3", console)};
        expect(!moved);
        if (!moved) {
            expect(moved.error().message.find("Only a player") !=
                   std::string::npos)
                << moved.error().message;
        }
    };

    "commands/me and msg report their text and reject an unknown target"_test =
        [] {
            fixture world{"chat"};
            expect(world.ok());
            if (!world.ok()) {
                return;
            }

            const auto me{world.execute("me waves")};
            expect(static_cast<bool>(me)) << (me ? "" : me.error().message);
            expect(world.replies_contain("* Alice waves"))
                << world.replies.front();

            const auto whisper{world.execute("msg Bob hello there")};
            expect(static_cast<bool>(whisper))
                << (whisper ? "" : whisper.error().message);
            expect(world.replies_contain("You whisper to Bob: hello there"))
                << world.replies.front();

            const auto tell{world.execute("tell Bob hello again")};
            expect(static_cast<bool>(tell))
                << (tell ? "" : tell.error().message);
            expect(world.replies_contain("You whisper to Bob: hello again"))
                << world.replies.front();

            const auto nobody{world.execute("msg Carol hi")};
            expect(!nobody);
            if (!nobody) {
                expect(nobody.error().message.find(
                           "No player is named 'Carol'") != std::string::npos)
                    << nobody.error().message;
            }

            const auto empty{world.execute("me")};
            expect(!empty);
            if (!empty) {
                expect(empty.error().message.find("expected argument") !=
                       std::string::npos)
                    << empty.error().message;
            }
        };

    "commands/save-all flushes the world and stays behind the owner level"_test =
        [] {
            fixture world{"save-all"};
            expect(world.ok());
            if (!world.ok()) {
                return;
            }

            expect(static_cast<bool>(
                world.execute("save-all", command::level::owner)));
            expect(world.replies_contain("Saved the world"))
                << world.replies.front();
            expect(world.port->store_valid());

            expect(static_cast<bool>(
                world.execute("save-all flush", command::level::owner)));

            const auto denied{world.execute("save-all", command::level::admin)};
            expect(!denied);
            if (!denied) {
                expect(denied.error().message.find("unknown command") !=
                       std::string::npos)
                    << denied.error().message;
            }
        };
};

const suite command_world_permission_tests = [] {
    "commands/the new nodes are declared with their fallback levels"_test = [] {
        fixture world{"nodes"};
        expect(world.ok());
        if (!world.ok()) {
            return;
        }
        const auto &dispatcher{world.environment->dispatcher};
        struct declaration {
            std::string_view command;
            std::string_view node;
            command::permission_level fallback;
        };
        const std::array<declaration, 10> declarations{{
            {"teleport", "refrain.command.teleport", command::level::gamemode},
            {"tp", "refrain.command.teleport", command::level::gamemode},
            {"setblock", "refrain.command.setblock", command::level::gamemode},
            {"fill", "refrain.command.fill", command::level::gamemode},
            {"clone", "refrain.command.clone", command::level::gamemode},
            {"give", "refrain.command.give", command::level::gamemode},
            {"me", "refrain.command.me", command::level::everyone},
            {"msg", "refrain.command.msg", command::level::everyone},
            {"tell", "refrain.command.msg", command::level::everyone},
            {"save-all", "refrain.command.save-all", command::level::owner},
        }};
        for (const auto &entry : declarations) {
            const auto declared{dispatcher.permission_of(entry.command)};
            expect(declared.node == entry.node) << entry.command;
            expect(declared.fallback == entry.fallback) << entry.command;
        }
    };

    "commands/a player below the fallback level cannot run the new commands"_test =
        [] {
            fixture world{"fallback"};
            expect(world.ok());
            if (!world.ok()) {
                return;
            }
            for (const auto *input :
                 {"teleport 0 64 0", "tp 0 64 0",
                  "setblock 0 0 0 minecraft:stone",
                  "fill 0 0 0 0 0 0 minecraft:stone", "clone 0 0 0 0 0 0 0 0 0",
                  "give Alice minecraft:apple", "save-all"}) {
                const auto executed{
                    world.execute(input, command::level::everyone)};
                expect(!executed) << input;
                if (!executed) {
                    expect(executed.error().message.find("unknown command") !=
                           std::string::npos)
                        << input << ": " << executed.error().message;
                }
            }
            // The public chat commands stay available to every player.
            expect(static_cast<bool>(
                world.execute("me waves", command::level::everyone)));
            expect(static_cast<bool>(
                world.execute("msg Bob hi", command::level::everyone)));
        };

    "commands/the bridge grants and denies the new nodes through the provider"_test =
        [] {
            test_support::permission_policy_provider provider{};
            expect(provider.reload(granted_policy_text));
            fixture world{"bridge-permissions", &provider};
            expect(world.ok());
            if (!world.ok()) {
                return;
            }

            const auto alice{
                world.identity(alice_connection, "Alice", alice_uuid)};
            const auto bob{world.identity(bob_connection, "Bob", bob_uuid)};

            const auto alice_tree{world.bridge.tree(alice)};
            expect(alice_tree != nullptr);
            if (alice_tree != nullptr) {
                expect(tree_has_literal(*alice_tree, "setblock"));
                expect(!tree_has_literal(*alice_tree, "fill"));
                expect(tree_has_literal(*alice_tree, "me"));
            }
            const auto bob_tree{world.bridge.tree(bob)};
            expect(bob_tree != nullptr);
            if (bob_tree != nullptr) {
                expect(!tree_has_literal(*bob_tree, "setblock"));
                expect(tree_has_literal(*bob_tree, "me"));
            }

            const auto granted{world.bridge.execute(
                alice, "setblock 4 0 4 minecraft:gold_block")};
            expect(granted.handled);
            expect(granted.success) << granted.message;
            expect(world.block_key_at(4, 0, 4) == "minecraft:gold_block");

            const auto denied{world.bridge.execute(
                bob, "setblock 4 0 5 minecraft:gold_block")};
            expect(denied.handled);
            expect(!denied.success);
            expect(denied.message.find("refrain.command.setblock") !=
                   std::string::npos)
                << denied.message;
            expect(world.block_key_at(4, 0, 5) == "minecraft:stone");

            // Completion below a hidden root offers nothing for the guest.
            const auto suggestions{world.bridge.suggest(bob, "setblock ", 9)};
            expect(suggestions.entries.empty());
        };
};

const suite command_world_tree_tests = [] {
    "commands/the tree and completion carry the new commands"_test = [] {
        fixture world{"tree"};
        expect(world.ok());
        if (!world.ok()) {
            return;
        }
        // The bridge falls back to the environment's development level when no
        // permission provider is active; /save-all needs the owner level.
        world.environment->permission = command::level::owner;
        const auto alice{world.identity(alice_connection, "Alice", alice_uuid)};
        const auto tree{world.bridge.tree(alice)};
        expect(tree != nullptr);
        if (tree == nullptr) {
            return;
        }
        for (const auto *name :
             {"tp", "teleport", "setblock", "fill", "clone", "give", "me",
              "msg", "tell", "w", "save-all"}) {
            expect(tree_has_literal(*tree, name)) << name;
        }

        // The teleport position argument offers the coordinate templates.
        const auto coordinates{world.bridge.suggest(alice, "tp ", 3)};
        const auto offered{suggestion_texts(coordinates)};
        expect(contains(offered, "~ ~ ~")) << offered.size();
        const auto local_coordinates{world.bridge.suggest(alice, "tp ^", 4)};
        const auto local_offered{suggestion_texts(local_coordinates)};
        expect(contains(local_offered, "^ ^ ^")) << local_offered.size();

        // The setblock mode literals complete after the block argument.
        const std::string setblock_line{"setblock 1 2 3 minecraft:stone "};
        const auto modes{world.bridge.suggest(
            alice, setblock_line,
            static_cast<std::int32_t>(setblock_line.size()))};
        const auto mode_texts{suggestion_texts(modes)};
        expect(contains(mode_texts, "keep")) << mode_texts.size();
        expect(contains(mode_texts, "replace")) << mode_texts.size();

        // A parse failure reaches the client with a readable message.
        const auto missing{world.bridge.execute(alice, "fill 0 0 0")};
        expect(missing.handled);
        expect(!missing.success);
        expect(missing.message.find("expected argument") != std::string::npos)
            << missing.message;
        expect(missing.message.find("position") != std::string::npos)
            << missing.message;
    };
};
