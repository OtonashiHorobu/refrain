// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/app/mod_profile.hpp>
#include <refrain/app/services.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <refrain/archive/manifest.hpp>
#include <refrain/archive/zip.hpp>
#include <refrain/core/config.hpp>
#include <refrain/core/io.hpp>
#include <refrain/core/log.hpp>
#include <refrain/core/semver.hpp>
#include <refrain/core/version.hpp>
#include <refrain/data/builtin_image.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/content_index.hpp>
#include <refrain/data/content_maintenance.hpp>
#include <refrain/data/content_pack.hpp>
#include <refrain/data/content_segment.hpp>
#include <refrain/data/content_store.hpp>
#include <refrain/data/datapack.hpp>
#include <refrain/data/json.hpp>
#include <refrain/data/path.hpp>
#include <refrain/data/registry.hpp>
#include <refrain/data/store_lock.hpp>
#include <refrain/data/vfs.hpp>
#include <refrain/kernel/crash.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/mod/manifest.hpp>
#include <refrain/mod/mod_ref.hpp>
#include <refrain/mod/rfmod.hpp>
#include <refrain/script/wasm.hpp>
#include <schema/schema.hpp>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace refrain::app {

namespace archive = refrain::archive;
namespace config = refrain::core::config;
namespace core = refrain::core;
namespace data = refrain::data;
namespace io = refrain::core::io;
namespace log = refrain::log;
namespace mod = refrain::mod;
namespace schema = refrain::schema;
namespace script = refrain::script;

// --- gamepack --------------------------------------------------------------

auto run_gamepack_dump(const std::string &path, std::string_view side) -> int {
    auto parsed = config::document::parse_file(path);
    if (!parsed) {
        return report_error(parsed.error());
    }
    std::cout << "# origin: " << parsed->origin() << '\n';
    std::cout << parsed->dump() << '\n';

    auto spec = mod::load_gamepack_spec(path, side);
    if (!spec) {
        return report_error(spec.error());
    }
    auto plan = mod::preview_composition(*spec);
    if (!plan) {
        return report_error(plan.error());
    }
    std::cout << "# resolved plan\n" << plan->format();
    return 0;
}

auto run_gamepack_check(const gamepack_check_options &options) -> int {
    // A profile check needs the profile's gamepack when --file is omitted; a
    // plain gamepack check keeps the historical default path.
    std::optional<mod_profile_spec> profile{};
    if (!options.profile.empty()) {
        auto loaded{
            load_mod_profile_named(options.profile, options.profiles_dir)};
        if (!loaded) {
            return report_error(loaded.error());
        }
        profile.emplace(std::move(*loaded));
    } else if (options.built) {
        return report_error(
            make_error(error_code::invalid_argument,
                       "gamepack check: --built needs --profile <name>"));
    }
    std::string path{options.file};
    if (path.empty()) {
        path = profile ? profile->gamepack_path
                       : std::string{"gamepack/gamepack.toml"};
    }

    if (profile) {
        auto checks{check_profile_gamepack(*profile, path)};
        for (const auto &warning : checks.warnings) {
            std::cout << "warning: " << warning << '\n';
        }
        for (const auto &failure : checks.errors) {
            std::cout << "error: " << failure << '\n';
        }
        if (!checks.ok()) {
            return report_error(make_error(
                error_code::invalid_argument,
                fmt::format("gamepack '{}' does not match profile '{}': {} "
                            "error(s)",
                            path, profile->name, checks.errors.size())));
        }
        std::cout << "profile: " << format_profile_identity(*profile) << '\n';
    }

    auto services{options.required |
                  std::ranges::to<std::vector<std::string_view>>()};

    auto spec = mod::load_gamepack_spec(path, options.side);
    if (!spec) {
        return report_error(spec.error());
    }
    auto plan = mod::preview_composition(*spec, services);
    if (!plan) {
        return report_error(plan.error());
    }
    std::cout << plan->format();

    if (profile && options.built) {
        auto checks{check_profile_against_binary(*profile)};
        for (const auto &failure : checks.errors) {
            std::cout << "error: " << failure << '\n';
        }
        if (!checks.ok()) {
            return report_error(make_error(
                error_code::invalid_argument,
                fmt::format("profile '{}' does not match this binary: {} "
                            "error(s)",
                            profile->name, checks.errors.size())));
        }
        std::cout << "binary: " << version::mod_profile << " build "
                  << version::mod_build_id << ", " << version::mod_ids().size()
                  << " mod(s)\n";
    }
    return 0;
}

} // namespace refrain::app
