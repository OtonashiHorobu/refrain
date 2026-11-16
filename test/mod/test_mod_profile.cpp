// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Build-time mod profiles: profile parsing and validation, the image/source
// consistency checks, the build identity recorded in mods.lock and the
// descriptor of a dynamically built first-party module.

#include <refrain/app/dynamic_modules.hpp>
#include <refrain/app/mod_profile.hpp>
#include <refrain/app/services.hpp>

#include <refrain/core/version.hpp>
#include <refrain/data/builtin_image.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/mod/discovery.hpp>

#include <boost/ut.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace boost::ut;

namespace app = refrain::app;
namespace data = refrain::data;
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

auto read_image(const fs::path &path) -> std::vector<std::byte> {
    std::ifstream stream{path, std::ios::binary};
    std::vector<std::byte> bytes{};
    char buffer[4096];
    while (stream) {
        stream.read(buffer, sizeof(buffer));
        const auto count{stream.gcount()};
        for (std::streamsize index{0}; index < count; ++index) {
            bytes.push_back(static_cast<std::byte>(
                static_cast<unsigned char>(buffer[index])));
        }
    }
    return bytes;
}

const suite mod_profile_tests = [] {
    "profile/vanilla declares the full first-party set"_test = [] {
        auto profile{
            app::load_mod_profile(source_path("profiles/vanilla.toml"))};
        expect(static_cast<bool>(profile)) << profile.error().message;
        if (!profile) {
            return;
        }
        expect(profile->name == "vanilla");
        expect(profile->mods.size() == 12U);
        expect(profile->dynamic.empty());
        expect(profile->gamepack == "gamepack/gamepack.toml");
        expect(profile->link == "static");
        expect(fs::path{profile->gamepack_path}.filename() == "gamepack.toml");
        auto checks =
            app::check_profile_mods_tree(*profile, source_path("mods"));
        expect(checks.ok()) << (checks.errors.empty() ? "" : checks.errors[0]);
        auto gamepack{app::check_profile_gamepack(*profile, "")};
        expect(gamepack.ok())
            << (gamepack.errors.empty() ? "" : gamepack.errors[0]);
    };

    "profile/minimal is closed and matches its default gamepack"_test = [] {
        auto profile{
            app::load_mod_profile(source_path("profiles/minimal.toml"))};
        expect(static_cast<bool>(profile)) << profile.error().message;
        if (!profile) {
            return;
        }
        expect(profile->mods.size() == 2U);
        expect(profile->gamepack == "gamepack/presets/data-only.toml");
        expect(
            app::check_profile_mods_tree(*profile, source_path("mods")).ok());
        expect(app::check_profile_gamepack(*profile, "").ok());
    };

    "profile/unknown mod fails with the profile name"_test = [] {
        app::mod_profile_spec profile{};
        profile.name = "test-profile";
        profile.mods = {"refrain.content.vanilla", "refrain.test.missing"};
        profile.gamepack = "gamepack/gamepack.toml";
        auto checks =
            app::check_profile_mods_tree(profile, source_path("mods"));
        expect(!checks.ok());
        expect(!checks.errors.empty());
        if (!checks.errors.empty()) {
            expect(checks.errors[0].find("refrain.test.missing") !=
                   std::string::npos);
            expect(checks.errors[0].find("test-profile") != std::string::npos);
        }
    };

    "profile/dependency outside the profile is reported"_test = [] {
        // content + data is closed; dropping data makes content's dependent
        // (data.vanilla is not built) visible only from the tree walk, so use
        // data alone: it requires content, which is absent from this profile.
        app::mod_profile_spec profile{};
        profile.name = "broken-closure";
        profile.mods = {"refrain.data.vanilla"};
        auto checks =
            app::check_profile_mods_tree(profile, source_path("mods"));
        expect(!checks.ok());
        expect(!checks.errors.empty());
        if (!checks.errors.empty()) {
            expect(checks.errors[0].find("refrain.content.vanilla") !=
                   std::string::npos);
        }
    };

    "profile/dynamic list must be a subset of mods"_test = [] {
        auto directory = fs::temp_directory_path();
        const auto path{directory / "refrain-test-profile-dynamic.toml"};
        expect(write_text(path, "name = \"dynamic-subset\"\n"
                                "gamepack = \"gamepack/gamepack.toml\"\n"
                                "mods = [\"refrain.data.vanilla\"]\n"
                                "dynamic = [\"refrain.world.vanilla\"]\n"));
        auto profile{app::load_mod_profile(path.string())};
        expect(!profile);
        if (!profile) {
            expect(profile.error().message.find("refrain.world.vanilla") !=
                   std::string::npos);
        }
        fs::remove(path);
    };

    "profile/name must match the file stem under profiles/"_test = [] {
        const auto directory{fs::temp_directory_path() /
                             "refrain-test-profiles"};
        fs::create_directories(directory);
        const auto path{directory / "actual-name.toml"};
        expect(write_text(path, "name = \"different-name\"\n"
                                "gamepack = \"gamepack/gamepack.toml\"\n"
                                "mods = [\"refrain.data.vanilla\"]\n"));
        auto profile{app::load_mod_profile(path.string())};
        // The rule applies to files inside a `profiles/` directory only; a
        // temp file outside one is accepted as an explicit path.
        expect(static_cast<bool>(profile));
        const auto inside{directory / "profiles"};
        fs::create_directories(inside);
        const auto inside_path{inside / "actual-name.toml"};
        expect(write_text(inside_path, "name = \"different-name\"\n"
                                       "gamepack = \"gamepack/gamepack.toml\"\n"
                                       "mods = [\"refrain.data.vanilla\"]\n"));
        auto mismatch{app::load_mod_profile(inside_path.string())};
        expect(!mismatch);
        fs::remove_all(directory);
    };

    "profile/gamepack pins outside the profile name both sides"_test = [] {
        app::mod_profile_spec profile{};
        profile.name = "content-only";
        profile.mods = {"refrain.content.vanilla"};
        auto checks{app::check_profile_gamepack(
            profile, source_path("gamepack/presets/data-only.toml"))};
        expect(!checks.ok());
        bool found{false};
        for (const auto &failure : checks.errors) {
            found =
                found ||
                (failure.find("refrain.data.vanilla") != std::string::npos &&
                 failure.find("content-only") != std::string::npos);
        }
        expect(found);
    };

    "profile/matches the binary it was built with"_test = [] {
        auto profile{app::load_mod_profile_named(refrain::version::mod_profile,
                                                 source_path("profiles"))};
        expect(static_cast<bool>(profile)) << profile.error().message;
        if (!profile) {
            return;
        }
        auto checks{app::check_profile_against_binary(*profile)};
        expect(checks.ok()) << (checks.errors.empty() ? "" : checks.errors[0]);

        const std::string_view other{
            std::string_view{refrain::version::mod_profile} == "minimal"
                ? "vanilla"
                : "minimal"};
        auto foreign{
            app::load_mod_profile_named(other, source_path("profiles"))};
        expect(static_cast<bool>(foreign));
        if (foreign) {
            auto mismatch{app::check_profile_against_binary(*foreign)};
            expect(!mismatch.ok());
            expect(!mismatch.errors.empty());
            if (!mismatch.errors.empty()) {
                expect(mismatch.errors[0].find(refrain::version::mod_profile) !=
                       std::string::npos);
            }
        }
    };

    "profile/unbuilt ids are listed with the build profile"_test = [] {
        const std::vector<std::string> built_ids{"refrain.data.vanilla"};
        expect(app::unbuilt_mod_ids(built_ids).empty());
        const std::vector<std::string> mixed_ids{"refrain.data.vanilla",
                                                 "refrain.test.not-built"};
        auto missing{app::unbuilt_mod_ids(mixed_ids)};
        expect(missing.size() == 1U);
        if (!missing.empty()) {
            expect(missing[0] == "refrain.test.not-built");
            const auto message{app::format_unbuilt_mod_error(
                "gamepack 'x'", std::span<const std::string>{missing})};
            expect(message.find("refrain.test.not-built") != std::string::npos);
            expect(message.find(refrain::version::mod_profile) !=
                   std::string::npos);
            expect(message.find(refrain::version::mod_build_id) !=
                   std::string::npos);
        }
    };

    "profile/builtin image packs exactly the selected mods"_test = [] {
        const auto out{fs::temp_directory_path() /
                       "refrain-test-builtin-selected.image"};
        app::builtin_image_options options{};
        options.mods_dir = source_path("mods");
        options.out = out.string();
        options.mods = {"refrain.content.vanilla"};
        expect(app::run_builtin_image(options) == 0);

        auto bytes{read_image(out)};
        expect(!bytes.empty());
        auto image{data::builtin_image::open(
            std::span<const std::byte>{bytes.data(), bytes.size()})};
        expect(static_cast<bool>(image)) << image.error().message;
        if (!image) {
            return;
        }
        expect(image->find("mods.index.toml") != nullptr);
        expect(image->find("mods/refrain.content.vanilla/manifest.toml") !=
               nullptr);
        expect(image->find("mods/refrain.data.vanilla/manifest.toml") ==
               nullptr);
        expect(image->entries().size() == 2U);

        // The profile-based selection produces the same image as the explicit
        // list with the same ids, which is the consistency contract between
        // CMake's custom command and the tools.
        const auto explicit_out{fs::temp_directory_path() /
                                "refrain-test-builtin-explicit.image"};
        app::builtin_image_options explicit_options{};
        explicit_options.mods_dir = source_path("mods");
        explicit_options.out = explicit_out.string();
        explicit_options.mods = {"refrain.content.vanilla",
                                 "refrain.data.vanilla"};
        expect(app::run_builtin_image(explicit_options) == 0);
        const auto profile_out{fs::temp_directory_path() /
                               "refrain-test-builtin-profile.image"};
        app::builtin_image_options profile_options{};
        profile_options.mods_dir = source_path("mods");
        profile_options.out = profile_out.string();
        profile_options.profile = "minimal";
        profile_options.profiles_dir = source_path("profiles");
        expect(app::run_builtin_image(profile_options) == 0);
        expect(static_cast<bool>(read_image(explicit_out) ==
                                 read_image(profile_out)));
        fs::remove(out);
        fs::remove(explicit_out);
        fs::remove(profile_out);
    };

    "profile/builtin image rejects a selected mod without a manifest"_test =
        [] {
            app::builtin_image_options options{};
            options.mods_dir = source_path("mods");
            options.out = (fs::temp_directory_path() /
                           "refrain-test-builtin-missing.image")
                              .string();
            options.mods = {"refrain.test.not-built"};
            expect(app::run_builtin_image(options) != 0);
            fs::remove(options.out);
        };

    "profile/image check finds a profile/image mismatch"_test = [] {
        const auto image_path{fs::temp_directory_path() /
                              "refrain-test-profile-image.image"};
        app::builtin_image_options options{};
        options.mods_dir = source_path("mods");
        options.out = image_path.string();
        options.mods = {"refrain.content.vanilla"};
        expect(app::run_builtin_image(options) == 0);

        app::mod_profile_spec profile{};
        profile.name = "content-and-data";
        profile.mods = {"refrain.content.vanilla", "refrain.data.vanilla"};
        auto checks{app::check_profile_image(profile, image_path.string())};
        expect(!checks.ok());
        bool missing_data{false};
        for (const auto &failure : checks.errors) {
            missing_data =
                missing_data ||
                failure.find("refrain.data.vanilla") != std::string::npos;
        }
        expect(missing_data);

        profile.mods = {"refrain.content.vanilla"};
        expect(app::check_profile_image(profile, image_path.string()).ok());
        fs::remove(image_path);
    };

    "profile/mods.lock records and validates the build identity"_test = [] {
        mod::mod_lock lock{};
        lock.gamepack_id = "refrain.vanilla";
        lock.gamepack_version = "26.3.0";
        lock.profile = refrain::version::mod_profile;
        lock.build_id = refrain::version::mod_build_id;
        const auto text{lock.to_toml()};
        expect(text.find("profile = \"" +
                         std::string{refrain::version::mod_profile} + "\"") !=
               std::string::npos);
        expect(text.find("build_id = \"" +
                         std::string{refrain::version::mod_build_id} + "\"") !=
               std::string::npos);

        expect(app::check_lock_profile(lock).has_value());
        auto mixed{lock};
        mixed.profile = "some-other-profile";
        auto refused{app::check_lock_profile(mixed)};
        expect(!refused);
        if (!refused) {
            expect(refused.error().message.find("some-other-profile") !=
                   std::string::npos);
            expect(refused.error().message.find(
                       refrain::version::mod_profile) != std::string::npos);
        }
        auto stale_build{lock};
        stale_build.build_id = "0000000000000000";
        expect(!app::check_lock_profile(stale_build));

        // Older locks carry no identity and stay accepted.
        mod::mod_lock legacy{};
        legacy.gamepack_id = "refrain.vanilla";
        legacy.gamepack_version = "26.3.0";
        expect(app::check_lock_profile(legacy).has_value());
        expect(legacy.to_toml().find("profile = ") == std::string::npos);
    };

    "profile/dynamic module exports a loadable descriptor"_test = [] {
        auto module{app::load_dynamic_module(REFRAIN_TEST_DYNAMIC_MOD)};
        expect(static_cast<bool>(module)) << module.error().message;
        if (!module) {
            return;
        }
        expect(module->id == "refrain.test.dynamic");
        expect(module->descriptor.version == "1.0.0");
        expect(module->descriptor.abi == 1U);
        expect(module->descriptor.factory != nullptr);
        expect(module->library.has_value());
        expect(module->library->valid());
    };
};

} // namespace
