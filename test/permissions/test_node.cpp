// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Node grammar: what a configured rule or a query node may look like, and how
// a rule matches a node. These checks are pure string work, so they run without
// a host, a service or a configuration file.

#include <refrain/permissions/node.hpp>

#include <boost/ut.hpp>

#include <string>
#include <string_view>

namespace {

using namespace boost::ut;

namespace permissions = refrain::permissions;

const suite node_tests = [] {
    "node/a namespaced node is valid"_test = [] {
        expect(permissions::valid_node("refrain.command.teleport"));
        expect(permissions::valid_node("refrain.mods.admin"));
        expect(permissions::valid_node("com.example.mymod.home"));
        expect(permissions::valid_node("a.b"));
        expect(permissions::valid_node("a0.b-1_c"));
    };

    "node/query nodes are concrete and namespaced"_test = [] {
        expect(!permissions::valid_node(""));
        expect(!permissions::valid_node("refrain"));
        expect(!permissions::valid_node("refrain."));
        expect(!permissions::valid_node(".refrain.command"));
        expect(!permissions::valid_node("refrain..command"));
        expect(!permissions::valid_node("Refrain.Command"));
        expect(!permissions::valid_node("refrain.command."));
        expect(!permissions::valid_node("refrain.command.*"));
        expect(!permissions::valid_node("!refrain.command.stop"));
        expect(!permissions::valid_node("refrain.(command)"));
        expect(!permissions::valid_node("refrain.command.teleport/extra"));
        // A segment starts with a lowercase letter or digit, so an underscore
        // or dash cannot open one.
        expect(!permissions::valid_node("refrain._command"));
        expect(!permissions::valid_node("refrain.-command"));
    };

    "node/overlong nodes are rejected"_test = [] {
        std::string long_segment(permissions::max_segment_length + 1, 'a');
        expect(!permissions::valid_node("refrain." + long_segment));
        std::string long_node{"a.b"};
        while (long_node.size() <= permissions::max_node_length) {
            long_node += ".abcdefgh";
        }
        expect(!permissions::valid_node(long_node));
    };

    "node/rule nodes allow one trailing wildcard"_test = [] {
        expect(permissions::valid_rule_node("*"));
        expect(permissions::valid_rule_node("refrain.*"));
        expect(permissions::valid_rule_node("refrain.command.*"));
        expect(permissions::valid_rule_node("com.example.mymod.*"));
        expect(!permissions::valid_rule_node(""));
        expect(!permissions::valid_rule_node("*.refrain"));
        expect(!permissions::valid_rule_node("refrain.*.command"));
        expect(!permissions::valid_rule_node("refrain.command.*.teleport"));
        expect(!permissions::valid_rule_node("**"));
        expect(!permissions::valid_rule_node("!refrain.command"));
    };

    "node/wildcards match descendants, not the prefix"_test = [] {
        expect(permissions::node_matches_rule("refrain.command.*",
                                              "refrain.command.teleport"));
        expect(permissions::node_matches_rule(
            "refrain.command.*", "refrain.command.teleport.world"));
        expect(!permissions::node_matches_rule("refrain.command.*",
                                               "refrain.command"));
        expect(!permissions::node_matches_rule("refrain.command.*",
                                               "refrain.commands.teleport"));
        expect(permissions::node_matches_rule("*", "refrain.command.teleport"));
        expect(permissions::node_matches_rule("*", "a.b"));
        expect(!permissions::node_matches_rule("*", "not a node"));
        expect(permissions::node_matches_rule("refrain.mods.admin",
                                              "refrain.mods.admin"));
        expect(!permissions::node_matches_rule("refrain.mods.admin",
                                               "refrain.mods.admin.read"));
        expect(!permissions::node_matches_rule("refrain.mods.admin",
                                               "refrain.mods.administrator"));
    };

    "node/rule nodes are detected as wildcards"_test = [] {
        expect(permissions::is_wildcard_rule("*"));
        expect(permissions::is_wildcard_rule("refrain.command.*"));
        expect(!permissions::is_wildcard_rule("refrain.command.teleport"));
        expect(!permissions::is_wildcard_rule("refrain.command"));
    };
};

} // namespace
