// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/command/examples.hpp>

#include <refrain/command/parsers.hpp>

#include <fmt/format.h>

#include <string>

namespace refrain::command {

namespace {

/// Report one argument line. Every typed value is read through the typed
/// `context` accessor, which is what a real command body does.
void report_example_values(context &ctx) {
    ctx.reply(fmt::format("flag {}", ctx.boolean("flag")));
    ctx.reply(fmt::format("count {}", ctx.integer("count")));
    ctx.reply(fmt::format("big {}", ctx.integer("big")));
    ctx.reply(fmt::format("ratio {}", ctx.number("ratio")));
    ctx.reply(fmt::format("precise {}", ctx.number("precise")));
    ctx.reply(fmt::format("word {}", ctx.string("word")));
    ctx.reply(fmt::format("quoted {}", ctx.string("quoted")));
    if (const auto *id = ctx.location("id"); id != nullptr) {
        ctx.reply(fmt::format("id {}", id->to_string()));
    }
    if (const auto *pos = ctx.position("pos"); pos != nullptr) {
        ctx.reply(fmt::format("pos {} {} {}{}{}", pos->x, pos->y, pos->z,
                              pos->local ? " local" : "",
                              pos->relative_x ? " relative-x" : ""));
    }
    if (const auto *vec = ctx.vec3("vec"); vec != nullptr) {
        ctx.reply(fmt::format("vec {} {} {}{}", vec->x, vec->y, vec->z,
                              vec->local ? " local" : ""));
    }
    if (const auto *rot = ctx.rotation_value("rot"); rot != nullptr) {
        ctx.reply(fmt::format("rot {} {}", rot->yaw, rot->pitch));
    }
    ctx.reply(fmt::format("player {}", ctx.string("player")));
    if (const auto mode = ctx.gamemode("mode"); mode.has_value()) {
        ctx.reply(fmt::format("mode {}", to_string(*mode)));
    }
    ctx.reply(fmt::format("time {}", ctx.integer("time")));
    if (const auto *dimension = ctx.location("dimension");
        dimension != nullptr) {
        ctx.reply(fmt::format("dimension {}", dimension->to_string()));
    }
    if (const auto *identifier = ctx.uuid_value("identifier");
        identifier != nullptr) {
        ctx.reply(fmt::format("identifier {}", identifier->to_string()));
    }
}

} // namespace

void register_parser_examples(dispatcher &into) {
    auto example{into.literal("example")
                     .require_node("refrain.command.example")
                     .tooltip("Exercise every typed command argument")};
    auto flag{into.argument("flag", bool_arg())};
    auto count{into.argument("count", integer_arg(0, 100))};
    auto big{into.argument("big", long_arg())};
    auto ratio{into.argument("ratio", float_arg(-1.0, 1.0))};
    auto precise{into.argument("precise", double_arg())};
    auto word{into.argument("word", word_arg())};
    auto quoted{into.argument("quoted", quoted_arg())};
    auto id{into.argument("id", resource_location_arg())};
    auto pos{into.argument("pos", block_position_arg())};
    auto vec{into.argument("vec", vec3_arg())};
    auto rot{into.argument("rot", rotation_arg())};
    auto player{into.argument("player", player_name_arg())};
    auto mode{into.argument("mode", gamemode_arg())};
    auto time{into.argument("time", time_arg(0, world_time_markers()))};
    auto dimension{into.argument("dimension", dimension_arg())};
    auto identifier{into.argument("identifier", uuid_arg())};

    count.then(big);
    big.then(ratio);
    ratio.then(precise);
    precise.then(word);
    word.then(quoted);
    quoted.then(id);
    id.then(pos);
    pos.then(vec);
    vec.then(rot);
    rot.then(player);
    player.then(mode);
    mode.then(time);
    time.then(dimension);
    dimension.then(identifier);
    example.then(flag);
    flag.then(count);
    identifier.executes([](context &ctx) -> int {
        report_example_values(ctx);
        return static_cast<int>(ctx.arguments().size());
    });
}

} // namespace refrain::command
