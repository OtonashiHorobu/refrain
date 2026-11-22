// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Generation of a deterministic starter gamepack from a build profile, the
// named selection error for a service with several providers, and the
// version-token-only gamepack synchronization path.

#include <refrain/app/mod_management.hpp>
#include <refrain/app/mod_profile.hpp>

#include <refrain/core/io.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>

#include <boost/ut.hpp>

#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;

namespace app = refrain::app;
namespace fs = std::filesystem;
namespace mod = refrain::mod;

constexpr std::string_view k_source_root{REFRAIN_SOURCE_DIR};

auto write_text(const fs::path &path, std::string_view text) -> bool {
    std::ofstream stream{path};
    if (!stream) {
        return false;
    }
    stream << text;
    return static_cast<bool>(stream);
}

auto source_path(std::string_view relative) -> std::string {
    return (fs::path{k_source_root} / relative).string();
}

auto profile_named(std::string_view name, std::vector<std::string> mods)
    -> app::mod_profile_spec {
    app::mod_profile_spec profile{};
    profile.name = std::string{name};
    profile.gamepack = "gamepack.toml";
    profile.mods = std::move(mods);
    return profile;
}

const suite gamepack_gen_sync_tests = [] {
    "gamepack gen is deterministic and passes the profile checks"_test = [] {
        auto profile{
            app::load_mod_profile(source_path("profiles/vanilla.toml"))};
        expect(static_cast<bool>(profile))
            << (profile ? "" : profile.error().message);
        if (!profile) {
            return;
        }
        auto first{app::generate_gamepack(*profile, source_path("mods"))};
        auto second{app::generate_gamepack(*profile, source_path("mods"))};
        expect(static_cast<bool>(first))
            << (first ? "" : first.error().message);
        expect(static_cast<bool>(second));
        if (!first || !second) {
            return;
        }
        expect(first->text == second->text);
        expect(first->spec.mods.size() == profile->mods.size());
        expect(!first->spec.providers.empty());
        expect(first->spec.auto_scan == false);
        expect(first->text.find("remains a deployment decision") !=
               std::string::npos);

        // The generated text is what the two CLI checks read back.
        const auto directory{fs::temp_directory_path() /
                             "refrain-test-gamepack-gen"};
        fs::remove_all(directory);
        fs::create_directories(directory);
        const auto path{directory / "gamepack.toml"};
        expect(write_text(path, first->text));
        auto reparsed{mod::load_gamepack_spec(path.string(), "server")};
        expect(static_cast<bool>(reparsed))
            << (reparsed ? "" : reparsed.error().message);
        if (reparsed) {
            expect(reparsed->mods.size() == profile->mods.size());
            expect(reparsed->providers.size() == first->spec.providers.size());
            auto plan{mod::preview_composition(*reparsed)};
            expect(static_cast<bool>(plan))
                << (plan ? "" : plan.error().message);
        }
        auto checks{app::check_profile_gamepack(*profile, path.string())};
        expect(checks.ok()) << (checks.errors.empty() ? "" : checks.errors[0]);
        fs::remove_all(directory);
    };

    "gamepack gen names a service with several candidate providers"_test = [] {
        const auto directory{fs::temp_directory_path() /
                             "refrain-test-gamepack-ambiguous"};
        fs::remove_all(directory);
        fs::create_directories(directory / "test.provider.one");
        fs::create_directories(directory / "test.provider.two");
        const auto manifest = [](std::string_view id) {
            return std::string{"[mod]\nid = \""} + std::string{id} +
                   "\"\nversion = \"1.0.0\"\nabi = \"1\"\nside = "
                   "\"server\"\n\n[meta]\ndisplay_name = \"" +
                   std::string{id} +
                   "\"\n\n[provides]\nservices = "
                   "[\"refrain:world\"]\n";
        };
        expect(write_text(directory / "test.provider.one" / "manifest.toml",
                          manifest("test.provider.one")));
        expect(write_text(directory / "test.provider.two" / "manifest.toml",
                          manifest("test.provider.two")));
        auto profile{profile_named("ambiguous",
                                   {"test.provider.one", "test.provider.two"})};
        auto generated{app::generate_gamepack(profile, directory.string())};
        expect(!generated);
        if (generated) {
            fs::remove_all(directory);
            return;
        }
        const auto message{generated.error().message};
        expect(message.find("selection") != std::string::npos) << message;
        expect(message.find("refrain:world") != std::string::npos) << message;
        expect(message.find("test.provider.one") != std::string::npos)
            << message;
        expect(message.find("test.provider.two") != std::string::npos)
            << message;
        fs::remove_all(directory);
    };

    "gamepack sync reports and writes pin drift"_test = [] {
        auto profile{
            app::load_mod_profile(source_path("profiles/vanilla.toml"))};
        expect(static_cast<bool>(profile));
        if (!profile) {
            return;
        }
        auto generated{app::generate_gamepack(*profile, source_path("mods"))};
        expect(static_cast<bool>(generated));
        if (!generated) {
            return;
        }
        const auto directory{fs::temp_directory_path() /
                             "refrain-test-gamepack-sync"};
        fs::remove_all(directory);
        fs::create_directories(directory);
        const auto path{directory / "gamepack.toml"};
        expect(write_text(path, generated->text));

        const std::vector<app::installed_mod_version> installed{
            {.id = "refrain.data.vanilla", .version = "1.1.0"}};
        auto plan{app::plan_gamepack_sync(generated->spec, installed)};
        expect(static_cast<bool>(plan));
        if (!plan) {
            fs::remove_all(directory);
            return;
        }
        expect(!plan->empty());
        expect(plan->changes.size() == 2U);
        bool saw_pin{false};
        bool saw_range{false};
        for (const auto &change : plan->changes) {
            saw_pin = saw_pin || change.line.find("pinned 1.0.0, installed "
                                                  "1.1.0") != std::string::npos;
            saw_range = saw_range ||
                        change.line.find("provider data") != std::string::npos;
        }
        expect(saw_pin);
        expect(saw_range);
        expect(app::gamepack_sync_plan{}.empty());

        const auto formatted{plan->format(path.string())};
        expect(formatted.find("is out of date") != std::string::npos);

        auto applied{app::apply_gamepack_sync(path.string(), *plan)};
        expect(static_cast<bool>(applied))
            << (applied ? "" : applied.error().message);
        auto rewritten{refrain::core::io::read_text(path.string())};
        expect(static_cast<bool>(rewritten));
        if (rewritten) {
            expect(rewritten->find("\"refrain.data.vanilla@1.1.0\"") !=
                   std::string::npos);
            expect(rewritten->find("\"refrain.data.vanilla@^1.1.0\"") !=
                   std::string::npos);
            expect(rewritten->find("remains a deployment decision") !=
                   std::string::npos);
        }
        fs::remove_all(directory);
    };

    "gamepack sync on an up-to-date gamepack is a no-op"_test = [] {
        auto profile{
            app::load_mod_profile(source_path("profiles/vanilla.toml"))};
        expect(static_cast<bool>(profile));
        if (!profile) {
            return;
        }
        auto generated{app::generate_gamepack(*profile, source_path("mods"))};
        expect(static_cast<bool>(generated));
        if (!generated) {
            return;
        }
        const std::vector<app::installed_mod_version> current{
            {.id = "refrain.data.vanilla", .version = "1.0.0"}};
        auto plan{app::plan_gamepack_sync(generated->spec, current)};
        expect(static_cast<bool>(plan));
        if (plan) {
            expect(plan->empty());
        }
        const std::vector<app::installed_mod_version> none{};
        auto empty{app::plan_gamepack_sync(generated->spec, none)};
        expect(static_cast<bool>(empty));
        if (empty) {
            expect(empty->empty());
        }
    };

    "gamepack sync refuses a rebase that duplicates one binding"_test = [] {
        mod::gamepack_spec spec{};
        spec.id = "refrain.test";
        spec.version = "1.0.0";
        spec.side = "server";
        spec.abi = 1;
        mod::provider_binding binding{};
        binding.service = "world";
        binding.candidates.push_back(
            mod::mod_requirement{"test.mod", "^1.0.0"});
        binding.candidates.push_back(
            mod::mod_requirement{"test.mod", "^1.1.0"});
        spec.providers.push_back(std::move(binding));

        const std::vector<app::installed_mod_version> installed{
            {.id = "test.mod", .version = "1.2.0"}};
        auto plan{app::plan_gamepack_sync(spec, installed)};
        expect(!plan);
        if (!plan) {
            expect(plan.error().message.find("more than once") !=
                   std::string::npos);
            expect(plan.error().message.find("world") != std::string::npos);
        }
    };

    "gamepack sync leaves dependency ranges and comments untouched"_test = [] {
        const auto directory{fs::temp_directory_path() /
                             "refrain-test-gamepack-sections"};
        fs::remove_all(directory);
        fs::create_directories(directory);
        const auto path{directory / "gamepack.toml"};
        const std::string text{
            "[gamepack]\n"
            "id = \"refrain.sections\"\n"
            "version = \"1.0.0\"\n"
            "side = \"server\"\n"
            "abi = \"1\"\n"
            "auto_scan = false\n\n"
            "# \"test.mod@1.0.0\" in a comment is not a pin\n\n"
            "[mods]\n"
            "list = [\n"
            "    \"test.mod@1.0.0\",\n"
            "]\n\n"
            "[mods.\"test.mod\"]\n"
            "requires = [ \"test.mod@1.0.0\" ]\n\n"
            "[providers]\n"
            "data = \"test.mod@^1.0.0\"\n"};
        expect(write_text(path, text));
        auto spec{mod::load_gamepack_spec(path.string(), "server")};
        expect(static_cast<bool>(spec)) << (spec ? "" : spec.error().message);
        if (!spec) {
            fs::remove_all(directory);
            return;
        }
        const std::vector<app::installed_mod_version> installed{
            {.id = "test.mod", .version = "1.1.0"}};
        auto plan{app::plan_gamepack_sync(*spec, installed)};
        expect(static_cast<bool>(plan));
        if (!plan) {
            fs::remove_all(directory);
            return;
        }
        expect(!plan->empty());
        auto applied{app::apply_gamepack_sync(path.string(), *plan)};
        expect(static_cast<bool>(applied))
            << (applied ? "" : applied.error().message);
        auto rewritten{refrain::core::io::read_text(path.string())};
        expect(static_cast<bool>(rewritten));
        if (rewritten) {
            expect(rewritten->find("# \"test.mod@1.0.0\" in a comment") !=
                   std::string::npos);
            expect(rewritten->find("requires = [ \"test.mod@1.0.0\" ]") !=
                   std::string::npos);
            expect(rewritten->find("\"test.mod@1.1.0\"") != std::string::npos);
            expect(rewritten->find("data = \"test.mod@^1.1.0\"") !=
                   std::string::npos);
        }
        auto again{mod::load_gamepack_spec(path.string(), "server")};
        expect(static_cast<bool>(again));
        if (again) {
            auto second{app::plan_gamepack_sync(*again, installed)};
            expect(second && second->empty());
        }
        fs::remove_all(directory);
    };

    "gamepack sync fails when a token is missing in its section"_test = [] {
        const auto directory{fs::temp_directory_path() /
                             "refrain-test-gamepack-missing-token"};
        fs::remove_all(directory);
        fs::create_directories(directory);
        const auto path{directory / "gamepack.toml"};
        const std::string text{
            "[gamepack]\nid = \"refrain.missing\"\nversion = \"1.0.0\"\n"
            "side = \"server\"\nabi = \"1\"\nauto_scan = false\n\n"
            "[mods]\nlist = [ \"test.mod@1.0.0\" ]\n"};
        expect(write_text(path, text));
        auto spec{mod::load_gamepack_spec(path.string(), "server")};
        expect(static_cast<bool>(spec));
        if (!spec) {
            fs::remove_all(directory);
            return;
        }
        // The file changes after the plan was derived from the loaded spec.
        std::string changed{text};
        const auto pin{changed.find("test.mod@1.0.0") +
                       std::string{"test.mod@"}.size()};
        changed.replace(pin, 5U, "2.0.0");
        expect(write_text(path, changed));
        const std::vector<app::installed_mod_version> installed{
            {.id = "test.mod", .version = "1.1.0"}};
        auto plan{app::plan_gamepack_sync(*spec, installed)};
        expect(static_cast<bool>(plan));
        if (plan) {
            auto applied{app::apply_gamepack_sync(path.string(), *plan)};
            expect(!applied);
            if (!applied) {
                expect(applied.error().code == refrain::error_code::not_found);
                expect(applied.error().message.find("[mods]") !=
                       std::string::npos);
            }
        }
        fs::remove_all(directory);
    };

    "gamepack sync rejects conflicting modes and invalid store roots"_test =
        [] {
            app::gamepack_sync_options options{};
            options.gamepack = "unused.toml";
            options.check = true;
            options.write = true;
            expect(app::run_gamepack_sync(options) == 1);

            const auto directory{fs::temp_directory_path() /
                                 "refrain-test-gamepack-invalid-root"};
            fs::remove_all(directory);
            fs::create_directories(directory);
            const auto file{directory / "not-a-store"};
            expect(write_text(file, "x"));
            const std::vector<std::string> roots{file.string()};
            auto found{app::installed_versions_from_stores(roots)};
            expect(!found);
            if (!found) {
                expect(found.error().message.find("not a directory") !=
                       std::string::npos);
            }
            fs::remove_all(directory);
        };

    "gamepack sync preserves compound provider ranges"_test = [] {
        mod::gamepack_spec spec{};
        spec.id = "refrain.test";
        spec.version = "1.0.0";
        spec.side = "server";
        spec.abi = 1;
        mod::provider_binding binding{};
        binding.service = "data";
        binding.candidates.push_back(
            mod::mod_requirement{"test.mod", ">=1.0.0 <2.0.0"});
        spec.providers.push_back(std::move(binding));
        const std::vector<app::installed_mod_version> installed{
            {.id = "test.mod", .version = "1.1.0"}};
        auto plan{app::plan_gamepack_sync(spec, installed)};
        expect(static_cast<bool>(plan));
        if (plan) {
            expect(plan->empty());
        }
    };

    "gamepack gen rejects two contracts that map to one slot"_test = [] {
        const auto directory{fs::temp_directory_path() /
                             "refrain-test-gamepack-slot-collision"};
        fs::remove_all(directory);
        const auto add_provider = [&directory](std::string_view id,
                                               std::string_view service) {
            const auto mod_dir{directory / std::string{id}};
            fs::create_directories(mod_dir);
            const std::string manifest{
                "[mod]\nid = \"" + std::string{id} +
                "\"\nversion = \"1.0.0\"\nabi = \"1\"\nside = "
                "\"server\"\n\n[provides]\nservices = [\"" +
                std::string{service} + "\"]\n"};
            expect(write_text(mod_dir / "manifest.toml", manifest));
        };
        add_provider("test.provider.data", "refrain:data.packs");
        add_provider("test.provider.other", "refrain:data");
        auto profile{profile_named(
            "slot-collision", {"test.provider.data", "test.provider.other"})};
        auto generated{app::generate_gamepack(profile, directory.string())};
        expect(!generated);
        if (!generated) {
            const auto message{generated.error().message};
            expect(message.find("refrain:data.packs") != std::string::npos)
                << message;
            expect(message.find("refrain:data'") != std::string::npos)
                << message;
            expect(message.find("'data'") != std::string::npos) << message;
        }
        fs::remove_all(directory);
    };

    "gamepack gen --check detects drift without writing"_test = [] {
        const auto directory{fs::temp_directory_path() /
                             "refrain-test-gamepack-gen-check"};
        fs::remove_all(directory);
        fs::create_directories(directory);
        app::gamepack_gen_options options{};
        options.profile = "vanilla";
        options.profiles_dir = source_path("profiles");
        options.mods_dir = source_path("mods");
        options.out = (directory / "gamepack.toml").string();

        options.check = true;
        expect(app::run_gamepack_gen(options) == 1); // missing file
        options.check = false;
        expect(app::run_gamepack_gen(options) == 0);
        options.check = true;
        expect(app::run_gamepack_gen(options) == 0);
        expect(write_text(options.out,
                          refrain::core::io::read_text(options.out).value() +
                              "# drift\n"));
        expect(app::run_gamepack_gen(options) == 1);
        auto current{refrain::core::io::read_text(options.out)};
        expect(static_cast<bool>(current));
        if (current) {
            expect(current->find("# drift") != std::string::npos);
        }
        fs::remove_all(directory);
    };

    "gamepack sync --write refreshes the lock"_test = [] {
        const auto directory{fs::temp_directory_path() /
                             "refrain-test-gamepack-write-lock"};
        fs::remove_all(directory);
        fs::create_directories(directory);
        const auto path{directory / "gamepack.toml"};
        expect(write_text(
            path,
            "[gamepack]\nid = \"refrain.test.empty\"\nversion = \"1.0.0\"\n"
            "side = \"server\"\nabi = \"1\"\nauto_scan = false\n\n"
            "[mods]\nlist = []\n"));
        app::gamepack_sync_options options{};
        options.gamepack = path.string();
        options.write = true;
        expect(app::run_gamepack_sync(options) == 0);
        expect(refrain::core::io::exists((directory / "mods.lock").string()));
        fs::remove_all(directory);
    };
};

} // namespace
