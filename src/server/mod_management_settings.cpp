// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/mod_management_settings.hpp>

#include <refrain/core/config.hpp>
#include <refrain/core/io.hpp>
#include <refrain/core/log.hpp>
#include <refrain/data/path.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr std::string_view policy_filename{"mods.toml"};
constexpr std::string_view admin_filename{"mods_admin.toml"};

constexpr refrain::log::logger log_mods{"mods"};

/// Read an optional boolean key. A missing key keeps the safe default (off);
/// an existing key of the wrong type is a configuration error, because a typo
/// must not silently enable a hot path.
[[nodiscard]] auto read_gate(const refrain::core::config::document &document,
                             std::span<const std::string_view> path,
                             std::string_view name, std::string_view file,
                             bool &out) -> refrain::status {
    using refrain::core::config::value_kind;
    const auto kind{document.kind_at(path)};
    if (kind == value_kind::missing) {
        return refrain::status_ok();
    }
    if (kind != value_kind::boolean) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::parse_error,
            fmt::format("{}: {} must be true or false", file, name))};
    }
    out = document.get_bool_at(path).value_or(out);
    return refrain::status_ok();
}

} // namespace

namespace refrain::server {

auto load_mod_management_settings(std::string_view config_dir)
    -> result<mod_management_settings> {
    mod_management_settings settings{};
    if (config_dir.empty()) {
        return settings;
    }

    // Install/trust policy: the same parser the startup content bootstrap
    // uses, so a runtime install and the next start agree on what is
    // acceptable. A missing file keeps the production default (strict
    // continuity, a trusted signature required).
    const auto policy_path{
        refrain::data::join_path(config_dir, policy_filename)};
    if (refrain::core::io::exists(policy_path)) {
        auto policy{refrain::mod::load_install_policy(policy_path)};
        if (!policy) {
            return std::unexpected{policy.error()};
        }
        settings.policy = std::move(*policy);
        settings.policy_source = policy_path;
    }

    // Hot gates. A missing file keeps every gate off; who may run `/mods` is
    // decided by the `refrain.mods.admin` permission node, not by this file.
    const auto admin_path{refrain::data::join_path(config_dir, admin_filename)};
    if (!refrain::core::io::exists(admin_path)) {
        return settings;
    }
    auto document{refrain::core::config::document::parse_file(admin_path)};
    if (!document) {
        return std::unexpected{document.error()};
    }
    constexpr std::array<std::string_view, 2> hot_install_key{"lifecycle",
                                                              "hot_install"};
    constexpr std::array<std::string_view, 2> hot_reload_key{"lifecycle",
                                                             "hot_reload"};
    constexpr std::array<std::string_view, 2> hot_reload_data_key{
        "lifecycle", "hot_reload_data"};
    if (auto read{read_gate(*document, hot_install_key, "lifecycle.hot_install",
                            admin_path, settings.hot_install)};
        !read) {
        return std::unexpected{read.error()};
    }
    if (auto read{read_gate(*document, hot_reload_key, "lifecycle.hot_reload",
                            admin_path, settings.hot_reload)};
        !read) {
        return std::unexpected{read.error()};
    }
    if (auto read{read_gate(*document, hot_reload_data_key,
                            "lifecycle.hot_reload_data", admin_path,
                            settings.hot_reload_data)};
        !read) {
        return std::unexpected{read.error()};
    }
    // A leftover `[admin]` table from before the permission service is not
    // read: importing the old list would silently grant operator rights the
    // permission policy does not express. The operator keeps the file for its
    // lifecycle gates and grants the node in `config/permissions.toml`.
    if (document->contains_at(std::array<std::string_view, 1>{"admin"})) {
        log_mods.warn(
            "{}: the [admin] table is ignored, grant refrain.mods.admin in "
            "config/permissions.toml to allow a player to run /mods",
            admin_path);
    }
    settings.admin_source = admin_path;
    return settings;
}

} // namespace refrain::server
