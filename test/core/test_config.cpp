// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/config.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using namespace boost::ut;

using refrain::core::config::document;

constexpr std::string_view sample_text{R"(title = "Refrain"
enabled = true
revision = 42
ratio = 0.5
"quoted key" = 7
"dotted.key" = 8
mixed = [1, "two"]

[gamepack]
id = "vanilla"
version = 1
tags = ["a", "b"]

[mods]
list = ["alpha", "beta"]
)"};

/// Unique file path under the system temp directory (parallel-safe enough for
/// one test process; the path is removed by the test that created it).
[[nodiscard]] auto make_temp_path(std::string_view suffix)
    -> std::filesystem::path {
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    return std::filesystem::temp_directory_path() /
           ("refrain_config_test_" + std::to_string(stamp) +
            std::string{suffix});
}

void remove_quietly(const std::filesystem::path &path) noexcept {
    auto error{std::error_code{}};
    std::filesystem::remove(path, error);
}

} // namespace

namespace {

using namespace boost::ut;

// Registered at static init; test/main.cpp runs it via cfg<override>.run().
suite config_tests = [] {
    "config/parse_text scalars and nested tables"_test = [] {
        const auto parsed{document::parse_text(sample_text, "sample.toml")};
        expect(static_cast<bool>(parsed));
        expect(parsed->origin() == "sample.toml");

        expect(parsed->get_string("title", "") == "Refrain");
        expect(parsed->get_bool("enabled", false));
        expect(parsed->get_int64("revision", 0) == 42);
        expect(parsed->get_double("ratio", 0.0) == 0.5);

        // Dotted paths traverse nested tables.
        expect(parsed->get_string("gamepack.id", "") == "vanilla");
        expect(parsed->get_int64("gamepack.version", 0) == 1);
    };

    "config/parse_text quoted keys"_test = [] {
        const auto parsed{document::parse_text(sample_text)};

        // A quoted key without a dot behaves like a bare key.
        expect(parsed->get_int64("quoted key", 0) == 7);

        // Dotted-path lookup splits on '.', so a quoted key that contains a
        // dot is only reachable through keys()/dump().
        expect(!parsed->contains("dotted.key"));
        const auto root_keys{parsed->keys("")};
        expect(root_keys.end() !=
               std::find(root_keys.begin(), root_keys.end(), "dotted.key"));
    };

    "config/missing keys return fallbacks"_test = [] {
        const auto parsed{document::parse_text("value = 1")};

        expect(!parsed->contains("missing"));
        expect(parsed->get_bool("missing", true));
        expect(parsed->get_int64("missing", -7) == -7);
        expect(parsed->get_double("missing", 1.5) == 1.5);
        expect(parsed->get_string("missing", "fallback") == "fallback");
        const auto fallback{std::array<std::string_view, 2>{"a", "b"}};
        expect(parsed->get_strings("missing", fallback) ==
               std::vector<std::string>{"a", "b"});
    };

    "config/type mismatches return fallbacks"_test = [] {
        const auto parsed{document::parse_text(sample_text)};

        // An integer is not a bool/double, a bool is not an integer.
        expect(!parsed->get_bool("revision", false));
        expect(parsed->get_double("revision", 9.5) == 9.5);
        expect(parsed->get_int64("enabled", -1) == -1);
        expect(parsed->get_string("enabled", "not a string") == "not a string");

        // A mixed array is not an array of strings.
        expect(parsed->get_int64("mixed", -1) == -1);
        const auto fallback{std::array<std::string_view, 1>{"fallback"}};
        expect(parsed->get_strings("mixed", fallback) ==
               std::vector<std::string>{"fallback"});
    };

    "config/get_strings returns array elements"_test = [] {
        const auto parsed{document::parse_text(sample_text)};

        expect(parsed->get_strings("gamepack.tags") ==
               std::vector<std::string>{"a", "b"});
        expect(parsed->get_strings("mods.list") ==
               std::vector<std::string>{"alpha", "beta"});
    };

    "config/keys returns sorted direct children"_test = [] {
        const auto parsed{document::parse_text(sample_text)};

        expect(parsed->keys("") ==
               (std::vector<std::string>{"dotted.key", "enabled", "gamepack",
                                         "mixed", "mods", "quoted key", "ratio",
                                         "revision", "title"}));
        expect(parsed->keys("gamepack") ==
               (std::vector<std::string>{"id", "tags", "version"}));
        expect(parsed->keys("mods").size() == 1U);

        expect(parsed->keys("missing").empty());
        expect(parsed->keys("title").empty()); // not a table
    };

    "config/contains reports nested keys"_test = [] {
        const auto parsed{document::parse_text(sample_text)};

        expect(parsed->contains("title"));
        expect(parsed->contains("gamepack"));
        expect(parsed->contains("gamepack.id"));
        expect(!parsed->contains("gamepack.missing"));
        expect(!parsed->contains("title.nested"));

        // The empty dotted key is "missing", matching every get_*() (which
        // fall back for ""); the document root is addressed through the
        // literal-path API instead.
        expect(!parsed->contains(""));
        expect(parsed->contains_at({}));
        expect(parsed->get_string("", "fallback") == "fallback");
    };

    "config/empty document is valid"_test = [] {
        const auto parsed{document::parse_text("")};
        expect(static_cast<bool>(parsed));
        expect(parsed->keys("").empty());
        expect(parsed->dump().empty());
    };

    "config/dump is deterministic TOML"_test = [] {
        const auto parsed{document::parse_text("b = 2\na = 1")};

        const auto first{parsed->dump()};
        expect(first == parsed->dump());
        expect(first.find("a = 1") != std::string::npos);
        expect(first.find("b = 2") != std::string::npos);
        expect(!first.empty());
        expect(first.find('[') == std::string::npos); // no nested tables here
    };

    "config/copies share the parsed document"_test = [] {
        const auto original{document::parse_text("value = 3", "origin.toml")};
        const auto copy{*original};

        expect(copy.get_int64("value", 0) == 3);
        expect(copy.origin() == "origin.toml");
        expect(original->get_int64("value", 0) == 3);
    };

    "config/parse_file reads a TOML file"_test = [] {
        const auto path{make_temp_path(".toml")};
        {
            auto out{std::ofstream{path}};
            expect(static_cast<bool>(out));
            out << "name = \"sample\"\ncount = 5\n[section]\nkey = true\n";
        }

        const auto parsed{document::parse_file(path.string())};
        expect(static_cast<bool>(parsed));
        if (parsed) {
            expect(parsed->origin() == path.string());
            expect(parsed->get_string("name", "") == "sample");
            expect(parsed->get_int64("count", 0) == 5);
            expect(parsed->get_bool("section.key", false));
        }
        remove_quietly(path);
    };

    "config/parse_file reports io_error for missing file"_test = [] {
        const auto path{make_temp_path(".toml")}; // deliberately never created
        const auto parsed{document::parse_file(path.string())};

        expect(!parsed);
        expect(parsed.error().code == refrain::error_code::io_error);
        expect(parsed.error().message.find(path.string()) != std::string::npos);
    };

    "config/parse_file enforces the manifest read limit"_test = [] {
        const auto path{make_temp_path("-oversized.toml")};
        {
            auto out{std::ofstream{path}};
            expect(static_cast<bool>(out));
            out << "# oversized manifest\n";
        }
        // Grow the file past the 16 MiB default without writing 16 MiB: the
        // TOML parser never sees it because the read limit fails first.
        constexpr std::uintmax_t size_limit{16U * 1024U * 1024U};
        std::error_code resize_error;
        std::filesystem::resize_file(path, size_limit + 1U, resize_error);
        if (resize_error) {
            remove_quietly(path);
            return; // filesystem without resize support
        }

        const auto parsed{document::parse_file(path.string())};
        expect(!parsed);
        if (!parsed) {
            expect(parsed.error().code ==
                   refrain::error_code::invalid_argument);
        }
        // An explicit smaller limit is forwarded, not ignored.
        const auto tight{document::parse_file(
            path.string(), refrain::core::io::read_options{
                               .max_bytes = 8U, .allow_symlink = false})};
        expect(!tight);
        if (!tight) {
            expect(tight.error().code == refrain::error_code::invalid_argument);
        }
        remove_quietly(path);
    };

    "config/parse errors carry origin line and column"_test = [] {
        const auto parsed{document::parse_text("enabled = trUe", "bad.toml")};

        expect(!parsed);
        expect(parsed.error().code == refrain::error_code::parse_error);
        expect(parsed.error().message.find("bad.toml:1:") != std::string::npos);
    };

    "config/literal paths address quoted table keys"_test = [] {
        constexpr std::string_view manifest{R"([mods]
list = ["refrain.sim.vanilla@1"]

[mods."refrain.sim.vanilla"]
difficulty = "normal"
requires = ["refrain.world.vanilla@1"]
)"};
        const auto parsed{document::parse_text(manifest)};

        const auto table_path{
            std::array<std::string_view, 2>{"mods", "refrain.sim.vanilla"}};
        expect(parsed->contains_at(table_path));
        expect(parsed->contains_at(std::array<std::string_view, 3>{
            "mods", "refrain.sim.vanilla", "difficulty"}));
        expect(!parsed->contains_at(
            std::array<std::string_view, 2>{"mods", "refrain"}));

        const auto requires_values =
            parsed->get_strings_at(std::array<std::string_view, 3>{
                "mods", "refrain.sim.vanilla", "requires"});
        expect(static_cast<bool>(requires_values));
        if (requires_values) {
            expect(*requires_values ==
                   std::vector<std::string>{"refrain.world.vanilla@1"});
        }

        expect(parsed->kind_at(table_path) ==
               refrain::core::config::value_kind::table);
        expect(parsed->kind_at(std::array<std::string_view, 3>{
                   "mods", "refrain.sim.vanilla", "difficulty"}) ==
               refrain::core::config::value_kind::string);

        const auto missing =
            parsed->get_strings_at(std::array<std::string_view, 3>{
                "mods", "refrain.sim.vanilla", "missing"});
        expect(!missing);
        expect(missing.error().code == refrain::error_code::not_found);
        const auto wrong_type =
            parsed->get_strings_at(std::array<std::string_view, 3>{
                "mods", "refrain.sim.vanilla", "difficulty"});
        expect(!wrong_type);
        expect(wrong_type.error().code == refrain::error_code::parse_error);

        const auto settings = parsed->dump_table_at(
            table_path, std::array<std::string_view, 1>{"requires"});
        expect(settings.find("difficulty = 'normal'") != std::string::npos ||
               settings.find("difficulty = \"normal\"") != std::string::npos);
        expect(settings.find("requires") == std::string::npos);
        expect(parsed->dump_table_at(table_path).find("requires") !=
               std::string::npos);

        // Not a table / missing paths dump nothing.
        expect(parsed
                   ->dump_table_at(std::array<std::string_view, 3>{
                       "mods", "refrain.sim.vanilla", "difficulty"})
                   .empty());
        expect(parsed
                   ->dump_table_at(
                       std::array<std::string_view, 2>{"mods", "missing"})
                   .empty());
    };

    "config/literal-path scalar getters and keys_at"_test = [] {
        constexpr std::string_view text{R"([manifest]
size = 12
enabled = true
name = "x"

[files]
"a/b.json" = { size = 3 }
"c d/e.json" = { size = 4 }
)"};
        const auto parsed{document::parse_text(text)};
        expect(static_cast<bool>(parsed));
        if (!parsed) {
            return;
        }
        // Literal components: no dotted-path splitting, so keys containing
        // dots/slashes/spaces stay addressable.
        const auto at = [](std::initializer_list<std::string_view> parts) {
            return std::vector<std::string_view>{parts};
        };

        auto size{parsed->get_int64_at(at({"manifest", "size"}))};
        expect(static_cast<bool>(size));
        if (size) {
            expect(*size == 12);
        }
        auto enabled{parsed->get_bool_at(at({"manifest", "enabled"}))};
        expect(static_cast<bool>(enabled));
        if (enabled) {
            expect(*enabled);
        }
        // TOML integers are not booleans and vice versa.
        auto wrong_bool{parsed->get_bool_at(at({"manifest", "size"}))};
        expect(!wrong_bool);
        expect(wrong_bool.error().code == refrain::error_code::parse_error);
        auto wrong_int{parsed->get_int64_at(at({"manifest", "name"}))};
        expect(!wrong_int);
        expect(wrong_int.error().code == refrain::error_code::parse_error);
        auto missing{parsed->get_int64_at(at({"manifest", "nope"}))};
        expect(!missing);
        expect(missing.error().code == refrain::error_code::not_found);

        auto file_keys{parsed->keys_at(at({"files"}))};
        expect(static_cast<bool>(file_keys));
        if (file_keys) {
            expect(*file_keys ==
                   (std::vector<std::string>{"a/b.json", "c d/e.json"}));
        }
        auto root_keys{parsed->keys_at({})};
        expect(static_cast<bool>(root_keys));
        if (root_keys) {
            expect(*root_keys ==
                   (std::vector<std::string>{"files", "manifest"}));
        }
        auto absent_table{parsed->keys_at(at({"nope"}))};
        expect(!absent_table);
        expect(absent_table.error().code == refrain::error_code::not_found);
        auto scalar_table{parsed->keys_at(at({"manifest", "name"}))};
        expect(!scalar_table);
        expect(scalar_table.error().code == refrain::error_code::parse_error);
        // keys_at on a nested table path (quoted keys are separate components).
        auto record_keys{parsed->keys_at(at({"files", "a/b.json"}))};
        expect(static_cast<bool>(record_keys));
        if (record_keys) {
            expect(*record_keys == (std::vector<std::string>{"size"}));
        }
    };

    "config/default origin is memory"_test = [] {
        const auto parsed{document::parse_text("x = 1")};
        expect(static_cast<bool>(parsed));
        expect(parsed->origin() == "<memory>");
    };
};

} // namespace
