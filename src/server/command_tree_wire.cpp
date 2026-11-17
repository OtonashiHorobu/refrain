// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/command_tree_wire.hpp>

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace refrain::server {

namespace {

/// Clientbound node flags of the 26.3 `commands` packet.
constexpr std::uint8_t flag_literal = 1U;
constexpr std::uint8_t flag_argument = 2U;
constexpr std::uint8_t flag_executable = 4U;
constexpr std::uint8_t flag_redirect = 8U;

/// Number-property flags of the brigadier number arguments.
constexpr std::uint8_t number_flag_min = 1U;
constexpr std::uint8_t number_flag_max = 2U;

} // namespace

auto wire_parser_id(command::parser_kind kind) -> std::optional<std::uint32_t> {
    switch (kind) {
    case command::parser_kind::boolean:
        return 0U; // brigadier:bool
    case command::parser_kind::float_number:
        return 1U; // brigadier:float
    case command::parser_kind::double_number:
        return 2U; // brigadier:double
    case command::parser_kind::integer:
        return 3U; // brigadier:integer
    case command::parser_kind::long_integer:
        return 4U; // brigadier:long
    case command::parser_kind::string:
        return 5U; // brigadier:string
    case command::parser_kind::player_name:
        return 7U; // game_profile
    case command::parser_kind::block_position:
        return 8U; // block_pos
    case command::parser_kind::rotation:
        return 29U; // rotation
    case command::parser_kind::resource_location:
        return 36U; // resource_location
    case command::parser_kind::dimension:
        return 41U; // dimension
    case command::parser_kind::gamemode:
        return 42U; // gamemode
    case command::parser_kind::time:
        return 43U; // time
    case command::parser_kind::uuid:
        return 61U; // uuid
    case command::parser_kind::vec3:
        return 10U; // vec3
    case command::parser_kind::enum_values:
        // The vocabulary is server-owned; the client renders the argument as a
        // greedy string and asks the server for values.
        return 5U; // brigadier:string
    case command::parser_kind::none:
        return std::nullopt;
    }
    return std::nullopt;
}

auto wire_parser_name(command::parser_kind kind) -> std::string_view {
    switch (kind) {
    case command::parser_kind::boolean:
        return "brigadier:bool";
    case command::parser_kind::float_number:
        return "brigadier:float";
    case command::parser_kind::double_number:
        return "brigadier:double";
    case command::parser_kind::integer:
        return "brigadier:integer";
    case command::parser_kind::long_integer:
        return "brigadier:long";
    case command::parser_kind::string:
        return "brigadier:string";
    case command::parser_kind::enum_values:
        return "brigadier:string (server vocabulary)";
    case command::parser_kind::resource_location:
        return "resource_location";
    case command::parser_kind::block_position:
        return "block_pos";
    case command::parser_kind::rotation:
        return "rotation";
    case command::parser_kind::player_name:
        return "game_profile";
    case command::parser_kind::gamemode:
        return "gamemode";
    case command::parser_kind::time:
        return "time";
    case command::parser_kind::dimension:
        return "dimension";
    case command::parser_kind::uuid:
        return "uuid";
    case command::parser_kind::vec3:
        return "vec3";
    case command::parser_kind::none:
        return {};
    }
    return {};
}

auto to_wire_tree(const command::tree &snapshot) -> net::command_tree {
    net::command_tree wire{};
    wire.root_index = snapshot.root;
    wire.nodes.reserve(snapshot.nodes.size());
    for (const auto &source : snapshot.nodes) {
        net::command_node node{};
        switch (source.kind) {
        case command::node_kind::root:
            break;
        case command::node_kind::literal:
            node.flags = flag_literal;
            break;
        case command::node_kind::argument:
            node.flags = flag_argument;
            break;
        }
        if (source.executable) {
            node.flags =
                static_cast<std::uint8_t>(node.flags | flag_executable);
        }
        if (source.redirect) {
            node.flags = static_cast<std::uint8_t>(node.flags | flag_redirect);
            node.redirect = source.redirect_index;
        }
        node.children = source.children;
        node.name = source.name;
        if (source.kind != command::node_kind::argument) {
            wire.nodes.push_back(std::move(node));
            continue;
        }

        const auto &parser = source.parser;
        const auto id = wire_parser_id(parser.kind);
        // An argument node always carries a parser; the framework registers no
        // untyped argument. Falling back to the greedy string keeps the payload
        // structurally valid rather than emitting a node the client cannot
        // parse, and the command module has no way to report an error here.
        node.parser_id = id.value_or(5U);
        switch (parser.kind) {
        case command::parser_kind::boolean:
        case command::parser_kind::resource_location:
        case command::parser_kind::block_position:
        case command::parser_kind::rotation:
        case command::parser_kind::player_name:
        case command::parser_kind::gamemode:
        case command::parser_kind::dimension:
        case command::parser_kind::uuid:
        case command::parser_kind::vec3:
        case command::parser_kind::none:
            break;
        case command::parser_kind::integer:
        case command::parser_kind::long_integer: {
            std::uint8_t flags{0};
            if (parser.has_min) {
                flags = static_cast<std::uint8_t>(flags | number_flag_min);
            }
            if (parser.has_max) {
                flags = static_cast<std::uint8_t>(flags | number_flag_max);
            }
            node.number_flags = flags;
            node.integer_min = parser.integer_min;
            node.integer_max = parser.integer_max;
            break;
        }
        case command::parser_kind::float_number:
        case command::parser_kind::double_number: {
            std::uint8_t flags{0};
            if (parser.has_min) {
                flags = static_cast<std::uint8_t>(flags | number_flag_min);
            }
            if (parser.has_max) {
                flags = static_cast<std::uint8_t>(flags | number_flag_max);
            }
            node.number_flags = flags;
            node.number_min = parser.number_min;
            node.number_max = parser.number_max;
            break;
        }
        case command::parser_kind::string:
            node.string_type = static_cast<std::uint32_t>(parser.string);
            break;
        case command::parser_kind::enum_values:
            node.string_type = 2U; // greedy phrase, server-side vocabulary
            break;
        case command::parser_kind::time:
            node.time_min = static_cast<std::int32_t>(std::clamp<std::int64_t>(
                parser.integer_min, std::numeric_limits<std::int32_t>::min(),
                std::numeric_limits<std::int32_t>::max()));
            break;
        }
        wire.nodes.push_back(std::move(node));
    }
    return wire;
}

} // namespace refrain::server
