// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/command_bridge.hpp>

#include <refrain/command/parsers.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
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

[[nodiscard]] auto make_source(const std::shared_ptr<command_environment> &env,
                               const net::player_identity &identity)
    -> command::player_source {
    return command::player_source{
        std::string{identity.name}, identity_key(identity),
        command::level::everyone,
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
