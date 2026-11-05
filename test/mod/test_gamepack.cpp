// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>

#include <refrain/core/config.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using namespace boost::ut;

namespace mod = refrain::mod;
using refrain::core::config::document;
using refrain::mod::composition;
using refrain::mod::gamepack_spec;
using refrain::mod::mod_entry;
using refrain::mod::mod_ref;
using refrain::mod::mod_requirement;
using refrain::mod::parse_mod_ref;
using refrain::mod::parse_mod_requirement;
using refrain::mod::provider_mode;

constexpr std::string_view valid_manifest{R"([gamepack]
id = "refrain.vanilla"
version = "26.3.0"
side = "server"
abi = "1"
auto_scan = false
mods_dir = ["mods", "extra-mods"]

[meta]
display_name = "Refrain vanilla"
description = "first-party server pack"
icon = "assets/icon.png"

[policy]
origin_priority = ["builtin", "first_party", "user"]
allow_custom_override = false
conflict = "fail"

[mods]
list = [
    "refrain.content.vanilla@1.0.0",
    "refrain.world.vanilla@1.0.0",
    "refrain.worldgen.vanilla@1.0.0",
    "refrain.storage.rwf@1.0.0",
    "refrain.storage.memory@1.0.0",
]

[mods."refrain.world.vanilla"]
requires = ["refrain.content.vanilla@^1.0.0"]

[mods."refrain.worldgen.vanilla"]
requires = ["refrain.world.vanilla@>=1.0.0 <2.0.0"]
difficulty = "normal"

[providers]
world = "refrain.world.vanilla@^1.0.0"
worldgen = ["refrain.worldgen.vanilla@^1.0.0", "refrain.worldgen.flat@^1.0.0"]
storage = { default = "rwf", instances = { rwf = "refrain.storage.rwf@^1.0.0", memory = "refrain.storage.memory@^1.0.0" } }
)"};

[[nodiscard]] auto parse_doc(std::string_view text) -> document {
    auto parsed{document::parse_text(text, "test.toml")};
    expect(static_cast<bool>(parsed));
    return parsed ? std::move(*parsed) : document::parse_text("").value();
}

[[nodiscard]] auto parse_spec(std::string_view text)
    -> refrain::result<gamepack_spec> {
    const auto doc{parse_doc(text)};
    return refrain::mod::parse_gamepack(doc, "server");
}

[[nodiscard]] auto resolve_spec(std::string_view text)
    -> refrain::result<composition> {
    auto spec{parse_spec(text)};
    if (!spec) {
        return std::unexpected{spec.error()};
    }
    return refrain::mod::preview_composition(*spec);
}

[[nodiscard]] auto make_temp_path() -> std::filesystem::path {
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    return std::filesystem::temp_directory_path() /
           ("refrain_gamepack_test_" + std::to_string(stamp) + ".toml");
}

void remove_quietly(const std::filesystem::path &path) noexcept {
    auto error{std::error_code{}};
    std::filesystem::remove(path, error);
}

} // namespace

namespace {

using namespace boost::ut;

namespace mod = refrain::mod;

const suite gamepack_tests = [] {
    "gamepack/parse valid manifest"_test = [] {
        auto spec{parse_spec(valid_manifest)};
        expect(static_cast<bool>(spec));
        if (!spec) {
            return;
        }
        expect(spec->id == "refrain.vanilla");
        expect(spec->version == "26.3.0");
        expect(spec->side == "server");
        expect(spec->abi == 1U);
        expect(!spec->auto_scan);
        expect(spec->mods_dir.size() == 2U);
        expect(spec->meta.display_name == "Refrain vanilla");
        expect(!spec->policy.allow_custom_override);
        expect(spec->policy.conflict == "fail");
        expect(spec->mods.size() == 5U);
        expect(spec->providers.size() == 3U);

        const auto *world{spec->find_mod("refrain.world.vanilla")};
        expect(world != nullptr);
        if (world != nullptr) {
            expect(world->dependencies.size() == 1U);
            expect(world->dependencies[0].id == "refrain.content.vanilla");
            expect(world->dependencies[0].range == "^1.0.0");
        }
        const auto *worldgen{spec->find_mod("refrain.worldgen.vanilla")};
        expect(worldgen != nullptr);
        if (worldgen != nullptr) {
            expect(worldgen->settings.find("difficulty") != std::string::npos);
            expect(worldgen->settings.find("requires") == std::string::npos);
        }
        expect(spec->find_mod("refrain.storage.rwf") != nullptr);
        expect(spec->find_mod("refrain.storage.missing") == nullptr);

        const auto storage_binding = std::find_if(
            spec->providers.begin(), spec->providers.end(),
            [](const auto &entry) { return entry.service == "storage"; });
        expect(storage_binding != spec->providers.end());
        if (storage_binding != spec->providers.end()) {
            expect(storage_binding->mode == provider_mode::instanced);
            expect(storage_binding->instances.size() == 2U);
            expect(storage_binding->default_instance == "rwf");
        }
    };

    "gamepack/mod references are exact SemVer"_test = [] {
        const auto valid{parse_mod_ref("refrain.world.vanilla@1.2.3")};
        expect(static_cast<bool>(valid));
        if (valid) {
            expect(valid->id == "refrain.world.vanilla");
            expect(valid->version == "1.2.3");
            expect(valid->text() == "refrain.world.vanilla@1.2.3");
            expect(static_cast<bool>(valid->parsed()));
        }
        const auto prerelease{parse_mod_ref("com.example.mod@2.0.0-rc.1")};
        expect(static_cast<bool>(prerelease));
        if (prerelease) {
            expect(prerelease->parsed()->is_prerelease());
        }

        for (const std::string_view bad :
             {"no-at-sign", "@1.0.0", "bad id@1.0.0", "refrain.world@1.0",
              "refrain.world@v1.0.0", "refrain.world@1.0.0@2.0.0",
              "refrain.world@"}) {
            expect(!parse_mod_ref(bad)) << "expected reject: " << bad;
        }
    };

    "gamepack/mod reference errors keep the split diagnostic"_test = [] {
        const auto missing{parse_mod_ref("noatsign")};
        expect(!missing);
        if (!missing) {
            expect(missing.error().code ==
                   refrain::error_code::invalid_argument);
            expect(missing.error().message.find("no '@version'") !=
                   std::string::npos);
            expect(missing.error().message.find("is malformed") !=
                   std::string::npos);
        }

        const auto extra{parse_mod_ref("a@1.0.0@2")};
        expect(!extra);
        if (!extra) {
            expect(extra.error().message.find("more than one '@'") !=
                   std::string::npos);
        }

        const auto no_id{parse_mod_ref("@1.0.0")};
        expect(!no_id);
        if (!no_id) {
            expect(no_id.error().message.find("invalid mod id") !=
                   std::string::npos);
        }
    };

    "gamepack/requirements are SemVer ranges"_test = [] {
        const auto caret{parse_mod_requirement("refrain.world@^1.0.0")};
        expect(static_cast<bool>(caret));
        if (caret) {
            expect(caret->id == "refrain.world");
            expect(caret->range == "^1.0.0");
        }
        expect(static_cast<bool>(
            parse_mod_requirement("refrain.world@>=1.0.0 <2.0.0")));
        expect(
            static_cast<bool>(parse_mod_requirement("refrain.world@~1.2.3")));
        expect(static_cast<bool>(parse_mod_requirement("refrain.world@1.2.3")));

        for (const std::string_view bad :
             {"refrain.world", "refrain.world@", "refrain.world@1.0",
              "refrain.world@nonsense", "bad id@^1.0.0"}) {
            expect(!parse_mod_requirement(bad)) << "expected reject: " << bad;
        }
    };

    "gamepack/abi mismatch is rejected"_test = [] {
        auto text{std::string{valid_manifest}};
        text.replace(text.find("abi = \"1\""), 9, "abi = \"2\"");
        const auto spec{parse_spec(text)};
        expect(!spec);
        if (!spec) {
            expect(spec.error().code == refrain::error_code::version_mismatch);
        }
    };

    "gamepack/abi accepts an unquoted integer"_test = [] {
        auto text{std::string{valid_manifest}};
        text.replace(text.find("abi = \"1\""), 9, "abi = 1");
        expect(static_cast<bool>(parse_spec(text)));
    };

    "gamepack/missing required fields are reported"_test = [] {
        for (const std::string_view field : {"id", "version", "side", "abi"}) {
            auto text{std::string{valid_manifest}};
            const auto marker{std::string{field} + " = "};
            const auto start{text.find(marker)};
            expect(start != std::string::npos);
            const auto end{text.find('\n', start)};
            text.erase(start, end - start + 1);
            expect(!parse_spec(text)) << "expected missing " << field;
        }
    };

    "gamepack/unknown top-level table is rejected"_test = [] {
        auto text{std::string{valid_manifest}};
        text += "\n[unknown]\nvalue = 1\n";
        expect(!parse_spec(text));
    };

    "gamepack/unknown section keys are rejected"_test = [] {
        constexpr std::string_view unknown_gamepack_key{R"([gamepack]
id = "refrain.typo"
version = "1.0.0"
side = "server"
abi = 1
auto_scan = false
mod_dir = "x"
[mods]
list = ["a.mod@1.0.0"]
)"};
        const auto section{parse_spec(unknown_gamepack_key)};
        expect(!section);
        if (!section) {
            expect(section.error().code ==
                   refrain::error_code::invalid_argument);
            expect(section.error().message.find("mod_dir") !=
                   std::string::npos);
        }

        constexpr std::string_view unknown_policy_key{R"([gamepack]
id = "refrain.typo"
version = "1.0.0"
side = "server"
abi = 1
auto_scan = false
[policy]
typo = 1
[mods]
list = ["a.mod@1.0.0"]
)"};
        const auto policy{parse_spec(unknown_policy_key)};
        expect(!policy);
        if (!policy) {
            expect(policy.error().code ==
                   refrain::error_code::invalid_argument);
            expect(policy.error().message.find("typo") != std::string::npos);
        }
    };

    "gamepack/unknown provider keys and ambiguous modes are rejected"_test =
        [] {
            const auto parse_with = [](std::string_view provider) {
                auto text{std::string{R"([gamepack]
id = "refrain.providers"
version = "1.0.0"
side = "server"
abi = 1
auto_scan = false
[mods]
list = ["a.mod@1.0.0", "b.mod@1.0.0"]
[providers]
)"}};
                text += provider;
                return parse_spec(text);
            };

            const auto unknown_key{
                parse_with("script = { wrapper = [\"a.mod@^1.0.0\"] }\n")};
            expect(!unknown_key);
            if (!unknown_key) {
                expect(unknown_key.error().code ==
                       refrain::error_code::invalid_argument);
                expect(unknown_key.error().message.find("wrapper") !=
                       std::string::npos);
            }

            const auto ambiguous{parse_with(
                "script = { instances = { wasm = \"a.mod@^1.0.0\" }, base = "
                "\"a.mod@^1.0.0\" }\n")};
            expect(!ambiguous);
            if (!ambiguous) {
                expect(ambiguous.error().code ==
                       refrain::error_code::invalid_argument);
                expect(ambiguous.error().message.find("instances") !=
                       std::string::npos);
                expect(ambiguous.error().message.find("base") !=
                       std::string::npos);
            }
        };

    "gamepack/invalid side and host-side mismatch"_test = [] {
        auto text{std::string{valid_manifest}};
        text.replace(text.find("side = \"server\""), 15, "side = \"nonsense\"");
        expect(!parse_spec(text));

        const auto doc{parse_doc(valid_manifest)};
        const auto spec{refrain::mod::parse_gamepack(doc, "client")};
        expect(!spec);
        if (!spec) {
            expect(spec.error().code == refrain::error_code::version_mismatch);
        }
    };

    "gamepack/duplicate mods and unknown settings tables"_test = [] {
        auto duplicate{std::string{valid_manifest}};
        duplicate.replace(
            duplicate.find("\"refrain.storage.rwf@1.0.0\","),
            std::string{"\"refrain.storage.rwf@1.0.0\","}.size(),
            "\"refrain.storage.rwf@1.0.0\", \"refrain.storage.rwf@1.0.0\",");
        expect(!parse_spec(duplicate));

        auto unknown_table{std::string{valid_manifest}};
        unknown_table += "\n[mods.\"com.example.not-listed\"]\nrequires = []\n";
        expect(!parse_spec(unknown_table));
    };

    "gamepack/dependency cycle is rejected"_test = [] {
        constexpr std::string_view cyclic{R"([gamepack]
id = "refrain.cyclic"
version = "1.0.0"
side = "server"
abi = 1
auto_scan = false
[mods]
list = ["a.mod@1.0.0", "b.mod@1.0.0"]
[mods."a.mod"]
requires = ["b.mod@^1.0.0"]
[mods."b.mod"]
requires = ["a.mod@^1.0.0"]
)"};
        const auto plan{resolve_spec(cyclic)};
        expect(!plan);
        if (!plan) {
            expect(plan.error().message.find("cycle") != std::string::npos);
        }
    };

    "gamepack/missing dependency and range mismatch"_test = [] {
        constexpr std::string_view missing{R"([gamepack]
id = "refrain.missing"
version = "1.0.0"
side = "server"
abi = 1
[mods]
list = ["a.mod@1.0.0"]
[mods."a.mod"]
requires = ["b.mod@^1.0.0"]
)"};
        const auto missing_plan{resolve_spec(missing)};
        expect(!missing_plan);
        expect(missing_plan.error().code == refrain::error_code::not_found);

        constexpr std::string_view mismatch{R"([gamepack]
id = "refrain.mismatch"
version = "1.0.0"
side = "server"
abi = 1
[mods]
list = ["a.mod@1.0.0", "b.mod@2.0.0"]
[mods."a.mod"]
requires = ["b.mod@^1.0.0"]
)"};
        const auto mismatch_plan{resolve_spec(mismatch)};
        expect(!mismatch_plan);
        expect(mismatch_plan.error().code ==
               refrain::error_code::version_mismatch);
    };

    "gamepack/topological load order is deterministic"_test = [] {
        constexpr std::string_view manifest{R"([gamepack]
id = "refrain.order"
version = "1.0.0"
side = "server"
abi = 1
auto_scan = false
[mods]
list = ["a.mod@1.0.0", "b.mod@1.0.0", "c.mod@1.0.0", "d.mod@1.0.0"]
[mods."b.mod"]
requires = ["a.mod@^1.0.0"]
[mods."c.mod"]
requires = ["a.mod@^1.0.0"]
[mods."d.mod"]
requires = ["b.mod@^1.0.0", "c.mod@^1.0.0"]
)"};
        const auto first{resolve_spec(manifest)};
        const auto second{resolve_spec(manifest)};
        expect(static_cast<bool>(first));
        expect(static_cast<bool>(second));
        if (!first || !second) {
            return;
        }
        std::vector<std::string> order{};
        for (const auto &entry : first->load_order) {
            order.push_back(entry.id);
        }
        std::vector<std::string> order_again{};
        for (const auto &entry : second->load_order) {
            order_again.push_back(entry.id);
        }
        expect(order == order_again);
        const auto position = [&order](std::string_view id) {
            return std::find(order.begin(), order.end(), id) - order.begin();
        };
        expect(position("a.mod") < position("b.mod"));
        expect(position("a.mod") < position("c.mod"));
        expect(position("b.mod") < position("d.mod"));
        expect(position("c.mod") < position("d.mod"));
    };

    "gamepack/provider fallback skips missing and unsatisfied candidates"_test =
        [] {
            constexpr std::string_view manifest{R"([gamepack]
id = "refrain.fallback"
version = "1.0.0"
side = "server"
abi = 1
auto_scan = false
[mods]
list = ["base.mod@1.0.0", "provider.mod@2.0.0"]
[providers]
world = ["missing.mod@^1.0.0", "provider.mod@^1.0.0", "provider.mod@^2.0.0"]
)"};
            const auto plan{resolve_spec(manifest)};
            expect(static_cast<bool>(plan));
            if (!plan) {
                return;
            }
            const auto *world{plan->provider("world")};
            expect(world != nullptr);
            if (world != nullptr) {
                expect(world->mode == provider_mode::exclusive);
                expect(world->provider == "provider.mod@2.0.0");
                expect(world->skipped.size() == 2U);
            }
        };

    "gamepack/providers must resolve against mods.list"_test = [] {
        constexpr std::string_view manifest{R"([gamepack]
id = "refrain.unresolved"
version = "1.0.0"
side = "server"
abi = 1
[mods]
list = ["base.mod@1.0.0"]
[providers]
world = "missing.mod@^1.0.0"
)"};
        const auto plan{resolve_spec(manifest)};
        expect(!plan);
        if (!plan) {
            expect(plan.error().code == refrain::error_code::not_found);
            expect(plan.error().message.find("world") != std::string::npos);
        }
    };

    "gamepack/instanced providers resolve and are sorted"_test = [] {
        constexpr std::string_view manifest{R"([gamepack]
id = "refrain.instanced"
version = "1.0.0"
side = "server"
abi = 1
auto_scan = false
[mods]
list = ["script.wasm@1.0.0", "script.angelscript@1.0.0"]
[providers]
script = { default = "wasm", instances = { wasm = "script.wasm@^1.0.0", angelscript = "script.angelscript@^1.0.0" } }
)"};
        const auto plan{resolve_spec(manifest)};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        const auto *script{plan->provider("script")};
        expect(script != nullptr);
        if (script != nullptr) {
            expect(script->mode == provider_mode::instanced);
            expect(script->instances.size() == 2U);
            expect(script->default_instance == "wasm");
            expect(script->instances[0].name == "angelscript");
            expect(script->instances[0].provider == "script.angelscript@1.0.0");
            expect(script->instances[1].name == "wasm");
            expect(script->instances[1].provider == "script.wasm@1.0.0");
        }
        expect(plan->format().find("[instanced:wasm] (default)") !=
               std::string::npos);
    };

    "gamepack/chain providers resolve base and wrappers"_test = [] {
        constexpr std::string_view manifest{R"([gamepack]
id = "refrain.chain"
version = "1.0.0"
side = "server"
abi = 1
[mods]
list = ["vanilla.worldgen@1.0.0", "tweaks.mod@1.0.0", "enhanced.mod@1.0.0"]
[providers]
worldgen = { base = "vanilla.worldgen@^1.0.0", wrappers = ["tweaks.mod@^1.0.0", "enhanced.mod@^1.0.0"] }
)"};
        const auto plan{resolve_spec(manifest)};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        const auto *worldgen{plan->provider("worldgen")};
        expect(worldgen != nullptr);
        if (worldgen != nullptr) {
            expect(worldgen->mode == provider_mode::chain);
            expect(worldgen->provider == "vanilla.worldgen@1.0.0");
            expect(worldgen->wrappers.size() == 2U);
            expect(worldgen->wrappers[0] == "tweaks.mod@1.0.0");
        }
        expect(plan->format().find("[chain]") != std::string::npos);
    };

    "gamepack/invalid provider tables are rejected"_test = [] {
        for (const std::string_view providers : {R"([providers]
script = { instances = {} }
)",
                                                 R"([providers]
script = { instances = { "Bad-Name" = "a.mod@1.0.0" } }
)",
                                                 R"([providers]
script = { default = "missing", instances = { wasm = "a.mod@1.0.0" } }
)",
                                                 R"([providers]
script = { }
)",
                                                 R"([providers]
script = "@auto"
)"}) {
            auto text{std::string{R"([gamepack]
id = "refrain.providers"
version = "1.0.0"
side = "server"
abi = 1
[mods]
list = ["a.mod@1.0.0", "b.mod@1.0.0"]
)"}};
            text += providers;
            expect(!parse_spec(text)) << "expected reject: " << providers;
        }
    };

    "gamepack/required services are enforced"_test = [] {
        constexpr std::string_view manifest{R"([gamepack]
id = "refrain.required"
version = "1.0.0"
side = "server"
abi = 1
[mods]
list = ["world.mod@1.0.0", "storage.mod@1.0.0"]
[providers]
world = "world.mod@^1.0.0"
storage = "storage.mod@^1.0.0"
)"};
        auto spec{parse_spec(manifest)};
        expect(static_cast<bool>(spec));
        if (!spec) {
            return;
        }
        const std::array<std::string_view, 2> required{"world", "storage"};
        const auto plan{refrain::mod::preview_composition(*spec, required)};
        expect(static_cast<bool>(plan));

        const std::array<std::string_view, 2> missing{"world", "worldgen"};
        const auto failed{refrain::mod::preview_composition(*spec, missing)};
        expect(!failed);
        if (!failed) {
            expect(failed.error().code == refrain::error_code::not_found);
        }
    };

    "gamepack/auto_scan with an empty visible catalog is rejected"_test = [] {
        constexpr std::string_view manifest{R"([gamepack]
id = "refrain.autoscan"
version = "1.0.0"
side = "server"
abi = 1
[mods]
list = []
)"};
        const auto plan{resolve_spec(manifest)};
        expect(!plan);
        if (!plan) {
            expect(plan.error().code == refrain::error_code::unsupported);
            expect(plan.error().message.find("auto-discovery") !=
                   std::string::npos);
            expect(plan.error().message.find("resolved mod catalog") !=
                   std::string::npos);
        }

        auto disabled{std::string{manifest}};
        disabled.replace(disabled.find("abi = 1"), 7,
                         "abi = 1\nauto_scan = false");
        const auto disabled_plan{resolve_spec(disabled)};
        expect(!disabled_plan);
        if (!disabled_plan) {
            expect(disabled_plan.error().code ==
                   refrain::error_code::invalid_argument);
        }
    };

    "gamepack/malformed gamepack version"_test = [] {
        auto text{std::string{valid_manifest}};
        text.replace(text.find("version = \"26.3.0\""), 18,
                     "version = \"26.3\"");
        expect(!parse_spec(text));
    };

    "gamepack/resolve rejects duplicate mod ids"_test = [] {
        gamepack_spec spec{};
        spec.id = "refrain.manual";
        spec.version = "1.0.0";
        spec.side = "server";
        spec.abi = 1;
        spec.mods.push_back(
            mod_entry{.ref = mod_ref{.id = "a.mod", .version = "1.0.0"}});
        spec.mods.push_back(
            mod_entry{.ref = mod_ref{.id = "a.mod", .version = "1.0.0"}});
        const auto plan{refrain::mod::preview_composition(spec)};
        expect(!plan);
        if (!plan) {
            expect(plan.error().code == refrain::error_code::already_exists);
        }
    };

    "gamepack/load_gamepack reads files and annotates errors"_test = [] {
        const auto path{make_temp_path()};
        {
            std::ofstream stream{path};
            stream << valid_manifest;
        }
        const auto plan{
            refrain::mod::load_gamepack_spec(path.string(), "server")};
        expect(static_cast<bool>(plan));
        remove_quietly(path);

        {
            std::ofstream stream{path};
            stream << "[gamepack]\nid = \"broken\"\n";
        }
        const auto broken{
            refrain::mod::load_gamepack_spec(path.string(), "server")};
        expect(!broken);
        if (!broken) {
            expect(broken.error().message.find(path.string()) !=
                   std::string::npos);
        }
        remove_quietly(path);
    };

    "gamepack/format reports load order, modes and dirs"_test = [] {
        const auto plan{resolve_spec(valid_manifest)};
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        const auto *storage{plan->provider("storage")};
        expect(storage != nullptr);
        if (storage != nullptr) {
            expect(storage->default_instance == "rwf");
        }
        const auto text{plan->format()};
        expect(text.find("mods: 5 (load order)") != std::string::npos);
        expect(text.find("[instanced:rwf]") != std::string::npos);
        expect(text.find("[instanced:rwf] (default)") != std::string::npos);
        expect(text.find("[instanced:memory] (default)") == std::string::npos);
        expect(text.find("mods_dir: mods, extra-mods") != std::string::npos);
    };
};

} // namespace
