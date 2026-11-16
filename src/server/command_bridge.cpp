// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/command_bridge.hpp>

#include <refrain/app/mod_management.hpp>
#include <refrain/command/parsers.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
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

[[nodiscard]] auto parse_mode(std::string_view text)
    -> std::optional<net::game_mode> {
    if (text == "survival" || text == "0") {
        return net::game_mode::survival;
    }
    if (text == "creative" || text == "1") {
        return net::game_mode::creative;
    }
    if (text == "adventure" || text == "2") {
        return net::game_mode::adventure;
    }
    if (text == "spectator" || text == "3") {
        return net::game_mode::spectator;
    }
    return std::nullopt;
}

[[nodiscard]] auto mode_name(net::game_mode mode) -> std::string_view {
    switch (mode) {
    case net::game_mode::survival:
        return "survival";
    case net::game_mode::creative:
        return "creative";
    case net::game_mode::adventure:
        return "adventure";
    case net::game_mode::spectator:
        return "spectator";
    }
    return "creative";
}

[[nodiscard]] auto make_source(const std::shared_ptr<command_environment> &env,
                               const net::player_identity &identity)
    -> command::player_source {
    const auto key{identity_key(identity)};
    env->remember_key(key, identity.connection, identity.id.to_string());
    return command::player_source{
        std::string{identity.name}, key, env->permission,
        [env, connection = identity.connection](std::string_view message) {
            if (env->listener != nullptr) {
                env->listener->send_system_chat(connection, message);
            }
        }};
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

[[nodiscard]] auto to_wire_tree(const command::tree &tree)
    -> net::command_tree {
    net::command_tree wire{};
    wire.root_index = tree.root;
    wire.nodes.reserve(tree.nodes.size());
    for (const auto &source : tree.nodes) {
        net::command_node node{};
        switch (source.kind) {
        case command::node_kind::root:
            break;
        case command::node_kind::literal:
            node.flags = static_cast<std::uint8_t>(1U);
            break;
        case command::node_kind::argument:
            node.flags = static_cast<std::uint8_t>(2U);
            break;
        }
        if (source.executable) {
            node.flags = static_cast<std::uint8_t>(node.flags | 4U);
        }
        if (source.redirect) {
            node.flags = static_cast<std::uint8_t>(node.flags | 8U);
            node.redirect = source.redirect_index;
        }
        node.children = source.children;
        node.name = source.name;
        const auto &parser{source.parser};
        switch (parser.kind) {
        case command::parser_kind::boolean:
            node.parser_id = 0U;
            break;
        case command::parser_kind::float_number:
            node.parser_id = 1U;
            break;
        case command::parser_kind::double_number:
            node.parser_id = 2U;
            break;
        case command::parser_kind::integer:
            node.parser_id = 3U;
            break;
        case command::parser_kind::long_integer:
            node.parser_id = 4U;
            break;
        case command::parser_kind::string:
            node.parser_id = 5U;
            node.string_type = static_cast<std::uint32_t>(parser.string);
            break;
        case command::parser_kind::enum_values:
            // The protocol adapter uses a greedy string and asks the server
            // for suggestions; the enum remains a server-side parser.
            node.parser_id = 5U;
            node.string_type = 2U;
            break;
        case command::parser_kind::resource_location:
            node.parser_id = 34U;
            break;
        case command::parser_kind::none:
            break;
        }
        wire.nodes.push_back(std::move(node));
    }
    return wire;
}

} // namespace

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
                                       std::string uuid) {
    const std::lock_guard lock{mode_mutex};
    key_to_connection[key] = connection;
    identity_uuids[key] = std::move(uuid);
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

namespace {

/// Register the in-game `/mods` tree. Every leaf hands its tokens to the
/// shared application service, so the console and the game report the same
/// lines and the same errors. The tree is filtered by the operator gate, and
/// the bridge answers an explicit denial when a non-operator types it.
void register_mod_commands(
    const std::shared_ptr<command_environment> &environment) {
    auto &dispatcher{environment->dispatcher};
    const auto permitted = [environment](const command::source &source) {
        return environment->mod_admin && environment->mod_admin(source);
    };
    /// Attach an executor that runs `prefix [<argument>] [suffix...]` through
    /// the shared service. `argument` names a parser node the tokens take the
    /// value from; the argument entry stays visible on the node's children.
    const auto leaf = [environment](command::node_ref node,
                                    std::vector<std::string> prefix,
                                    std::string argument,
                                    std::vector<std::string> suffix = {}) {
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

    auto mods{dispatcher.literal("mods")};
    mods.require(permitted);
    mods.executes(
        [environment](command::context &context) -> refrain::result<int> {
            return execute_mods_command(environment, context, {});
        });
    mods.then(leaf(dispatcher.literal("list"), {"list"}, {}));
    mods.then(leaf(dispatcher.literal("installed"), {"installed"}, {}));

    auto verify{dispatcher.literal("verify")};
    verify.then(leaf(dispatcher.argument("package", command::word_arg()),
                     {"verify"}, "package"));
    mods.then(verify);

    auto install{dispatcher.literal("install")};
    auto install_package{
        leaf(dispatcher.argument("package", command::word_arg()), {"install"},
             "package")};
    install_package.then(leaf(dispatcher.literal("confirm"), {"install"},
                              "package", {"confirm"}));
    install_package.then(leaf(dispatcher.literal("--dry-run"), {"install"},
                              "package", {"--dry-run"}));
    install.then(install_package);
    mods.then(install);

    auto uninstall{dispatcher.literal("uninstall")};
    auto uninstall_id{leaf(dispatcher.argument("id", command::word_arg()),
                           {"uninstall"}, "id")};
    uninstall_id.then(
        leaf(dispatcher.literal("confirm"), {"uninstall"}, "id", {"confirm"}));
    uninstall_id.then(leaf(dispatcher.literal("--dry-run"), {"uninstall"}, "id",
                           {"--dry-run"}));
    uninstall.then(uninstall_id);
    mods.then(uninstall);

    auto reload{dispatcher.literal("reload")};
    auto reload_id{
        leaf(dispatcher.argument("id", command::word_arg()), {"reload"}, "id")};
    reload_id.then(
        leaf(dispatcher.literal("--dry-run"), {"reload"}, "id", {"--dry-run"}));
    reload.then(reload_id);
    mods.then(reload);

    auto reload_data{dispatcher.literal("reload-data")};
    auto reload_data_node{leaf(reload_data, {"reload-data"}, {})};
    reload_data_node.then(leaf(dispatcher.literal("--dry-run"), {"reload-data"},
                               {}, {"--dry-run"}));
    mods.then(reload_data_node);

    auto trust{dispatcher.literal("trust")};
    trust.executes(
        [environment](command::context &context) -> refrain::result<int> {
            return execute_mods_command(environment, context, {"trust"});
        });
    trust.then(leaf(dispatcher.literal("list"), {"trust", "list"}, {}));
    auto pin{dispatcher.literal("pin")};
    auto pin_id{leaf(dispatcher.argument("id", command::word_arg()),
                     {"trust", "pin"}, "id")};
    pin_id.then(leaf(dispatcher.literal("confirm"), {"trust", "pin"}, "id",
                     {"confirm"}));
    pin_id.then(leaf(dispatcher.literal("--dry-run"), {"trust", "pin"}, "id",
                     {"--dry-run"}));
    pin.then(pin_id);
    trust.then(pin);
    auto clear{dispatcher.literal("clear")};
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

    auto help{dispatcher.literal("help")};
    help.executes([](command::context &context) -> refrain::result<int> {
        context.reply("Commands: /help, /list, /say <message>");
        return 1;
    });

    auto list{dispatcher.literal("list")};
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

    auto say{dispatcher.literal("say")};
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

    auto gamemode{dispatcher.literal("gamemode")};
    auto mode_argument{dispatcher.argument("mode", command::gamemode_arg())};
    mode_argument.require(command::level::gamemode);
    mode_argument.executes(
        [environment](command::context &context) -> refrain::result<int> {
            const auto mode{parse_mode(context.string("mode"))};
            if (!mode) {
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
                environment->modes[*connection] = *mode;
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
                            static_cast<std::uint32_t>(*mode));
                    }
                }
            }
            if (environment->listener != nullptr) {
                environment->listener->set_player_game_mode(*connection, *mode);
            }
            context.reply(std::string{"Set game mode to "} +
                          std::string{mode_name(*mode)});
            return 1;
        });
    gamemode.then(mode_argument);

    auto default_gamemode{dispatcher.literal("defaultgamemode")};
    auto default_argument{dispatcher.argument("mode", command::gamemode_arg())};
    default_argument.require(command::level::gamemode);
    default_argument.executes(
        [environment](command::context &context) -> refrain::result<int> {
            const auto mode{parse_mode(context.string("mode"))};
            if (!mode) {
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
            settings->game_mode = static_cast<std::int32_t>(*mode);
            if (auto written{environment->world->write_gameplay(*settings)};
                !written) {
                context.fail("Cannot persist the default game mode");
                return std::unexpected{std::move(written.error())};
            }
            {
                const std::lock_guard lock{environment->mode_mutex};
                environment->default_mode = *mode;
            }
            context.reply(std::string{"Default game mode set to "} +
                          std::string{mode_name(*mode)});
            return 1;
        });
    default_gamemode.then(default_argument);

    auto difficulty{dispatcher.literal("difficulty")};
    auto difficulty_argument{
        dispatcher.argument("level", command::integer_arg(0, 3))};
    difficulty_argument.require(command::level::gamemode);
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

    auto hardcore{dispatcher.literal("hardcore")};
    auto hardcore_argument{dispatcher.argument("enabled", command::bool_arg())};
    hardcore_argument.require(command::level::admin);
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

    auto time{dispatcher.literal("time")};
    time.require(command::level::gamemode);
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
    auto time_set_argument{dispatcher.argument("time", command::word_arg())};
    time_set_argument.executes([world_clock_of,
                                publish_clock](command::context &context)
                                   -> refrain::result<int> {
        auto *clock{world_clock_of(context)};
        if (clock == nullptr) {
            return std::unexpected{
                make_error(error_code::not_found, "no world clock")};
        }
        const auto name{context.string("time")};
        std::int64_t ticks{0};
        if (name == "day" || name == "morning") {
            ticks = 1000;
        } else if (name == "noon") {
            ticks = 6000;
        } else if (name == "night") {
            ticks = 13000;
        } else if (name == "midnight") {
            ticks = 18000;
        } else {
            const auto parsed{
                std::from_chars(name.data(), name.data() + name.size(), ticks)};
            if (parsed.ec != std::errc{} ||
                parsed.ptr != name.data() + name.size()) {
                context.fail("Time must be a tick count or day, noon, night or "
                             "midnight");
                return std::unexpected{make_error(error_code::invalid_argument,
                                                  "invalid time value")};
            }
        }
        clock->set_total_ticks(ticks);
        publish_clock();
        context.reply("Set the overworld clock to " + std::to_string(ticks) +
                      " ticks");
        return static_cast<int>(ticks % 2147483647LL);
    });
    time_set.then(time_set_argument);
    auto time_add{dispatcher.literal("add")};
    auto time_add_argument{dispatcher.argument("time", command::long_arg())};
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
    auto weather{dispatcher.literal("weather")};
    weather.require(command::level::gamemode);
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

    register_mod_commands(environment);
}

auto make_command_bridge(std::shared_ptr<command_environment> environment)
    -> net::command_bridge {
    if (!environment) {
        return {};
    }
    register_runtime_commands(environment);

    net::command_bridge bridge{};
    bridge.tree = [environment](const net::player_identity &identity) {
        auto source{make_source(environment, identity)};
        // The tree is filtered by permission, so a snapshot is cached per
        // (revision, permission level, mod-admin grant): two players must
        // never receive a tree built for another permission.
        const auto permitted{static_cast<std::uint64_t>(
            environment->mod_admin && environment->mod_admin(source))};
        const auto key{environment->dispatcher.revision() * 8U +
                       static_cast<std::uint64_t>(source.permission()) * 2U +
                       permitted};
        {
            const std::lock_guard lock{environment->tree_cache_mutex};
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
        // The tree hides `/mods` from a player without the operator
        // permission; answer an explicit denial instead of a generic parse
        // error so the player learns why.
        if (first_word(input) == "mods" &&
            !(environment->mod_admin && environment->mod_admin(source))) {
            return net::command_feedback{
                .handled = true,
                .success = false,
                .message =
                    std::string{"You do not have permission to manage mods "
                                "(requires the '"} +
                    std::string{app::mod_admin_permission} + "' permission)",
            };
        }
        auto result{environment->dispatcher.execute(input, source)};
        if (!result) {
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
        return environment->dispatcher.revision();
    };
    return bridge;
}

} // namespace refrain::server
