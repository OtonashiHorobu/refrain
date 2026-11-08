// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/command/command.hpp>

#include <boost/ut.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

class source_impl final : public refrain::command::source {
  public:
    explicit source_impl(refrain::command::permission_level required =
                             refrain::command::level::owner)
        : level_{required} {}

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

  private:
    refrain::command::permission_level level_;
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

        auto wrong_case = dispatcher.suggest("GA", 2, source);
        expect(wrong_case.empty());
    };

    "enum suggestions complete names and numeric aliases"_test = [] {
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

        auto numeric = dispatcher.suggest("gamemode 0", 10, source);
        expect(numeric.entries.size() == 1U);
        expect(numeric.entries.front().text == "survival");

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
        expect(hidden.start == 9U);
        expect(hidden.end == 9U);
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
