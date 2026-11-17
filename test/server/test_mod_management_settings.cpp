// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Mod management configuration: the install/trust policy reused from the
// startup bootstrap and the hot lifecycle gates of the command surface.

#include <refrain/server/mod_management_settings.hpp>

#include <refrain/core/io.hpp>

#include <boost/ut.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace fs = std::filesystem;
namespace server = refrain::server;

[[nodiscard]] auto scratch_directory(std::string_view name) -> fs::path {
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    auto path{fs::temp_directory_path() /
              ("refrain-mod-settings-" + std::to_string(stamp) + "-" +
               std::string{name})};
    std::error_code error{};
    fs::create_directories(path, error);
    return path;
}

[[nodiscard]] auto write_file(const fs::path &directory, std::string_view name,
                              std::string_view text) -> bool {
    return static_cast<bool>(refrain::core::io::write_text(
        (directory / fs::path{std::string{name}}).string(), text));
}

} // namespace

const suite mod_management_settings_tests = [] {
    "mod settings/missing directory keeps the production defaults"_test = [] {
        const auto directory{scratch_directory("defaults")};
        auto settings{server::load_mod_management_settings(directory.string())};
        expect(static_cast<bool>(settings)) << "load failed";
        if (settings) {
            expect(settings->policy.require_signature);
            expect(settings->policy.signature_policy ==
                   refrain::mod::rfmod_signature_policy::strict);
            expect(!settings->hot_install);
            expect(!settings->hot_reload);
            expect(!settings->hot_reload_data);
            expect(settings->policy_source.empty());
            expect(settings->admin_source.empty());
        }
        std::error_code error;
        fs::remove_all(directory, error);
    };

    "mod settings/reads the policy and the hot gates"_test = [] {
        const auto directory{scratch_directory("gates")};
        expect(write_file(directory, "mods.toml",
                          "[signature]\n"
                          "require = false\n"
                          "policy = \"trusted-any\"\n"
                          "\n[install]\nallow_downgrade = true\n"));
        expect(write_file(directory, "mods_admin.toml",
                          "[lifecycle]\n"
                          "hot_install = true\n"
                          "hot_reload = true\n"
                          "hot_reload_data = true\n"));
        auto settings{server::load_mod_management_settings(directory.string())};
        expect(static_cast<bool>(settings)) << "load failed";
        if (settings) {
            expect(!settings->policy.require_signature);
            expect(settings->policy.signature_policy ==
                   refrain::mod::rfmod_signature_policy::trusted_any);
            expect(settings->policy.allow_downgrade);
            expect(settings->hot_install);
            expect(settings->hot_reload);
            expect(settings->hot_reload_data);
            expect(settings->policy_source.find("mods.toml") !=
                   std::string::npos);
            expect(settings->admin_source.find("mods_admin.toml") !=
                   std::string::npos);
        }
        std::error_code error;
        fs::remove_all(directory, error);
    };

    "mod settings/a gate of the wrong type is an error"_test = [] {
        const auto directory{scratch_directory("gate-type")};
        expect(write_file(directory, "mods_admin.toml",
                          "[lifecycle]\nhot_install = \"yes\"\n"));
        auto settings{server::load_mod_management_settings(directory.string())};
        expect(!settings);
        if (!settings) {
            expect(settings.error().code == refrain::error_code::parse_error);
            expect(settings.error().message.find("hot_install") !=
                   std::string::npos)
                << settings.error().message;
        }
        std::error_code error;
        fs::remove_all(directory, error);
    };

    "mod settings/the content reload gate of the wrong type is an error"_test =
        [] {
            const auto directory{scratch_directory("data-gate-type")};
            expect(write_file(directory, "mods_admin.toml",
                              "[lifecycle]\nhot_reload_data = 1\n"));
            auto settings{
                server::load_mod_management_settings(directory.string())};
            expect(!settings);
            if (!settings) {
                expect(settings.error().code ==
                       refrain::error_code::parse_error);
                expect(settings.error().message.find("hot_reload_data") !=
                       std::string::npos)
                    << settings.error().message;
            }
            std::error_code error;
            fs::remove_all(directory, error);
        };

    "mod settings/the legacy admin table is ignored without granting anyone"_test =
        [] {
            // The operator allowlist is no longer a configuration of this
            // file: `/mods` is gated by the `refrain.mods.admin` permission
            // node. An old file must load for its lifecycle gates and must not
            // turn its list into an effective grant, whatever it contains.
            const auto directory{scratch_directory("legacy-admin")};
            expect(write_file(directory, "mods_admin.toml",
                              "[lifecycle]\n"
                              "hot_reload = true\n"
                              "\n[admin]\n"
                              "operators = [\"not-a-uuid\"]\n"));
            auto settings{
                server::load_mod_management_settings(directory.string())};
            expect(static_cast<bool>(settings)) << "load failed";
            if (settings) {
                expect(settings->hot_reload);
                expect(settings->admin_source.find("mods_admin.toml") !=
                       std::string::npos);
            }
            std::error_code error;
            fs::remove_all(directory, error);
        };

    "mod settings/a malformed admin file is an error"_test = [] {
        const auto directory{scratch_directory("malformed")};
        expect(write_file(directory, "mods_admin.toml", "[lifecycle\n"));
        auto settings{server::load_mod_management_settings(directory.string())};
        expect(!settings);
        std::error_code error;
        fs::remove_all(directory, error);
    };

    "mod settings/a malformed policy file is an error"_test = [] {
        const auto directory{scratch_directory("policy")};
        expect(write_file(directory, "mods.toml",
                          "[signature]\nunknown_key = true\n"));
        auto settings{server::load_mod_management_settings(directory.string())};
        expect(!settings);
        if (!settings) {
            expect(settings.error().message.find("mods.toml") !=
                   std::string::npos)
                << settings.error().message;
        }
        std::error_code error;
        fs::remove_all(directory, error);
    };

    "mod settings/an empty configuration directory means defaults"_test = [] {
        auto settings{server::load_mod_management_settings({})};
        expect(static_cast<bool>(settings));
        if (settings) {
            expect(settings->policy.require_signature);
            expect(!settings->hot_install);
            expect(!settings->hot_reload_data);
        }
    };
};
