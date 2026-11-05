// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/resource_location.hpp>

#include <boost/ut.hpp>

#include <string>
#include <string_view>
#include <unordered_set>

namespace {

using namespace boost::ut;
using refrain::data::resource_location;

[[nodiscard]] auto parse_ok(std::string_view text) -> resource_location {
    auto parsed{resource_location::parse(text)};
    expect(static_cast<bool>(parsed)) << "expected valid location: " << text;
    return parsed ? *parsed : resource_location{};
}

const suite resource_location_tests = [] {
    "resource_location: parses namespaced and default forms"_test = [] {
        const auto stone{parse_ok("minecraft:block/stone")};
        expect(stone.namespace_part() == "minecraft");
        expect(stone.path_part() == "block/stone");
        expect(stone.to_string() == "minecraft:block/stone");

        const auto implicit{parse_ok("block/stone")};
        expect(implicit.namespace_part() == "minecraft");
        expect(implicit.path_part() == "block/stone");
        expect(implicit == stone);

        const auto custom{parse_ok("com.example:path/to/file.json")};
        expect(custom.namespace_part() == "com.example");
        expect(custom.path_part() == "path/to/file.json");
    };

    "resource_location: canonical equality and ordering"_test = [] {
        expect(parse_ok("minecraft:block/stone") == parse_ok("block/stone"));
        expect(parse_ok("a:b") != parse_ok("a:c"));
        expect(parse_ok("a:b") < parse_ok("b:a"));
        expect(parse_ok("a:b") < parse_ok("a:c"));

        std::unordered_set<resource_location> set{};
        set.insert(parse_ok("minecraft:block/stone"));
        set.insert(parse_ok("block/stone")); // same canonical value
        set.insert(parse_ok("refrain:content"));
        expect(set.size() == 2U);
    };

    "resource_location: rejects malformed input"_test = [] {
        for (const std::string_view bad :
             {"", ":", ":path", "ns:", "a:b:c", "Minecraft:block",
              "minecraft:Block", "minecraft:/block", "minecraft:block/",
              "minecraft:block//stone", "minecraft:.", "minecraft:..",
              "minecraft:block/../stone", "ns:block nbt", "ns:block#tag"}) {
            expect(!resource_location::parse(bad))
                << "expected reject: " << bad;
        }
        // Dots are legal in namespace and path segments (vanilla rule).
        expect(static_cast<bool>(resource_location::parse("mine.craft:block")));
        expect(static_cast<bool>(resource_location::parse("ns:block.nbt")));
    };

    "resource_location: make validates parts independently"_test = [] {
        auto good{resource_location::make("refrain", "data/pack")};
        expect(static_cast<bool>(good));
        if (good) {
            expect(good->to_string() == "refrain:data/pack");
        }
        expect(!resource_location::make("Refrain", "data"));
        expect(!resource_location::make("refrain", ""));
        expect(!resource_location::make("", "data"));
        expect(!resource_location::make("refrain", "a/../b"));
    };

    "resource_location: validators are exposed"_test = [] {
        expect(resource_location::is_valid_namespace("minecraft"));
        expect(resource_location::is_valid_namespace("com.example"));
        expect(!resource_location::is_valid_namespace(""));
        expect(!resource_location::is_valid_namespace("Minecraft"));
        expect(resource_location::is_valid_path("block/stone"));
        expect(resource_location::is_valid_path("a.b-c_d/0"));
        expect(!resource_location::is_valid_path(""));
        expect(!resource_location::is_valid_path("/a"));
        expect(!resource_location::is_valid_path("a/"));
        expect(!resource_location::is_valid_path("a//b"));
        expect(!resource_location::is_valid_path("a/./b"));
    };

    "resource_location: empty default is distinguishable"_test = [] {
        const resource_location empty{};
        expect(empty.empty());
        expect(empty.namespace_part().empty());
        expect(empty.path_part().empty());
        expect(empty != parse_ok("minecraft:a"));
    };
};

} // namespace
