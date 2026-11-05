// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/config.hpp>
#include <refrain/core/io.hpp>
#include <refrain/kernel/gamepack.hpp>
#include <refrain/kernel/manifest.hpp>
#include <refrain/kernel/static_mod.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace boost::ut;

namespace config = refrain::core::config;
namespace io = refrain::core::io;
namespace kernel = refrain::kernel;
using refrain::error_code;

// Two statically linked mods registered through the public macro, proving the
// registrar mechanics (executed before main / before the test cases).
namespace {
// NOLINTBEGIN(cert-err58-cpp): intentional static-initialization registration.
REFR_MOD_STATIC("refrain.test.static_a", "1.0.0", 1, nullptr);
REFR_MOD_STATIC("refrain.test.static_b", "2.3.4", 1, nullptr);
// NOLINTEND(cert-err58-cpp)
} // namespace

[[nodiscard]] auto parse_mod(std::string_view text)
    -> refrain::result<kernel::mod_manifest> {
    auto doc{config::document::parse_text(text, "<test>")};
    if (!doc) {
        return std::unexpected{doc.error()};
    }
    return kernel::parse_mod_manifest(*doc);
}

[[nodiscard]] auto expect_mod_error(std::string_view text, error_code code) {
    auto manifest{parse_mod(text)};
    expect(!manifest) << "expected a manifest error";
    if (!manifest) {
        expect(manifest.error().code == code)
            << "unexpected error: " << manifest.error().message;
    }
}

constexpr std::string_view k_internal_manifest = R"(
[mod]
id = "refrain.content.vanilla"
version = "1.0.0"
abi = "1"
side = "both"

[meta]
display_name = "Refrain vanilla content"
description = "First-party built-in content."
icon = "assets/refrain/icon.png"
license = "MIT"
homepage = "https://example.invalid/refrain"
authors = ["Refrain contributors"]

[requires]
"refrain.data.vanilla" = "^1.0.0"
)";

constexpr std::string_view k_provider_manifest = R"(
[mod]
id = "refrain.data.vanilla"
version = "1.0.0"
abi = 1
side = "server"

[requires]
"refrain.content.vanilla" = "^1.0.0"

[optional_requires]
"refrain.script" = ">=1.0.0"

[provides]
services = ["refrain:data.packs", "refrain:content"]
)";

constexpr std::string_view k_external_manifest = R"(
[mod]
id = "com.example.rpg"
version = "0.1.0+build.7"
abi = "1"

[entry]
native = "lib/libexample_rpg.so"
symbol = "refr_mod_entry"
)";

const suite manifest_tests = [] {
    "manifest/mod: parses an internal content manifest"_test = [] {
        auto manifest{parse_mod(k_internal_manifest)};
        expect(static_cast<bool>(manifest));
        if (!manifest) {
            return;
        }
        expect(manifest->common.id == "refrain.content.vanilla");
        expect(manifest->common.version == "1.0.0");
        expect(manifest->common.side == kernel::manifest_side::both);
        expect(manifest->common.abi == 1U);
        expect(!manifest->is_external());
        expect(manifest->common.meta.display_name == "Refrain vanilla content");
        expect(manifest->common.meta.license == "MIT");
        expect(manifest->common.meta.authors.size() == 1U);
        expect(manifest->required.size() == 1U);
        expect(manifest->required.front().id == "refrain.data.vanilla");
        expect(manifest->required.front().range == "^1.0.0");
        expect(manifest->optional.empty());
        expect(manifest->provides.empty());
        expect(manifest->entry == std::nullopt);
    };

    "manifest/mod: parses provides and optional requirements"_test = [] {
        auto manifest{parse_mod(k_provider_manifest)};
        expect(static_cast<bool>(manifest));
        if (!manifest) {
            return;
        }
        expect(manifest->common.side == kernel::manifest_side::server);
        expect(manifest->provides.size() == 2U);
        expect(manifest->provides[0] == "refrain:content");
        expect(manifest->provides[1] == "refrain:data.packs");
        expect(manifest->required.size() == 1U);
        expect(manifest->optional.size() == 1U);
        expect(manifest->optional.front().id == "refrain.script");
    };

    "manifest/mod: external native entry point"_test = [] {
        auto manifest{parse_mod(k_external_manifest)};
        expect(static_cast<bool>(manifest));
        if (!manifest) {
            return;
        }
        expect(manifest->is_external());
        expect(manifest->entry->native == "lib/libexample_rpg.so");
        expect(manifest->entry->symbol == "refr_mod_entry");
        expect(manifest->entry->wasm.empty());
        expect(manifest->common.version == "0.1.0+build.7");
        expect(manifest->common.side == kernel::manifest_side::both);
    };

    "manifest/mod: wasm entry point"_test = [] {
        auto manifest{parse_mod(R"(
[mod]
id = "com.example.script"
version = "1.0.0"
abi = "1"

[entry]
wasm = "scripts/main.wasm"
)")};
        expect(static_cast<bool>(manifest));
        if (manifest) {
            expect(manifest->is_external());
            expect(manifest->entry->wasm == "scripts/main.wasm");
            expect(manifest->entry->native.empty());
        }
    };

    "manifest/mod: rejects malformed documents"_test = [] {
        expect_mod_error("", error_code::parse_error);
        expect_mod_error("[mod]\nid = \"x.y\"\n", error_code::parse_error);
        expect_mod_error(R"(
[mod]
id = "a b"
version = "1.0.0"
abi = "1"
)",
                         error_code::invalid_argument);
        expect_mod_error(R"(
[mod]
id = "a.b"
version = "1.0"
abi = "1"
)",
                         error_code::invalid_argument);
        expect_mod_error(R"(
[mod]
id = "a.b"
version = "1.0.0"
)",
                         error_code::parse_error);
        expect_mod_error(R"(
[mod]
id = "a.b"
version = "1.0.0"
abi = "0"
)",
                         error_code::parse_error);
        expect_mod_error(R"(
[mod]
id = "a.b"
version = "1.0.0"
abi = "1"
side = "everywhere"
)",
                         error_code::invalid_argument);
        expect_mod_error(R"(
[mod]
id = "a.b"
version = "1.0.0"
abi = "1"
unknown = 1
)",
                         error_code::invalid_argument);
        expect_mod_error(R"(
[mod]
id = "a.b"
version = "1.0.0"
abi = "1"

[meta]
colour = "red"
)",
                         error_code::invalid_argument);
    };

    "manifest/mod: rejects invalid dependency tables"_test = [] {
        expect_mod_error(R"(
[mod]
id = "a.b"
version = "1.0.0"
abi = "1"

[requires]
"a.b" = "^1.0.0"
)",
                         error_code::invalid_argument);
        expect_mod_error(R"(
[mod]
id = "a.b"
version = "1.0.0"
abi = "1"

[requires]
"other.mod" = "not-a-range"
)",
                         error_code::invalid_argument);
        expect_mod_error(R"(
[mod]
id = "a.b"
version = "1.0.0"
abi = "1"

[requires]
"other.mod" = "^1.0.0"

[optional_requires]
"other.mod" = ">=1.0.0"
)",
                         error_code::invalid_argument);
        expect_mod_error(R"(
[mod]
id = "a.b"
version = "1.0.0"
abi = "1"

[requires]
"Bad Id" = "^1.0.0"
)",
                         error_code::invalid_argument);
    };

    "manifest/mod: rejects invalid provides and entries"_test = [] {
        expect_mod_error(R"(
[mod]
id = "a.b"
version = "1.0.0"
abi = "1"

[provides]
services = ["not-a-service-id"]
)",
                         error_code::invalid_argument);
        expect_mod_error(R"(
[mod]
id = "a.b"
version = "1.0.0"
abi = "1"

[provides]
services = ["refrain:data.packs", "refrain:data.packs"]
)",
                         error_code::already_exists);
        expect_mod_error(R"(
[mod]
id = "a.b"
version = "1.0.0"
abi = "1"

[entry]
native = "lib/lib.so"
wasm = "scripts/main.wasm"
)",
                         error_code::invalid_argument);
        expect_mod_error(R"(
[mod]
id = "a.b"
version = "1.0.0"
abi = "1"

[entry]
native = "../escape.so"
)",
                         error_code::invalid_argument);
        expect_mod_error(R"(
[mod]
id = "a.b"
version = "1.0.0"
abi = "1"

[entry]
symbol = "entry"
)",
                         error_code::invalid_argument);
    };

    "manifest/mod: requires table order is normalized"_test = [] {
        auto manifest{parse_mod(R"(
[mod]
id = "a.b"
version = "1.0.0"
abi = "1"

[requires]
"zzz.mod" = ">=1.0.0"
"aaa.mod" = "^2.0.0"
)")};
        expect(static_cast<bool>(manifest));
        if (manifest) {
            expect(manifest->required.size() == 2U);
            expect(manifest->required[0].id == "aaa.mod");
            expect(manifest->required[1].id == "zzz.mod");
        }
    };

    "manifest/mod: parse options gate abi and display name"_test = [] {
        auto doc{config::document::parse_text(k_internal_manifest, "<test>")};
        expect(static_cast<bool>(doc));
        if (!doc) {
            return;
        }

        auto matching{kernel::parse_mod_manifest(
            *doc, kernel::manifest_parse_options{.expected_abi = 1})};
        expect(static_cast<bool>(matching));

        auto mismatched{kernel::parse_mod_manifest(
            *doc, kernel::manifest_parse_options{.expected_abi = 2})};
        expect(!mismatched);
        if (!mismatched) {
            expect(mismatched.error().code == error_code::version_mismatch);
        }

        auto unnamed{config::document::parse_text(R"(
[mod]
id = "a.b"
version = "1.0.0"
abi = "1"
)",
                                                  "<test>")};
        expect(static_cast<bool>(unnamed));
        if (!unnamed) {
            return;
        }
        auto missing_display{kernel::parse_mod_manifest(
            *unnamed,
            kernel::manifest_parse_options{.require_display_name = true})};
        expect(!missing_display);
        if (!missing_display) {
            expect(missing_display.error().code == error_code::parse_error);
        }
    };

    "manifest/mod: load_mod_manifest annotates the path"_test = [] {
        const auto dir{std::filesystem::temp_directory_path() /
                       "refrain-test-manifest"};
        std::filesystem::create_directories(dir);
        const auto path{dir / "manifest.toml"};
        auto written{io::write_text(path.string(), k_internal_manifest)};
        expect(static_cast<bool>(written));

        auto manifest{kernel::load_mod_manifest(path.string())};
        expect(static_cast<bool>(manifest));

        auto missing{kernel::load_mod_manifest((dir / "nope.toml").string())};
        expect(!missing);
        if (!missing) {
            expect(missing.error().message.find("nope.toml") !=
                   std::string::npos);
        }
        std::filesystem::remove_all(dir);
    };

    "manifest/common: shared meta and identity formatting"_test = [] {
        auto manifest{parse_mod(k_internal_manifest)};
        expect(static_cast<bool>(manifest));
        if (!manifest) {
            return;
        }
        const auto text{kernel::format_manifest_common(manifest->common)};
        expect(text == "refrain.content.vanilla@1.0.0 (side=both, abi=1)");
        const auto formatted{kernel::format_mod_manifest(*manifest)};
        expect(formatted.find("internal") != std::string::npos);
        expect(formatted.find("refrain.data.vanilla@^1.0.0") !=
               std::string::npos);
    };

    "manifest/common: gamepack shares the layer"_test = [] {
        auto doc{config::document::parse_text(R"(
[gamepack]
id = "refrain.vanilla"
version = "26.3.0"
side = "server"
abi = "1"

[meta]
display_name = "Refrain vanilla"
)")};
        expect(static_cast<bool>(doc));
        if (!doc) {
            return;
        }
        auto common{kernel::parse_manifest_common(
            *doc, "gamepack",
            kernel::manifest_parse_options{.expected_abi = 1,
                                           .require_side = true})};
        expect(static_cast<bool>(common));
        if (!common) {
            return;
        }
        expect(common->id == "refrain.vanilla");
        expect(common->side == kernel::manifest_side::server);
        expect(kernel::manifest_side_matches(common->side, "server"));
        expect(!kernel::manifest_side_matches(common->side, "client"));

        auto mismatch{kernel::parse_manifest_common(
            *doc, "gamepack",
            kernel::manifest_parse_options{.expected_abi = 2})};
        expect(!mismatch);
        if (!mismatch) {
            expect(mismatch.error().code == error_code::version_mismatch);
        }
    };

    "mod_ref: id and service id validation"_test = [] {
        expect(kernel::is_valid_mod_id("refrain.world.vanilla"));
        expect(kernel::is_valid_mod_id("com.example.rpg"));
        expect(kernel::is_valid_mod_id("a"));
        expect(!kernel::is_valid_mod_id(""));
        expect(!kernel::is_valid_mod_id(".a"));
        expect(!kernel::is_valid_mod_id("a b"));
        expect(!kernel::is_valid_mod_id(std::string(129, 'a')));
        expect(kernel::is_valid_service_id("refrain:data.packs"));
        expect(!kernel::is_valid_service_id("refrain"));
        expect(!kernel::is_valid_service_id(":data"));
        expect(!kernel::is_valid_service_id("refrain:"));
        expect(!kernel::is_valid_service_id("Refrain:data"));
        expect(!kernel::is_valid_service_id("a:b:c"));
    };

    "static_mod: macro registrations are visible"_test = [] {
        auto &registry{kernel::static_mod_registry::global()};
        const auto *a{registry.find("refrain.test.static_a")};
        expect(a != nullptr);
        if (a != nullptr) {
            expect(a->version == "1.0.0");
            expect(a->abi == 1U);
            expect(a->factory == nullptr);
        }
        expect(registry.find("refrain.test.static_b") != nullptr);
        expect(registry.find("refrain.data.vanilla") != nullptr);
        expect(registry.size() >= 3U);
    };

    "static_mod: duplicate and invalid registrations are deferred"_test = [] {
        kernel::static_mod_registry registry{};
        auto added{
            registry.add(kernel::static_mod_descriptor{.id = "refrain.test.dup",
                                                       .version = "1.0.0",
                                                       .abi = 1,
                                                       .factory = nullptr})};
        expect(static_cast<bool>(added));
        auto duplicate{
            registry.add(kernel::static_mod_descriptor{.id = "refrain.test.dup",
                                                       .version = "1.0.0",
                                                       .abi = 1,
                                                       .factory = nullptr})};
        expect(!duplicate);
        if (!duplicate) {
            expect(duplicate.error().code == error_code::already_exists);
        }
        // The deferred error must surface when the host freezes the registry.
        auto frozen{registry.freeze()};
        expect(!frozen);
        if (!frozen) {
            expect(frozen.error().code == error_code::already_exists);
            expect(frozen.error().message.find("refrain.test.dup") !=
                   std::string::npos);
        }
        // Frozen registries reject further registrations.
        auto late{registry.add(
            kernel::static_mod_descriptor{.id = "refrain.test.late",
                                          .version = "1.0.0",
                                          .abi = 1,
                                          .factory = nullptr})};
        expect(!late);
        if (!late) {
            expect(late.error().code == error_code::permission_denied);
        }

        auto invalid_id{registry.add(kernel::static_mod_descriptor{
            .id = "Bad Id", .version = "1.0.0", .abi = 1, .factory = nullptr})};
        expect(!invalid_id);
        auto invalid_version{registry.add(
            kernel::static_mod_descriptor{.id = "refrain.test.badver",
                                          .version = "1.0",
                                          .abi = 1,
                                          .factory = nullptr})};
        expect(!invalid_version);
        auto invalid_abi{registry.add(
            kernel::static_mod_descriptor{.id = "refrain.test.badabi",
                                          .version = "1.0.0",
                                          .abi = 0,
                                          .factory = nullptr})};
        expect(!invalid_abi);
    };

    "static_mod: matches manifests and reports drift"_test = [] {
        kernel::static_mod_registry registry{};
        expect(static_cast<bool>(registry.add(
            kernel::static_mod_descriptor{.id = "refrain.data.vanilla",
                                          .version = "1.0.0",
                                          .abi = 1,
                                          .factory = nullptr})));

        auto manifest{parse_mod(k_provider_manifest)};
        expect(static_cast<bool>(manifest));
        if (!manifest) {
            return;
        }
        auto matched{registry.match(*manifest)};
        expect(static_cast<bool>(matched));
        if (matched) {
            expect(*matched != nullptr);
            expect((*matched)->id == "refrain.data.vanilla");
        }

        auto drifted{parse_mod(k_provider_manifest)};
        expect(static_cast<bool>(drifted));
        if (!drifted) {
            return;
        }
        drifted->common.version = "1.0.1";
        auto mismatch{registry.match(*drifted)};
        expect(!mismatch);
        if (!mismatch) {
            expect(mismatch.error().code == error_code::version_mismatch);
            expect(mismatch.error().message.find("drift") != std::string::npos);
        }

        // Content-only mods have no static code: match returns nullptr.
        auto content{parse_mod(k_internal_manifest)};
        expect(static_cast<bool>(content));
        if (content) {
            auto no_code{registry.match(*content)};
            expect(static_cast<bool>(no_code));
            if (no_code) {
                expect(*no_code == nullptr);
            }
        }
    };
};

} // namespace
