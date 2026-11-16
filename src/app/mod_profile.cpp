// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/app/mod_profile.hpp>

#include <refrain/core/config.hpp>
#include <refrain/core/io.hpp>
#include <refrain/core/semver.hpp>
#include <refrain/core/version.hpp>
#include <refrain/data/builtin_image.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/manifest.hpp>
#include <refrain/mod/mod_ref.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace refrain::app {

namespace config = refrain::core::config;
namespace core = refrain::core;
namespace fs = std::filesystem;
namespace mod = refrain::mod;

namespace {

[[nodiscard]] auto profile_failure(std::string_view path, std::string message)
    -> error {
    return make_error(error_code::parse_error,
                      fmt::format("mod profile '{}': {}", path, message));
}

/// Root the profile's repo-relative paths are resolved against. A profile read
/// from `<root>/profiles/<name>.toml` resolves against `<root>`; any other
/// location resolves against the working directory.
[[nodiscard]] auto profile_root(std::string_view path) -> fs::path {
    const fs::path file{path};
    const auto parent{file.parent_path()};
    if (parent.filename() == "profiles" && !parent.parent_path().empty()) {
        return parent.parent_path();
    }
    std::error_code error;
    const auto current{fs::current_path(error)};
    return error ? fs::path{} : current;
}

[[nodiscard]] auto sorted_unique(std::vector<std::string> values)
    -> std::vector<std::string> {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

[[nodiscard]] auto contains_id(const std::vector<std::string> &ids,
                               std::string_view id) -> bool {
    return std::binary_search(ids.begin(), ids.end(), id);
}

/// Directory holds at least one `*.cpp`. Dynamic mods are only meaningful when
/// the mod has code to put into the shared module.
[[nodiscard]] auto mod_has_code(std::string_view mods_root, std::string_view id)
    -> bool {
    const fs::path source_directory =
        fs::path{mods_root} / std::string{id} / "src";
    std::error_code error;
    if (!fs::is_directory(source_directory, error)) {
        return false;
    }
    for (const auto &entry : fs::directory_iterator{source_directory, error}) {
        if (error) {
            return false;
        }
        if (entry.is_regular_file(error) &&
            entry.path().extension() == ".cpp") {
            return true;
        }
    }
    return false;
}

} // namespace

auto load_mod_profile(std::string_view path) -> result<mod_profile_spec> {
    auto parsed{config::document::parse_file(path)};
    if (!parsed) {
        return std::unexpected{std::move(parsed.error())};
    }
    constexpr std::array<std::string_view, 7> allowed_keys{
        "name",    "description", "gamepack", "mods",
        "dynamic", "build_id",    "link"};
    for (const auto &key : parsed->keys("")) {
        const auto known{std::any_of(
            allowed_keys.begin(), allowed_keys.end(),
            [&key](std::string_view candidate) { return candidate == key; })};
        if (!known) {
            return std::unexpected{profile_failure(
                path, fmt::format("unknown key '{}' (known keys: name, "
                                  "description, gamepack, mods, dynamic)",
                                  key))};
        }
    }

    mod_profile_spec profile{};
    profile.source = std::string{path};
    profile.name = parsed->get_string("name", "");
    profile.description = parsed->get_string("description", "");
    profile.gamepack = parsed->get_string("gamepack", "");
    profile.link = parsed->get_string("link", "static");
    profile.build_id = parsed->get_string("build_id", "");
    profile.mods = sorted_unique(parsed->get_strings("mods"));
    profile.dynamic = sorted_unique(parsed->get_strings("dynamic"));

    if (profile.name.empty()) {
        return std::unexpected{profile_failure(path, "missing 'name' string")};
    }
    if (profile.gamepack.empty()) {
        return std::unexpected{
            profile_failure(path, "missing 'gamepack' string")};
    }
    if (profile.mods.empty()) {
        return std::unexpected{
            profile_failure(path, "missing or empty 'mods' array")};
    }
    if (profile.link != "static" && profile.link != "mixed" &&
        profile.link != "dynamic") {
        return std::unexpected{profile_failure(
            path, fmt::format("invalid 'link' value '{}' (static, mixed or "
                              "dynamic)",
                              profile.link))};
    }
    if (!profile.build_id.empty() &&
        (profile.build_id.size() != 16U ||
         !std::all_of(profile.build_id.begin(), profile.build_id.end(),
                      [](char value) {
                          return (value >= '0' && value <= '9') ||
                                 (value >= 'a' && value <= 'f');
                      }))) {
        return std::unexpected{profile_failure(
            path, "'build_id' must be 16 lowercase hex characters")};
    }

    // The file name and the declared name must agree, otherwise `--profile
    // <name>` would silently load a different selection than it reports.
    const auto stem{fs::path{path}.stem().string()};
    if (!stem.empty() && profile.name != stem &&
        fs::path{path}.has_parent_path() &&
        fs::path{path}.parent_path().filename() == "profiles") {
        return std::unexpected{profile_failure(
            path, fmt::format("name '{}' does not match the file name '{}'",
                              profile.name, stem))};
    }

    for (const auto &id : profile.mods) {
        if (!mod::is_valid_mod_id(id)) {
            return std::unexpected{
                profile_failure(path, fmt::format("invalid mod id '{}'", id))};
        }
    }
    for (const auto &id : profile.dynamic) {
        if (!contains_id(profile.mods, id)) {
            return std::unexpected{profile_failure(
                path, fmt::format("dynamic mod '{}' is not in the profile's "
                                  "mods list",
                                  id))};
        }
    }

    const auto root{profile_root(path)};
    const fs::path gamepack_path{root / profile.gamepack};
    profile.gamepack_path = gamepack_path.string();
    return profile;
}

auto load_mod_profile_named(std::string_view name, std::string_view directory)
    -> result<mod_profile_spec> {
    if (name.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument, "empty profile name")};
    }
    if (name.find('/') != std::string_view::npos ||
        name.find('\\') != std::string_view::npos || name == "." ||
        name == "..") {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       fmt::format("invalid profile name '{}'", name))};
    }
    const fs::path path{fs::path{directory} / (std::string{name} + ".toml")};
    return load_mod_profile(path.string());
}

auto check_profile_mods_tree(const mod_profile_spec &profile,
                             std::string_view mods_root)
    -> profile_check_result {
    profile_check_result result{};
    const fs::path root{mods_root};

    // Read every manifest once: existence, identity and the `[requires]`
    // closure are all checked against the same parsed documents.
    std::vector<mod::mod_manifest> manifests{};
    manifests.reserve(profile.mods.size());
    for (const auto &id : profile.mods) {
        const fs::path manifest_path{root / id / "manifest.toml"};
        std::error_code error;
        if (!fs::is_directory(root / id, error)) {
            result.error(
                fmt::format("mod '{}' has no directory under '{}'; add the mod "
                            "or remove it from profile '{}'",
                            id, mods_root, profile.name));
            continue;
        }
        auto manifest{mod::load_mod_manifest(manifest_path.string())};
        if (!manifest) {
            result.error(
                fmt::format("mod '{}': {}", id, manifest.error().message));
            continue;
        }
        if (manifest->common.id != id) {
            result.error(
                fmt::format("mod '{}' declares id '{}' in its manifest", id,
                            manifest->common.id));
            continue;
        }
        manifests.push_back(std::move(*manifest));
    }
    for (const auto &manifest : manifests) {
        for (const auto &requirement : manifest.required) {
            if (!contains_id(profile.mods, requirement.id)) {
                result.error(fmt::format(
                    "mod '{}' requires '{}' but profile '{}' does not build "
                    "it; add the dependency to the profile",
                    manifest.common.id, requirement.id, profile.name));
                continue;
            }
            const auto found{std::find_if(
                manifests.begin(), manifests.end(),
                [&requirement](const mod::mod_manifest &candidate) {
                    return candidate.common.id == requirement.id;
                })};
            if (found != manifests.end() &&
                !core::satisfies(found->common.version, requirement.range)) {
                result.error(fmt::format(
                    "mod '{}' requires '{}' {} but the profile builds {}",
                    manifest.common.id, requirement.id, requirement.range,
                    found->common.version));
            }
        }
    }
    for (const auto &id : profile.dynamic) {
        if (!mod_has_code(mods_root, id)) {
            result.error(fmt::format(
                "dynamic mod '{}' has no source under '{}'; only a mod with "
                "code can be built as a shared module",
                id, (root / id / "src").string()));
        }
    }
    return result;
}

auto check_profile_gamepack(const mod_profile_spec &profile,
                            std::string_view gamepack_path)
    -> profile_check_result {
    profile_check_result result{};
    const std::string path{gamepack_path.empty() ? profile.gamepack_path
                                                 : std::string{gamepack_path}};
    auto spec{mod::load_gamepack_spec(path, "server")};
    if (!spec) {
        result.error(fmt::format("default gamepack '{}': {}", path,
                                 spec.error().message));
        return result;
    }
    std::vector<std::string> pins{};
    pins.reserve(spec->mods.size());
    for (const auto &entry : spec->mods) {
        pins.push_back(entry.ref.id);
    }
    pins = sorted_unique(std::move(pins));
    for (const auto &id : pins) {
        if (!contains_id(profile.mods, id)) {
            result.error(fmt::format(
                "gamepack '{}' pins '{}' but profile '{}' does not build it",
                path, id, profile.name));
        }
    }
    for (const auto &id : profile.mods) {
        if (!contains_id(pins, id)) {
            result.error(fmt::format(
                "profile '{}' builds '{}' but gamepack '{}' does not pin it; "
                "the same list must drive code, image and gamepack",
                profile.name, id, path));
        }
    }
    return result;
}

auto check_profile_image(const mod_profile_spec &profile,
                         std::string_view image_path) -> profile_check_result {
    profile_check_result result{};
    auto bytes{refrain::core::io::read_file(image_path)};
    if (!bytes) {
        result.error(fmt::format("cannot read built-in image '{}': {}",
                                 image_path, bytes.error().message));
        return result;
    }
    auto image{
        refrain::data::builtin_image::open(std::span<const std::byte>{*bytes})};
    if (!image) {
        result.error(fmt::format("built-in image '{}' is invalid: {}",
                                 image_path, image.error().message));
        return result;
    }
    if (image->find(mod::builtin_index_filename) == nullptr) {
        result.error(fmt::format("built-in image '{}' has no '{}'", image_path,
                                 mod::builtin_index_filename));
    }
    std::vector<std::string> image_mods{};
    for (const auto &entry : image->entries()) {
        constexpr std::string_view prefix{"mods/"};
        if (!entry.path.starts_with(prefix)) {
            continue;
        }
        const auto rest{entry.path.substr(prefix.size())};
        const auto slash{rest.find('/')};
        if (slash == std::string_view::npos) {
            continue;
        }
        image_mods.emplace_back(rest.substr(0, slash));
    }
    image_mods = sorted_unique(std::move(image_mods));
    for (const auto &id : profile.mods) {
        if (!contains_id(image_mods, id)) {
            result.error(fmt::format(
                "built-in image '{}' has no files for '{}' but profile '{}' "
                "builds it; rebuild the image from the profile",
                image_path, id, profile.name));
        }
    }
    for (const auto &id : image_mods) {
        if (!contains_id(profile.mods, id)) {
            result.error(fmt::format(
                "built-in image '{}' contains '{}' but profile '{}' does not "
                "build it",
                image_path, id, profile.name));
        }
    }
    return result;
}

auto check_profile_against_binary(const mod_profile_spec &profile)
    -> profile_check_result {
    profile_check_result result{};
    const std::string_view built_profile{version::mod_profile};
    const std::string_view built_build_id{version::mod_build_id};
    if (profile.name != built_profile) {
        result.error(fmt::format(
            "profile '{}' does not match this binary, which was built with "
            "profile '{}' (build {})",
            profile.name, built_profile, built_build_id));
    }
    std::vector<std::string> built_mods{};
    built_mods.reserve(version::mod_ids().size());
    for (const auto &id : version::mod_ids()) {
        built_mods.emplace_back(id);
    }
    for (const auto &id : profile.mods) {
        if (!contains_id(built_mods, id)) {
            result.error(fmt::format(
                "profile '{}' builds '{}' but this binary does not contain it "
                "(built profile '{}', build {})",
                profile.name, id, built_profile, built_build_id));
        }
    }
    for (const auto &id : built_mods) {
        if (!contains_id(profile.mods, id)) {
            result.error(fmt::format(
                "this binary contains '{}' but profile '{}' does not list it",
                id, profile.name));
        }
    }
    std::vector<std::string> built_dynamic{};
    built_dynamic.reserve(version::dynamic_modules().size());
    for (const auto &entry : version::dynamic_modules()) {
        built_dynamic.emplace_back(entry.id);
    }
    for (const auto &id : profile.dynamic) {
        if (!contains_id(built_dynamic, id)) {
            result.error(fmt::format(
                "profile '{}' declares dynamic mod '{}' but this binary was "
                "built with it as static code",
                profile.name, id));
        }
    }
    for (const auto &id : built_dynamic) {
        if (!contains_id(profile.dynamic, id)) {
            result.error(fmt::format(
                "this binary builds '{}' as a dynamic module but profile '{}' "
                "does not declare it as dynamic",
                id, profile.name));
        }
    }
    return result;
}

auto unbuilt_mod_ids(std::span<const std::string> ids)
    -> std::vector<std::string> {
    std::vector<std::string> missing{};
    for (const auto &id : ids) {
        if (!version::is_mod_built(id)) {
            missing.push_back(id);
        }
    }
    std::sort(missing.begin(), missing.end());
    missing.erase(std::unique(missing.begin(), missing.end()), missing.end());
    return missing;
}

auto format_unbuilt_mod_error(std::string_view source,
                              std::span<const std::string> ids) -> std::string {
    const bool single{ids.size() == 1U};
    std::string listed{};
    for (std::size_t index{0}; index < ids.size(); ++index) {
        if (index != 0) {
            listed += index + 1U == ids.size() ? " and " : ", ";
        }
        listed += fmt::format("'{}'", ids[index]);
    }
    return fmt::format(
        "mod{} {} {} not built into this binary (profile={}, build {}); {} "
        "pins {}. Rebuild with a profile that includes {} or use a "
        "composition without {}",
        single ? "" : "s", listed, single ? "is" : "are", version::mod_profile,
        version::mod_build_id,
        source.empty() ? std::string_view{"the composition"} : source,
        single ? "it" : "them", single ? "it" : "them", single ? "it" : "them");
}

auto check_lock_profile(const mod::mod_lock &lock) -> status {
    if (lock.profile.empty()) {
        return std::unexpected{make_error(
            error_code::version_mismatch,
            "mods.lock does not record the build profile that wrote it; "
            "rewrite it with --update-lock")};
    }
    if (lock.profile != version::mod_profile) {
        return std::unexpected{make_error(
            error_code::version_mismatch,
            fmt::format("mods.lock was written by build profile '{}' but this "
                        "binary was built with profile '{}' (build {}); locks "
                        "cannot be mixed between profiles",
                        lock.profile, version::mod_profile,
                        version::mod_build_id))};
    }
    if (lock.build_id.empty()) {
        return std::unexpected{make_error(
            error_code::version_mismatch,
            "mods.lock does not record the mod build id that wrote it; "
            "rewrite it with --update-lock")};
    }
    if (lock.build_id != version::mod_build_id) {
        return std::unexpected{make_error(
            error_code::version_mismatch,
            fmt::format("mods.lock was written by build {} of profile '{}' but "
                        "this binary is build {} of profile '{}'; rebuild the "
                        "lock for this binary",
                        lock.build_id, lock.profile, version::mod_build_id,
                        version::mod_profile))};
    }
    return status_ok();
}

auto format_profile_identity(const mod_profile_spec &profile) -> std::string {
    return fmt::format("{} ({} mods, {} dynamic, build {})", profile.name,
                       profile.mods.size(), profile.dynamic.size(),
                       profile.build_id.empty() ? std::string{"<source>"}
                                                : profile.build_id);
}

} // namespace refrain::app
