// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/server_properties.hpp>

#include <refrain/core/config.hpp>
#include <refrain/core/io.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fmt/format.h>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace refrain::server {

namespace {

constexpr std::array<std::string_view, 15> importable_keys{
    "server-ip",
    "server-port",
    "motd",
    "max-players",
    "enable-status",
    "view-distance",
    "simulation-distance",
    "online-mode",
    "network-compression-threshold",
    "enforce-secure-profile",
    "prevent-proxy-connections",
    "rate-limit",
    "player-idle-timeout",
    "level-name",
    "allow-nether",
};

struct location {
    std::string_view name_space;
    std::string_view key;
};

[[nodiscard]] auto location_of(std::string_view property) -> location {
    if (property == "level-name") {
        return {"world", "level_name"};
    }
    if (property == "allow-nether") {
        return {"world", "allow_nether"};
    }
    if (property == "server-ip") {
        return {"network", "bind_host"};
    }
    if (property == "server-port") {
        return {"network", "port"};
    }
    if (property == "motd") {
        return {"network", "status.motd"};
    }
    if (property == "max-players") {
        return {"network", "status.max_players"};
    }
    if (property == "enable-status") {
        return {"network", "status.enabled"};
    }
    return {"network", property};
}

[[nodiscard]] auto apply_value(core::config::store &store,
                               std::string_view property,
                               const server_properties &properties,
                               core::config::layer target) -> status {
    if (property == "server-ip") {
        return store.set_string("network", "bind_host", properties.server_ip,
                                target);
    }
    if (property == "server-port") {
        return store.set_int64("network", "port", properties.server_port,
                               target);
    }
    if (property == "motd") {
        return store.set_string("network", "status.motd", properties.motd,
                                target);
    }
    if (property == "max-players") {
        return store.set_int64("network", "status.max_players",
                               properties.max_players, target);
    }
    if (property == "enable-status") {
        return store.set_bool("network", "status.enabled",
                              properties.enable_status, target);
    }
    if (property == "view-distance") {
        return store.set_int64("network", "view_distance",
                               properties.view_distance, target);
    }
    if (property == "simulation-distance") {
        return store.set_int64("network", "simulation_distance",
                               properties.simulation_distance, target);
    }
    if (property == "online-mode") {
        return store.set_bool("network", "online_mode", properties.online_mode,
                              target);
    }
    if (property == "network-compression-threshold") {
        return store.set_int64("network", "compression_threshold",
                               properties.network_compression_threshold,
                               target);
    }
    if (property == "enforce-secure-profile") {
        return store.set_bool("network", "enforce_secure_profile",
                              properties.enforce_secure_profile, target);
    }
    if (property == "prevent-proxy-connections") {
        return store.set_bool("network", "prevent_proxy_connections",
                              properties.prevent_proxy_connections, target);
    }
    if (property == "rate-limit") {
        return store.set_int64("network", "rate_limit", properties.rate_limit,
                               target);
    }
    if (property == "player-idle-timeout") {
        return store.set_int64("network", "player_idle_timeout",
                               properties.player_idle_timeout, target);
    }
    if (property == "level-name") {
        return store.set_string("world", "level_name", properties.level_name,
                                target);
    }
    if (property == "allow-nether") {
        return store.set_bool("world", "allow_nether", properties.allow_nether,
                              target);
    }
    return std::unexpected{
        make_error(error_code::invalid_argument,
                   "server.properties: unexpected importable key '" +
                       std::string{property} + "'")};
}

} // namespace

auto apply_server_properties(std::string_view text, core::config::store &store,
                             core::config::layer target, bool strict)
    -> result<server_properties_import_report> {
    auto present_keys{std::vector<std::string>{}};
    auto properties{parse_server_properties(text, &present_keys)};
    if (!properties) {
        return std::unexpected{std::move(properties.error())};
    }
    server_properties_import_report report{};
    for (const auto &property : present_keys) {
        if (std::ranges::find(importable_keys, property) ==
            importable_keys.end()) {
            report.unknown_keys.push_back(property);
            continue;
        }
        auto status{apply_value(store, property, *properties, target)};
        if (!status) {
            return std::unexpected{std::move(status.error())};
        }
        ++report.applied;
    }
    if (strict && !report.unknown_keys.empty()) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "server.properties: unknown key '" +
                                              report.unknown_keys.front() +
                                              "'")};
    }
    return report;
}

auto import_server_properties(std::string_view text,
                              std::string_view config_dir,
                              server_properties_import_mode mode, bool strict)
    -> result<server_properties_import_report> {
    if (config_dir.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "server.properties: empty configuration directory")};
    }
    auto present_keys{std::vector<std::string>{}};
    auto properties{parse_server_properties(text, &present_keys)};
    if (!properties) {
        return std::unexpected{std::move(properties.error())};
    }

    std::error_code error;
    const std::filesystem::path directory{config_dir};
    std::filesystem::create_directories(directory, error);
    if (error) {
        return std::unexpected{make_error(error_code::io_error,
                                          "server.properties: cannot create '" +
                                              directory.string() +
                                              "': " + error.message())};
    }

    const auto network_path{(directory / "network.toml").string()};
    const auto world_path{(directory / "world.toml").string()};
    auto store{core::config::store::empty()};
    if (auto status{store.mount(
            {.name = "network", .path = network_path, .defaults_path = {}})};
        !status) {
        return std::unexpected{std::move(status.error())};
    }
    if (auto status{store.mount(
            {.name = "world", .path = world_path, .defaults_path = {}})};
        !status) {
        return std::unexpected{std::move(status.error())};
    }
    if (auto status{store.load_all()}; !status) {
        return std::unexpected{std::move(status.error())};
    }

    server_properties_import_report report{};
    for (const auto &property : present_keys) {
        if (std::ranges::find(importable_keys, property) ==
            importable_keys.end()) {
            report.unknown_keys.push_back(property);
            continue;
        }
        const auto [name_space, key]{location_of(property)};
        auto module{store.module(name_space)};
        if (!module) {
            return std::unexpected{std::move(module.error())};
        }
        if (mode == server_properties_import_mode::merge &&
            module->contains(key)) {
            continue;
        }
        if (auto status{apply_value(store, property, *properties,
                                    core::config::layer::user)};
            !status) {
            return std::unexpected{std::move(status.error())};
        }
        ++report.applied;
    }

    if (strict && !report.unknown_keys.empty()) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "server.properties: unknown key '" +
                                              report.unknown_keys.front() +
                                              "'")};
    }

    if (store.dirty("network")) {
        if (auto status{store.save("network")}; !status) {
            return std::unexpected{std::move(status.error())};
        }
        report.written_files.push_back(network_path);
    }
    if (store.dirty("world")) {
        if (auto status{store.save("world")}; !status) {
            return std::unexpected{std::move(status.error())};
        }
        report.written_files.push_back(world_path);
    }
    return report;
}

} // namespace refrain::server
