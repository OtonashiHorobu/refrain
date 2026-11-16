// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/mod/install_policy.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace refrain::server {

// Configuration of the mod management command surface.
//
// Two files under the host configuration directory feed it:
//
//   mods.toml        install/trust policy, read by the same parser the startup
//                    content bootstrap uses, so a runtime install cannot
//                    accept a package the next start would reject;
//   mods_admin.toml  the hot gates and the in-game operator list:
//
//     [lifecycle]
//     hot_install = false      # apply install/uninstall while running
//     hot_reload = false       # rebuild a loaded mod in place
//     hot_reload_data = false  # rebuild the datapack content view
//
//     [admin]
//     operators = ["<uuid>"]   # players allowed to run /mods (console is
//                              # always allowed)
//
// A missing file keeps the safe default (hot off, no player operator). The
// console reads the policy from the file; a one-run CLI override applies to the
// startup bootstrap only.

struct mod_management_settings {
    mod::install_policy_config policy{};
    /// `mods.toml` the policy was read from; empty means the built-in default.
    std::string policy_source{};
    /// `mods_admin.toml` the hot gates were read from; empty means defaults.
    std::string admin_source{};
    bool hot_install{false};
    bool hot_reload{false};
    /// `[lifecycle] hot_reload_data`: rebuild the datapack content view
    /// (recipes, loot tables, advancements, worldgen documents) in place.
    bool hot_reload_data{false};
    /// Canonical lowercase player uuids allowed to run the in-game command,
    /// in configuration order.
    std::vector<std::string> operators{};
};

/// Read the settings from `config_dir`. An empty directory returns the
/// defaults. A malformed file is an error naming the file and the offending
/// key: silently narrowing or widening the install policy is worse than a
/// failed start.
[[nodiscard]] auto load_mod_management_settings(std::string_view config_dir)
    -> result<mod_management_settings>;

} // namespace refrain::server
