// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/command/command.hpp>

#include <boost/ut.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

class tree_source final : public refrain::command::source {
  public:
    explicit tree_source(refrain::command::permission_level required =
                             refrain::command::level::owner)
        : level_{required} {}

    [[nodiscard]] auto identity() const noexcept
        -> refrain::command::source_identity override {
        return refrain::command::source_identity{
            std::string_view{"tree-tester"}, 3U, true};
    }
    [[nodiscard]] auto permission() const noexcept
        -> refrain::command::permission_level override {
        return level_;
    }
    void reply(std::string_view) override {}

  private:
    refrain::command::permission_level level_;
};

[[nodiscard]] auto find_node(const refrain::command::tree &snapshot,
                             std::string_view name)
    -> const refrain::command::tree_node * {
    for (const auto &node : snapshot.nodes) {
        if (node.name == name) {
            return &node;
        }
    }
    return nullptr;
}

[[nodiscard]] auto index_of(const refrain::command::tree &snapshot,
                            std::string_view name) -> std::size_t {
    for (std::size_t index = 0; index < snapshot.nodes.size(); ++index) {
        if (snapshot.nodes[index].name == name) {
            return index;
        }
    }
    return snapshot.nodes.size();
}

[[nodiscard]] auto contains(const std::vector<std::string> &values,
                            std::string_view expected) -> bool {
    for (const auto &value : values) {
        if (value == expected) {
            return true;
        }
    }
    return false;
}

[[maybe_unused]] const suite command_tree_tests = [] {
    "tree describes literals, arguments and parser properties"_test = [] {
        refrain::command::dispatcher dispatcher;
        auto gamemode = dispatcher.literal("gamemode")
                            .require(refrain::command::level::gamemode);
        gamemode.then(
            dispatcher.argument("mode", refrain::command::gamemode_arg())
                .executes([](refrain::command::context &) { return 1; }));
        dispatcher.literal("help").executes(
            [](refrain::command::context &) { return 1; });
        dispatcher.literal("echo").then(
            dispatcher.argument("text", refrain::command::quoted_arg())
                .executes([](refrain::command::context &) { return 1; }));
        dispatcher.literal("count").then(
            dispatcher.argument("n", refrain::command::integer_arg(1, 10))
                .executes([](refrain::command::context &) { return 1; }));
        dispatcher.literal("rate").then(
            dispatcher.argument("x", refrain::command::double_arg(0.0, 1.0))
                .executes([](refrain::command::context &) { return 1; }));
        dispatcher.literal("say").then(
            dispatcher.argument("message", refrain::command::greedy_arg())
                .executes([](refrain::command::context &) { return 1; }));
        dispatcher.literal("loc").then(
            dispatcher.argument("id", refrain::command::resource_location_arg())
                .executes([](refrain::command::context &) { return 1; }));
        dispatcher.literal("big").then(
            dispatcher.argument("value", refrain::command::long_arg())
                .executes([](refrain::command::context &) { return 1; }));
        dispatcher.literal("small").then(
            dispatcher.argument("ratio", refrain::command::float_arg())
                .executes([](refrain::command::context &) { return 1; }));
        dispatcher.literal("flag").then(
            dispatcher.argument("on", refrain::command::bool_arg())
                .executes([](refrain::command::context &) { return 1; }));

        tree_source source;
        const auto snapshot = dispatcher.tree(source);
        expect(snapshot != nullptr);
        if (snapshot == nullptr) {
            return;
        }
        expect(snapshot->root == 0U);
        expect(snapshot->revision == dispatcher.revision());
        expect(snapshot->nodes.size() == 20U);
        expect(snapshot->root_node().is_root());
        expect(snapshot->root_node().children.size() == 10U);

        const auto *gamemode_node = find_node(*snapshot, "gamemode");
        expect(gamemode_node != nullptr);
        if (gamemode_node != nullptr) {
            expect(gamemode_node->is_literal());
            expect(!gamemode_node->executable);
            expect(gamemode_node->children.size() == 1U);
            const auto &mode = snapshot->nodes[gamemode_node->children.front()];
            expect(mode.name == "mode");
            expect(mode.is_argument());
            expect(mode.executable);
            expect(mode.parser.kind ==
                   refrain::command::parser_kind::enum_values);
            expect(mode.parser.enum_values.size() == 4U);
            expect(contains(mode.parser.enum_values, "survival"));
            expect(contains(mode.parser.enum_values, "spectator"));
            expect(mode.parser.suggestion_type.empty());
        }

        const auto *help_node = find_node(*snapshot, "help");
        expect(help_node != nullptr);
        if (help_node != nullptr) {
            expect(help_node->executable);
            expect(help_node->parser.kind ==
                   refrain::command::parser_kind::none);
        }

        const auto *echo_node = find_node(*snapshot, "text");
        expect(echo_node != nullptr);
        if (echo_node != nullptr) {
            expect(echo_node->parser.kind ==
                   refrain::command::parser_kind::string);
            expect(echo_node->parser.string ==
                   refrain::command::string_mode::quotable_phrase);
        }

        const auto *count_node = find_node(*snapshot, "n");
        expect(count_node != nullptr);
        if (count_node != nullptr) {
            expect(count_node->parser.kind ==
                   refrain::command::parser_kind::integer);
            expect(count_node->parser.has_min);
            expect(count_node->parser.has_max);
            expect(count_node->parser.integer_min == 1);
            expect(count_node->parser.integer_max == 10);
        }

        const auto *rate_node = find_node(*snapshot, "x");
        expect(rate_node != nullptr);
        if (rate_node != nullptr) {
            expect(rate_node->parser.kind ==
                   refrain::command::parser_kind::double_number);
            expect(rate_node->parser.has_min);
            expect(rate_node->parser.has_max);
            expect(rate_node->parser.number_min == 0.0);
            expect(rate_node->parser.number_max == 1.0);
        }

        const auto *say_node = find_node(*snapshot, "message");
        expect(say_node != nullptr);
        if (say_node != nullptr) {
            expect(say_node->parser.kind ==
                   refrain::command::parser_kind::string);
            expect(say_node->parser.string ==
                   refrain::command::string_mode::greedy_phrase);
        }

        const auto *loc_node = find_node(*snapshot, "id");
        expect(loc_node != nullptr);
        if (loc_node != nullptr) {
            expect(loc_node->parser.kind ==
                   refrain::command::parser_kind::resource_location);
        }

        const auto *long_node = find_node(*snapshot, "value");
        expect(long_node != nullptr);
        if (long_node != nullptr) {
            expect(long_node->parser.kind ==
                   refrain::command::parser_kind::long_integer);
            expect(!long_node->parser.has_min);
            expect(!long_node->parser.has_max);
        }

        const auto *float_node = find_node(*snapshot, "ratio");
        expect(float_node != nullptr);
        if (float_node != nullptr) {
            expect(float_node->parser.kind ==
                   refrain::command::parser_kind::float_number);
        }

        const auto *bool_node = find_node(*snapshot, "on");
        expect(bool_node != nullptr);
        if (bool_node != nullptr) {
            expect(bool_node->parser.kind ==
                   refrain::command::parser_kind::boolean);
        }

        // Every child index must address a node of the same snapshot.
        for (const auto &node : snapshot->nodes) {
            for (const auto child : node.children) {
                expect(child < snapshot->nodes.size());
            }
            if (node.redirect) {
                expect(node.redirect_index < snapshot->nodes.size());
            }
        }
    };

    "tree carries a configured suggestion provider string"_test = [] {
        refrain::command::dispatcher dispatcher;
        dispatcher.literal("custom").then(
            dispatcher
                .argument("value", refrain::command::enum_arg(
                                       {{"alpha"}, {"beta", "Beta value"}},
                                       "example:values"))
                .executes([](refrain::command::context &) { return 1; }));
        tree_source source;
        const auto snapshot = dispatcher.tree(source);
        const auto *value = find_node(*snapshot, "value");
        expect(value != nullptr);
        if (value != nullptr) {
            expect(value->parser.suggestion_type == "example:values");
            expect(value->parser.enum_values.size() == 2U);
            expect(contains(value->parser.enum_values, "alpha"));
            expect(contains(value->parser.enum_values, "beta"));
        }
    };

    "tree filters permission subtrees and drops dangling redirects"_test = [] {
        refrain::command::dispatcher dispatcher;
        auto gamemode = dispatcher.literal("gamemode")
                            .require(refrain::command::level::gamemode);
        gamemode.then(
            dispatcher.argument("mode", refrain::command::gamemode_arg())
                .executes([](refrain::command::context &) { return 1; }));
        dispatcher.literal("help").executes(
            [](refrain::command::context &) { return 1; });
        dispatcher.literal("gm").redirect(gamemode);

        tree_source high{refrain::command::level::owner};
        tree_source low{refrain::command::level::everyone};

        const auto high_tree = dispatcher.tree(high);
        const auto low_tree = dispatcher.tree(low);
        expect(high_tree->nodes.size() == 5U);
        expect(low_tree->nodes.size() == 3U);
        expect(low_tree->root_node().children.size() == 2U);

        if (high_tree->nodes.size() == 5U) {
            const auto *alias = find_node(*high_tree, "gm");
            expect(alias != nullptr);
            if (alias != nullptr) {
                expect(alias->redirect);
                expect(alias->redirect_index ==
                       index_of(*high_tree, "gamemode"));
            }
        }
        const auto *low_alias = find_node(*low_tree, "gm");
        expect(low_alias != nullptr);
        if (low_alias != nullptr) {
            expect(!low_alias->redirect);
            expect(!low_alias->redirect_index);
        }
    };

    "snapshot survives dispatcher mutation and reports its revision"_test = [] {
        tree_source source;
        refrain::command::dispatcher dispatcher;
        dispatcher.literal("help").executes(
            [](refrain::command::context &) { return 1; });

        const auto revision_before = dispatcher.revision();
        const auto snapshot = dispatcher.tree(source);
        expect(snapshot->revision == revision_before);
        expect(snapshot->nodes.size() == 2U);
        expect(find_node(*snapshot, "new") == nullptr);

        dispatcher.literal("new").executes(
            [](refrain::command::context &) { return 1; });
        expect(dispatcher.revision() > revision_before);

        const auto after = dispatcher.tree(source);
        expect(after->revision == dispatcher.revision());
        expect(after->nodes.size() == 3U);
        expect(find_node(*after, "new") != nullptr);
        // The earlier snapshot owns its storage and never observes the new
        // registration.
        expect(snapshot->nodes.size() == 2U);
        expect(find_node(*snapshot, "new") == nullptr);

        std::shared_ptr<const refrain::command::tree> detached;
        {
            refrain::command::dispatcher inner;
            inner.literal("kept").executes(
                [](refrain::command::context &) { return 1; });
            detached = inner.tree(source);
        }
        expect(detached != nullptr);
        if (detached != nullptr) {
            expect(find_node(*detached, "kept") != nullptr);
            expect(detached->nodes.front().is_root());
        }
    };

    "root requirement hides the graph but keeps a serializable root"_test = [] {
        refrain::command::dispatcher dispatcher;
        dispatcher.root().require(refrain::command::level::gamemode);
        dispatcher.literal("help").executes(
            [](refrain::command::context &) { return 1; });

        tree_source low{refrain::command::level::everyone};
        tree_source high{refrain::command::level::owner};
        const auto low_tree = dispatcher.tree(low);
        expect(low_tree->nodes.size() == 1U);
        expect(low_tree->root_node().is_root());
        expect(low_tree->root_node().children.empty());
        expect(!dispatcher.execute("help", low));
        expect(dispatcher.tree(high)->nodes.size() == 2U);
    };

    "snapshot remains readable while commands execute concurrently"_test = [] {
        refrain::command::dispatcher dispatcher;
        dispatcher.literal("inc").executes(
            [](refrain::command::context &) -> int { return 1; });
        tree_source source;
        const auto snapshot = dispatcher.tree(source);
        expect(snapshot->nodes.size() == 2U);

        std::atomic<bool> okay{true};
        std::vector<std::thread> workers;
        workers.reserve(4U);
        for (int worker = 0; worker < 4; ++worker) {
            workers.emplace_back([&dispatcher, &okay] {
                tree_source local;
                for (int iteration = 0; iteration < 500; ++iteration) {
                    auto outcome = dispatcher.execute("inc", local);
                    if (!outcome || outcome->value != 1 || !outcome->success) {
                        okay.store(false, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (int iteration = 0; iteration < 500; ++iteration) {
            if (snapshot->nodes.size() != 2U ||
                snapshot->root_node().children.size() != 1U) {
                okay.store(false, std::memory_order_relaxed);
            }
            const auto &node = snapshot->nodes[1];
            if (node.name != "inc" || !node.executable) {
                okay.store(false, std::memory_order_relaxed);
            }
        }
        for (auto &worker : workers) {
            worker.join();
        }
        expect(okay.load());
    };
};

} // namespace
