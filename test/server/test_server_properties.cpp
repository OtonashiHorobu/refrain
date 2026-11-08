// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/config.hpp>
#include <refrain/core/io.hpp>
#include <refrain/server/server_properties.hpp>

#include <boost/ut.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {
using namespace boost::ut;

[[nodiscard]] auto make_temp_dir() -> std::filesystem::path {
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    const auto path{std::filesystem::temp_directory_path() /
                    ("refrain_properties_import_" + std::to_string(stamp))};
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return path;
}

void write_text_file(const std::filesystem::path &path, std::string_view text) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    stream << text;
}

[[maybe_unused]] const suite tests = [] {
    "server-properties/defaults for empty input"_test = [] {
        const auto properties{refrain::server::parse_server_properties("")};
        expect(properties.has_value());
        if (properties) {
            expect(properties->level_name == "world");
            expect(properties->max_players == 20);
            expect(properties->view_distance == 10);
            expect(properties->online_mode);
        }
    };

    "server-properties/accepts =, :, comments and CRLF"_test = [] {
        constexpr std::string_view text{"# comment\r\n"
                                        "! another comment\r\n"
                                        "level-name: my-world\r\n"
                                        "max-players=42\r\n"
                                        "view-distance = 8\r\n"
                                        "simulation-distance:6\r\n"
                                        "online-mode=false\r\n"
                                        "allow-nether=false\r\n"
                                        "motd=Hello\\ World\\nSecond line\r\n"
                                        "unknown-key=ignored\r\n"};
        const auto properties{refrain::server::parse_server_properties(text)};
        expect(properties.has_value()) << "parse failed";
        if (properties) {
            expect(properties->level_name == "my-world");
            expect(properties->max_players == 42);
            expect(properties->view_distance == 8);
            expect(properties->simulation_distance == 6);
            expect(!properties->online_mode);
            expect(!properties->allow_nether);
            expect(properties->motd == "Hello World\nSecond line");
        }
    };

    "server-properties/presence records known and unknown keys"_test = [] {
        std::vector<std::string> keys;
        const auto properties{refrain::server::parse_server_properties(
            "motd=Hello\nunknown-key=1\n", &keys)};
        expect(properties.has_value());
        expect(keys == (std::vector<std::string>{"motd", "unknown-key"}));
    };

    "server-properties/import writes per-namespace TOML"_test = [] {
        const auto dir{make_temp_dir()};
        constexpr std::string_view text{"server-ip=0.0.0.0\n"
                                        "server-port=25566\n"
                                        "motd=Imported\n"
                                        "max-players=30\n"
                                        "enable-status=false\n"
                                        "view-distance=8\n"
                                        "simulation-distance=6\n"
                                        "online-mode=false\n"
                                        "network-compression-threshold=-1\n"
                                        "enforce-secure-profile=false\n"
                                        "prevent-proxy-connections=true\n"
                                        "rate-limit=10\n"
                                        "player-idle-timeout=60\n"
                                        "level-name=imported-world\n"
                                        "allow-nether=false\n"
                                        "unknown-key=ignored\n"};
        auto report{refrain::server::import_server_properties(
            text, dir.string(),
            refrain::server::server_properties_import_mode::merge, false)};
        expect(static_cast<bool>(report));
        if (report) {
            expect(report->applied == 15);
            expect(report->unknown_keys.size() == 1);
            expect(report->written_files.size() == 2);
        }

        const auto network{refrain::core::config::document::parse_file(
            (dir / "network.toml").string())};
        expect(static_cast<bool>(network));
        if (network) {
            expect(network->get_string("bind_host", "") == "0.0.0.0");
            expect(network->get_int64("port", 0) == 25566);
            expect(network->get_string("status.motd", "") == "Imported");
            expect(network->get_int64("status.max_players", 0) == 30);
            expect(!network->get_bool("status.enabled", true));
            expect(network->get_int64("view_distance", 0) == 8);
            expect(network->get_int64("simulation_distance", 0) == 6);
            expect(!network->get_bool("online_mode", true));
            expect(network->get_int64("compression_threshold", 0) == -1);
            expect(!network->get_bool("enforce_secure_profile", true));
            expect(network->get_bool("prevent_proxy_connections", false));
            expect(network->get_int64("rate_limit", 0) == 10);
            expect(network->get_int64("player_idle_timeout", 0) == 60);
        }
        const auto world{refrain::core::config::document::parse_file(
            (dir / "world.toml").string())};
        expect(static_cast<bool>(world));
        if (world) {
            expect(world->get_string("level_name", "") == "imported-world");
            expect(!world->get_bool("allow_nether", true));
        }
        std::error_code error;
        std::filesystem::remove_all(dir, error);
    };

    "server-properties/import merge keeps existing keys, force overwrites"_test =
        [] {
            const auto dir{make_temp_dir()};
            write_text_file(dir / "network.toml",
                            "port = 1\nstatus.motd = \"kept\"\n");
            auto merged{refrain::server::import_server_properties(
                "server-port=25565\nmotd=New\n", dir.string(),
                refrain::server::server_properties_import_mode::merge, false)};
            expect(static_cast<bool>(merged));
            auto merged_document{refrain::core::config::document::parse_file(
                (dir / "network.toml").string())};
            expect(static_cast<bool>(merged_document));
            if (merged_document) {
                expect(merged_document->get_int64("port", 0) == 1);
                expect(merged_document->get_string("status.motd", "") ==
                       "kept");
            }

            auto forced{refrain::server::import_server_properties(
                "server-port=25565\nmotd=New\n", dir.string(),
                refrain::server::server_properties_import_mode::force, false)};
            expect(static_cast<bool>(forced));
            auto forced_document{refrain::core::config::document::parse_file(
                (dir / "network.toml").string())};
            expect(static_cast<bool>(forced_document));
            if (forced_document) {
                expect(forced_document->get_int64("port", 0) == 25565);
                expect(forced_document->get_string("status.motd", "") == "New");
            }
            std::error_code error;
            std::filesystem::remove_all(dir, error);
        };

    "server-properties/apply installs a non-persisted fallback layer"_test =
        [] {
            const auto dir{make_temp_dir()};
            auto store{refrain::core::config::store::empty()};
            expect(static_cast<bool>(store.mount(
                {.name = "network", .path = (dir / "network.toml").string()})));
            expect(static_cast<bool>(store.mount(
                {.name = "world", .path = (dir / "world.toml").string()})));
            expect(static_cast<bool>(store.load_all()));
            auto report{refrain::server::apply_server_properties(
                "server-port=25565\nmotd=Imported\n", store,
                refrain::core::config::layer::fallback, false)};
            expect(static_cast<bool>(report));
            if (report) {
                expect(report->applied == 2);
            }
            auto network{store.module("network")};
            expect(static_cast<bool>(network));
            if (network) {
                expect(network->get_int64("port", 0) == 25565);
                expect(network->get_string("status.motd", "") == "Imported");
            }
            // Applying a fallback does not create files.
            expect(!refrain::core::io::exists((dir / "network.toml").string()));
            std::error_code error;
            std::filesystem::remove_all(dir, error);
        };

    "server-properties/import strict rejects unknown keys"_test = [] {
        const auto dir{make_temp_dir()};
        auto report{refrain::server::import_server_properties(
            "motd=Hello\nunknown-key=1\n", dir.string(),
            refrain::server::server_properties_import_mode::merge, true)};
        expect(!report);
        expect(report.error().code == refrain::error_code::invalid_argument);
        std::error_code error;
        std::filesystem::remove_all(dir, error);
    };

    "server-properties/rejects malformed assignments"_test = [] {
        expect(!refrain::server::parse_server_properties("just-a-key"));
        expect(!refrain::server::parse_server_properties("max-players=many"));
        expect(!refrain::server::parse_server_properties("online-mode=maybe"));
        expect(!refrain::server::parse_server_properties("=value"));
    };
};

} // namespace
