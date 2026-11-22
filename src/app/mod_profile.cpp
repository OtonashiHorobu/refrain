// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/app/mod_profile.hpp>
#include <refrain/app/services.hpp>

#include <refrain/contracts/abi.h>
#include <refrain/contracts/services/mod_loader.hpp>
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
#include <iostream>
#include <iterator>
#include <map>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace refrain::app {

namespace config = refrain::core::config;
namespace core = refrain::core;
namespace fs = std::filesystem;
namespace io = refrain::core::io;
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

/// Map a declared service contract id to the `[providers]` key the host
/// resolves for it. A custom `namespace:name` contract keeps its full id; a
/// first-party contract uses the short slot spelling, with the two historical
/// aliases (`data.packs`, `net.protocol`) written out explicitly. The result
/// always satisfies `mod::matches_slot(service, key)`.
[[nodiscard]] auto provider_slot(std::string_view service) -> std::string {
    constexpr std::string_view first_party{"refrain:"};
    if (!service.starts_with(first_party)) {
        return std::string{service};
    }
    const auto rest{service.substr(first_party.size())};
    if (rest == "data.packs") {
        return "data";
    }
    if (rest == "net.protocol") {
        return "protocol";
    }
    std::string slot{};
    slot.reserve(rest.size());
    for (const char character : rest) {
        slot.push_back(character == '.' ? '_' : character);
    }
    return slot;
}

/// Bare TOML key only for the lowercase slot spelling; quoted literal key for
/// custom service ids, which contain ':'.
[[nodiscard]] auto toml_key(std::string_view key) -> std::string {
    const bool bare{!key.empty() && key.front() >= 'a' && key.front() <= 'z' &&
                    std::all_of(key.begin() + 1, key.end(), [](char value) {
                        return (value >= 'a' && value <= 'z') ||
                               (value >= '0' && value <= '9') || value == '_';
                    })};
    return bare ? std::string{key} : fmt::format("\"{}\"", key);
}

[[nodiscard]] auto
render_gamepack(const mod_profile_spec &profile,
                const std::vector<mod::mod_manifest> &manifests,
                const std::map<std::string, std::string> &providers)
    -> std::string {
    std::string out{
        "# SPDX-License-Identifier: MIT\n"
        "#\n"
        "# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu\n"
        "#\n"};
    out += fmt::format(
        "# Generated from build profile '{}' by `refrain-tools gamepack gen`.\n"
        "# Every profile mod is pinned at its manifest version and each "
        "service\n"
        "# is bound to its single declaring mod. Provider composition\n"
        "# (exclusive, instanced or chain) remains a deployment decision: "
        "edit\n"
        "# [providers] to choose one. After an install or update, run\n"
        "# `refrain-tools gamepack sync` to refresh pins together with the "
        "lock.\n"
        "\n"
        "[gamepack]\n"
        "id = \"refrain.{}\"\n"
        "version = \"1.0.0\"\n"
        "side = \"server\"\n"
        "abi = \"{}\"\n"
        "auto_scan = false\n"
        "\n"
        "[meta]\n"
        "display_name = \"{} generated gamepack\"\n"
        "description = \"Starter composition generated from build profile "
        "'{}'.\"\n"
        "\n"
        "# An installed update of a pinned mod wins over the built-in copy;\n"
        "# `gamepack sync` then brings the pin in line with the receipt.\n"
        "[policy]\n"
        "conflict = \"explicit\"\n"
        "allow_custom_override = true\n"
        "\n"
        "[mods]\n"
        "list = [\n",
        profile.name, profile.name, REFRAIN_ABI_MAJOR, profile.name,
        profile.name);
    for (const auto &manifest : manifests) {
        out += fmt::format("    \"{}@{}\",\n", manifest.common.id,
                           manifest.common.version);
    }
    out += "]\n";

    // Mirror the authoritative manifest dependencies so the composition root
    // can order the listed mods without a second manifest read.
    for (const auto &manifest : manifests) {
        if (manifest.required.empty()) {
            continue;
        }
        out +=
            fmt::format("\n[mods.\"{}\"]\nrequires = [\n", manifest.common.id);
        for (const auto &requirement : manifest.required) {
            out += fmt::format("    \"{}\",\n", requirement.text());
        }
        out += "]\n";
    }

    if (!providers.empty()) {
        out += "\n[providers]\n";
        for (const auto &[slot, binding] : providers) {
            out += fmt::format("{} = \"{}\"\n", toml_key(slot), binding);
        }
    }
    return out;
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
    auto pins{spec->mods | std::views::transform([](const auto &entry) {
                  return entry.ref.id;
              }) |
              std::ranges::to<std::vector<std::string>>()};
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
    auto built_mods{version::mod_ids() |
                    std::ranges::to<std::vector<std::string>>()};
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
    auto built_dynamic{
        version::dynamic_modules() |
        std::views::transform([](const auto &entry) { return entry.id; }) |
        std::ranges::to<std::vector<std::string>>()};
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

auto generate_gamepack(const mod_profile_spec &profile,
                       std::string_view mods_root)
    -> result<generated_gamepack> {
    const std::string gamepack_id{fmt::format("refrain.{}", profile.name)};
    if (!mod::is_valid_mod_id(gamepack_id)) {
        return std::unexpected{profile_failure(
            profile.source,
            fmt::format("profile name '{}' cannot form a gamepack id",
                        profile.name))};
    }

    // One manifest read per profile mod. Anything missing, mis-typed or
    // unreadable stops generation: a starter with a silent gap would pass a
    // later check with a different mod set than the profile declares.
    std::vector<mod::mod_manifest> manifests{};
    manifests.reserve(profile.mods.size());
    for (const auto &id : profile.mods) {
        const fs::path path{fs::path{mods_root} / id /
                            mod::mod_manifest_filename};
        auto manifest{mod::load_mod_manifest(path.string())};
        if (!manifest) {
            return std::unexpected{make_error(
                error_code::io_error,
                fmt::format("gamepack gen: profile '{}' mod '{}': {}",
                            profile.name, id, manifest.error().message))};
        }
        if (manifest->common.id != id) {
            return std::unexpected{make_error(
                error_code::parse_error,
                fmt::format("gamepack gen: profile '{}' lists '{}' but its "
                            "manifest declares '{}'",
                            profile.name, id, manifest->common.id))};
        }
        manifests.push_back(std::move(*manifest));
    }

    // Group declaring mods by the provider slot their contract maps to. Two
    // different contracts mapping to the same slot are also a conflict: the
    // gamepack cannot name both in one binding.
    std::map<std::string, std::string> slot_service{};
    std::map<std::string, std::vector<std::string>> slot_candidates{};
    for (const auto &manifest : manifests) {
        for (const auto &service : manifest.provides) {
            if (mod::matches_slot(refrain::services::mod_loader::service_id,
                                  service)) {
                continue; // loaders are declared in [loaders], not [providers]
            }
            const auto slot{provider_slot(service)};
            const auto [it, inserted]{slot_service.emplace(slot, service)};
            if (!inserted && it->second != service) {
                return std::unexpected{make_error(
                    error_code::invalid_argument,
                    fmt::format("gamepack gen: provider selection for services "
                                "'{}' and '{}' is ambiguous: both map to slot "
                                "'{}'",
                                it->second, service, slot))};
            }
            slot_candidates[slot].emplace_back(fmt::format(
                "{}@^{}", manifest.common.id, manifest.common.version));
        }
    }
    std::map<std::string, std::string> providers{};
    for (const auto &[slot, candidates] : slot_candidates) {
        if (candidates.size() != 1U) {
            std::string names{};
            for (const auto &candidate : candidates) {
                if (!names.empty()) {
                    names += ", ";
                }
                const auto at{candidate.find('@')};
                names += candidate.substr(0, at);
            }
            return std::unexpected{make_error(
                error_code::invalid_argument,
                fmt::format("gamepack gen: provider selection for service '{}' "
                            "is ambiguous: {} candidate providers ({})",
                            slot_service.at(slot), candidates.size(), names))};
        }
        providers.emplace(slot, candidates.front());
    }

    std::string text{render_gamepack(profile, manifests, providers)};
    auto document{config::document::parse_text(text, "generated gamepack")};
    if (!document) {
        return std::unexpected{std::move(document.error())};
    }
    auto spec{mod::parse_gamepack(*document, "server")};
    if (!spec) {
        return std::unexpected{std::move(spec.error())};
    }
    return generated_gamepack{.spec = std::move(*spec),
                              .text = std::move(text)};
}

auto run_gamepack_gen(const gamepack_gen_options &options) -> int {
    if (options.profile.empty()) {
        return report_error(
            make_error(error_code::invalid_argument,
                       "gamepack gen: --profile <name> is required"));
    }
    if (options.out.empty()) {
        return report_error(
            make_error(error_code::invalid_argument,
                       "gamepack gen: --out <path> is required"));
    }
    auto profile{load_mod_profile_named(options.profile, options.profiles_dir)};
    if (!profile) {
        return report_error(profile.error());
    }
    auto generated{generate_gamepack(*profile, options.mods_dir)};
    if (!generated) {
        return report_error(generated.error());
    }
    if (options.check) {
        auto existing{io::read_text(options.out)};
        if (!existing) {
            return report_error(make_error(
                error_code::not_found,
                fmt::format(
                    "gamepack gen: '{}' is missing; run without --check "
                    "to create it",
                    options.out)));
        }
        if (*existing != generated->text) {
            return report_error(make_error(
                error_code::version_mismatch,
                fmt::format("gamepack gen: '{}' differs from the generated "
                            "gamepack for profile '{}'; regenerate to apply",
                            options.out, profile->name)));
        }
        std::cout << fmt::format("gamepack gen: '{}' matches profile '{}'\n",
                                 options.out, profile->name);
        return 0;
    }
    const fs::path path{options.out};
    if (const auto parent{path.parent_path()}; !parent.empty()) {
        if (const auto made{io::create_directories(parent.string())}; !made) {
            return report_error(made.error());
        }
    }
    if (const auto written{io::atomic_write_text(options.out, generated->text)};
        !written) {
        return report_error(written.error());
    }
    std::cout << fmt::format(
        "gamepack gen: wrote '{}' from profile '{}' ({} mods, {} providers)\n",
        options.out, profile->name, generated->spec.mods.size(),
        generated->spec.providers.size());
    return 0;
}

} // namespace refrain::app
