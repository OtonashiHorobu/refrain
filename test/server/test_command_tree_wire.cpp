// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/command/command.hpp>
#include <refrain/net/wire.hpp>
#include <refrain/server/command_bridge.hpp>
#include <refrain/server/command_tree_wire.hpp>

#include <boost/ut.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

constexpr std::uint8_t flag_literal = 1U;
constexpr std::uint8_t flag_argument = 2U;
constexpr std::uint8_t flag_executable = 4U;
constexpr std::uint8_t flag_redirect = 8U;

[[nodiscard]] auto node_type(std::uint8_t flags) -> std::uint8_t {
    return static_cast<std::uint8_t>(flags & 3U);
}

[[nodiscard]] auto find_wire_node(const refrain::net::command_tree &tree,
                                  std::string_view name)
    -> const refrain::net::command_node * {
    for (const auto &node : tree.nodes) {
        if (node.name == name) {
            return &node;
        }
    }
    return nullptr;
}

[[nodiscard]] auto wire_has_literal(const refrain::net::command_tree &tree,
                                    std::string_view name) -> bool {
    for (const auto &node : tree.nodes) {
        if (node_type(node.flags) == flag_literal && node.name == name) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] auto example_identity(std::string_view name,
                                    std::uint64_t connection = 1)
    -> refrain::net::player_identity {
    const auto id{
        refrain::net::uuid::parse("dd12be42-52a9-4a91-a8a1-11c01849e498")};
    expect(id.has_value());
    return refrain::net::player_identity{
        .connection = connection,
        .id = id.value_or(refrain::net::uuid{}),
        .name = std::string{name},
        .world = {},
    };
}

[[maybe_unused]] const suite command_tree_wire_tests = [] {
    "parser ids follow the 26.3 command argument registry"_test = [] {
        using refrain::command::parser_kind;
        using refrain::server::wire_parser_id;
        expect(wire_parser_id(parser_kind::boolean) == 0U);
        expect(wire_parser_id(parser_kind::float_number) == 1U);
        expect(wire_parser_id(parser_kind::double_number) == 2U);
        expect(wire_parser_id(parser_kind::integer) == 3U);
        expect(wire_parser_id(parser_kind::long_integer) == 4U);
        expect(wire_parser_id(parser_kind::string) == 5U);
        expect(wire_parser_id(parser_kind::player_name) == 7U);
        expect(wire_parser_id(parser_kind::block_position) == 8U);
        expect(wire_parser_id(parser_kind::rotation) == 29U);
        expect(wire_parser_id(parser_kind::resource_location) == 36U);
        expect(wire_parser_id(parser_kind::dimension) == 41U);
        expect(wire_parser_id(parser_kind::gamemode) == 42U);
        expect(wire_parser_id(parser_kind::time) == 43U);
        expect(wire_parser_id(parser_kind::uuid) == 61U);
        expect(wire_parser_id(parser_kind::enum_values) == 5U);
        expect(!wire_parser_id(parser_kind::none).has_value());
        expect(
            !refrain::server::wire_parser_name(parser_kind::integer).empty());
        expect(refrain::server::wire_parser_name(parser_kind::none).empty());
    };

    "wire tree mirrors nodes, flags, properties and redirects"_test = [] {
        refrain::command::dispatcher dispatcher;
        dispatcher.literal("help").executes(
            [](refrain::command::context &) { return 1; });
        auto gamemode = dispatcher.literal("gamemode")
                            .require(refrain::command::level::gamemode);
        gamemode.then(
            dispatcher.argument("mode", refrain::command::gamemode_arg())
                .executes([](refrain::command::context &) { return 1; }));
        dispatcher.literal("gm").redirect(gamemode);
        dispatcher.literal("count").then(
            dispatcher.argument("n", refrain::command::integer_arg(1, 10))
                .executes([](refrain::command::context &) { return 1; }));
        dispatcher.literal("ratio").then(
            dispatcher.argument("x", refrain::command::double_arg(0.0, 1.0))
                .executes([](refrain::command::context &) { return 1; }));
        dispatcher.literal("say").then(
            dispatcher.argument("message", refrain::command::greedy_arg())
                .executes([](refrain::command::context &) { return 1; }));
        dispatcher.literal("blocks").then(
            dispatcher.argument("pos", refrain::command::block_position_arg())
                .executes([](refrain::command::context &) { return 1; }));
        dispatcher.literal("wait").then(
            dispatcher.argument("time", refrain::command::time_arg(5))
                .executes([](refrain::command::context &) { return 1; }));
        // A node can be executable and still have children: the parent form is
        // the command without the optional trailing argument.
        auto weather{dispatcher.literal("weather")};
        weather.executes([](refrain::command::context &) { return 1; });
        weather.then(dispatcher.literal("clear").executes(
            [](refrain::command::context &) { return 1; }));

        refrain::command::console_source source;
        const auto snapshot = dispatcher.tree(source);
        expect(snapshot != nullptr);
        if (snapshot == nullptr) {
            return;
        }
        const auto wire = refrain::server::to_wire_tree(*snapshot);
        expect(wire.root_index == 0U);
        expect(wire.nodes.size() == snapshot->nodes.size());

        for (std::size_t index = 0; index < wire.nodes.size(); ++index) {
            const auto &source_node = snapshot->nodes[index];
            const auto &wire_node = wire.nodes[index];
            switch (source_node.kind) {
            case refrain::command::node_kind::root:
                expect(node_type(wire_node.flags) == 0U);
                break;
            case refrain::command::node_kind::literal:
                expect(node_type(wire_node.flags) == flag_literal);
                break;
            case refrain::command::node_kind::argument:
                expect(node_type(wire_node.flags) == flag_argument);
                break;
            }
            expect(((wire_node.flags & flag_executable) != 0U) ==
                   source_node.executable);
            expect(wire_node.children == source_node.children);
            expect(wire_node.name == source_node.name);
            if (source_node.redirect) {
                expect((wire_node.flags & flag_redirect) != 0U);
                expect(wire_node.redirect == source_node.redirect_index);
            }
        }

        const auto *count = find_wire_node(wire, "n");
        expect(count != nullptr);
        if (count != nullptr) {
            expect(count->parser_id == 3U);
            expect(count->number_flags == 3U);
            expect(count->integer_min == 1);
            expect(count->integer_max == 10);
        }
        const auto *ratio = find_wire_node(wire, "x");
        expect(ratio != nullptr);
        if (ratio != nullptr) {
            expect(ratio->parser_id == 2U);
            expect(ratio->number_flags == 3U);
            expect(ratio->number_min == 0.0);
            expect(ratio->number_max == 1.0);
        }
        const auto *message = find_wire_node(wire, "message");
        expect(message != nullptr);
        if (message != nullptr) {
            expect(message->parser_id == 5U);
            expect(message->string_type == 2U);
        }
        const auto *mode = find_wire_node(wire, "mode");
        expect(mode != nullptr);
        if (mode != nullptr) {
            expect(mode->parser_id == 42U);
        }
        const auto *pos = find_wire_node(wire, "pos");
        expect(pos != nullptr);
        if (pos != nullptr) {
            expect(pos->parser_id == 8U);
        }
        const auto *time = find_wire_node(wire, "time");
        expect(time != nullptr);
        if (time != nullptr) {
            expect(time->parser_id == 43U);
            expect(time->time_min == 5);
        }
        const auto *weather_node = find_wire_node(wire, "weather");
        expect(weather_node != nullptr);
        if (weather_node != nullptr) {
            expect(node_type(weather_node->flags) == flag_literal);
            expect((weather_node->flags & flag_executable) != 0U);
            expect(weather_node->children.size() == 1U);
            if (weather_node->children.size() == 1U) {
                const auto &child = wire.nodes[weather_node->children.front()];
                expect(child.name == "clear");
                expect((child.flags & flag_executable) != 0U);
            }
        }

        const auto *alias = find_wire_node(wire, "gm");
        expect(alias != nullptr);
        if (alias != nullptr) {
            expect((alias->flags & flag_redirect) != 0U);
            std::uint32_t gamemode_index{0};
            for (std::uint32_t index = 0; index < wire.nodes.size(); ++index) {
                if (wire.nodes[index].name == "gamemode") {
                    gamemode_index = index;
                }
            }
            expect(alias->redirect == gamemode_index);
        }

        // The payload survives the protocol codec unchanged.
        std::vector<std::byte> bytes{};
        refrain::net::writer out{bytes};
        expect(static_cast<bool>(refrain::net::encode_command_tree(out, wire)));
        refrain::net::reader in{bytes};
        refrain::net::command_tree decoded{};
        expect(
            static_cast<bool>(refrain::net::decode_command_tree(in, decoded)));
        expect(in.empty());
        expect(decoded == wire);
    };

    "permission filtering removes subtrees and dangling redirects"_test = [] {
        refrain::command::dispatcher dispatcher;
        auto gamemode = dispatcher.literal("gamemode")
                            .require(refrain::command::level::gamemode);
        gamemode.then(
            dispatcher.argument("mode", refrain::command::gamemode_arg())
                .executes([](refrain::command::context &) { return 1; }));
        dispatcher.literal("gm").redirect(gamemode);
        dispatcher.literal("help").executes(
            [](refrain::command::context &) { return 1; });

        refrain::command::console_source high{refrain::command::level::owner};
        refrain::command::console_source low{refrain::command::level::everyone};
        const auto high_wire =
            refrain::server::to_wire_tree(*dispatcher.tree(high));
        const auto low_wire =
            refrain::server::to_wire_tree(*dispatcher.tree(low));
        expect(wire_has_literal(high_wire, "gamemode"));
        expect(!wire_has_literal(low_wire, "gamemode"));
        expect(wire_has_literal(low_wire, "help"));
        const auto *alias = find_wire_node(low_wire, "gm");
        expect(alias != nullptr);
        if (alias != nullptr) {
            expect((alias->flags & flag_redirect) == 0U);
        }
    };

    "bridge serves the typed tree, completion and typed execution"_test = [] {
        auto environment{
            std::make_shared<refrain::server::command_environment>()};
        refrain::command::register_parser_examples(environment->dispatcher);
        auto bridge = refrain::server::make_command_bridge(environment);
        expect(static_cast<bool>(bridge.tree));
        expect(static_cast<bool>(bridge.suggest));
        expect(static_cast<bool>(bridge.execute));

        const std::string alice_name{"Alice"};
        const auto identity = example_identity(alice_name);
        const auto tree = bridge.tree(identity);
        expect(tree != nullptr);
        if (tree != nullptr) {
            expect(wire_has_literal(*tree, "example"));
            const auto *flag = find_wire_node(*tree, "flag");
            expect(flag != nullptr);
            if (flag != nullptr) {
                expect(flag->parser_id == 0U);
            }
        }

        auto suggestions = bridge.suggest(identity, "example ", 8);
        expect(suggestions.start == 8);
        expect(suggestions.length == 0);
        bool offered_true{false};
        bool offered_false{false};
        for (const auto &entry : suggestions.entries) {
            offered_true = offered_true || entry.text == "true";
            offered_false = offered_false || entry.text == "false";
        }
        expect(offered_true);
        expect(offered_false);

        const std::string before_mode{
            "example true 7 1 0.5 1.25 w x minecraft:stone 1 2 3 90 45 "
            "Steve "};
        auto modes =
            bridge.suggest(identity, before_mode,
                           static_cast<std::int32_t>(before_mode.size()));
        expect(modes.start == static_cast<std::int32_t>(before_mode.size()));
        expect(modes.length == 0);
        bool offered_mode_tooltip{false};
        for (const auto &entry : modes.entries) {
            if (entry.text == "creative") {
                offered_mode_tooltip = !entry.tooltip.empty();
            }
        }
        expect(offered_mode_tooltip);

        const auto partial_mode = before_mode + "cre";
        auto partial =
            bridge.suggest(identity, partial_mode,
                           static_cast<std::int32_t>(partial_mode.size()));
        expect(partial.start == static_cast<std::int32_t>(before_mode.size()));
        expect(partial.length == 3);
        expect(partial.entries.size() == 1U);
        if (!partial.entries.empty()) {
            expect(partial.entries.front().text == "creative");
        }

        const std::string line{
            "example true 7 123456789012 0.5 1.25 word \"quoted text\" "
            "minecraft:stone 1 2 3 90 45 Steve creative 20s "
            "minecraft:overworld dd12be42-52a9-4a91-a8a1-11c01849e498"};
        const auto feedback = bridge.execute(identity, line);
        expect(feedback.handled);
        expect(feedback.success) << feedback.message;

        const auto invalid =
            bridge.execute(identity, "example true 7 not-a-long");
        expect(invalid.handled);
        expect(!invalid.success);
        expect(invalid.message.find("not a valid") != std::string::npos)
            << invalid.message;
    };

    "bridge tree cache separates sources by permission"_test = [] {
        auto environment{
            std::make_shared<refrain::server::command_environment>()};
        environment->dispatcher.literal("admin-only")
            .require(refrain::command::level::admin)
            .executes([](refrain::command::context &) { return 1; });
        environment->permission = refrain::command::level::owner;
        auto bridge = refrain::server::make_command_bridge(environment);

        const std::string owner_name{"Alice"};
        const auto owner_identity = example_identity(owner_name, 1);
        const auto owner_tree = bridge.tree(owner_identity);
        expect(owner_tree != nullptr);
        if (owner_tree != nullptr) {
            expect(wire_has_literal(*owner_tree, "admin-only"));
        }
        // The same source class reuses the cached snapshot.
        expect(bridge.tree(owner_identity) == owner_tree);

        environment->permission = refrain::command::level::everyone;
        const std::string guest_name{"Bob"};
        const auto guest_identity = example_identity(guest_name, 2);
        const auto guest_tree = bridge.tree(guest_identity);
        expect(guest_tree != nullptr);
        if (guest_tree != nullptr) {
            expect(!wire_has_literal(*guest_tree, "admin-only"));
            expect(guest_tree != owner_tree);
        }
    };
};

} // namespace
