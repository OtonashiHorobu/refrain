// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/mod/manifest.hpp>

#include <refrain/core/config.hpp>
#include <refrain/core/semver.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace refrain::mod {

namespace {

namespace config = core::config;

[[nodiscard]] auto fail(error_code code, std::string message) -> error {
    return make_error(code, std::move(message));
}

[[nodiscard]] auto required_string(const config::document &doc,
                                   std::span<const std::string_view> path,
                                   std::string_view section,
                                   std::string_view what)
    -> result<std::string> {
    auto value{doc.get_string_at(path)};
    if (!value) {
        if (value.error().code == error_code::not_found) {
            return std::unexpected{fail(
                error_code::parse_error,
                fmt::format("{}: missing required field '{}'", section, what))};
        }
        return std::unexpected{value.error()};
    }
    return value;
}

[[nodiscard]] auto optional_string(const config::document &doc,
                                   std::span<const std::string_view> path,
                                   std::string_view section,
                                   std::string_view what)
    -> result<std::string> {
    if (!doc.contains_at(path)) {
        return std::string{};
    }
    auto value{required_string(doc, path, section, what)};
    if (!value) {
        return std::unexpected{value.error()};
    }
    // Normalize surrounding whitespace (TOML keeps it inside strings).
    auto &text{*value};
    const auto first{text.find_first_not_of(" \t")};
    if (first == std::string::npos) {
        return std::string{};
    }
    const auto last{text.find_last_not_of(" \t")};
    return text.substr(first, last - first + 1);
}

[[nodiscard]] auto parse_abi_value(const config::document &doc,
                                   std::span<const std::string_view> path,
                                   std::string_view section)
    -> result<std::uint32_t> {
    const auto kind{doc.kind_at(path)};
    std::uint64_t value{0};
    if (kind == config::value_kind::string) {
        auto text{doc.get_string_at(path)};
        if (!text) {
            return std::unexpected{text.error()};
        }
        if (text->empty() || text->size() > 10) {
            return std::unexpected{
                fail(error_code::parse_error,
                     fmt::format("{}: invalid abi '{}'", section, *text))};
        }
        for (const char character : *text) {
            if (character < '0' || character > '9') {
                return std::unexpected{
                    fail(error_code::parse_error,
                         fmt::format("{}: invalid abi '{}'", section, *text))};
            }
            value = value * 10U + static_cast<std::uint64_t>(character - '0');
        }
    } else if (kind == config::value_kind::integer) {
        // The TOML generic DOM stores integers as int64; a numeric string is
        // accepted for readability in manifests (`abi = "1"`). The dotted-path
        // getter is safe here: section names and keys never contain '.'.
        auto dotted{std::string{}};
        for (const auto part : path) {
            if (!dotted.empty()) {
                dotted += '.';
            }
            dotted += part;
        }
        const auto raw{doc.get_int64(dotted, -1)};
        if (raw < 0) {
            return std::unexpected{
                fail(error_code::parse_error,
                     fmt::format("{}: abi must be a non-negative integer or "
                                 "numeric string",
                                 section))};
        }
        value = static_cast<std::uint64_t>(raw);
    } else {
        return std::unexpected{fail(
            error_code::parse_error,
            fmt::format("{}: abi must be a non-negative integer or numeric "
                        "string",
                        section))};
    }
    if (value == 0 || value > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected{
            fail(error_code::parse_error,
                 fmt::format("{}: abi is out of range", section))};
    }
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] auto parse_meta(const config::document &doc,
                              std::string_view section)
    -> result<manifest_meta> {
    manifest_meta meta{};
    constexpr std::array<std::string_view, 1> meta_path{"meta"};
    const auto kind{doc.kind_at(meta_path)};
    if (kind == config::value_kind::missing) {
        return meta;
    }
    if (kind != config::value_kind::table) {
        return std::unexpected{
            fail(error_code::parse_error,
                 fmt::format("{}: [meta] must be a table", section))};
    }
    constexpr std::array<std::string_view, 6> allowed{
        "display_name", "description", "icon", "license", "homepage", "authors",
    };
    for (const auto &key : doc.keys("meta")) {
        const auto known = std::any_of(
            allowed.begin(), allowed.end(),
            [&key](std::string_view candidate) { return candidate == key; });
        if (!known) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     fmt::format("{}: unknown [meta] key '{}'", section, key))};
        }
    }

    const auto read_optional =
        [&doc, section](std::string_view key) -> result<std::string> {
        const std::array<std::string_view, 2> path{"meta", key};
        return optional_string(doc, path, section, fmt::format("meta.{}", key));
    };
    if (auto value{read_optional("display_name")}; value) {
        meta.display_name = std::move(*value);
    } else {
        return std::unexpected{value.error()};
    }
    if (auto value{read_optional("description")}; value) {
        meta.description = std::move(*value);
    } else {
        return std::unexpected{value.error()};
    }
    if (auto value{read_optional("icon")}; value) {
        meta.icon = std::move(*value);
    } else {
        return std::unexpected{value.error()};
    }
    if (auto value{read_optional("license")}; value) {
        meta.license = std::move(*value);
    } else {
        return std::unexpected{value.error()};
    }
    if (auto value{read_optional("homepage")}; value) {
        meta.homepage = std::move(*value);
    } else {
        return std::unexpected{value.error()};
    }

    const std::array<std::string_view, 2> authors_path{"meta", "authors"};
    if (doc.contains_at(authors_path)) {
        auto authors{doc.get_strings_at(authors_path)};
        if (!authors) {
            return std::unexpected{fail(
                error_code::parse_error,
                fmt::format("{}: [meta].authors must be an array of strings",
                            section))};
        }
        meta.authors = std::move(*authors);
    }
    return meta;
}

/// Package-relative file path safety (external mod entry points, icon paths):
/// relative, '/'-separated, no empty/`.`/`..` segments, no leading '/', no
/// backslashes or control bytes, no Windows drive prefix.
[[nodiscard]] auto is_safe_package_path(std::string_view path) noexcept
    -> bool {
    if (path.empty() || path.size() > 1024 || path.front() == '/' ||
        path.back() == '/' || path.find('\\') != std::string_view::npos) {
        return false;
    }
    if (path.size() >= 2 && path[1] == ':') {
        return false;
    }
    std::size_t start{0};
    while (start <= path.size()) {
        const auto end{path.find('/', start)};
        const auto segment{path.substr(start, end == std::string_view::npos
                                                  ? std::string_view::npos
                                                  : end - start)};
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        for (const char character : segment) {
            const auto byte{static_cast<unsigned char>(character)};
            if (byte < 0x20 || byte == 0x7F) {
                return false;
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return true;
}

[[nodiscard]] auto parse_requirement_table(const config::document &doc,
                                           std::string_view table_name,
                                           std::string_view section,
                                           std::vector<mod_requirement> &out)
    -> status {
    const std::array<std::string_view, 1> table_path{table_name};
    const auto kind{doc.kind_at(table_path)};
    if (kind == config::value_kind::missing) {
        return status_ok();
    }
    if (kind != config::value_kind::table) {
        return std::unexpected{
            fail(error_code::parse_error,
                 fmt::format("{}: [{}] must be a table", section, table_name))};
    }
    for (const auto &key : doc.keys(table_name)) {
        if (!is_valid_mod_id(key)) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     fmt::format("{}: [{}] has an invalid mod id '{}'", section,
                                 table_name, key))};
        }
        const std::array<std::string_view, 2> value_path{table_name, key};
        auto range{doc.get_string_at(value_path)};
        if (!range) {
            return std::unexpected{
                fail(error_code::parse_error,
                     fmt::format("{}: [{}.{}] must be a SemVer range string",
                                 section, table_name, key))};
        }
        auto requirement{
            parse_mod_requirement(fmt::format("{}@{}", key, *range))};
        if (!requirement) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     fmt::format("{}: [{}] '{}': {}", section, table_name, key,
                                 requirement.error().message))};
        }
        const auto duplicate = std::find_if(
            out.begin(), out.end(), [&key](const mod_requirement &existing) {
                return existing.id == key;
            });
        if (duplicate != out.end()) {
            return std::unexpected{
                fail(error_code::already_exists,
                     fmt::format("{}: [{}] lists mod '{}' more than once",
                                 section, table_name, key))};
        }
        out.push_back(std::move(*requirement));
    }
    return status_ok();
}

[[nodiscard]] auto parse_provides(const config::document &doc,
                                  std::string_view section,
                                  std::vector<std::string> &out) -> status {
    constexpr std::array<std::string_view, 1> table_path{"provides"};
    const auto kind{doc.kind_at(table_path)};
    if (kind == config::value_kind::missing) {
        return status_ok();
    }
    if (kind != config::value_kind::table) {
        return std::unexpected{
            fail(error_code::parse_error,
                 fmt::format("{}: [provides] must be a table", section))};
    }
    for (const auto &key : doc.keys("provides")) {
        if (key != "services") {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     fmt::format("{}: unknown [provides] key '{}' (expected "
                                 "'services')",
                                 section, key))};
        }
    }
    const std::array<std::string_view, 2> services_path{"provides", "services"};
    if (!doc.contains_at(services_path)) {
        return std::unexpected{fail(
            error_code::parse_error,
            fmt::format("{}: [provides] must define 'services'", section))};
    }
    auto services{doc.get_strings_at(services_path)};
    if (!services) {
        return std::unexpected{fail(
            error_code::parse_error,
            fmt::format("{}: [provides].services must be an array of strings",
                        section))};
    }
    if (services->empty()) {
        return std::unexpected{fail(
            error_code::invalid_argument,
            fmt::format("{}: [provides].services must not be empty", section))};
    }
    for (const auto &service : *services) {
        if (!is_valid_service_id(service)) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     fmt::format("{}: invalid service id '{}' (expected "
                                 "namespace:name, e.g. refrain:data.packs)",
                                 section, service))};
        }
        if (std::find(out.begin(), out.end(), service) != out.end()) {
            return std::unexpected{
                fail(error_code::already_exists,
                     fmt::format("{}: [provides].services lists '{}' twice",
                                 section, service))};
        }
        out.push_back(service);
    }
    return status_ok();
}

[[nodiscard]] auto parse_entry(const config::document &doc,
                               std::optional<manifest_entry_point> &out)
    -> status {
    constexpr std::array<std::string_view, 1> table_path{"entry"};
    const auto kind{doc.kind_at(table_path)};
    if (kind == config::value_kind::missing) {
        return status_ok();
    }
    if (kind != config::value_kind::table) {
        return std::unexpected{
            fail(error_code::parse_error, "mod: [entry] must be a table")};
    }
    constexpr std::array<std::string_view, 3> allowed{"native", "wasm",
                                                      "symbol"};
    for (const auto &key : doc.keys("entry")) {
        const auto known = std::any_of(
            allowed.begin(), allowed.end(),
            [&key](std::string_view candidate) { return candidate == key; });
        if (!known) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     fmt::format("mod: unknown [entry] key '{}'", key))};
        }
    }

    manifest_entry_point entry{};
    const auto read_path = [&doc](std::string_view key) -> result<std::string> {
        const std::array<std::string_view, 2> path{"entry", key};
        if (!doc.contains_at(path)) {
            return std::string{};
        }
        auto value{doc.get_string_at(path)};
        if (!value) {
            return std::unexpected{
                fail(error_code::parse_error,
                     fmt::format("mod: [entry].{} must be a string", key))};
        }
        return value;
    };
    if (auto native{read_path("native")}; native) {
        entry.native = std::move(*native);
    } else {
        return std::unexpected{native.error()};
    }
    if (auto wasm{read_path("wasm")}; wasm) {
        entry.wasm = std::move(*wasm);
    } else {
        return std::unexpected{wasm.error()};
    }
    if (auto symbol{read_path("symbol")}; symbol) {
        entry.symbol = std::move(*symbol);
    } else {
        return std::unexpected{symbol.error()};
    }

    if (entry.native.empty() == entry.wasm.empty()) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "mod: [entry] must define exactly one of 'native' or 'wasm'")};
    }
    if (!entry.native.empty() && !is_safe_package_path(entry.native)) {
        return std::unexpected{fail(
            error_code::invalid_argument,
            fmt::format("mod: unsafe native entry path '{}'", entry.native))};
    }
    if (!entry.wasm.empty() && !is_safe_package_path(entry.wasm)) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 fmt::format("mod: unsafe wasm entry path '{}'", entry.wasm))};
    }
    if (!entry.symbol.empty() && entry.native.empty()) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "mod: [entry].symbol is only valid together with 'native'")};
    }
    out = std::move(entry);
    return status_ok();
}

[[nodiscard]] auto validate_mod_root_keys(const config::document &doc,
                                          const manifest_parse_options &options)
    -> status {
    constexpr std::array<std::string_view, 6> source_tables{
        "mod", "meta", "requires", "optional_requires", "provides", "entry"};
    // Package tables: only accepted when the caller (the `.rfmod`
    // container layer) opts in; their contents are that layer's business.
    constexpr std::array<std::string_view, 3> package_tables{
        "files", "packaging", "signature"};
    for (const auto &key : doc.keys("")) {
        const auto known = std::any_of(
            source_tables.begin(), source_tables.end(),
            [&key](std::string_view candidate) { return candidate == key; });
        if (known) {
            continue;
        }
        const auto package_table =
            options.allow_package_tables &&
            std::any_of(package_tables.begin(), package_tables.end(),
                        [&key](std::string_view candidate) {
                            return candidate == key;
                        });
        if (!package_table) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     fmt::format("mod: unknown top-level table '{}'", key))};
        }
    }
    return status_ok();
}

/// Shape check for `[mod].format_version` (`.rfmod` container version). The
/// value itself is interpreted by the container layer; the
/// kernel only rejects a wrong TOML type so a typo cannot slip through.
[[nodiscard]] auto validate_format_version(const config::document &doc)
    -> status {
    constexpr std::array<std::string_view, 2> path{"mod", "format_version"};
    const auto kind{doc.kind_at(path)};
    if (kind == config::value_kind::missing) {
        return status_ok();
    }
    if (kind != config::value_kind::integer) {
        return std::unexpected{
            fail(error_code::parse_error,
                 "mod: [mod].format_version must be an integer (container "
                 "version)")};
    }
    return status_ok();
}

[[nodiscard]] auto annotate(error original, std::string_view path) -> error {
    if (original.message.find(path) != std::string::npos) {
        return original;
    }
    original.message = std::string{path} + ": " + original.message;
    return original;
}

} // namespace

auto to_string(manifest_side side) noexcept -> std::string_view {
    switch (side) {
    case manifest_side::server:
        return "server";
    case manifest_side::client:
        return "client";
    case manifest_side::both:
        return "both";
    }
    return "both";
}

auto parse_manifest_side(std::string_view text) noexcept
    -> std::optional<manifest_side> {
    if (text == "server") {
        return manifest_side::server;
    }
    if (text == "client") {
        return manifest_side::client;
    }
    if (text == "both") {
        return manifest_side::both;
    }
    return std::nullopt;
}

auto manifest_side_matches(manifest_side declared,
                           std::string_view host_side) noexcept -> bool {
    if (host_side.empty() || host_side == "any" || host_side == "both") {
        return true;
    }
    if (declared == manifest_side::both) {
        return true;
    }
    return to_string(declared) == host_side;
}

auto parse_manifest_common(const core::config::document &doc,
                           std::string_view section,
                           const manifest_parse_options &options)
    -> result<manifest_common> {
    const std::array<std::string_view, 1> section_path{section};
    const auto section_kind{doc.kind_at(section_path)};
    if (section_kind != config::value_kind::table) {
        return std::unexpected{fail(
            error_code::parse_error,
            fmt::format("{}: missing required [{}] table", section, section))};
    }

    manifest_common common{};

    const std::array<std::string_view, 2> id_path{section, "id"};
    auto id{
        required_string(doc, id_path, section, fmt::format("{}.id", section))};
    if (!id) {
        return std::unexpected{id.error()};
    }
    if (!is_valid_mod_id(*id)) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 fmt::format("{}: invalid {}.id '{}'", section, section, *id))};
    }
    common.id = std::move(*id);

    const std::array<std::string_view, 2> version_path{section, "version"};
    auto version{required_string(doc, version_path, section,
                                 fmt::format("{}.version", section))};
    if (!version) {
        return std::unexpected{version.error()};
    }
    if (!core::semver::parse(*version)) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 fmt::format("{}: invalid {}.version '{}' (expected SemVer "
                             "major.minor.patch)",
                             section, section, *version))};
    }
    common.version = std::move(*version);

    const std::array<std::string_view, 2> side_path{section, "side"};
    if (doc.contains_at(side_path)) {
        auto side_text{required_string(doc, side_path, section,
                                       fmt::format("{}.side", section))};
        if (!side_text) {
            return std::unexpected{side_text.error()};
        }
        const auto side{parse_manifest_side(*side_text)};
        if (!side) {
            return std::unexpected{fail(
                error_code::invalid_argument,
                fmt::format("{}: invalid side '{}' (expected server, client or "
                            "both)",
                            section, *side_text))};
        }
        common.side = *side;
    } else if (options.require_side) {
        return std::unexpected{
            fail(error_code::parse_error,
                 fmt::format("{}: missing required field '{}.side'", section,
                             section))};
    }

    const std::array<std::string_view, 2> abi_path{section, "abi"};
    auto abi{parse_abi_value(doc, abi_path, section)};
    if (!abi) {
        return std::unexpected{abi.error()};
    }
    if (options.expected_abi != 0 && *abi != options.expected_abi) {
        return std::unexpected{fail(
            error_code::version_mismatch,
            fmt::format("{}: abi {} is not supported by this runtime (requires "
                        "abi {})",
                        section, *abi, options.expected_abi))};
    }
    common.abi = *abi;

    auto meta{parse_meta(doc, section)};
    if (!meta) {
        return std::unexpected{meta.error()};
    }
    common.meta = std::move(*meta);
    if (options.require_display_name && common.meta.display_name.empty()) {
        return std::unexpected{
            fail(error_code::parse_error,
                 fmt::format("{}: [meta].display_name is required", section))};
    }
    return common;
}

auto format_manifest_common(const manifest_common &common) -> std::string {
    return fmt::format("{}@{} (side={}, abi={})", common.id, common.version,
                       to_string(common.side), common.abi);
}

auto parse_mod_manifest(const core::config::document &doc,
                        const manifest_parse_options &options)
    -> result<mod_manifest> {
    if (auto root_check{validate_mod_root_keys(doc, options)}; !root_check) {
        return std::unexpected{root_check.error()};
    }

    // The shared common parser also serves gamepack manifests, whose section
    // carries loader keys (auto_scan/mods_dir); a mod manifest has the fixed
    // identity quartet, so reject typos here instead of silently ignoring them.
    // `format_version` is the optional container version.
    constexpr std::array<std::string_view, 5> mod_section_keys{
        "id", "version", "side", "abi", "format_version"};
    for (const auto &key : doc.keys("mod")) {
        const auto known = std::any_of(
            mod_section_keys.begin(), mod_section_keys.end(),
            [&key](std::string_view candidate) { return candidate == key; });
        if (!known) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     fmt::format("mod: unknown [mod] key '{}'", key))};
        }
    }
    if (auto version{validate_format_version(doc)}; !version) {
        return std::unexpected{version.error()};
    }

    auto common{parse_manifest_common(doc, "mod", options)};
    if (!common) {
        return std::unexpected{common.error()};
    }
    mod_manifest manifest{};
    manifest.common = std::move(*common);

    if (auto required{
            parse_requirement_table(doc, "requires", "mod", manifest.required)};
        !required) {
        return std::unexpected{required.error()};
    }
    if (auto optional{parse_requirement_table(doc, "optional_requires", "mod",
                                              manifest.optional)};
        !optional) {
        return std::unexpected{optional.error()};
    }
    if (auto provides{parse_provides(doc, "mod", manifest.provides)};
        !provides) {
        return std::unexpected{provides.error()};
    }
    if (auto entry{parse_entry(doc, manifest.entry)}; !entry) {
        return std::unexpected{entry.error()};
    }

    // Self-dependency and hard/optional overlap are manifest bugs, not
    // resolution failures: reject them at parse time.
    for (const auto &requirement : manifest.required) {
        if (requirement.id == manifest.common.id) {
            return std::unexpected{fail(
                error_code::invalid_argument,
                fmt::format("mod: '{}' requires itself", manifest.common.id))};
        }
        const auto clash =
            std::find_if(manifest.optional.begin(), manifest.optional.end(),
                         [&requirement](const mod_requirement &optional) {
                             return optional.id == requirement.id;
                         });
        if (clash != manifest.optional.end()) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     fmt::format("mod: '{}' is listed in both [requires] and "
                                 "[optional_requires]",
                                 requirement.id))};
        }
    }
    for (const auto &requirement : manifest.optional) {
        if (requirement.id == manifest.common.id) {
            return std::unexpected{
                fail(error_code::invalid_argument,
                     fmt::format("mod: '{}' optionally requires itself",
                                 manifest.common.id))};
        }
    }

    // Deterministic output order regardless of TOML table iteration order.
    const auto by_id = [](const mod_requirement &lhs,
                          const mod_requirement &rhs) {
        return lhs.id < rhs.id;
    };
    std::sort(manifest.required.begin(), manifest.required.end(), by_id);
    std::sort(manifest.optional.begin(), manifest.optional.end(), by_id);
    std::sort(manifest.provides.begin(), manifest.provides.end());
    return manifest;
}

auto load_mod_manifest(std::string_view path) -> result<mod_manifest> {
    auto doc{config::document::parse_file(path)};
    if (!doc) {
        return std::unexpected{annotate(std::move(doc.error()), path)};
    }
    auto manifest{parse_mod_manifest(*doc)};
    if (!manifest) {
        return std::unexpected{annotate(std::move(manifest.error()), path)};
    }
    return manifest;
}

auto format_mod_manifest(const mod_manifest &manifest) -> std::string {
    auto out{fmt::format("mod {}@{} (side={}, abi={}{})\n", manifest.common.id,
                         manifest.common.version,
                         to_string(manifest.common.side), manifest.common.abi,
                         manifest.is_external() ? ", external" : ", internal")};
    if (!manifest.common.meta.display_name.empty()) {
        out += fmt::format("display name: {}\n",
                           manifest.common.meta.display_name);
    }
    if (!manifest.provides.empty()) {
        out += "provides:";
        for (const auto &service : manifest.provides) {
            out += fmt::format(" {}", service);
        }
        out += '\n';
    }
    if (!manifest.required.empty()) {
        out += "requires:";
        for (const auto &requirement : manifest.required) {
            out += fmt::format(" {}", requirement.text());
        }
        out += '\n';
    }
    if (!manifest.optional.empty()) {
        out += "optional:";
        for (const auto &requirement : manifest.optional) {
            out += fmt::format(" {}", requirement.text());
        }
        out += '\n';
    }
    if (manifest.entry) {
        out +=
            fmt::format("entry: {}\n",
                        manifest.entry->native.empty()
                            ? fmt::format("wasm {}", manifest.entry->wasm)
                            : fmt::format("native {}", manifest.entry->native));
    }
    return out;
}

} // namespace refrain::mod
