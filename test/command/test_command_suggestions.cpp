// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/command/command.hpp>

#include <boost/ut.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

class source_impl final : public refrain::command::source {
  public:
    explicit source_impl(refrain::command::permission_level required =
                             refrain::command::level::owner,
                         std::vector<std::string> online = {})
        : level_{required}, online_{std::move(online)} {}

    [[nodiscard]] auto identity() const noexcept
        -> refrain::command::source_identity override {
        return refrain::command::source_identity{std::string_view{"suggester"},
                                                 11U, true};
    }
    [[nodiscard]] auto permission() const noexcept
        -> refrain::command::permission_level override {
        return level_;
    }
    void reply(std::string_view) override {}
    [[nodiscard]] auto online_players() const
        -> std::vector<std::string> override {
        return online_;
    }

  private:
    refrain::command::permission_level level_;
    std::vector<std::string> online_;
};

[[nodiscard]] auto find_entry(const refrain::command::suggestions &offered,
                              std::string_view text)
    -> const refrain::command::suggestion * {
    for (const auto &entry : offered.entries) {
        if (entry.text == text) {
            return &entry;
        }
    }
    return nullptr;
}

[[nodiscard]] auto contains(const refrain::command::suggestions &offered,
                            std::string_view text) -> bool {
    return find_entry(offered, text) != nullptr;
}

[[maybe_unused]] const suite command_suggestion_tests = [] {
    "literal suggestions carry prefix range and tooltip"_test = [] {
        refrain::command::dispatcher dispatcher;
        dispatcher.literal("gamemode")
            .tooltip("Change game mode")
            .require(refrain::command::level::gamemode)
            .then(dispatcher.argument("mode", refrain::command::gamemode_arg())
                      .executes([](refrain::command::context &) { return 1; }));
        dispatcher.literal("give")
            .tooltip("Give items")
            .executes([](refrain::command::context &) { return 1; });
        dispatcher.literal("help").executes(
            [](refrain::command::context &) { return 1; });

        source_impl source;
        auto all = dispatcher.suggest("", 0, source);
        expect(all.entries.size() == 3U);
        expect(all.start == 0U);
        expect(all.end == 0U);
        const auto *gamemode = find_entry(all, "gamemode");
        expect(gamemode != nullptr);
        if (gamemode != nullptr) {
            expect(gamemode->tooltip == "Change game mode");
        }

        auto prefix = dispatcher.suggest("ga", 2, source);
        expect(prefix.entries.size() == 1U);
        expect(prefix.start == 0U);
        expect(prefix.end == 2U);
        expect(prefix.entries.front().text == "gamemode");

        auto leading = dispatcher.suggest("  ga", 4, source);
        expect(leading.start == 2U);
        expect(leading.end == 4U);
        expect(leading.entries.size() == 1U);
    };

    "gamemode suggestions complete canonical names only"_test = [] {
        refrain::command::dispatcher dispatcher;
        dispatcher.literal("gamemode")
            .then(dispatcher.argument("mode", refrain::command::gamemode_arg())
                      .executes([](refrain::command::context &) { return 1; }));

        source_impl source;
        auto all = dispatcher.suggest("gamemode ", 9, source);
        expect(all.entries.size() == 4U);
        expect(all.start == 9U);
        expect(all.end == 9U);
        const auto *survival = find_entry(all, "survival");
        expect(survival != nullptr);
        if (survival != nullptr) {
            expect(survival->tooltip == "Survival mode");
        }

        auto creative = dispatcher.suggest("gamemode cr", 11, source);
        expect(creative.entries.size() == 1U);
        expect(creative.start == 9U);
        expect(creative.end == 11U);
        expect(creative.entries.front().text == "creative");

        // A cursor on the last byte of a parsed argument completes that
        // argument, so a complete value suggests itself.
        auto complete = dispatcher.suggest("gamemode creative", 17, source);
        expect(complete.entries.size() == 1U);
        expect(complete.entries.front().text == "creative");
        expect(complete.start == 9U);
        expect(complete.end == 17U);
        // A numeric alias parses but is not a completion candidate.
        auto numeric = dispatcher.suggest("gamemode 0", 10, source);
        expect(numeric.empty());

        auto middle = dispatcher.suggest("gamemode creative", 12, source);
        expect(middle.start == 9U);
        expect(middle.end == 12U);
        expect(find_entry(middle, "creative") != nullptr);
    };

    "boolean parser suggests both literals"_test = [] {
        refrain::command::dispatcher dispatcher;
        dispatcher.literal("flag").then(
            dispatcher.argument("on", refrain::command::bool_arg())
                .executes([](refrain::command::context &) { return 1; }));
        source_impl source;
        auto all = dispatcher.suggest("flag ", 5, source);
        expect(all.entries.size() == 2U);
        expect(find_entry(all, "true") != nullptr);
        expect(find_entry(all, "false") != nullptr);
        auto only_true = dispatcher.suggest("flag t", 6, source);
        expect(only_true.entries.size() == 1U);
        expect(only_true.entries.front().text == "true");
    };

    "block position suggestions replace the whole partial argument"_test = [] {
        refrain::command::dispatcher dispatcher;
        dispatcher.literal("blocks").then(
            dispatcher.argument("pos", refrain::command::block_position_arg())
                .executes([](refrain::command::context &) { return 1; }));
        source_impl source;

        auto empty = dispatcher.suggest("blocks ", 7, source);
        expect(empty.start == 7U);
        expect(empty.end == 7U);
        expect(contains(empty, "~"));
        expect(contains(empty, "~ ~"));
        expect(contains(empty, "~ ~ ~"));

        auto first = dispatcher.suggest("blocks 1", 8, source);
        expect(first.start == 7U);
        expect(first.end == 8U);
        expect(contains(first, "1 ~"));
        expect(contains(first, "1 ~ ~"));

        auto second = dispatcher.suggest("blocks 1 2", 10, source);
        expect(second.start == 7U);
        expect(second.end == 10U);
        expect(contains(second, "1 2 ~"));

        auto local = dispatcher.suggest("blocks ^", 8, source);
        expect(contains(local, "^ ^"));
        expect(contains(local, "^ ^ ^"));

        // A trailing separator after a complete position moves completion to
        // the node's children, of which there are none.
        auto complete = dispatcher.suggest("blocks 1 2 3 ", 12, source);
        expect(complete.empty());
    };

    "vec3 suggestions replace the whole partial argument"_test = [] {
        refrain::command::dispatcher dispatcher;
        dispatcher.literal("move").then(
            dispatcher.argument("location", refrain::command::vec3_arg())
                .executes([](refrain::command::context &) { return 1; }));
        source_impl source;

        auto empty = dispatcher.suggest("move ", 5, source);
        expect(empty.start == 5U);
        expect(empty.end == 5U);
        expect(contains(empty, "~"));
        expect(contains(empty, "~ ~"));
        expect(contains(empty, "~ ~ ~"));

        auto partial = dispatcher.suggest("move 0.5", 8, source);
        expect(partial.start == 5U);
        expect(partial.end == 8U);
        expect(contains(partial, "0.5 ~"));
        expect(contains(partial, "0.5 ~ ~"));

        auto local = dispatcher.suggest("move ^", 6, source);
        expect(contains(local, "^ ^"));
        expect(contains(local, "^ ^ ^"));
    };

    "rotation suggestions fill the missing pitch"_test = [] {
        refrain::command::dispatcher dispatcher;
        dispatcher.literal("rotate").then(
            dispatcher.argument("rot", refrain::command::rotation_arg())
                .executes([](refrain::command::context &) { return 1; }));
        source_impl source;
        auto all = dispatcher.suggest("rotate ", 7, source);
        expect(all.start == 7U);
        expect(contains(all, "~"));
        expect(contains(all, "~ ~"));
        auto partial = dispatcher.suggest("rotate 90", 9, source);
        expect(contains(partial, "90 ~"));
    };

    "time suggestions offer units and clock markers"_test = [] {
        refrain::command::dispatcher dispatcher;
        dispatcher.literal("set").then(
            dispatcher
                .argument("time",
                          refrain::command::time_arg(
                              0, refrain::command::world_time_markers()))
                .executes([](refrain::command::context &) { return 1; }));
        source_impl source;
        auto markers = dispatcher.suggest("set ", 4, source);
        expect(markers.start == 4U);
        expect(contains(markers, "day"));
        expect(contains(markers, "noon"));
        expect(contains(markers, "midnight"));
        expect(contains(markers, "1d"));

        auto units = dispatcher.suggest("set 2", 5, source);
        expect(contains(units, "2d"));
        expect(contains(units, "2s"));
        expect(contains(units, "2t"));

        auto marker = dispatcher.suggest("set mi", 6, source);
        expect(marker.entries.size() == 1U);
        expect(marker.entries.front().text == "midnight");
    };

    "player name suggestions come from the source player list"_test = [] {
        refrain::command::dispatcher dispatcher;
        dispatcher.literal("msg").then(
            dispatcher.argument("target", refrain::command::player_name_arg())
                .executes([](refrain::command::context &) { return 1; }));
        source_impl source{refrain::command::level::owner, {"Alice", "Bob"}};
        auto all = dispatcher.suggest("msg ", 4, source);
        expect(all.entries.size() == 2U);
        expect(contains(all, "Alice"));
        expect(contains(all, "Bob"));
        auto prefix = dispatcher.suggest("msg Al", 6, source);
        expect(prefix.entries.size() == 1U);
        expect(prefix.entries.front().text == "Alice");

        source_impl empty;
        expect(dispatcher.suggest("msg ", 4, empty).empty());
    };

    "dimension suggestions offer installed ids"_test = [] {
        refrain::command::dispatcher dispatcher;
        dispatcher.literal("dimension")
            .then(dispatcher.argument("id", refrain::command::dimension_arg())
                      .executes([](refrain::command::context &) { return 1; }));
        source_impl source;
        auto all = dispatcher.suggest("dimension ", 10, source);
        expect(all.entries.size() == 3U);
        expect(contains(all, "minecraft:overworld"));
        auto prefix = dispatcher.suggest("dimension minecraft:n", 21, source);
        expect(prefix.entries.size() == 1U);
        expect(prefix.entries.front().text == "minecraft:nether");
    };

    "suggestions are filtered by requirements"_test = [] {
        refrain::command::dispatcher dispatcher;
        dispatcher.literal("gamemode")
            .require(refrain::command::level::gamemode)
            .then(dispatcher.argument("mode", refrain::command::gamemode_arg())
                      .executes([](refrain::command::context &) { return 1; }));
        dispatcher.literal("help").executes(
            [](refrain::command::context &) { return 1; });

        source_impl low{refrain::command::level::everyone};
        auto all = dispatcher.suggest("", 0, low);
        expect(all.entries.size() == 1U);
        expect(all.entries.front().text == "help");

        auto hidden = dispatcher.suggest("gamemode ", 9, low);
        expect(hidden.empty());
        expect(hidden.start == 0U);
        expect(hidden.end == 9U);
    };

    "suggestions are filtered by the visibility hook"_test = [] {
        refrain::command::dispatcher dispatcher;
        dispatcher.literal("public").executes(
            [](refrain::command::context &) { return 1; });
        dispatcher.literal("secret")
            .require_node("refrain.command.secret")
            .executes([](refrain::command::context &) { return 1; });
        dispatcher.set_permission_filter(
            [](const refrain::command::source &,
               const refrain::command::node_permission &permission) {
                return permission.node == "refrain.command.secret"
                           ? refrain::command::permission_decision::denied
                           : refrain::command::permission_decision::allowed;
            });
        source_impl source;
        auto all = dispatcher.suggest("", 0, source);
        expect(all.entries.size() == 1U);
        expect(all.entries.front().text == "public");
    };

    "redirected nodes suggest the target children"_test = [] {
        refrain::command::dispatcher dispatcher;
        auto gamemode = dispatcher.literal("gamemode");
        gamemode.then(
            dispatcher.argument("mode", refrain::command::gamemode_arg())
                .executes([](refrain::command::context &) { return 1; }));
        dispatcher.literal("gm").redirect(gamemode);

        source_impl source;
        auto alias = dispatcher.suggest("gm ", 3, source);
        expect(alias.entries.size() == 4U);
        expect(alias.start == 3U);
        expect(alias.end == 3U);
        expect(find_entry(alias, "creative") != nullptr);
    };

    "suggestion output is bounded and marked truncated"_test = [] {
        refrain::command::dispatcher dispatcher;
        for (int index = 0; index < 1'100; ++index) {
            static_cast<void>(dispatcher.literal("c" + std::to_string(index)));
        }
        source_impl source;
        auto offered = dispatcher.suggest("", 0, source);
        expect(offered.entries.size() == refrain::command::max_suggestions);
        expect(offered.truncated);
    };

    "cursors outside the input are clamped"_test = [] {
        refrain::command::dispatcher dispatcher;
        dispatcher.literal("help").executes(
            [](refrain::command::context &) { return 1; });
        source_impl source;
        auto offered = dispatcher.suggest("help", 9'999, source);
        expect(offered.start == 0U);
        expect(offered.end == 4U);
        expect(offered.entries.size() == 1U);
        expect(offered.entries.front().text == "help");
    };

    "builder rejects empty and duplicate entries"_test = [] {
        refrain::command::suggestion_builder builder{4, 9};
        builder.add("");
        builder.add("stone");
        builder.add("stone");
        auto offered = std::move(builder).build();
        expect(offered.entries.size() == 1U);
        expect(offered.start == 4U);
        expect(offered.end == 9U);
    };
};

} // namespace
