// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/command/command.hpp>

#include <boost/ut.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

class example_source final : public refrain::command::source {
  public:
    [[nodiscard]] auto identity() const noexcept
        -> refrain::command::source_identity override {
        return refrain::command::source_identity{std::string_view{"example"},
                                                 1U, true};
    }
    [[nodiscard]] auto permission() const noexcept
        -> refrain::command::permission_level override {
        return refrain::command::level::owner;
    }
    void reply(std::string_view message) override {
        replies.emplace_back(message);
    }

    std::vector<std::string> replies{};
};

[[nodiscard]] auto has_reply(const std::vector<std::string> &replies,
                             std::string_view expected) -> bool {
    for (const auto &reply : replies) {
        if (reply == expected) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] auto has_suggestion(const refrain::command::suggestions &offered,
                                  std::string_view text) -> bool {
    for (const auto &entry : offered.entries) {
        if (entry.text == text) {
            return true;
        }
    }
    return false;
}

constexpr std::string_view example_line =
    "example true 7 123456789012 0.5 1.25 word \"quoted text\" "
    "minecraft:stone 1 2 3 0.5 64 1.5 90 45 Steve creative 20s "
    "minecraft:overworld dd12be42-52a9-4a91-a8a1-11c01849e498";

[[maybe_unused]] const suite command_example_tests = [] {
    "the example command parses every typed argument end to end"_test = [] {
        refrain::command::dispatcher dispatcher;
        refrain::command::register_parser_examples(dispatcher);

        example_source source;
        auto outcome = dispatcher.execute(example_line, source);
        expect(static_cast<bool>(outcome));
        if (!outcome) {
            return;
        }
        expect(outcome->success);
        expect(outcome->value == 16);
        expect(has_reply(source.replies, "flag true")) << source.replies.size();
        expect(has_reply(source.replies, "count 7"));
        expect(has_reply(source.replies, "big 123456789012"));
        expect(has_reply(source.replies, "ratio 0.5"));
        expect(has_reply(source.replies, "precise 1.25"));
        expect(has_reply(source.replies, "word word"));
        expect(has_reply(source.replies, "quoted quoted text"));
        expect(has_reply(source.replies, "id minecraft:stone"));
        expect(has_reply(source.replies, "pos 1 2 3"));
        expect(has_reply(source.replies, "vec 0.5 64 1.5"));
        expect(has_reply(source.replies, "rot 90 45"));
        expect(has_reply(source.replies, "player Steve"));
        expect(has_reply(source.replies, "mode creative"));
        expect(has_reply(source.replies, "time 400"));
        expect(has_reply(source.replies, "dimension minecraft:overworld"));
        expect(has_reply(source.replies,
                         "identifier dd12be42-52a9-4a91-a8a1-11c01849e498"));
    };

    "the example command reports a typed argument failure"_test = [] {
        refrain::command::dispatcher dispatcher;
        refrain::command::register_parser_examples(dispatcher);
        example_source source;

        auto bad_mode = dispatcher.execute(
            "example true 7 1 0.5 1.25 w x minecraft:stone 1 2 3 0.5 64 1.5 "
            "90 45 Steve builder 20s minecraft:overworld "
            "dd12be42-52a9-4a91-a8a1-11c01849e498",
            source);
        expect(!bad_mode);
        if (!bad_mode) {
            expect(bad_mode.error().message.find("unknown game mode") !=
                   std::string::npos);
        }

        auto missing = dispatcher.execute("example true", source);
        expect(!missing);
        if (!missing) {
            expect(missing.error().message.find("expected argument") !=
                   std::string::npos);
            expect(missing.error().message.find("at position 12") !=
                   std::string::npos);
        }
    };

    "the example command produces a multi-level tree"_test = [] {
        refrain::command::dispatcher dispatcher;
        refrain::command::register_parser_examples(dispatcher);
        example_source source;
        const auto snapshot = dispatcher.tree(source);
        expect(snapshot != nullptr);
        if (snapshot == nullptr) {
            return;
        }
        expect(snapshot->nodes.size() == 18U);
        expect(snapshot->root_node().children.size() == 1U);
        if (snapshot->root_node().children.size() != 1U) {
            return;
        }
        const auto example_index = snapshot->root_node().children.front();
        expect(snapshot->nodes[example_index].name == "example");
        expect(snapshot->nodes[example_index].is_literal());

        // The chain is linear; walk it and check the parser of every step.
        const std::vector<
            std::pair<std::string_view, refrain::command::parser_kind>>
            expected{
                {"flag", refrain::command::parser_kind::boolean},
                {"count", refrain::command::parser_kind::integer},
                {"big", refrain::command::parser_kind::long_integer},
                {"ratio", refrain::command::parser_kind::float_number},
                {"precise", refrain::command::parser_kind::double_number},
                {"word", refrain::command::parser_kind::string},
                {"quoted", refrain::command::parser_kind::string},
                {"id", refrain::command::parser_kind::resource_location},
                {"pos", refrain::command::parser_kind::block_position},
                {"vec", refrain::command::parser_kind::vec3},
                {"rot", refrain::command::parser_kind::rotation},
                {"player", refrain::command::parser_kind::player_name},
                {"mode", refrain::command::parser_kind::gamemode},
                {"time", refrain::command::parser_kind::time},
                {"dimension", refrain::command::parser_kind::dimension},
                {"identifier", refrain::command::parser_kind::uuid},
        };
        auto index = example_index;
        for (const auto &[name, kind] : expected) {
            const auto &node = snapshot->nodes[index];
            expect(node.children.size() == 1U) << std::string{name};
            if (node.children.size() != 1U) {
                return;
            }
            index = node.children.front();
            const auto &child = snapshot->nodes[index];
            expect(child.name == name) << std::string{name};
            expect(child.parser.kind == kind) << std::string{name};
        }
        expect(snapshot->nodes[index].executable);
        expect(snapshot->nodes[index].children.empty());
    };

    "the example command suggests at every typed argument"_test = [] {
        refrain::command::dispatcher dispatcher;
        refrain::command::register_parser_examples(dispatcher);
        example_source source;

        auto flags = dispatcher.suggest("example ", 8, source);
        expect(has_suggestion(flags, "true"));
        expect(has_suggestion(flags, "false"));

        auto counts = dispatcher.suggest("example true ", 13, source);
        expect(has_suggestion(counts, "0"));
        expect(has_suggestion(counts, "100"));

        const std::string before_pos{
            "example true 7 1 0.5 1.25 w x minecraft:stone "};
        auto positions =
            dispatcher.suggest(before_pos, before_pos.size(), source);
        expect(positions.start == before_pos.size());
        expect(has_suggestion(positions, "~"));
        expect(has_suggestion(positions, "~ ~ ~"));

        const std::string before_mode{
            "example true 7 1 0.5 1.25 w x minecraft:stone 1 2 3 0.5 64 1.5 "
            "90 45 Steve "};
        auto modes =
            dispatcher.suggest(before_mode, before_mode.size(), source);
        expect(has_suggestion(modes, "creative"));
        expect(has_suggestion(modes, "spectator"));

        const std::string before_time{before_mode + "creative "};
        auto times =
            dispatcher.suggest(before_time, before_time.size(), source);
        expect(has_suggestion(times, "day"));
        expect(has_suggestion(times, "1d"));

        const std::string before_dimension{before_time + "20s "};
        auto dimensions = dispatcher.suggest(before_dimension,
                                             before_dimension.size(), source);
        expect(has_suggestion(dimensions, "minecraft:overworld"));

        const std::string before_uuid{before_dimension +
                                      "minecraft:overworld "};
        auto identifiers =
            dispatcher.suggest(before_uuid, before_uuid.size(), source);
        expect(identifiers.empty());
    };
};

} // namespace
