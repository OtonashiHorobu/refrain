// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Composition resolution for package-provided loaders: the gamepack `[loaders]`
// declaration selects the provider mods, orders every package that names a
// declared loader after its provider, and rejects an undeclared namespaced
// loader, a provider that cannot be a loader and a loader ordering cycle.

#include <refrain/core/config.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;

namespace config = refrain::core::config;
namespace mod = refrain::mod;
using refrain::error_code;

constexpr std::string_view loader_service{"refrain:mod.loader"};

[[nodiscard]] auto gamepack_text(std::string_view mods_list,
                                 std::string_view loaders = {}) -> std::string {
    auto text{std::string{"[gamepack]\n"}};
    text += "id = \"refrain.test.loader.plan\"\n";
    text += "version = \"1.0.0\"\n";
    text += "side = \"server\"\n";
    text += "abi = \"1\"\n";
    text += "auto_scan = false\n\n[mods]\nlist = [";
    text += mods_list;
    text += "]\n";
    if (!loaders.empty()) {
        text += "\n[loaders]\n";
        text += loaders;
    }
    return text;
}

[[nodiscard]] auto parse_spec(std::string_view text)
    -> refrain::result<mod::gamepack_spec> {
    auto doc{config::document::parse_text(text, "<gamepack>")};
    if (!doc) {
        return std::unexpected{doc.error()};
    }
    return mod::parse_gamepack(*doc, "server");
}

[[nodiscard]] auto make_candidate(std::string id, std::string version = "1.0.0")
    -> mod::candidate {
    return mod::candidate{
        .id = std::move(id),
        .version = std::move(version),
        .abi = 1,
        .origin = mod::mod_origin::builtin,
        .side = mod::manifest_side::server,
        .path = "<test>/manifest.toml",
        .kind = mod::package_kind::builtin,
    };
}

/// Candidate that provides the loader service and is executed by `loader`.
[[nodiscard]] auto make_provider(std::string id, std::string loader = "native")
    -> mod::candidate {
    auto candidate{make_candidate(std::move(id))};
    candidate.provides = {std::string{loader_service}};
    candidate.entry = mod::manifest_entry_point{.loader = std::move(loader)};
    return candidate;
}

[[nodiscard]] auto make_guest(std::string id, std::string loader)
    -> mod::candidate {
    auto candidate{make_candidate(std::move(id))};
    candidate.entry = mod::manifest_entry_point{.loader = std::move(loader)};
    return candidate;
}

[[nodiscard]] auto index_of(const mod::composition &plan, std::string_view id)
    -> std::size_t {
    const auto match{std::find_if(
        plan.load_order.begin(), plan.load_order.end(),
        [id](const mod::candidate &entry) { return entry.id == id; })};
    return static_cast<std::size_t>(
        std::distance(plan.load_order.begin(), match));
}

const suite loader_plan_tests = [] {
    "loader_plan/declares the provider and orders guests after it"_test = [] {
        auto spec{parse_spec(gamepack_text(
            "\"refrain.test.guest@1.0.0\", "
            "\"refrain.test.provider@1.0.0\"",
            "\"com.test.loader\" = \"refrain.test.provider@^1.0.0\"\n"))};
        expect(static_cast<bool>(spec));
        if (!spec) {
            return;
        }
        std::vector<mod::candidate> catalog{};
        catalog.push_back(make_guest("refrain.test.guest", "com.test.loader"));
        catalog.push_back(make_provider("refrain.test.provider"));
        const auto plan{mod::resolve(*spec, catalog)};
        expect(static_cast<bool>(plan))
            << (plan ? std::string{"resolve ok"} : plan.error().message);
        if (!plan) {
            return;
        }
        expect(plan->loaders.size() == 1U);
        if (!plan->loaders.empty()) {
            expect(plan->loaders.front().id == "com.test.loader");
            expect(plan->loaders.front().provider ==
                   "refrain.test.provider@1.0.0");
        }
        // The guest is listed first in the gamepack, but the loader ordering
        // edge puts its provider first.
        expect(index_of(*plan, "refrain.test.provider") <
               index_of(*plan, "refrain.test.guest"));
    };

    "loader_plan/one provider mod may provide several loader ids"_test = [] {
        auto spec{
            parse_spec(gamepack_text("\"refrain.test.guest.one@1.0.0\", "
                                     "\"refrain.test.guest.two@1.0.0\", "
                                     "\"refrain.test.provider@1.0.0\"",
                                     "\"com.test.loader.one\" = "
                                     "\"refrain.test.provider@^1.0.0\"\n"
                                     "\"com.test.loader.two\" = "
                                     "\"refrain.test.provider@^1.0.0\"\n"))};
        expect(static_cast<bool>(spec));
        if (!spec) {
            return;
        }
        std::vector<mod::candidate> catalog{};
        catalog.push_back(
            make_guest("refrain.test.guest.one", "com.test.loader.one"));
        catalog.push_back(
            make_guest("refrain.test.guest.two", "com.test.loader.two"));
        catalog.push_back(make_provider("refrain.test.provider"));
        const auto plan{mod::resolve(*spec, catalog)};
        expect(static_cast<bool>(plan))
            << (plan ? std::string{"resolve ok"} : plan.error().message);
        if (!plan) {
            return;
        }
        expect(plan->loaders.size() == 2U);
        if (plan->loaders.size() == 2U) {
            expect(plan->loaders[0].id == "com.test.loader.one");
            expect(plan->loaders[0].provider == "refrain.test.provider@1.0.0");
            expect(plan->loaders[1].id == "com.test.loader.two");
            expect(plan->loaders[1].provider == "refrain.test.provider@1.0.0");
        }
        // The provider is ordered before both guests.
        expect(index_of(*plan, "refrain.test.provider") <
               index_of(*plan, "refrain.test.guest.one"));
        expect(index_of(*plan, "refrain.test.provider") <
               index_of(*plan, "refrain.test.guest.two"));
    };

    "loader_plan/rejects a cycle between loader providers and guests"_test =
        [] {
            auto spec{parse_spec(
                gamepack_text("\"refrain.test.cycle.provider.one@1.0.0\", "
                              "\"refrain.test.cycle.provider.two@1.0.0\", "
                              "\"refrain.test.cycle.guest.one@1.0.0\", "
                              "\"refrain.test.cycle.guest.two@1.0.0\"",
                              "\"com.test.cycle.one\" = "
                              "\"refrain.test.cycle.provider.one@^1.0.0\"\n"
                              "\"com.test.cycle.two\" = "
                              "\"refrain.test.cycle.provider.two@^1.0.0\"\n"))};
            expect(static_cast<bool>(spec));
            if (!spec) {
                return;
            }
            // provider one requires guest one; guest one names provider two's
            // loader; provider two requires guest two; guest two names provider
            // one's loader: the loader ordering edges close the cycle.
            auto provider_one{make_provider("refrain.test.cycle.provider.one")};
            provider_one.required.push_back(mod::mod_requirement{
                .id = "refrain.test.cycle.guest.one", .range = "^1.0.0"});
            auto provider_two{make_provider("refrain.test.cycle.provider.two")};
            provider_two.required.push_back(mod::mod_requirement{
                .id = "refrain.test.cycle.guest.two", .range = "^1.0.0"});
            auto guest_one{make_guest("refrain.test.cycle.guest.one",
                                      "com.test.cycle.two")};
            auto guest_two{make_guest("refrain.test.cycle.guest.two",
                                      "com.test.cycle.one")};
            std::vector<mod::candidate> catalog{};
            catalog.push_back(std::move(provider_one));
            catalog.push_back(std::move(provider_two));
            catalog.push_back(std::move(guest_one));
            catalog.push_back(std::move(guest_two));
            const auto plan{mod::resolve(*spec, catalog)};
            expect(!plan);
            if (!plan) {
                expect(plan.error().code == error_code::mod_error);
                expect(plan.error().message.find("cycle") != std::string::npos);
                for (const auto *id : {"refrain.test.cycle.provider.one",
                                       "refrain.test.cycle.provider.two",
                                       "refrain.test.cycle.guest.one",
                                       "refrain.test.cycle.guest.two"}) {
                    expect(plan.error().message.find(id) != std::string::npos);
                }
            }
        };

    "loader_plan/rejects a provider that does not declare the loader "
    "service"_test = [] {
        auto spec{parse_spec(gamepack_text(
            "\"refrain.test.provider@1.0.0\"",
            "\"com.test.loader\" = \"refrain.test.provider@^1.0.0\"\n"))};
        expect(static_cast<bool>(spec));
        if (!spec) {
            return;
        }
        auto provider{make_candidate("refrain.test.provider")};
        provider.entry = mod::manifest_entry_point{.loader = "native"};
        std::vector<mod::candidate> catalog{};
        catalog.push_back(std::move(provider));
        const auto plan{mod::resolve(*spec, catalog)};
        expect(!plan);
        if (!plan) {
            expect(plan.error().code == error_code::permission_denied);
            expect(plan.error().message.find("does not declare") !=
                   std::string::npos);
            expect(plan.error().message.find(loader_service) !=
                   std::string::npos);
        }
    };

    "loader_plan/rejects a provider executed by a sandboxed loader"_test = [] {
        auto spec{parse_spec(gamepack_text(
            "\"refrain.test.provider@1.0.0\"",
            "\"com.test.loader\" = \"refrain.test.provider@^1.0.0\"\n"))};
        expect(static_cast<bool>(spec));
        if (!spec) {
            return;
        }
        auto provider{make_provider("refrain.test.provider", "wasm")};
        provider.entry->wasm = "scripts/loader.wasm";
        std::vector<mod::candidate> catalog{};
        catalog.push_back(std::move(provider));
        const auto plan{mod::resolve(*spec, catalog)};
        expect(!plan);
        if (!plan) {
            expect(plan.error().code == error_code::unsupported);
            expect(plan.error().message.find("wasm") != std::string::npos);
            expect(plan.error().message.find("sandboxed") != std::string::npos);
            expect(plan.error().message.find("builtin or native") !=
                   std::string::npos);
        }
    };

    "loader_plan/rejects an undeclared namespaced loader and lists the "
    "declared ones"_test = [] {
        auto spec{parse_spec(gamepack_text(
            "\"refrain.test.guest@1.0.0\", "
            "\"refrain.test.provider@1.0.0\"",
            "\"com.test.loader\" = \"refrain.test.provider@^1.0.0\"\n"))};
        expect(static_cast<bool>(spec));
        if (!spec) {
            return;
        }
        std::vector<mod::candidate> catalog{};
        catalog.push_back(make_guest("refrain.test.guest", "com.other.loader"));
        catalog.push_back(make_provider("refrain.test.provider"));
        const auto plan{mod::resolve(*spec, catalog)};
        expect(!plan);
        if (!plan) {
            expect(plan.error().code == error_code::not_found);
            expect(plan.error().message.find("com.other.loader") !=
                   std::string::npos);
            expect(plan.error().message.find("declared loaders: "
                                             "com.test.loader") !=
                   std::string::npos);
        }
    };

    "loader_plan/keeps an embedder-registered loader usable when the plan "
    "declares none"_test = [] {
        auto spec{parse_spec(gamepack_text("\"refrain.test.guest@1.0.0\""))};
        expect(static_cast<bool>(spec));
        if (!spec) {
            return;
        }
        std::vector<mod::candidate> catalog{};
        catalog.push_back(make_guest("refrain.test.guest", "test.embedder"));
        const auto plan{mod::resolve(*spec, catalog)};
        expect(static_cast<bool>(plan))
            << (plan ? std::string{"resolve ok"} : plan.error().message);
        if (plan) {
            expect(plan->loaders.empty());
        }
    };

    "loader_plan/rejects a declared loader whose provider is not in the "
    "resolved set"_test = [] {
        auto spec{parse_spec(gamepack_text(
            "\"refrain.test.guest@1.0.0\"",
            "\"com.test.loader\" = \"refrain.test.absent@^1.0.0\"\n"))};
        expect(static_cast<bool>(spec));
        if (!spec) {
            return;
        }
        std::vector<mod::candidate> catalog{};
        catalog.push_back(make_guest("refrain.test.guest", "com.test.loader"));
        const auto plan{mod::resolve(*spec, catalog)};
        expect(!plan);
        if (!plan) {
            expect(plan.error().code == error_code::not_found);
            expect(plan.error().message.find("com.test.loader") !=
                   std::string::npos);
            expect(plan.error().message.find("refrain.test.absent") !=
                   std::string::npos);
        }
    };

    "loader_plan/rejects a loader declaration outside the provider's "
    "range"_test = [] {
        auto spec{parse_spec(gamepack_text(
            "\"refrain.test.provider@2.0.0\"",
            "\"com.test.loader\" = \"refrain.test.provider@^1.0.0\"\n"))};
        expect(static_cast<bool>(spec));
        if (!spec) {
            return;
        }
        auto provider{make_provider("refrain.test.provider")};
        provider.version = "2.0.0";
        std::vector<mod::candidate> catalog{};
        catalog.push_back(std::move(provider));
        const auto plan{mod::resolve(*spec, catalog)};
        expect(!plan);
        if (!plan) {
            expect(plan.error().code == error_code::version_mismatch);
            expect(plan.error().message.find("com.test.loader") !=
                   std::string::npos);
            expect(plan.error().message.find("2.0.0") != std::string::npos);
        }
    };
};

} // namespace
