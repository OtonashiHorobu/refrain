// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/command/command.hpp>

#include <boost/ut.hpp>

#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

class recording_source final : public refrain::command::source {
  public:
    explicit recording_source(refrain::command::permission_level required =
                                  refrain::command::level::owner)
        : level_{required} {}

    [[nodiscard]] auto identity() const noexcept
        -> refrain::command::source_identity override {
        return refrain::command::source_identity{std::string_view{"tester"}, 7U,
                                                 true};
    }

    [[nodiscard]] auto permission() const noexcept
        -> refrain::command::permission_level override {
        return level_;
    }

    void reply(std::string_view message) override {
        replies.emplace_back(message);
    }

    std::vector<std::string> replies{};

  private:
    refrain::command::permission_level level_;
};

class even_parser final : public refrain::command::argument_parser {
  public:
    [[nodiscard]] auto descriptor() const
        -> refrain::command::parser_descriptor override {
        refrain::command::parser_descriptor descriptor{};
        descriptor.kind = refrain::command::parser_kind::integer;
        return descriptor;
    }

    [[nodiscard]] auto parse(refrain::command::reader &in) const
        -> refrain::result<refrain::command::argument_value> override {
        const auto word = in.read_word();
        std::int64_t value{0};
        const auto parsed =
            std::from_chars(word.data(), word.data() + word.size(), value);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != word.data() + word.size() || value % 2 != 0) {
            return std::unexpected{refrain::make_error(
                refrain::error_code::parse_error, "expected an even integer")};
        }
        return refrain::command::argument_value{value};
    }

    void suggest(std::string_view prefix, const refrain::command::source &,
                 refrain::command::suggestion_builder &out) const override {
        if (std::string_view{"24"}.starts_with(prefix)) {
            out.add("24");
        }
    }
};

[[maybe_unused]] const suite command_tests = [] {
    "literal and greedy argument execute"_test = [] {
        refrain::command::dispatcher dispatcher;
        dispatcher.literal("say").then(
            dispatcher.argument("message", refrain::command::greedy_arg())
                .executes(
                    [](refrain::command::context &ctx) -> refrain::result<int> {
                        ctx.reply(ctx.string("message"));
                        return 1;
                    }));

        recording_source source;
        const std::string_view input{"say hello  world"};
        auto outcome = dispatcher.execute(input, source);
        expect(static_cast<bool>(outcome));
        if (!outcome) {
            return;
        }
        expect(outcome->success);
        expect(outcome->value == 1);
        expect(outcome->consumed_start == 0U);
        expect(outcome->consumed_end == input.size());
        expect(outcome->feedback.size() == 1U);
        expect(source.replies.size() == 1U);
        expect(source.replies.front() == "hello  world");
    };

    "void and status executors map to result zero"_test = [] {
        refrain::command::dispatcher dispatcher;
        dispatcher.literal("help").executes([](refrain::command::context &) {});
        dispatcher.literal("stat").executes(
            [](refrain::command::context &) -> refrain::status {
                return refrain::status_ok();
            });

        recording_source source;
        auto help = dispatcher.execute("help", source);
        expect(static_cast<bool>(help));
        expect(help && help->value == 0);
        auto stat = dispatcher.execute("stat", source);
        expect(static_cast<bool>(stat));
        expect(stat && stat->value == 0);
    };

    "empty, unknown and trailing input are rejected"_test = [] {
        refrain::command::dispatcher dispatcher;
        dispatcher.literal("help").executes([](refrain::command::context &) {});
        recording_source source;

        auto empty = dispatcher.execute("", source);
        expect(!empty);
        if (!empty) {
            expect(empty.error().code == refrain::error_code::parse_error);
            expect(empty.error().message.find("unknown command") !=
                   std::string::npos);
            expect(empty.error().message.find("at position 0") !=
                   std::string::npos);
        }

        auto unknown = dispatcher.execute("nope", source);
        expect(!unknown);
        if (!unknown) {
            expect(unknown.error().message.find("unknown command") !=
                   std::string::npos);
        }

        auto trailing = dispatcher.execute("help extra", source);
        expect(!trailing);
        if (!trailing) {
            expect(trailing.error().message.find("trailing input") !=
                   std::string::npos);
            expect(trailing.error().message.find("at position 5") !=
                   std::string::npos);
        }
    };

    "missing required argument is reported at the cursor"_test = [] {
        refrain::command::dispatcher dispatcher;
        dispatcher.literal("give").then(
            dispatcher.argument("target", refrain::command::word_arg())
                .executes([](refrain::command::context &) { return 1; }));
        recording_source source;

        auto missing = dispatcher.execute("give", source);
        expect(!missing);
        if (!missing) {
            expect(missing.error().message.find("expected argument") !=
                   std::string::npos);
            expect(missing.error().message.find("at position 4") !=
                   std::string::npos);
        }
        auto trailing_space = dispatcher.execute("give ", source);
        expect(!trailing_space);
        if (!trailing_space) {
            expect(trailing_space.error().message.find("at position 5") !=
                   std::string::npos);
        }
    };

    "quoted strings decode quotes, backslashes and reject bad tails"_test = [] {
        refrain::command::dispatcher dispatcher;
        dispatcher.literal("echo").then(
            dispatcher.argument("text", refrain::command::quoted_arg())
                .executes([](refrain::command::context &ctx) -> int {
                    ctx.reply(ctx.string("text"));
                    return 1;
                }));

        recording_source source;
        auto plain = dispatcher.execute("echo \"hello world\"", source);
        expect(static_cast<bool>(plain));
        expect(source.replies.back() == "hello world");

        auto quoted = dispatcher.execute("echo \"a\\\"b\"", source);
        expect(static_cast<bool>(quoted));
        expect(source.replies.back() == "a\"b");

        auto slashed = dispatcher.execute("echo 'a\\\\b'", source);
        expect(static_cast<bool>(slashed));
        expect(source.replies.back() == "a\\b");

        auto preserved = dispatcher.execute("echo \"a\\nb\"", source);
        expect(static_cast<bool>(preserved));
        expect(source.replies.back() == "a\\nb");

        auto unclosed = dispatcher.execute("echo \"oops", source);
        expect(!unclosed);
        if (!unclosed) {
            expect(unclosed.error().message.find(
                       "unterminated quoted string") != std::string::npos);
            expect(unclosed.error().message.find("at position 5") !=
                   std::string::npos);
        }

        auto glued = dispatcher.execute("echo \"a\"b", source);
        expect(!glued);
        if (!glued) {
            expect(glued.error().message.find("unexpected character") !=
                   std::string::npos);
        }
    };

    "integer bounds and forms are enforced"_test = [] {
        refrain::command::dispatcher dispatcher;
        dispatcher.literal("count").then(
            dispatcher.argument("n", refrain::command::integer_arg(1, 10))
                .executes([](refrain::command::context &ctx) -> int {
                    return static_cast<int>(ctx.integer("n"));
                }));
        dispatcher.literal("big").then(
            dispatcher.argument("n", refrain::command::long_arg())
                .executes([](refrain::command::context &ctx) -> int {
                    const auto *value = ctx.get<std::int64_t>("n");
                    return value != nullptr ? 1 : 0;
                }));

        recording_source source;
        auto middle = dispatcher.execute("count 5", source);
        expect(static_cast<bool>(middle));
        expect(middle && middle->value == 5);
        auto signed_value = dispatcher.execute("count +7", source);
        expect(static_cast<bool>(signed_value));
        expect(signed_value && signed_value->value == 7);

        auto below = dispatcher.execute("count 0", source);
        expect(!below);
        if (!below) {
            expect(below.error().message.find("below the minimum") !=
                   std::string::npos);
        }
        auto above = dispatcher.execute("count 11", source);
        expect(!above);
        if (!above) {
            expect(above.error().message.find("above the maximum") !=
                   std::string::npos);
        }
        expect(!dispatcher.execute("count nope", source));
        expect(!dispatcher.execute("count 999999999999999999999", source));

        auto huge = dispatcher.execute("big 9223372036854775807", source);
        expect(static_cast<bool>(huge));
        if (huge) {
            expect(huge->value == 1);
        }
        expect(!dispatcher.execute("big 9223372036854775808", source));
    };

    "double, float and boolean parsers validate their domain"_test = [] {
        refrain::command::dispatcher dispatcher;
        dispatcher.literal("rate").then(
            dispatcher.argument("x", refrain::command::double_arg(0.0, 1.0))
                .executes([](refrain::command::context &ctx) -> int {
                    return static_cast<int>(ctx.number("x") * 10.0);
                }));
        dispatcher.literal("small").then(
            dispatcher.argument("x", refrain::command::float_arg())
                .executes([](refrain::command::context &ctx) -> int {
                    return ctx.get<double>("x") != nullptr ? 1 : 0;
                }));
        dispatcher.literal("flag").then(
            dispatcher.argument("on", refrain::command::bool_arg())
                .executes([](refrain::command::context &ctx) -> int {
                    return ctx.boolean("on") ? 1 : 0;
                }));

        recording_source source;
        auto half = dispatcher.execute("rate 0.5", source);
        expect(static_cast<bool>(half));
        expect(half && half->value == 5);
        expect(!dispatcher.execute("rate 1.5", source));
        expect(!dispatcher.execute("rate nan", source));
        expect(!dispatcher.execute("rate 1e999", source));

        expect(static_cast<bool>(dispatcher.execute("small 1.5", source)));
        expect(!dispatcher.execute("small 1e39", source));

        auto truth = dispatcher.execute("flag true", source);
        expect(static_cast<bool>(truth));
        expect(truth && truth->value == 1);
        auto lie = dispatcher.execute("flag false", source);
        expect(static_cast<bool>(lie));
        expect(lie && lie->value == 0);
        expect(!dispatcher.execute("flag yes", source));
    };

    "resource location parser canonicalizes and validates"_test = [] {
        refrain::command::dispatcher dispatcher;
        dispatcher.literal("loc").then(
            dispatcher.argument("id", refrain::command::resource_location_arg())
                .executes([](refrain::command::context &ctx) -> int {
                    const auto *location = ctx.location("id");
                    if (location == nullptr) {
                        return 0;
                    }
                    ctx.reply(location->to_string());
                    return 1;
                }));

        recording_source source;
        expect(static_cast<bool>(dispatcher.execute("loc stone", source)));
        expect(source.replies.back() == "minecraft:stone");
        expect(static_cast<bool>(
            dispatcher.execute("loc minecraft:block/stone", source)));
        expect(source.replies.back() == "minecraft:block/stone");
        expect(!dispatcher.execute("loc Bad", source));
        expect(!dispatcher.execute("loc minecraft:block//stone", source));
    };

    "enum parser accepts names, numeric aliases and mixed case"_test = [] {
        refrain::command::dispatcher dispatcher;
        dispatcher.literal("gamemode")
            .then(dispatcher.argument("mode", refrain::command::gamemode_arg())
                      .executes([](refrain::command::context &ctx) -> int {
                          ctx.reply(ctx.string("mode"));
                          return 1;
                      }));

        recording_source source;
        expect(
            static_cast<bool>(dispatcher.execute("gamemode Creative", source)));
        expect(source.replies.back() == "creative");
        expect(static_cast<bool>(dispatcher.execute("gamemode 2", source)));
        expect(source.replies.back() == "adventure");
        expect(static_cast<bool>(
            dispatcher.execute("gamemode spectator", source)));
        expect(source.replies.back() == "spectator");
        expect(!dispatcher.execute("gamemode builder", source));
    };

    "custom parsers plug into argument registration"_test = [] {
        refrain::command::dispatcher dispatcher;
        dispatcher.literal("even").then(
            dispatcher.argument("n", even_parser{})
                .executes([](refrain::command::context &ctx) -> int {
                    return static_cast<int>(ctx.integer("n"));
                }));
        recording_source source;
        auto accepted = dispatcher.execute("even 24", source);
        expect(static_cast<bool>(accepted));
        expect(accepted && accepted->value == 24);
        expect(!dispatcher.execute("even 23", source));

        auto offered = dispatcher.suggest("even ", 5, source);
        expect(offered.entries.size() == 1U);
        expect(offered.entries.front().text == "24");
    };

    "requirements filter dispatch and do not leak hidden commands"_test = [] {
        refrain::command::dispatcher dispatcher;
        dispatcher.literal("gamemode")
            .require(refrain::command::level::gamemode)
            .then(dispatcher.argument("mode", refrain::command::gamemode_arg())
                      .executes([](refrain::command::context &ctx) -> int {
                          ctx.reply(ctx.string("mode"));
                          return 1;
                      }));

        recording_source low{refrain::command::level::everyone};
        recording_source high{refrain::command::level::owner};

        auto denied = dispatcher.execute("gamemode creative", low);
        expect(!denied);
        if (!denied) {
            expect(denied.error().message.find("unknown command") !=
                   std::string::npos);
        }
        auto allowed = dispatcher.execute("gamemode creative", high);
        expect(static_cast<bool>(allowed));
        expect(allowed && allowed->success);
        expect(high.replies.back() == "creative");
    };

    "requirements can use an arbitrary predicate"_test = [] {
        refrain::command::dispatcher dispatcher;
        dispatcher.literal("debug")
            .require([](const refrain::command::source &source) {
                return source.identity().key == 7U;
            })
            .executes([](refrain::command::context &) { return 1; });

        recording_source matching;
        auto allowed = dispatcher.execute("debug", matching);
        expect(static_cast<bool>(allowed));
        expect(allowed && allowed->value == 1);

        refrain::command::player_source other{"other", 9U};
        expect(!dispatcher.execute("debug", other));
    };

    "executor failure and errors are reported as values"_test = [] {
        refrain::command::dispatcher dispatcher;
        dispatcher.literal("deny").executes(
            [](refrain::command::context &ctx) -> int {
                ctx.fail("not allowed");
                return 0;
            });
        dispatcher.literal("boom").executes(
            [](refrain::command::context &) -> refrain::result<int> {
                return std::unexpected{refrain::make_error(
                    refrain::error_code::cancelled, "cancelled by test")};
            });

        recording_source source;
        auto denied = dispatcher.execute("deny", source);
        expect(static_cast<bool>(denied));
        if (denied) {
            expect(!denied->success);
            expect(denied->failure == "not allowed");
            expect(denied->value == 0);
        }
        expect(source.replies.back() == "not allowed");

        auto failed = dispatcher.execute("boom", source);
        expect(!failed);
        if (!failed) {
            expect(failed.error().code == refrain::error_code::cancelled);
            expect(failed.error().message == "cancelled by test");
        }
    };

    "context exposes names, values and byte ranges"_test = [] {
        refrain::command::dispatcher dispatcher;
        dispatcher.literal("count").then(
            dispatcher.argument("n", refrain::command::integer_arg())
                .executes([](refrain::command::context &ctx) -> int {
                    const auto arguments = ctx.arguments();
                    if (arguments.size() != 1U) {
                        return 0;
                    }
                    if (arguments[0].name != "n" || arguments[0].start != 6U ||
                        arguments[0].end != 7U) {
                        return 0;
                    }
                    const auto *value = ctx.get<std::int64_t>("n");
                    return value != nullptr && *value == 5 ? 1 : 0;
                }));

        recording_source source;
        auto outcome = dispatcher.execute("count 5", source);
        expect(static_cast<bool>(outcome));
        expect(outcome && outcome->value == 1);
    };

    "source implementations expose identity, level and replies"_test = [] {
        std::vector<std::string> lines;
        refrain::command::player_source player{
            "Steve", 42U, refrain::command::level::gamemode,
            [&lines](std::string_view message) {
                lines.emplace_back(message);
            }};
        expect(player.name() == "Steve");
        expect(player.identity().key == 42U);
        expect(player.identity().player);
        expect(player.has_permission(refrain::command::level::gamemode));
        expect(!player.has_permission(refrain::command::level::owner));
        player.reply("hello");
        expect(lines.size() == 1U);
        expect(lines.front() == "hello");

        refrain::command::console_source console;
        expect(console.name() == "console");
        expect(!console.identity().player);
        expect(console.permission() == refrain::command::level::owner);
        expect(console.has_permission(refrain::command::level::owner));
    };

    "oversized input is rejected by execution and suggestions"_test = [] {
        refrain::command::dispatcher dispatcher;
        dispatcher.literal("help").executes([](refrain::command::context &) {});
        recording_source source;
        const std::string huge(refrain::command::max_command_bytes + 1, 'a');
        auto outcome = dispatcher.execute(huge, source);
        expect(!outcome);
        if (!outcome) {
            expect(outcome.error().code ==
                   refrain::error_code::invalid_argument);
        }
        auto offered = dispatcher.suggest(huge, huge.size(), source);
        expect(offered.empty());
    };
};

} // namespace
