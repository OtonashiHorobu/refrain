// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/command_bridge.hpp>

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

namespace {

[[nodiscard]] auto identity_key(const net::player_identity &identity)
    -> std::uint64_t {
    std::uint64_t key{1469598103934665603ULL};
    for (const auto byte : identity.id.bytes) {
        key ^= byte;
        key *= 1099511628211ULL;
    }
    return key;
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
    env->remember_key(key, identity.connection);
    return command::player_source{
        std::string{identity.name}, key, env->permission,
        [env, connection = identity.connection](std::string_view message) {
            if (env->listener != nullptr) {
                env->listener->send_system_chat(connection, message);
            }
        }};
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
                                       std::uint64_t connection) {
    const std::lock_guard lock{mode_mutex};
    key_to_connection[key] = connection;
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
}

auto make_command_bridge(std::shared_ptr<command_environment> environment)
    -> net::command_bridge {
    if (!environment) {
        return {};
    }
    register_runtime_commands(environment);

    net::command_bridge bridge{};
    bridge.tree = [environment](const net::player_identity &identity) {
        if (environment->tree_cache) {
            return environment->tree_cache;
        }
        auto source{make_source(environment, identity)};
        auto tree{environment->dispatcher.tree(source)};
        if (tree) {
            environment->tree_cache =
                std::make_shared<const net::command_tree>(to_wire_tree(*tree));
        }
        return environment->tree_cache;
    };
    bridge.execute = [environment](const net::player_identity &identity,
                                   std::string_view input) {
        auto source{make_source(environment, identity)};
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
