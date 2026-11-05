// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/datapack.hpp>

#include <refrain/data/path.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace refrain::data {
namespace {

constexpr std::string_view data_prefix{"data/"};
constexpr std::string_view manifest_path{"pack.mcmeta"};
/// Jar-root format metadata (client.jar has no pack.mcmeta;
/// `pack_version.data_major` is the data format, 26.3 = 121).
constexpr std::string_view version_metadata_path{"version.json"};

[[nodiscard]] auto diagnostic(std::string_view label, std::string_view path,
                              std::string detail) -> std::string {
    std::string text{"datapack '"};
    text += label;
    text += "': ";
    text += path;
    text += ": ";
    text += detail;
    return text;
}

[[nodiscard]] auto file_error(std::string_view label, std::string_view path,
                              error_code code, std::string detail) -> error {
    return make_error(code, diagnostic(label, path, std::move(detail)));
}

[[nodiscard]] auto split_segments(std::string_view text)
    -> std::vector<std::string_view> {
    std::vector<std::string_view> segments;
    std::size_t start{0};
    while (start <= text.size()) {
        const auto slash{text.find('/', start)};
        segments.push_back(text.substr(start, slash == std::string_view::npos
                                                  ? std::string_view::npos
                                                  : slash - start));
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return segments;
}

[[nodiscard]] auto has_suffix(std::string_view text,
                              std::string_view suffix) noexcept -> bool {
    return text.size() >= suffix.size() &&
           text.substr(text.size() - suffix.size()) == suffix;
}

[[nodiscard]] auto strip_suffix(std::string_view text, std::string_view suffix)
    -> std::string_view {
    return has_suffix(text, suffix)
               ? text.substr(0, text.size() - suffix.size())
               : text;
}

/// Kind directory names are lowercase identifiers (`loot_table`, `worldgen`).
[[nodiscard]] auto is_valid_kind_dir(std::string_view text) noexcept -> bool {
    if (text.empty()) {
        return false;
    }
    for (const char character : text) {
        const bool valid = (character >= 'a' && character <= 'z') ||
                           (character >= '0' && character <= '9') ||
                           character == '_';
        if (!valid) {
            return false;
        }
    }
    return true;
}

/// Raw path segments may contain dots (file extensions/names) but must be
/// lowercase and must not be empty/`.`/`..`.
[[nodiscard]] auto is_valid_path_segment(std::string_view text) noexcept
    -> bool {
    if (text.empty() || text == "." || text == "..") {
        return false;
    }
    for (const char character : text) {
        const bool valid = (character >= 'a' && character <= 'z') ||
                           (character >= '0' && character <= '9') ||
                           character == '_' || character == '-' ||
                           character == '.';
        if (!valid) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] auto join_segments(std::span<const std::string_view> segments)
    -> std::string {
    std::string text;
    for (std::size_t index{0}; index < segments.size(); ++index) {
        if (index != 0) {
            text.push_back('/');
        }
        text += segments[index];
    }
    return text;
}

/// Classification of one `data/` virtual path.
enum class path_role : std::uint8_t {
    content,         ///< `data/<namespace>/<kind>/<file>` content entry
    ignored,         ///< layout marker / stray nested manifest, not content
    nested_datapack, ///< `data/<namespace>/datapacks/<name>/**`
};

struct classified_path {
    path_role role{path_role::content};
    content_kind kind{content_kind::other};
    std::string kind_dir{};
    std::string tag_type{};
    std::string worldgen_subkind{};
    /// Identity of a nested datapack (`<namespace>/datapacks/<name>`).
    std::string nested_datapack{};
    resource_location id{};
    bool json{false};
};

/// Vanilla tag registries whose key spans more than one path segment (26.3:
/// the worldgen content registries). Matching takes the longest prefix; every
/// other tag directory is a single-segment registry (`block`, `item`, ...) and
/// the remaining path stays part of the tag id (`block/mineable/pickaxe`).
constexpr std::array<std::string_view, 5> multi_segment_tag_registries{
    "worldgen/biome", "worldgen/feature",
    "worldgen/flat_level_generator_preset", "worldgen/structure",
    "worldgen/world_preset"};

/// Worldgen subkinds dispatched by a top-level `type` codec in vanilla data.
/// The remaining subkinds (`biome`, `placed_feature`, `noise`,
/// `noise_settings`, `processor_list`, `structure_set`, `template_pool`,
/// `world_preset`, ...) are dispatched by their directory and have no `type`
/// field.
constexpr std::array<std::string_view, 7> dispatch_worldgen_subkinds{
    "feature",         "structure",          "carver",
    "material_rule",   "material_condition", "block_state_provider",
    "density_function"};

[[nodiscard]] auto
is_dispatch_worldgen_subkind(std::string_view subkind) noexcept -> bool {
    return std::find(dispatch_worldgen_subkinds.begin(),
                     dispatch_worldgen_subkinds.end(),
                     subkind) != dispatch_worldgen_subkinds.end();
}

/// Longest known multi-segment tag registry prefix of `segments`, if any.
/// `segments` are the path segments below `data/<ns>/tags/` (file last).
[[nodiscard]] auto
match_tag_registry(std::span<const std::string_view> segments)
    -> std::pair<std::string_view, std::size_t> {
    std::string_view registry{};
    std::size_t registry_segments{0};
    for (const auto candidate : multi_segment_tag_registries) {
        const auto parts{split_segments(candidate)};
        // The registry must leave at least one segment for the tag id.
        if (parts.size() >= segments.size()) {
            continue;
        }
        bool matches{true};
        for (std::size_t index{0}; index < parts.size(); ++index) {
            if (segments[index] != parts[index]) {
                matches = false;
                break;
            }
        }
        if (matches && parts.size() > registry_segments) {
            registry = candidate;
            registry_segments = parts.size();
        }
    }
    return {registry, registry_segments};
}

/// Human-readable tag identity without the ambiguous `<registry>:<namespace>:
/// ...` double colon: the registry and the namespaced tag id are slash-joined
/// (`block/minecraft:logs`, `worldgen/biome/minecraft:is_ocean`).
[[nodiscard]] auto tag_identity(std::string_view tag_type,
                                const resource_location &id) -> std::string {
    std::string text{tag_type};
    text.push_back('/');
    text += id.to_string();
    return text;
}

/// Parse the id part of a known content path and validate it as a resource
/// location path (`/`-separated lowercase segments).
[[nodiscard]] auto make_content_id(std::string_view namespace_part,
                                   std::string_view relative)
    -> result<resource_location> {
    if (!resource_location::is_valid_path(relative)) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "invalid content id path '" +
                                              std::string{relative} + "'")};
    }
    return resource_location::make(namespace_part, relative);
}

/// Classify a virtual path below `data/` without reading the file. The error
/// carries a detail message; the caller wraps it with pack label + path.
[[nodiscard]] auto classify_data_path(std::string_view path)
    -> result<classified_path> {
    if (!is_safe_relative_path(path)) {
        return std::unexpected{
            make_error(error_code::parse_error, "unsafe virtual path")};
    }
    if (!path.starts_with(data_prefix)) {
        return std::unexpected{
            make_error(error_code::parse_error, "not a data/ path")};
    }
    classified_path result;
    const auto segments{split_segments(path.substr(data_prefix.size()))};
    // Vanilla jar layout markers (`data/.mcassetsroot`, see
    // VanillaPackResourcesBuilder) only locate the classpath root; they are not
    // data and must never abort a full-jar scan.
    if (!segments.empty() && segments.back() == ".mcassetsroot") {
        result.role = path_role::ignored;
        return result;
    }
    // Nested datapacks are separate packs mounted by vanilla; this loader
    // counts but neither parses nor validates them (no recursive mounting).
    if (segments.size() >= 3 && segments[1] == "datapacks") {
        result.role = path_role::nested_datapack;
        result.nested_datapack = std::string{segments[0]};
        result.nested_datapack += "/datapacks/";
        result.nested_datapack += segments[2];
        return result;
    }
    // A manifest inside the data tree (`data/<ns>/pack.mcmeta`, usually part of
    // a nested pack) is metadata, not `data/<ns>/<kind>/<file>` content.
    if (!segments.empty() && segments.back() == "pack.mcmeta") {
        result.role = path_role::ignored;
        return result;
    }
    if (segments.size() < 3) {
        return std::unexpected{make_error(
            error_code::parse_error,
            "invalid layout: expected data/<namespace>/<kind>/<file>")};
    }
    for (const auto segment : segments) {
        if (!is_valid_path_segment(segment)) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "path segment '" + std::string{segment} +
                               "' is not lowercase [a-z0-9_.-]")};
        }
    }
    const auto namespace_part{segments[0]};
    if (!resource_location::is_valid_namespace(namespace_part)) {
        return std::unexpected{make_error(
            error_code::parse_error,
            "invalid namespace '" + std::string{namespace_part} + "'")};
    }
    const auto kind_dir{segments[1]};
    if (!is_valid_kind_dir(kind_dir)) {
        return std::unexpected{make_error(error_code::parse_error,
                                          "invalid kind directory '" +
                                              std::string{kind_dir} + "'")};
    }
    const std::vector<std::string_view> rest(segments.begin() + 2,
                                             segments.end());

    result.kind_dir = std::string{kind_dir};

    if (kind_dir == "recipe" || kind_dir == "loot_table" ||
        kind_dir == "advancement") {
        const auto &file{rest.back()};
        if (!has_suffix(file, ".json")) {
            return std::unexpected{make_error(
                error_code::parse_error,
                std::string{kind_dir} + " entries must end in .json")};
        }
        const std::string joined{join_segments(rest)};
        const auto relative{strip_suffix(joined, ".json")};
        auto id{make_content_id(namespace_part, relative)};
        if (!id) {
            return std::unexpected{std::move(id.error())};
        }
        if (kind_dir == "recipe") {
            result.kind = content_kind::recipe;
        } else if (kind_dir == "loot_table") {
            result.kind = content_kind::loot_table;
        } else {
            result.kind = content_kind::advancement;
        }
        result.id = std::move(*id);
        result.json = true;
        return result;
    }
    if (kind_dir == "worldgen") {
        if (rest.size() < 2) {
            return std::unexpected{make_error(
                error_code::parse_error,
                "worldgen entries must be worldgen/<subkind>/<file>.json")};
        }
        if (!is_valid_kind_dir(rest.front())) {
            return std::unexpected{make_error(
                error_code::parse_error, "invalid worldgen subkind '" +
                                             std::string{rest.front()} + "'")};
        }
        if (!has_suffix(rest.back(), ".json")) {
            return std::unexpected{make_error(
                error_code::parse_error, "worldgen entries must end in .json")};
        }
        const std::string joined{join_segments(rest)};
        const auto relative{strip_suffix(joined, ".json")};
        auto id{make_content_id(namespace_part, relative)};
        if (!id) {
            return std::unexpected{std::move(id.error())};
        }
        result.kind = content_kind::worldgen;
        result.worldgen_subkind = std::string{rest.front()};
        result.id = std::move(*id);
        result.json = true;
        return result;
    }
    if (kind_dir == "structure") {
        if (!has_suffix(rest.back(), ".nbt")) {
            return std::unexpected{make_error(
                error_code::parse_error, "structure entries must end in .nbt")};
        }
        const std::string joined{join_segments(rest)};
        const auto relative{strip_suffix(joined, ".nbt")};
        auto id{make_content_id(namespace_part, relative)};
        if (!id) {
            return std::unexpected{std::move(id.error())};
        }
        result.kind = content_kind::structure;
        result.id = std::move(*id);
        return result;
    }
    if (kind_dir == "tags") {
        if (rest.size() < 2) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "tag entries must be tags/<tag_type>/<file>.json")};
        }
        const auto tag_type{rest.front()};
        if (!is_valid_kind_dir(tag_type)) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "invalid tag type '" + std::string{tag_type} + "'")};
        }
        if (!has_suffix(rest.back(), ".json")) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "tag entries must end in .json")};
        }
        // Tag identity is (registry, tag id). A handful of vanilla registries
        // are keyed with a slash (`worldgen/biome`); match the longest known
        // prefix and keep the rest as the tag id. Everything else keeps the
        // historical single-segment registry (`block`, `item`, ...).
        const auto [matched_registry,
                    registry_segments]{match_tag_registry(rest)};
        std::size_t id_offset{1};
        std::string registry{tag_type};
        if (registry_segments != 0) {
            id_offset = registry_segments;
            registry.assign(matched_registry);
        }
        if (rest.size() <= id_offset) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "tag entries must be tags/<tag_type>/<file>.json")};
        }
        const std::vector<std::string_view> id_segments(
            rest.begin() + static_cast<std::ptrdiff_t>(id_offset), rest.end());
        const std::string id_joined{join_segments(id_segments)};
        const auto relative{strip_suffix(id_joined, ".json")};
        auto id{make_content_id(namespace_part, relative)};
        if (!id) {
            return std::unexpected{std::move(id.error())};
        }
        result.kind = content_kind::tags;
        result.tag_type = std::move(registry);
        result.id = std::move(*id);
        result.json = true;
        return result;
    }
    // Unmodelled kind: counted, and `*.json` files are parse-checked but not
    // retained. This keeps vanilla jars/mods with many kinds loadable.
    result.kind = content_kind::other;
    result.json = has_suffix(path, ".json");
    return result;
}

/// Read a positive integer format field from a JSON object; `field_path` is the
/// dotted name used in diagnostics (`pack.pack_format`, `pack.min_format`,
/// ...).
[[nodiscard]] auto read_format_field(const json::value &parent,
                                     std::string_view key,
                                     std::string_view field_path,
                                     std::uint32_t &out) -> status {
    const auto *node{parent.find(key)};
    if (node == nullptr) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "missing '" + std::string{field_path} + "'")};
    }
    auto value{node->as_int64()};
    if (!value) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "'" + std::string{field_path} + "' must be an integer")};
    }
    if (*value <= 0) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "'" + std::string{field_path} + "' must be positive")};
    }
    if (*value >
        static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "'" + std::string{field_path} + "' is out of range")};
    }
    out = static_cast<std::uint32_t>(*value);
    return status_ok();
}

[[nodiscard]] auto read_manifest(const pack &source,
                                 const datapack_load_options &options,
                                 layer_info &info) -> status {
    const auto *entry{source.find(manifest_path)};
    info.has_manifest = entry != nullptr;
    if (!info.has_manifest) {
        if (options.expected_pack_format == 0) {
            return status_ok();
        }
        // A jar root without pack.mcmeta (26.3 client.jar) declares its data
        // format in version.json. Use it to satisfy an explicit gate, but never
        // require it when no format was requested.
        const auto *version{source.find(version_metadata_path)};
        if (version == nullptr) {
            return std::unexpected{file_error(
                source.label(), manifest_path, error_code::version_mismatch,
                "pack declares no format (no pack.mcmeta and no version.json); "
                "cannot match expected " +
                    std::to_string(options.expected_pack_format))};
        }
        info.has_version_metadata = true;
        auto version_text{source.read_text(version_metadata_path)};
        if (!version_text) {
            return std::unexpected{file_error(
                source.label(), version_metadata_path,
                version_text.error().code, version_text.error().message)};
        }
        auto version_document{json::value::parse(*version_text)};
        if (!version_document) {
            return std::unexpected{file_error(
                source.label(), version_metadata_path, error_code::parse_error,
                "invalid JSON: " + version_document.error().message)};
        }
        const auto *pack_version{version_document->find("pack_version")};
        if (pack_version == nullptr || !pack_version->is_object()) {
            return std::unexpected{file_error(
                source.label(), version_metadata_path, error_code::parse_error,
                "missing 'pack_version' object (cannot read the data format)")};
        }
        if (auto read{read_format_field(*pack_version, "data_major",
                                        "pack_version.data_major",
                                        info.version_data_major)};
            !read) {
            return std::unexpected{
                file_error(source.label(), version_metadata_path,
                           error_code::parse_error, read.error().message)};
        }
        if (info.version_data_major != options.expected_pack_format) {
            return std::unexpected{
                file_error(source.label(), version_metadata_path,
                           error_code::version_mismatch,
                           "version.json data_major " +
                               std::to_string(info.version_data_major) +
                               " does not match expected " +
                               std::to_string(options.expected_pack_format))};
        }
        return status_ok();
    }
    auto text{source.read_text(manifest_path)};
    if (!text) {
        return std::unexpected{file_error(source.label(), manifest_path,
                                          text.error().code,
                                          text.error().message)};
    }
    auto document{json::value::parse(*text)};
    if (!document) {
        return std::unexpected{
            file_error(source.label(), manifest_path, error_code::parse_error,
                       "invalid JSON: " + document.error().message)};
    }
    const auto *pack_node{document->find("pack")};
    if (pack_node == nullptr || !pack_node->is_object()) {
        return std::unexpected{file_error(source.label(), manifest_path,
                                          error_code::parse_error,
                                          "missing 'pack' object")};
    }
    const auto *legacy_node{pack_node->find("pack_format")};
    const auto *min_node{pack_node->find("min_format")};
    const auto *max_node{pack_node->find("max_format")};
    // 26.3 `min_format`/`max_format` are a pair; `pack_format` stays a legacy
    // fallback (and is still accepted alone).
    if ((min_node == nullptr) != (max_node == nullptr)) {
        return std::unexpected{
            file_error(source.label(), manifest_path, error_code::parse_error,
                       "'pack.min_format' and 'pack.max_format' must be "
                       "declared together")};
    }
    const bool has_range{min_node != nullptr};
    if (has_range) {
        if (auto read{read_format_field(*pack_node, "min_format",
                                        "pack.min_format",
                                        info.manifest_min_format)};
            !read) {
            return std::unexpected{file_error(source.label(), manifest_path,
                                              error_code::parse_error,
                                              read.error().message)};
        }
        if (auto read{read_format_field(*pack_node, "max_format",
                                        "pack.max_format",
                                        info.manifest_max_format)};
            !read) {
            return std::unexpected{file_error(source.label(), manifest_path,
                                              error_code::parse_error,
                                              read.error().message)};
        }
        if (info.manifest_min_format > info.manifest_max_format) {
            return std::unexpected{file_error(
                source.label(), manifest_path, error_code::parse_error,
                "'pack.min_format' must not exceed 'pack.max_format'")};
        }
    }
    if (legacy_node != nullptr) {
        if (auto read{read_format_field(*pack_node, "pack_format",
                                        "pack.pack_format",
                                        info.manifest_pack_format)};
            !read) {
            return std::unexpected{file_error(source.label(), manifest_path,
                                              error_code::parse_error,
                                              read.error().message)};
        }
    }
    if (!has_range && legacy_node == nullptr) {
        return std::unexpected{
            file_error(source.label(), manifest_path, error_code::parse_error,
                       "missing 'pack.pack_format' (or the 26.3 pair "
                       "'pack.min_format'/'pack.max_format')")};
    }
    if (options.expected_pack_format != 0) {
        const auto expected{options.expected_pack_format};
        if (has_range) {
            if (expected < info.manifest_min_format ||
                expected > info.manifest_max_format) {
                return std::unexpected{file_error(
                    source.label(), manifest_path, error_code::version_mismatch,
                    "min_format " + std::to_string(info.manifest_min_format) +
                        "..max_format " +
                        std::to_string(info.manifest_max_format) +
                        " does not match expected " +
                        std::to_string(expected))};
            }
        } else if (info.manifest_pack_format != expected) {
            return std::unexpected{file_error(
                source.label(), manifest_path, error_code::version_mismatch,
                "pack_format " + std::to_string(info.manifest_pack_format) +
                    " does not match expected " + std::to_string(expected))};
        }
    }
    return status_ok();
}

/// Parse one tag `values` element into a merged entry.
[[nodiscard]] auto parse_tag_value(const json::value &element)
    -> result<tag_value> {
    tag_value parsed;
    if (element.is_string()) {
        auto text{element.as_string()};
        if (!text) {
            return std::unexpected{
                make_error(error_code::parse_error, text.error().message)};
        }
        std::string_view id_text{*text};
        if (!id_text.empty() && id_text.front() == '#') {
            parsed.tag_reference = true;
            id_text.remove_prefix(1);
        }
        auto id{resource_location::parse(id_text)};
        if (!id) {
            return std::unexpected{
                make_error(error_code::parse_error, id.error().message)};
        }
        parsed.id = std::move(*id);
        return parsed;
    }
    if (!element.is_object()) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "tag value must be a string id or an object with 'id'")};
    }
    const auto *id_node{element.find("id")};
    if (id_node == nullptr || !id_node->is_string()) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       "tag value object requires a string 'id'")};
    }
    auto id_text{id_node->as_string()};
    if (!id_text) {
        return std::unexpected{
            make_error(error_code::parse_error, id_text.error().message)};
    }
    std::string_view text{*id_text};
    if (!text.empty() && text.front() == '#') {
        parsed.tag_reference = true;
        text.remove_prefix(1);
    }
    auto id{resource_location::parse(text)};
    if (!id) {
        return std::unexpected{
            make_error(error_code::parse_error, id.error().message)};
    }
    parsed.id = std::move(*id);
    if (const auto *required_node{element.find("required")};
        required_node != nullptr) {
        auto required{required_node->as_bool()};
        if (!required) {
            return std::unexpected{make_error(error_code::parse_error,
                                              "'required' must be a boolean")};
        }
        parsed.required = *required;
    }
    return parsed;
}

/// Count NBT tags and measure nesting iteratively (no recursion on untrusted
/// input; the NBT codec already bounded the depth).
[[nodiscard]] auto summarize_nbt(const nbt::value &root) -> nbt_structure_info {
    nbt_structure_info info;
    struct node_state {
        const nbt::value *node;
        std::size_t depth;
    };
    std::vector<node_state> pending{node_state{&root, 1}};
    while (!pending.empty()) {
        const auto current{pending.back()};
        pending.pop_back();
        ++info.nodes;
        info.depth = std::max(info.depth, current.depth);
        if (const auto *compound{current.node->as_compound()};
            compound != nullptr) {
            for (const auto &item : *compound) {
                pending.push_back(node_state{&item.second, current.depth + 1});
            }
        } else if (const auto *list{current.node->as_list()}; list != nullptr) {
            for (const auto &child : list->items) {
                pending.push_back(node_state{&child, current.depth + 1});
            }
        }
    }
    return info;
}

void increment_kind_count(kind_counts &counts, content_kind kind) noexcept {
    switch (kind) {
    case content_kind::recipe:
        ++counts.recipe;
        break;
    case content_kind::loot_table:
        ++counts.loot_table;
        break;
    case content_kind::advancement:
        ++counts.advancement;
        break;
    case content_kind::worldgen:
        ++counts.worldgen;
        break;
    case content_kind::structure:
        ++counts.structure;
        break;
    case content_kind::tags:
        ++counts.tags;
        break;
    case content_kind::other:
        ++counts.other;
        break;
    }
}

void set_kind_count(kind_counts &counts, content_kind kind,
                    std::size_t value) noexcept {
    switch (kind) {
    case content_kind::recipe:
        counts.recipe = value;
        break;
    case content_kind::loot_table:
        counts.loot_table = value;
        break;
    case content_kind::advancement:
        counts.advancement = value;
        break;
    case content_kind::worldgen:
        counts.worldgen = value;
        break;
    case content_kind::structure:
        counts.structure = value;
        break;
    case content_kind::tags:
        counts.tags = value;
        break;
    case content_kind::other:
        counts.other = value;
        break;
    }
}

} // namespace

auto content_kind_name(content_kind kind) noexcept -> std::string_view {
    switch (kind) {
    case content_kind::recipe:
        return "recipe";
    case content_kind::loot_table:
        return "loot_table";
    case content_kind::advancement:
        return "advancement";
    case content_kind::worldgen:
        return "worldgen";
    case content_kind::structure:
        return "structure";
    case content_kind::tags:
        return "tags";
    case content_kind::other:
        return "other";
    }
    return "other";
}

// ---------------------------------------------------------------------------
// tag_set
// ---------------------------------------------------------------------------

auto tag_set::find(std::string_view tag_type, const resource_location &id) const
    -> const tag_record * {
    // Not `noexcept`: the temporary key owns a `std::string`.
    const auto found{records_.find(key{std::string{tag_type}, id})};
    return found == records_.end() ? nullptr : &found->second;
}

auto tag_set::tag_types() const -> std::vector<std::string> {
    std::vector<std::string> types;
    types.reserve(records_.size());
    for (const auto &[key_value, record] : records_) {
        static_cast<void>(record);
        if (types.empty() || types.back() != key_value.first) {
            types.push_back(key_value.first);
        }
    }
    return types;
}

auto tag_set::tags_of(std::string_view tag_type) const
    -> std::vector<const tag_record *> {
    std::vector<const tag_record *> records;
    for (const auto &[key_value, record] : records_) {
        if (key_value.first == tag_type) {
            records.push_back(&record);
        }
    }
    return records;
}

auto tag_set::all() const -> std::vector<const tag_record *> {
    std::vector<const tag_record *> records;
    records.reserve(records_.size());
    for (const auto &[key_value, record] : records_) {
        static_cast<void>(key_value);
        records.push_back(&record);
    }
    return records;
}

tag_set::tag_set(std::map<key, tag_record> records)
    : records_{std::move(records)} {}

auto tag_set::value_count() const noexcept -> std::size_t {
    std::size_t count{0};
    for (const auto &[key_value, record] : records_) {
        static_cast<void>(key_value);
        count += record.values.size();
    }
    return count;
}

// ---------------------------------------------------------------------------
// datapack_set::impl
// ---------------------------------------------------------------------------

struct datapack_set::impl {
    datapack_stats stats{};
    tag_set tags{};
    std::map<content_kind, std::map<resource_location, content_entry>>
        content{};
    std::vector<std::string> duplicates{};
    std::map<std::string, std::size_t> other_counts{};
    /// Distinct `data/<ns>/datapacks/<name>` roots seen (counted, not mounted).
    std::set<std::string> nested_datapacks{};
    /// Accumulated merged tags keyed by (tag type, id). Moved into `tags` by
    /// `finalize()` (the loader is a datapack_set member, so it can build the
    /// tag_set from this plain map).
    std::map<std::pair<std::string, resource_location>, tag_record>
        tag_records{};

    [[nodiscard]] auto process_layer(const pack &source,
                                     const datapack_load_options &options)
        -> status {
        layer_info info{};
        info.label = std::string{source.label()};
        info.format = source.format();
        info.file_count = source.file_count();
        if (auto manifest = read_manifest(source, options, info); !manifest) {
            return manifest;
        }
        stats.total_files += info.file_count;
        stats.layers.push_back(std::move(info));
        for (const auto &file : source.entries()) {
            if (!file.path.starts_with(data_prefix)) {
                continue;
            }
            ++stats.data_files;
            if (auto processed = process_data_file(source, file.path, options);
                !processed) {
                return processed;
            }
        }
        return status_ok();
    }

  private:
    [[nodiscard]] auto process_data_file(const pack &source,
                                         std::string_view path,
                                         const datapack_load_options &options)
        -> status {
        auto classified{classify_data_path(path)};
        if (!classified) {
            return std::unexpected{file_error(source.label(), path,
                                              error_code::parse_error,
                                              classified.error().message)};
        }
        const auto &info{*classified};
        if (info.role == path_role::ignored) {
            // Layout markers / stray data-tree manifests: never content.
            ++stats.ignored_files;
            return status_ok();
        }
        if (info.role == path_role::nested_datapack) {
            // Nested packs are separate packs in vanilla; count them but do not
            // parse or semantically validate their contents.
            ++stats.nested_datapack_files;
            nested_datapacks.insert(info.nested_datapack);
            return status_ok();
        }
        switch (info.kind) {
        case content_kind::recipe:
        case content_kind::loot_table:
        case content_kind::advancement:
        case content_kind::worldgen:
            return process_json_entry(source, path, info);
        case content_kind::structure:
            return process_structure(source, path, info, options);
        case content_kind::tags:
            return process_tag(source, path, info);
        case content_kind::other:
            return process_other(source, path, info);
        }
        return status_ok();
    }

    [[nodiscard]] auto process_json_entry(const pack &source,
                                          std::string_view path,
                                          const classified_path &info)
        -> status {
        auto text{source.read_text(path)};
        if (!text) {
            return std::unexpected{file_error(
                source.label(), path, text.error().code, text.error().message)};
        }
        auto document{json::value::parse(*text)};
        if (!document) {
            return std::unexpected{
                file_error(source.label(), path, error_code::parse_error,
                           "invalid JSON: " + document.error().message)};
        }
        // Vanilla density functions may be constants: a bare JSON number
        // (`zero.json` = 0.0) or a string reference. Every other content kind
        // (and an object density function) is validated below.
        const bool density_constant{
            info.kind == content_kind::worldgen &&
            info.worldgen_subkind == "density_function" &&
            (document->is_number() || document->is_string())};
        if (!density_constant) {
            if (!document->is_object()) {
                return std::unexpected{
                    file_error(source.label(), path, error_code::parse_error,
                               "content root must be a JSON object")};
            }
            if (auto validated{validate_kind_payload(*document, info)};
                !validated) {
                return std::unexpected{file_error(source.label(), path,
                                                  error_code::parse_error,
                                                  validated.error().message)};
            }
        }
        content_entry entry{};
        entry.kind = info.kind;
        entry.id = info.id;
        entry.source_label = std::string{source.label()};
        entry.source_path = std::string{path};
        if (const auto *file{source.find(path)}; file != nullptr) {
            entry.byte_size = file->size;
        }
        entry.raw = std::move(*document);
        increment_kind_count(stats.parsed, info.kind);
        store_entry(std::move(entry));
        return status_ok();
    }

    /// Kind-specific JSON shape checks done at load time (semantic cross-refs
    /// are deferred to `validate()`).
    [[nodiscard]] static auto validate_kind_payload(const json::value &document,
                                                    const classified_path &info)
        -> status {
        const auto kind{info.kind};
        if (kind == content_kind::advancement) {
            // Vanilla advancements have no top-level "type"; `criteria` is the
            // mandatory object.
            const auto *criteria{document.find("criteria")};
            if (criteria == nullptr || !criteria->is_object()) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "advancement requires a 'criteria' object")};
            }
            if (criteria->size() == 0) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "advancement 'criteria' must not be empty")};
            }
            if (const auto *requirements{document.find("requirements")};
                requirements != nullptr && !requirements->is_array()) {
                return std::unexpected{
                    make_error(error_code::parse_error,
                               "advancement 'requirements' must be an array")};
            }
            return status_ok();
        }
        if (kind == content_kind::worldgen &&
            !is_dispatch_worldgen_subkind(info.worldgen_subkind)) {
            // Directory-dispatched subkind (`biome`, `placed_feature`, `noise`,
            // `noise_settings`, `processor_list`, `structure_set`,
            // `template_pool`, `world_preset`, ...): vanilla data has no
            // top-level "type"; the object root is enough.
            return status_ok();
        }
        const auto *type_node{document.find("type")};
        if (type_node == nullptr) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           std::string{content_kind_name(kind)} +
                               " requires a top-level 'type' field")};
        }
        auto type_text{type_node->as_string()};
        if (!type_text) {
            return std::unexpected{make_error(
                error_code::parse_error, std::string{content_kind_name(kind)} +
                                             " 'type' must be a string")};
        }
        auto type_id{resource_location::parse(*type_text)};
        if (!type_id) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "invalid '" + std::string{content_kind_name(kind)} +
                               ".type': " + type_id.error().message)};
        }
        return status_ok();
    }

    [[nodiscard]] auto process_structure(const pack &source,
                                         std::string_view path,
                                         const classified_path &info,
                                         const datapack_load_options &options)
        -> status {
        content_entry entry{};
        entry.kind = content_kind::structure;
        entry.id = info.id;
        entry.source_label = std::string{source.label()};
        entry.source_path = std::string{path};
        if (const auto *file{source.find(path)}; file != nullptr) {
            entry.byte_size = file->size;
        }
        if (options.parse_structures) {
            auto bytes{source.read(path)};
            if (!bytes) {
                return std::unexpected{file_error(source.label(), path,
                                                  bytes.error().code,
                                                  bytes.error().message)};
            }
            auto root{nbt::read(*bytes, nbt::compression::gzip)};
            if (!root) {
                return std::unexpected{file_error(
                    source.label(), path, error_code::parse_error,
                    "invalid structure NBT: " + root.error().message)};
            }
            if (!root->is(nbt::tag_type::compound)) {
                return std::unexpected{
                    file_error(source.label(), path, error_code::parse_error,
                               "structure root must be TAG_Compound")};
            }
            entry.structure_info = summarize_nbt(*root);
            entry.structure = std::move(*root);
        }
        increment_kind_count(stats.parsed, content_kind::structure);
        store_entry(std::move(entry));
        return status_ok();
    }

    [[nodiscard]] auto process_tag(const pack &source, std::string_view path,
                                   const classified_path &info) -> status {
        auto text{source.read_text(path)};
        if (!text) {
            return std::unexpected{file_error(
                source.label(), path, text.error().code, text.error().message)};
        }
        auto document{json::value::parse(*text)};
        if (!document) {
            return std::unexpected{
                file_error(source.label(), path, error_code::parse_error,
                           "invalid JSON: " + document.error().message)};
        }
        if (!document->is_object()) {
            return std::unexpected{
                file_error(source.label(), path, error_code::parse_error,
                           "tag root must be a JSON object")};
        }
        bool replace{false};
        if (const auto *replace_node{document->find("replace")};
            replace_node != nullptr) {
            auto value{replace_node->as_bool()};
            if (!value) {
                return std::unexpected{
                    file_error(source.label(), path, error_code::parse_error,
                               "tag 'replace' must be a boolean")};
            }
            replace = *value;
        }
        const auto *values{document->find("values")};
        if (values == nullptr || !values->is_array()) {
            return std::unexpected{file_error(source.label(), path,
                                              error_code::parse_error,
                                              "tag requires a 'values' array")};
        }
        auto elements{values->as_array()};
        if (!elements) {
            return std::unexpected{file_error(source.label(), path,
                                              error_code::parse_error,
                                              elements.error().message)};
        }
        // Parse everything before mutating so a malformed file cannot leave a
        // partially merged tag behind.
        std::vector<tag_value> parsed;
        parsed.reserve(elements->size());
        for (const auto &element : *elements) {
            auto value{parse_tag_value(element)};
            if (!value) {
                return std::unexpected{file_error(source.label(), path,
                                                  error_code::parse_error,
                                                  value.error().message)};
            }
            parsed.push_back(std::move(*value));
        }
        auto &record{tag_records[{info.tag_type, info.id}]};
        if (record.tag_type.empty()) {
            record.tag_type = info.tag_type;
            record.id = info.id;
        }
        record.source_path = std::string{path};
        if (replace) {
            record.values.clear();
        }
        for (auto &value : parsed) {
            record.values.push_back(std::move(value));
        }
        ++record.sources;
        increment_kind_count(stats.parsed, content_kind::tags);
        return status_ok();
    }

    [[nodiscard]] auto process_other(const pack &source, std::string_view path,
                                     const classified_path &info) -> status {
        ++stats.other_files;
        ++other_counts[info.kind_dir];
        if (info.json) {
            // Counted, not retained: still fail on malformed JSON so a broken
            // pack cannot hide a syntax error in an unmodelled kind.
            auto text{source.read_text(path)};
            if (!text) {
                return std::unexpected{file_error(source.label(), path,
                                                  text.error().code,
                                                  text.error().message)};
            }
            auto document{json::value::parse(*text)};
            if (!document) {
                return std::unexpected{
                    file_error(source.label(), path, error_code::parse_error,
                               "invalid JSON: " + document.error().message)};
            }
        }
        return status_ok();
    }

    void store_entry(content_entry entry) {
        auto &by_kind{content[entry.kind]};
        const auto duplicate{by_kind.find(entry.id)};
        if (duplicate != by_kind.end()) {
            ++stats.override_count;
            duplicates.push_back("content '" + entry.id.to_string() + "' (" +
                                 std::string{content_kind_name(entry.kind)} +
                                 ") from pack '" + entry.source_label +
                                 "' overrides pack '" +
                                 duplicate->second.source_label + "'");
            duplicate->second = std::move(entry);
            return;
        }
        by_kind.emplace(entry.id, std::move(entry));
    }

  public:
    void finalize() {
        for (const auto kind :
             {content_kind::recipe, content_kind::loot_table,
              content_kind::advancement, content_kind::worldgen,
              content_kind::structure}) {
            if (const auto found{content.find(kind)}; found != content.end()) {
                set_kind_count(stats.counts, kind, found->second.size());
            }
        }
        stats.counts.tags = tag_records.size();
        stats.tag_value_count = 0;
        stats.merged_tag_count = 0;
        for (const auto &[key_value, record] : tag_records) {
            static_cast<void>(key_value);
            stats.tag_value_count += record.values.size();
            if (record.sources > 1) {
                ++stats.merged_tag_count;
            }
        }
        stats.counts.other = stats.other_files;
        stats.other_kinds = other_counts;
        stats.nested_datapacks = nested_datapacks.size();
    }
};

// ---------------------------------------------------------------------------
// datapack_set
// ---------------------------------------------------------------------------

datapack_set::datapack_set() : impl_{std::make_unique<impl>()} {}

datapack_set::datapack_set(datapack_set &&other) noexcept = default;

auto datapack_set::operator=(datapack_set &&other) noexcept
    -> datapack_set & = default;

datapack_set::~datapack_set() = default;

auto datapack_set::load(std::span<const pack *const> layers,
                        datapack_load_options options) -> result<datapack_set> {
    datapack_set set;
    for (const pack *layer : layers) {
        if (layer == nullptr) {
            return std::unexpected{
                make_error(error_code::invalid_argument,
                           "datapack layer must not be null")};
        }
        if (auto processed = set.impl_->process_layer(*layer, options);
            !processed) {
            return std::unexpected{std::move(processed.error())};
        }
    }
    set.impl_->finalize();
    set.impl_->tags = tag_set{std::move(set.impl_->tag_records)};
    return set;
}

auto datapack_set::load(const vfs &source, datapack_load_options options)
    -> result<datapack_set> {
    // vfs::packs() exposes the physical stack (low -> high priority), so the
    // vfs entry point has exactly the same tag-merge semantics as the
    // explicit-layer overload (hidden/lower-priority tag files included).
    const auto layers{source.packs()};
    if (layers.empty()) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            "cannot load a datapack_set from a vfs without mounted packs")};
    }
    return load(std::span<const pack *const>{layers.data(), layers.size()},
                options);
}

auto datapack_set::stats() const noexcept -> const datapack_stats & {
    return impl_->stats;
}

auto datapack_set::counts() const noexcept -> const kind_counts & {
    return impl_->stats.counts;
}

auto datapack_set::tags() const noexcept -> const tag_set & {
    return impl_->tags;
}

auto datapack_set::find(content_kind kind,
                        const resource_location &id) const noexcept
    -> const content_entry * {
    const auto by_kind{impl_->content.find(kind)};
    if (by_kind == impl_->content.end()) {
        return nullptr;
    }
    const auto found{by_kind->second.find(id)};
    return found == by_kind->second.end() ? nullptr : &found->second;
}

auto datapack_set::entries(content_kind kind) const
    -> std::vector<const content_entry *> {
    std::vector<const content_entry *> result;
    const auto by_kind{impl_->content.find(kind)};
    if (by_kind == impl_->content.end()) {
        return result;
    }
    result.reserve(by_kind->second.size());
    for (const auto &[id, entry] : by_kind->second) {
        static_cast<void>(id);
        result.push_back(&entry);
    }
    return result;
}

auto datapack_set::other_counts() const noexcept
    -> const std::map<std::string, std::size_t> & {
    return impl_->stats.other_kinds;
}

auto datapack_set::duplicates() const noexcept
    -> const std::vector<std::string> & {
    return impl_->duplicates;
}

auto datapack_set::layer_count() const noexcept -> std::size_t {
    return impl_->stats.layers.size();
}

auto datapack_set::validate() const -> validation_report {
    validation_report report;
    report.stats = impl_->stats;
    report.warnings = impl_->duplicates;

    // 1) tag references resolve within the same tag registry (vanilla
    // semantics: a `worldgen/biome` tag cannot reference a `worldgen/structure`
    // tag, while `#minecraft:is_deep_ocean` inside a `worldgen/biome` tag does
    // resolve because both share the multi-segment registry).
    for (const auto &[key_value, record] : impl_->tags.records_) {
        static_cast<void>(key_value);
        for (const auto &value : record.values) {
            if (!value.tag_reference) {
                continue;
            }
            if (impl_->tags.find(record.tag_type, value.id) != nullptr) {
                continue;
            }
            const std::string owner{tag_identity(record.tag_type, record.id)};
            std::string from{};
            if (!record.source_path.empty()) {
                from = " (from " + record.source_path + ")";
            }
            const std::string reference{"#" + value.id.to_string() +
                                        "' (registry '" + record.tag_type +
                                        "')"};
            if (value.required) {
                report.errors.push_back("tag '" + owner + "'" + from +
                                        " references missing tag '" +
                                        reference);
            } else {
                report.warnings.push_back("tag '" + owner + "'" + from +
                                          " optional reference '" + reference +
                                          " does not resolve to a loaded tag");
            }
        }
    }

    // 2) tag reference cycles (deterministic DFS over sorted tags/edges).
    // The walk is depth-capped: parsing already bounds JSON (128) and NBT (512)
    // nesting, so an untrusted datapack must not exhaust the stack with a long
    // tag -> tag chain (CWE-674). Reaching the cap abandons the walk and
    // reports a single error.
    constexpr std::size_t max_tag_reference_depth{1024};
    enum class mark : std::uint8_t { unvisited, active, done };
    std::map<const tag_record *, mark> marks;
    std::vector<const tag_record *> stack;
    std::set<std::string> reported_cycles;
    bool depth_exceeded{false};
    const auto visit = [&](const auto &self, const tag_record &node,
                           std::size_t depth) -> void {
        if (depth > max_tag_reference_depth) {
            depth_exceeded = true;
            report.errors.push_back(fmt::format(
                "tag reference chain exceeds the maximum depth of {}",
                max_tag_reference_depth));
            return;
        }
        marks[&node] = mark::active;
        stack.push_back(&node);
        std::vector<const tag_record *> references;
        for (const auto &value : node.values) {
            if (!value.tag_reference) {
                continue;
            }
            if (const auto *target{impl_->tags.find(node.tag_type, value.id)};
                target != nullptr) {
                references.push_back(target);
            }
        }
        std::sort(references.begin(), references.end(),
                  [](const tag_record *lhs, const tag_record *rhs) {
                      return lhs->id < rhs->id;
                  });
        const auto describe = [](const tag_record &record) {
            return tag_identity(record.tag_type, record.id);
        };
        for (const auto *target : references) {
            if (depth_exceeded) {
                break;
            }
            const auto state{marks.find(target)};
            if (state == marks.end() || state->second == mark::unvisited) {
                self(self, *target, depth + 1);
                continue;
            }
            if (state->second != mark::active) {
                continue;
            }
            // Back edge: stack[pos..] + target forms a cycle.
            const auto begin{std::find(stack.begin(), stack.end(), target)};
            std::string text;
            for (auto current = begin; current != stack.end(); ++current) {
                if (!text.empty()) {
                    text += " -> ";
                }
                text += describe(**current);
            }
            text += " -> " + describe(*target);
            if (reported_cycles.insert(text).second) {
                report.errors.push_back("tag reference cycle: " + text);
            }
        }
        stack.pop_back();
        marks[&node] = mark::done;
    };
    for (const auto &[key_value, record] : impl_->tags.records_) {
        static_cast<void>(key_value);
        if (depth_exceeded) {
            break;
        }
        if (!marks.contains(&record)) {
            visit(visit, record, 1);
        }
    }
    return report;
}

} // namespace refrain::data
