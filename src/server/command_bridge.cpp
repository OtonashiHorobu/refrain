// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/command_bridge.hpp>

#include <refrain/app/mod_management.hpp>
#include <refrain/command/parsers.hpp>
#include <refrain/permissions/client.hpp>
#include <refrain/server/command_tree_wire.hpp>

#include "command_world_commands.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace refrain::server {

auto player_identity_key(const net::uuid &id) -> std::uint64_t {
    std::uint64_t key{1469598103934665603ULL};
    for (const auto byte : id.bytes) {
        key ^= byte;
        key *= 1099511628211ULL;
    }
    return key;
}

namespace {

[[nodiscard]] auto identity_key(const net::player_identity &identity)
    -> std::uint64_t {
    return player_identity_key(identity.id);
}

/// The command module's typed game mode and the network layer's enum enumerate
/// the same four modes in the same order; keep the conversion explicit so a
/// renumbering on either side fails to compile instead of silently mapping.
[[nodiscard]] auto to_net_mode(command::game_mode mode) -> net::game_mode {
    switch (mode) {
    case command::game_mode::survival:
        return net::game_mode::survival;
    case command::game_mode::creative:
        return net::game_mode::creative;
    case command::game_mode::adventure:
        return net::game_mode::adventure;
    case command::game_mode::spectator:
        return net::game_mode::spectator;
    }
    return net::game_mode::survival;
}

[[nodiscard]] auto make_source(const std::shared_ptr<command_environment> &env,
                               const net::player_identity &identity)
    -> command::player_source {
    const auto key{identity_key(identity)};
    env->remember_key(key, identity.connection, identity.id.to_string(),
                      std::string{identity.world});
    auto online = [env]() -> std::vector<std::string> {
        std::vector<std::string> names{};
        if (env->listener == nullptr) {
            return names;
        }
        for (auto &player : env->listener->player_names()) {
            names.push_back(std::move(player.name));
        }
        return names;
    };
    // An active permission provider makes the policy authoritative, and no
    // persisted operator list supplies a real vanilla level yet, so the
    // subject's op level is zero. Without a provider the development level is
    // what the declared node fallbacks compare against.
    const auto ladder{env->permission_query ? command::level::everyone
                                            : env->permission};
    return command::player_source{
        std::string{identity.name}, key, ladder,
        [env, connection = identity.connection](std::string_view message) {
            if (env->listener != nullptr) {
                env->listener->send_system_chat(connection, message);
            }
        },
        std::move(online)};
}

/// Fingerprint of the command source a tree is filtered for: the display name
/// (a policy may grant by profile name), the vanilla ladder level (the
/// fallback when no provider is active) and the opaque identity key.
[[nodiscard]] auto subject_fingerprint(const command::source &source)
    -> std::uint64_t {
    std::uint64_t hash{1469598103934665603ULL};
    for (const char character : source.name()) {
        hash ^=
            static_cast<std::uint64_t>(static_cast<unsigned char>(character));
        hash *= 1099511628211ULL;
    }
    hash ^= 0xFFU;
    hash *= 1099511628211ULL;
    hash ^= static_cast<std::uint64_t>(source.permission());
    hash *= 1099511628211ULL;
    hash ^= source.identity().key;
    return hash;
}

/// First whitespace-delimited word of a command line, used to answer an
/// explicit permission denial for a command the tree hides from this source.
[[nodiscard]] auto first_word(std::string_view input) -> std::string_view {
    while (!input.empty() && input.front() == ' ') {
        input.remove_prefix(1);
    }
    const auto end{input.find(' ')};
    return input.substr(0, end);
}

/// Run one shared mod management command for the calling player and deliver
/// its lines and failure through the command feedback path.
[[nodiscard]] auto
execute_mods_command(const std::shared_ptr<command_environment> &environment,
                     command::context &context, std::vector<std::string> tokens)
    -> refrain::result<int> {
    if (environment->mods == nullptr) {
        context.fail("Mod management is unavailable");
        return std::unexpected{refrain::make_error(
            refrain::error_code::unsupported, "mod management is unavailable")};
    }
    app::mod_management_actor actor{};
    actor.kind = app::mod_management_actor_kind::player;
    actor.name = std::string{context.source().name()};
    if (const auto uuid{environment->uuid_for(context.source().identity().key)};
        uuid.has_value()) {
        actor.uuid = *uuid;
    }
    std::vector<std::string_view> arguments{};
    arguments.reserve(tokens.size());
    for (const auto &token : tokens) {
        arguments.push_back(token);
    }
    const auto report{environment->mods->execute(arguments, actor)};
    for (const auto &line : report.lines) {
        context.reply(line);
    }
    if (!report.ok) {
        // `fail` already delivers the message through the source sink; the
        // returned value only marks the execution as failed, so the bridge does
        // not send the reason a second time.
        context.fail(report.error);
        return 1;
    }
    return 1;
}

} // namespace

void bind_permissions(std::shared_ptr<command_environment> environment,
                      const refrain_services_permissions_vtable &vtable,
                      void *self) {
    if (!environment || vtable.query == nullptr) {
        return;
    }
    const refrain::permissions::client client{vtable, self};
    command_environment *const raw{environment.get()};
    environment->permission_query =
        [client, raw](const command::source &source,
                      std::string_view node) -> command::permission_decision {
        // The subject and its strings must outlive the provider call: the ABI
        // borrows them, so the uuid is owned here rather than in the branch
        // that resolves it.
        std::string uuid_text{};
        refrain::permissions::subject who{};
        if (!source.identity().player) {
            // The server console holds every node; the resolver answers that
            // itself, but building the subject here keeps the command module
            // free of the policy.
            who = refrain::permissions::subject::console();
        } else {
            const auto uuid{raw->uuid_for(source.identity().key)};
            if (!uuid.has_value()) {
                // A source the bridge never saw join has no canonical profile
                // uuid, so the provider cannot identify it. The declared
                // op-level fallback decides.
                return command::permission_decision::not_handled;
            }
            uuid_text = *uuid;
            who = refrain::permissions::subject::player(
                uuid_text, source.name(),
                static_cast<std::uint32_t>(source.permission()));
        }
        const auto is_player{source.identity().player};
        const auto player_world{
            is_player ? raw->world_for(source.identity().key) : std::string{}};
        const auto player_gamemode{
            is_player ? raw->gamemode_name(source.identity().key)
                      : std::string{}};
        const refrain::permissions::context query_context{
            .world = player_world, .gamemode = player_gamemode, .extra = {}};
        switch (client.query(who, node, query_context)) {
        case refrain::permissions::permission_result::allowed:
            return command::permission_decision::allowed;
        case refrain::permissions::permission_result::denied:
            return command::permission_decision::denied;
        case refrain::permissions::permission_result::not_handled:
            break;
        }
        return command::permission_decision::not_handled;
    };
    environment->permission_revision = [client] { return client.revision(); };
}

auto command_environment::join_state(std::uint64_t connection) const
    -> net::join_state {
    const std::lock_guard lock{mode_mutex};
    if (const auto found{modes.find(connection)}; found != modes.end()) {
        return net::join_state{.mode = found->second};
    }
    return net::join_state{.mode = default_mode};
}

auto command_environment::mode_of(std::uint64_t connection) const
    -> net::game_mode {
    return join_state(connection).mode;
}

void command_environment::remember_key(std::uint64_t key,
                                       std::uint64_t connection,
                                       std::string uuid,
                                       std::string world_name) {
    const std::lock_guard lock{mode_mutex};
    key_to_connection[key] = connection;
    identity_uuids[key] = std::move(uuid);
    identity_worlds[key] = std::move(world_name);
}

auto command_environment::connection_for(std::uint64_t key) const
    -> std::optional<std::uint64_t> {
    const std::lock_guard lock{mode_mutex};
    if (const auto found{key_to_connection.find(key)};
        found != key_to_connection.end()) {
        return found->second;
    }
    return std::nullopt;
}

auto command_environment::uuid_for(std::uint64_t key) const
    -> std::optional<std::string> {
    const std::lock_guard lock{mode_mutex};
    if (const auto found{identity_uuids.find(key)};
        found != identity_uuids.end()) {
        return found->second;
    }
    return std::nullopt;
}

auto command_environment::world_for(std::uint64_t key) const -> std::string {
    const std::lock_guard lock{mode_mutex};
    if (const auto found{identity_worlds.find(key)};
        found != identity_worlds.end()) {
        return found->second;
    }
    return {};
}

auto command_environment::gamemode_name(std::uint64_t key) const
    -> std::string {
    const auto connection{connection_for(key)};
    if (!connection.has_value()) {
        return {};
    }
    switch (mode_of(*connection)) {
    case net::game_mode::survival:
        return "survival";
    case net::game_mode::creative:
        return "creative";
    case net::game_mode::adventure:
        return "adventure";
    case net::game_mode::spectator:
        return "spectator";
    }
    return "survival";
}

auto command_environment::query_permission(const command::source &source,
                                           std::string_view node) const
    -> command::permission_decision {
    if (!permission_query) {
        return command::permission_decision::not_handled;
    }
    return permission_query(source, node);
}

namespace {

/// Declare the mod-administration permission on one command node. The `/mods`
/// tree is gated by `refrain.mods.admin`; the op-level fallback keeps the
/// command usable in a composition without the permission service, and the
/// console is always allowed by its own source.
[[nodiscard]] auto gate_mod_node(command::node_ref node) -> command::node_ref {
    return node.require_node(app::mod_admin_permission, command::level::admin);
}

/// Register the in-game `/mods` tree. Every leaf hands its tokens to the
/// shared application service, so the console and the game report the same
/// lines and the same errors. Every node of the tree declares
/// `refrain.mods.admin`, so the tree snapshot, the completion list and the
/// execution path agree on who may use it; the bridge answers an explicit
/// denial when a player without the node types it.
void register_mod_commands(
    const std::shared_ptr<command_environment> &environment) {
    auto &dispatcher{environment->dispatcher};
    /// Attach an executor that runs `prefix [<argument>] [suffix...]` through
    /// the shared service. `argument` names a parser node the tokens take the
    /// value from; the argument entry stays visible on the node's children.
    const auto leaf = [environment](command::node_ref node,
                                    std::vector<std::string> prefix,
                                    std::string argument,
                                    std::vector<std::string> suffix = {}) {
        static_cast<void>(gate_mod_node(node));
        node.executes([environment, prefix = std::move(prefix),
                       argument = std::move(argument),
                       suffix = std::move(suffix)](
                          command::context &context) -> refrain::result<int> {
            auto tokens{prefix};
            if (!argument.empty()) {
                tokens.emplace_back(context.string(argument));
            }
            tokens.insert(tokens.end(), suffix.begin(), suffix.end());
            return execute_mods_command(environment, context,
                                        std::move(tokens));
        });
        return node;
    };

    auto mods{gate_mod_node(dispatcher.literal("mods"))};
    mods.executes(
        [environment](command::context &context) -> refrain::result<int> {
            return execute_mods_command(environment, context, {});
        });
    mods.then(leaf(dispatcher.literal("list"), {"list"}, {}));
    mods.then(leaf(dispatcher.literal("installed"), {"installed"}, {}));

    auto verify{gate_mod_node(dispatcher.literal("verify"))};
    verify.then(leaf(dispatcher.argument("package", command::word_arg()),
                     {"verify"}, "package"));
    mods.then(verify);

    auto install{gate_mod_node(dispatcher.literal("install"))};
    auto install_package{
        leaf(dispatcher.argument("package", command::word_arg()), {"install"},
             "package")};
    install_package.then(leaf(dispatcher.literal("confirm"), {"install"},
                              "package", {"confirm"}));
    install_package.then(leaf(dispatcher.literal("--dry-run"), {"install"},
                              "package", {"--dry-run"}));
    install.then(install_package);
    mods.then(install);

    auto uninstall{gate_mod_node(dispatcher.literal("uninstall"))};
    auto uninstall_id{leaf(dispatcher.argument("id", command::word_arg()),
                           {"uninstall"}, "id")};
    uninstall_id.then(
        leaf(dispatcher.literal("confirm"), {"uninstall"}, "id", {"confirm"}));
    uninstall_id.then(leaf(dispatcher.literal("--dry-run"), {"uninstall"}, "id",
                           {"--dry-run"}));
    uninstall.then(uninstall_id);
    mods.then(uninstall);

    auto reload{gate_mod_node(dispatcher.literal("reload"))};
    auto reload_id{
        leaf(dispatcher.argument("id", command::word_arg()), {"reload"}, "id")};
    reload_id.then(
        leaf(dispatcher.literal("--dry-run"), {"reload"}, "id", {"--dry-run"}));
    reload.then(reload_id);
    mods.then(reload);

    auto reload_data{gate_mod_node(dispatcher.literal("reload-data"))};
    auto reload_data_node{leaf(reload_data, {"reload-data"}, {})};
    reload_data_node.then(leaf(dispatcher.literal("--dry-run"), {"reload-data"},
                               {}, {"--dry-run"}));
    mods.then(reload_data_node);

    auto trust{gate_mod_node(dispatcher.literal("trust"))};
    trust.executes(
        [environment](command::context &context) -> refrain::result<int> {
            return execute_mods_command(environment, context, {"trust"});
        });
    trust.then(leaf(dispatcher.literal("list"), {"trust", "list"}, {}));
    auto pin{gate_mod_node(dispatcher.literal("pin"))};
    auto pin_id{leaf(dispatcher.argument("id", command::word_arg()),
                     {"trust", "pin"}, "id")};
    pin_id.then(leaf(dispatcher.literal("confirm"), {"trust", "pin"}, "id",
                     {"confirm"}));
    pin_id.then(leaf(dispatcher.literal("--dry-run"), {"trust", "pin"}, "id",
                     {"--dry-run"}));
    pin.then(pin_id);
    trust.then(pin);
    auto clear{gate_mod_node(dispatcher.literal("clear"))};
    auto clear_id{leaf(dispatcher.argument("id", command::word_arg()),
                       {"trust", "clear"}, "id")};
    clear_id.then(leaf(dispatcher.literal("confirm"), {"trust", "clear"}, "id",
                       {"confirm"}));
    clear_id.then(leaf(dispatcher.literal("--dry-run"), {"trust", "clear"},
                       "id", {"--dry-run"}));
    clear.then(clear_id);
    trust.then(clear);
    mods.then(trust);
}

} // namespace

void register_runtime_commands(
    std::shared_ptr<command_environment> environment) {
    if (!environment) {
        return;
    }
    auto &dispatcher{environment->dispatcher};

    auto help{dispatcher.literal("help").require_node("refrain.command.help")};
    help.executes([](command::context &context) -> refrain::result<int> {
        context.reply("Commands: /help, /list, /say <message>");
        return 1;
    });

    auto list{dispatcher.literal("list").require_node("refrain.command.list")};
    list.executes([environment](
                      command::context &context) -> refrain::result<int> {
        std::vector<std::string> names{};
        if (environment->listener != nullptr) {
            for (const auto &player : environment->listener->player_names()) {
                names.push_back(player.name);
            }
        }
        std::string reply{"Players (" + std::to_string(names.size()) + "): "};
        for (std::size_t index{0}; index < names.size(); ++index) {
            if (index != 0) {
                reply += ", ";
            }
            reply += names[index];
        }
        context.reply(reply);
        return static_cast<int>(names.size());
    });

    auto say{dispatcher.literal("say").require_node("refrain.command.say",
                                                    command::level::gamemode)};
    auto message{dispatcher.argument("message", command::greedy_arg())};
    message.executes(
        [environment](command::context &context) -> refrain::result<int> {
            const auto text{context.string("message")};
            if (environment->listener != nullptr) {
                std::string broadcast{"["};
                broadcast += context.source().name();
                broadcast += "] ";
                broadcast += text;
                environment->listener->broadcast_system_chat(broadcast);
            }
            return 1;
        });
    say.then(message);

    auto gamemode{dispatcher.literal("gamemode")
                      .require_node("refrain.command.gamemode",
                                    command::level::gamemode)};
    auto mode_argument{dispatcher.argument("mode", command::gamemode_arg())};
    mode_argument.executes(
        [environment](command::context &context) -> refrain::result<int> {
            const auto mode{context.gamemode("mode")};
            if (!mode.has_value()) {
                context.fail("Unknown game mode");
                return std::unexpected{make_error(error_code::invalid_argument,
                                                  "unknown game mode")};
            }
            const auto connection{
                environment->connection_for(context.source().identity().key)};
            if (!connection) {
                context.fail("Player is not in play");
                return std::unexpected{
                    make_error(error_code::not_found, "player not found")};
            }
            {
                const std::lock_guard lock{environment->mode_mutex};
                environment->modes[*connection] = to_net_mode(*mode);
            }
            // The simulation owns the game-mode rules; the network push only
            // tells the client what changed. Applying both in the same command
            // keeps damage, flight and collision rules effective on the next
            // tick.
            if (environment->world != nullptr) {
                if (auto *sim_level{environment->world->sim_world()};
                    sim_level != nullptr) {
                    if (auto *record{sim_level->players().find_by_connection(
                            *connection)};
                        record != nullptr) {
                        refrain::sim::set_player_game_mode(
                            *sim_level, *record,
                            static_cast<std::uint32_t>(
                                static_cast<std::uint8_t>(*mode)));
                    }
                }
            }
            if (environment->listener != nullptr) {
                environment->listener->set_player_game_mode(*connection,
                                                            to_net_mode(*mode));
            }
            context.reply(
                fmt::format("Set game mode to {}", command::to_string(*mode)));
            return 1;
        });
    gamemode.then(mode_argument);

    auto default_gamemode{dispatcher.literal("defaultgamemode")
                              .require_node("refrain.command.defaultgamemode",
                                            command::level::gamemode)};
    auto default_argument{dispatcher.argument("mode", command::gamemode_arg())};
    default_argument.executes(
        [environment](command::context &context) -> refrain::result<int> {
            const auto mode{context.gamemode("mode")};
            if (!mode.has_value()) {
                context.fail("Unknown game mode");
                return std::unexpected{make_error(error_code::invalid_argument,
                                                  "unknown game mode")};
            }
            if (environment->world == nullptr) {
                context.fail("No world is open");
                return std::unexpected{
                    make_error(error_code::not_found, "no world")};
            }
            auto settings{environment->world->read_gameplay()};
            if (!settings) {
                context.fail("Cannot read world gameplay settings");
                return std::unexpected{std::move(settings.error())};
            }
            settings->game_mode =
                static_cast<std::int32_t>(static_cast<std::uint8_t>(*mode));
            if (auto written{environment->world->write_gameplay(*settings)};
                !written) {
                context.fail("Cannot persist the default game mode");
                return std::unexpected{std::move(written.error())};
            }
            {
                const std::lock_guard lock{environment->mode_mutex};
                environment->default_mode = to_net_mode(*mode);
            }
            context.reply(fmt::format("Default game mode set to {}",
                                      command::to_string(*mode)));
            return 1;
        });
    default_gamemode.then(default_argument);

    auto difficulty{dispatcher.literal("difficulty")
                        .require_node("refrain.command.difficulty",
                                      command::level::gamemode)};
    auto difficulty_argument{
        dispatcher.argument("level", command::integer_arg(0, 3))};
    difficulty_argument.executes(
        [environment](command::context &context) -> refrain::result<int> {
            if (environment->world == nullptr) {
                context.fail("No world is open");
                return std::unexpected{
                    make_error(error_code::not_found, "no world")};
            }
            auto settings{environment->world->read_gameplay()};
            if (!settings) {
                context.fail("Cannot read world gameplay settings");
                return std::unexpected{std::move(settings.error())};
            }
            settings->difficulty =
                static_cast<std::int32_t>(context.integer("level"));
            if (auto written{environment->world->write_gameplay(*settings)};
                !written) {
                context.fail("Cannot persist the difficulty");
                return std::unexpected{std::move(written.error())};
            }
            if (environment->listener != nullptr) {
                environment->listener->set_difficulty(
                    static_cast<std::uint32_t>(settings->difficulty),
                    settings->difficulty_locked);
            }
            context.reply(std::string{"Difficulty set to "} +
                          std::to_string(settings->difficulty));
            return 1;
        });
    difficulty.then(difficulty_argument);

    auto hardcore{
        dispatcher.literal("hardcore")
            .require_node("refrain.command.hardcore", command::level::admin)};
    auto hardcore_argument{dispatcher.argument("enabled", command::bool_arg())};
    hardcore_argument.executes(
        [environment](command::context &context) -> refrain::result<int> {
            if (environment->world == nullptr) {
                context.fail("No world is open");
                return std::unexpected{
                    make_error(error_code::not_found, "no world")};
            }
            auto settings{environment->world->read_gameplay()};
            if (!settings) {
                context.fail("Cannot read world gameplay settings");
                return std::unexpected{std::move(settings.error())};
            }
            settings->hardcore = context.boolean("enabled");
            if (auto written{environment->world->write_gameplay(*settings)};
                !written) {
                context.fail("Cannot persist hardcore");
                return std::unexpected{std::move(written.error())};
            }
            if (environment->listener != nullptr) {
                environment->listener->set_hardcore(settings->hardcore);
            }
            context.reply(settings->hardcore ? "Hardcore enabled"
                                             : "Hardcore disabled");
            return 1;
        });
    hardcore.then(hardcore_argument);

    // /time: the daylight clock of the overworld. `set` accepts absolute
    // ticks and the reference's named times; `add`, `pause` and `resume`
    // mirror the reference's clock controls. The command is deliberately
    // unqualified: only the overworld clock is modelled.
    const auto publish_clock = [environment] {
        if (environment->listener == nullptr || environment->world == nullptr) {
            return;
        }
        const auto *clock{environment->world->clock()};
        if (clock == nullptr) {
            return;
        }
        const auto snapshot{clock->snapshot()};
        const auto weather{clock->weather()};
        environment->listener->set_world_state(net::world_state_snapshot{
            .game_time = snapshot.game_time,
            .clock_total_ticks = snapshot.total_ticks,
            .clock_partial_tick = snapshot.partial_tick,
            .clock_rate = snapshot.paused ? 0.0F : snapshot.rate,
            .raining = weather.raining,
            .rain_level = weather.rain_level,
            .thunder_level = weather.thunder_level,
        });
        environment->listener->broadcast_world_time();
    };
    const auto world_clock_of =
        [environment](command::context &context) -> sim::world_clock * {
        if (environment->world == nullptr) {
            context.fail("No world is open");
            return nullptr;
        }
        auto *clock{environment->world->clock()};
        if (clock == nullptr) {
            context.fail("The world provider keeps no clock");
        }
        return clock;
    };

    auto time{dispatcher.literal("time").require_node(
        "refrain.command.time", command::level::gamemode)};
    time.then(dispatcher.literal("query").executes(
        [world_clock_of](command::context &context) -> refrain::result<int> {
            auto *clock{world_clock_of(context)};
            if (clock == nullptr) {
                return std::unexpected{
                    make_error(error_code::not_found, "no world clock")};
            }
            context.reply("The game time is " +
                          std::to_string(clock->game_time()) +
                          " and the overworld clock is at " +
                          std::to_string(clock->total_ticks()) + " ticks");
            return static_cast<int>(clock->total_ticks() % 2147483647LL);
        }));
    auto time_set{dispatcher.literal("set")};
    // The clock's day markers are named arguments of the same parser that
    // accepts a tick count with the reference's `d`/`s`/`t` suffixes, so
    // `/time set day` and `/time set 1000` stay one executable node.
    auto time_set_argument{dispatcher.argument(
        "time", command::time_arg(0, command::world_time_markers()))};
    time_set_argument.executes(
        [world_clock_of,
         publish_clock](command::context &context) -> refrain::result<int> {
            auto *clock{world_clock_of(context)};
            if (clock == nullptr) {
                return std::unexpected{
                    make_error(error_code::not_found, "no world clock")};
            }
            const auto ticks{context.integer("time")};
            clock->set_total_ticks(ticks);
            publish_clock();
            context.reply(
                fmt::format("Set the overworld clock to {} ticks", ticks));
            return static_cast<int>(ticks % 2147483647LL);
        });
    time_set.then(time_set_argument);
    auto time_add{dispatcher.literal("add")};
    auto time_add_argument{dispatcher.argument(
        "time", command::time_arg(std::numeric_limits<std::int32_t>::min()))};
    time_add_argument.executes(
        [world_clock_of,
         publish_clock](command::context &context) -> refrain::result<int> {
            auto *clock{world_clock_of(context)};
            if (clock == nullptr) {
                return std::unexpected{
                    make_error(error_code::not_found, "no world clock")};
            }
            clock->add_total_ticks(context.integer("time"));
            publish_clock();
            context.reply("The overworld clock is now at " +
                          std::to_string(clock->total_ticks()) + " ticks");
            return static_cast<int>(clock->total_ticks() % 2147483647LL);
        });
    time_add.then(time_add_argument);
    time.then(time_set);
    time.then(time_add);
    time.then(dispatcher.literal("pause").executes(
        [world_clock_of,
         publish_clock](command::context &context) -> refrain::result<int> {
            auto *clock{world_clock_of(context)};
            if (clock == nullptr) {
                return std::unexpected{
                    make_error(error_code::not_found, "no world clock")};
            }
            clock->set_paused(true);
            publish_clock();
            context.reply("Paused the overworld clock");
            return 1;
        }));
    time.then(dispatcher.literal("resume").executes(
        [world_clock_of,
         publish_clock](command::context &context) -> refrain::result<int> {
            auto *clock{world_clock_of(context)};
            if (clock == nullptr) {
                return std::unexpected{
                    make_error(error_code::not_found, "no world clock")};
            }
            clock->set_paused(false);
            publish_clock();
            context.reply("Resumed the overworld clock");
            return 1;
        }));

    // /weather: switch the level weather with the reference's timers.
    auto weather{dispatcher.literal("weather").require_node(
        "refrain.command.weather", command::level::gamemode)};
    const auto set_weather =
        [environment, world_clock_of](command::context &context, bool raining,
                                      bool thundering) -> refrain::result<int> {
        auto *clock{world_clock_of(context)};
        if (clock == nullptr) {
            return std::unexpected{
                make_error(error_code::not_found, "no weather")};
        }
        std::int32_t duration{6000};
        if (context.has("duration")) {
            duration = static_cast<std::int32_t>(context.integer("duration"));
        }
        duration = std::max<std::int32_t>(duration, 1);
        if (raining && thundering) {
            clock->set_weather(true, true, 0, duration, duration);
        } else if (raining) {
            clock->set_weather(true, false, 0, duration, 0);
        } else {
            clock->set_weather(false, false, duration, 0, 0);
        }
        if (environment->listener != nullptr) {
            const auto snapshot{clock->weather()};
            environment->listener->broadcast_game_event(raining ? 1 : 2, 0.0F);
            environment->listener->broadcast_game_event(7, snapshot.rain_level);
            environment->listener->broadcast_game_event(8,
                                                        snapshot.thunder_level);
        }
        context.reply(raining ? (thundering ? "Set the weather to thunder"
                                            : "Set the weather to rain")
                              : "Set the weather to clear");
        return 1;
    };
    for (const auto &[name, raining, thundering] :
         std::array<std::tuple<std::string_view, bool, bool>, 3>{{
             {"clear", false, false},
             {"rain", true, false},
             {"thunder", true, true},
         }}) {
        auto node{dispatcher.literal(name)};
        node.executes([set_weather, raining, thundering](
                          command::context &context) -> refrain::result<int> {
            return set_weather(context, raining, thundering);
        });
        auto duration{dispatcher.argument(
            "duration", command::integer_arg(1, std::nullopt))};
        duration.executes(
            [set_weather, raining,
             thundering](command::context &context) -> refrain::result<int> {
                return set_weather(context, raining, thundering);
            });
        node.then(duration);
        weather.then(node);
    }

    register_world_commands(environment);
    register_mod_commands(environment);
}

auto make_command_bridge(std::shared_ptr<command_environment> environment)
    -> net::command_bridge {
    if (!environment) {
        return {};
    }
    register_runtime_commands(environment);

    // Install the injected permission filter after registration, so every
    // node that declares a node is also checked by it. Without a provider the
    // filter stays unset and every node falls back to its declared op level.
    if (environment->permission_query) {
        // The dispatcher lives inside the environment, so the filter borrows
        // the environment instead of owning it: a captured `shared_ptr` here
        // would make the two keep each other alive.
        command_environment *const raw{environment.get()};
        environment->dispatcher.set_permission_filter(
            [raw](const command::source &source,
                  const command::node_permission &permission) {
                return raw->query_permission(source, permission.node);
            });
    }

    net::command_bridge bridge{};
    bridge.tree = [environment](const net::player_identity &identity) {
        auto source{make_source(environment, identity)};
        // A tree is filtered by permission, so the cache key is the player
        // identity plus both revisions that can change the answer: a
        // dispatcher registration and a permission policy revision. Two
        // players must never share a snapshot built for another permission,
        // and a policy change must not reuse a tree built from the old one.
        const command_environment::tree_cache_key key{
            .dispatcher_revision = environment->dispatcher.revision(),
            .permission_revision = environment->permission_revision
                                       ? environment->permission_revision()
                                       : 0U,
            .subject = subject_fingerprint(source),
        };
        {
            const std::lock_guard lock{environment->tree_cache_mutex};
            if (environment->tree_cache_dispatcher_revision !=
                    key.dispatcher_revision ||
                environment->tree_cache_permission_revision !=
                    key.permission_revision) {
                environment->tree_cache.clear();
                environment->tree_cache_dispatcher_revision =
                    key.dispatcher_revision;
                environment->tree_cache_permission_revision =
                    key.permission_revision;
            }
            if (const auto found{environment->tree_cache.find(key)};
                found != environment->tree_cache.end()) {
                return found->second;
            }
        }
        auto tree{environment->dispatcher.tree(source)};
        auto wire{
            std::make_shared<const net::command_tree>(to_wire_tree(*tree))};
        {
            const std::lock_guard lock{environment->tree_cache_mutex};
            environment->tree_cache[key] = wire;
        }
        return wire;
    };
    bridge.execute = [environment](const net::player_identity &identity,
                                   std::string_view input) {
        auto source{make_source(environment, identity)};
        auto result{environment->dispatcher.execute(input, source)};
        if (!result) {
            // A command hidden by the permission filter answers with the node
            // the subject is missing instead of a generic parse error, so a
            // player learns why a command does not work for them.
            const auto command_name{first_word(input)};
            const auto permission{
                environment->dispatcher.permission_of(command_name)};
            if (!permission.node.empty()) {
                const auto decision{
                    environment->query_permission(source, permission.node)};
                const auto denied{
                    decision == command::permission_decision::denied ||
                    (decision == command::permission_decision::not_handled &&
                     !source.has_permission(permission.fallback))};
                if (denied) {
                    return net::command_feedback{
                        .handled = true,
                        .success = false,
                        .message = fmt::format(
                            "You do not have permission to run /{} (requires "
                            "the '{}' permission)",
                            command_name, permission.node),
                    };
                }
            }
            return net::command_feedback{
                .handled = true,
                .success = false,
                .message = result.error().message,
            };
        }
        return net::command_feedback{
            .handled = true,
            .success = result->success,
            .message = {},
        };
    };
    bridge.suggest = [environment](const net::player_identity &identity,
                                   std::string_view input,
                                   std::int32_t cursor) {
        auto source{make_source(environment, identity)};
        auto result{environment->dispatcher.suggest(
            input, static_cast<std::size_t>(std::max(cursor, 0)), source)};
        net::suggestion_result out{};
        out.start = static_cast<std::int32_t>(result.start);
        out.length = static_cast<std::int32_t>(result.end - result.start);
        out.entries.reserve(result.entries.size());
        for (const auto &entry : result.entries) {
            out.entries.push_back(
                {.text = entry.text, .tooltip = entry.tooltip});
        }
        return out;
    };
    bridge.revision = [environment] {
        // The network layer compares this value to decide whether a cached
        // encoded tree is still current, so both revisions that filter a tree
        // contribute to it.
        const auto policy_revision{environment->permission_revision
                                       ? environment->permission_revision()
                                       : 0U};
        return environment->dispatcher.revision() * 1'000'003ULL +
               policy_revision;
    };
    return bridge;
}

} // namespace refrain::server
