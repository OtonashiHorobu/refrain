// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/command/command.hpp>

#include <boost/ut.hpp>

#include <algorithm>
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
            expect(mode.parser.kind == refrain::command::parser_kind::gamemode);
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

    "tree describes every typed parser kind"_test = [] {
        refrain::command::dispatcher dispatcher;
        auto shell{dispatcher.literal("typed")};
        auto flag{dispatcher.argument("flag", refrain::command::bool_arg())};
        auto pos{
            dispatcher.argument("pos", refrain::command::block_position_arg())};
        auto rot{dispatcher.argument("rot", refrain::command::rotation_arg())};
        auto who{
            dispatcher.argument("who", refrain::command::player_name_arg())};
        auto mode{
            dispatcher.argument("mode", refrain::command::gamemode_arg())};
        auto wait{dispatcher.argument("wait", refrain::command::time_arg(5))};
        auto dimension{dispatcher.argument("dimension",
                                           refrain::command::dimension_arg())};
        auto identifier{
            dispatcher.argument("identifier", refrain::command::uuid_arg())};
        rot.then(who);
        who.then(mode);
        mode.then(wait);
        wait.then(dimension);
        dimension.then(identifier);
        pos.then(rot);
        flag.then(pos);
        shell.then(flag);

        tree_source source;
        const auto snapshot = dispatcher.tree(source);
        expect(snapshot != nullptr);
        if (snapshot == nullptr) {
            return;
        }
        const auto *pos_node = find_node(*snapshot, "pos");
        expect(pos_node != nullptr);
        if (pos_node != nullptr) {
            expect(pos_node->parser.kind ==
                   refrain::command::parser_kind::block_position);
        }
        const auto *rot_node = find_node(*snapshot, "rot");
        expect(rot_node != nullptr);
        if (rot_node != nullptr) {
            expect(rot_node->parser.kind ==
                   refrain::command::parser_kind::rotation);
        }
        const auto *who_node = find_node(*snapshot, "who");
        expect(who_node != nullptr);
        if (who_node != nullptr) {
            expect(who_node->parser.kind ==
                   refrain::command::parser_kind::player_name);
        }
        const auto *mode_node = find_node(*snapshot, "mode");
        expect(mode_node != nullptr);
        if (mode_node != nullptr) {
            expect(mode_node->parser.kind ==
                   refrain::command::parser_kind::gamemode);
        }
        const auto *wait_node = find_node(*snapshot, "wait");
        expect(wait_node != nullptr);
        if (wait_node != nullptr) {
            expect(wait_node->parser.kind ==
                   refrain::command::parser_kind::time);
            expect(wait_node->parser.integer_min == 5);
        }
        const auto *dimension_node = find_node(*snapshot, "dimension");
        expect(dimension_node != nullptr);
        if (dimension_node != nullptr) {
            expect(dimension_node->parser.kind ==
                   refrain::command::parser_kind::dimension);
        }
        const auto *uuid_node = find_node(*snapshot, "identifier");
        expect(uuid_node != nullptr);
        if (uuid_node != nullptr) {
            expect(uuid_node->parser.kind ==
                   refrain::command::parser_kind::uuid);
        }
        for (const auto *name :
             {"pos", "rot", "who", "mode", "wait", "dimension", "identifier"}) {
            const auto *node = find_node(*snapshot, name);
            expect(node != nullptr) << name;
        }
    };

    "visibility hook hides a subtree from the tree snapshot"_test = [] {
        refrain::command::dispatcher dispatcher;
        dispatcher.literal("public").executes(
            [](refrain::command::context &) { return 1; });
        auto hidden = dispatcher.literal("secret");
        hidden.require_node("refrain.command.secret");
        hidden.executes([](refrain::command::context &) { return 1; });
        hidden.then(
            dispatcher.argument("value", refrain::command::word_arg())
                .executes([](refrain::command::context &) { return 1; }));
        dispatcher.set_permission_filter(
            [](const refrain::command::source &,
               const refrain::command::node_permission &permission) {
                return permission.node == "refrain.command.secret"
                           ? refrain::command::permission_decision::denied
                           : refrain::command::permission_decision::allowed;
            });

        tree_source source;
        const auto snapshot = dispatcher.tree(source);
        expect(snapshot != nullptr);
        if (snapshot == nullptr) {
            return;
        }
        expect(find_node(*snapshot, "public") != nullptr);
        expect(find_node(*snapshot, "secret") == nullptr);
        expect(find_node(*snapshot, "value") == nullptr);
        expect(!dispatcher.execute("secret", source));
    };

    "node permission declarations gate the tree, completion and execution"_test =
        [] {
            refrain::command::dispatcher dispatcher;
            auto gated = dispatcher.literal("gated").require_node(
                "refrain.command.gated", refrain::command::level::gamemode);
            gated.executes([](refrain::command::context &) { return 1; });
            dispatcher.literal("open")
                .require_node("refrain.command.open")
                .executes([](refrain::command::context &) { return 1; });

            // Without an injected filter the declared vanilla level is the
            // whole policy, which is what a composition without the permission
            // service falls back to.
            tree_source guest{refrain::command::level::everyone};
            tree_source owner{refrain::command::level::owner};
            expect(find_node(*dispatcher.tree(guest), "gated") == nullptr);
            expect(find_node(*dispatcher.tree(owner), "gated") != nullptr);

            // The filter is authoritative and receives the declared node plus
            // its fallback level.
            std::vector<std::string> asked{};
            refrain::command::permission_level gated_fallback{
                refrain::command::level::everyone};
            dispatcher.set_permission_filter(
                [&asked, &gated_fallback](
                    const refrain::command::source &,
                    const refrain::command::node_permission &permission) {
                    asked.emplace_back(permission.node);
                    if (permission.node == "refrain.command.gated") {
                        gated_fallback = permission.fallback;
                        return refrain::command::permission_decision::allowed;
                    }
                    if (permission.node == "refrain.command.open") {
                        return refrain::command::permission_decision::denied;
                    }
                    return refrain::command::permission_decision::not_handled;
                });

            const auto visible{dispatcher.tree(guest)};
            expect(find_node(*visible, "gated") != nullptr);
            expect(find_node(*visible, "open") == nullptr);
            expect(dispatcher.execute("gated", guest).has_value());
            expect(!dispatcher.execute("open", guest));
            expect(gated_fallback == refrain::command::level::gamemode);
            const auto gated_asked{
                std::ranges::find(asked,
                                  std::string{"refrain.command.gated"}) !=
                asked.end()};
            const auto open_asked{
                std::ranges::find(asked, std::string{"refrain.command.open"}) !=
                asked.end()};
            expect(gated_asked);
            expect(open_asked);

            const auto completion{dispatcher.suggest("", 0, guest)};
            expect(completion.entries.size() == 1U);
            expect(completion.entries.front().text == "gated");
        };

    "a not_handled permission answer falls back to the declared level"_test =
        [] {
            refrain::command::dispatcher dispatcher;
            dispatcher.literal("gated")
                .require_node("refrain.command.gated",
                              refrain::command::level::admin)
                .executes([](refrain::command::context &) { return 1; });
            dispatcher.set_permission_filter(
                [](const refrain::command::source &,
                   const refrain::command::node_permission &) {
                    return refrain::command::permission_decision::not_handled;
                });

            tree_source guest{refrain::command::level::everyone};
            tree_source owner{refrain::command::level::owner};
            expect(find_node(*dispatcher.tree(guest), "gated") == nullptr);
            expect(find_node(*dispatcher.tree(owner), "gated") != nullptr);
            expect(!dispatcher.execute("gated", guest));
            expect(dispatcher.execute("gated", owner).has_value());
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
