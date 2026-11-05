// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/archive/manifest.hpp>

#include <refrain/core/io.hpp>

#include <simdjson.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace refrain::archive {

namespace {

[[nodiscard]] auto starts_with(std::string_view value,
                               std::string_view prefix) noexcept -> bool {
    return value.size() >= prefix.size() &&
           value.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] auto category_of(std::string_view path) noexcept
    -> std::string_view {
    if (starts_with(path, "data/")) {
        return "data";
    }
    if (starts_with(path, "assets/")) {
        return "assets";
    }
    if (starts_with(path, "META-INF/")) {
        return "meta";
    }
    return "other";
}

[[nodiscard]] auto file_name_of(std::string_view path) -> std::string {
    // Only the archive *file name* may reach the manifest. Host paths can
    // be Windows-style even when the tool runs on POSIX (e.g. tests, WSL or a
    // future launcher), so strip both separators instead of leaking
    // `C:\Users\me\client.jar` into the manifest.
    const auto separator{path.find_last_of("/\\")};
    return std::string{separator == std::string_view::npos
                           ? path
                           : path.substr(separator + 1)};
}

/// Portable duplicate detection. Windows/macOS default file systems are
/// case-insensitive, so `A.txt` and `a.txt` would extract onto the same file;
/// reject such archives regardless of the host so a manifest built here behaves
/// identically everywhere. ASCII-only fold keeps the result locale-independent.
[[nodiscard]] auto portable_name_key(std::string_view name) -> std::string {
    std::string key{name};
    for (auto &character : key) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return key;
}

} // namespace

auto filter_from_string(std::string_view name) noexcept -> extract_filter {
    if (name == "data") {
        return extract_filter::data;
    }
    if (name == "assets") {
        return extract_filter::assets;
    }
    return extract_filter::all;
}

auto to_string(extract_filter filter) noexcept -> std::string_view {
    switch (filter) {
    case extract_filter::data:
        return "data";
    case extract_filter::assets:
        return "assets";
    case extract_filter::all:
        break;
    }
    return "all";
}

auto matches_filter(std::string_view path, extract_filter filter) noexcept
    -> bool {
    switch (filter) {
    case extract_filter::all:
        return true;
    case extract_filter::data:
        return starts_with(path, "data/");
    case extract_filter::assets:
        return starts_with(path, "assets/");
    }
    return false;
}

auto build_manifest(const zip_reader &reader, extract_filter filter)
    -> result<data_manifest> {
    data_manifest manifest{};
    manifest.source = file_name_of(reader.archive_path());
    auto source_size{core::io::file_size(reader.archive_path())};
    if (!source_size) {
        return std::unexpected{source_size.error()};
    }
    manifest.source_size = *source_size;
    manifest.total_entries = reader.entries().size();

    std::unordered_set<std::string> seen{};
    seen.reserve(reader.entries().size());
    std::unordered_set<std::string> categories{};
    for (const auto &entry : reader.entries()) {
        if (entry.directory || !matches_filter(entry.name, filter)) {
            continue;
        }
        if (!safe_relative_path(entry.name)) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "extract: archive entry has an unsafe path '" +
                               entry.name + "' (zip-slip protection)")};
        }
        if (!seen.insert(portable_name_key(entry.name)).second) {
            return std::unexpected{make_error(
                error_code::already_exists,
                "extract: duplicate archive entry '" + entry.name +
                    "' (names that differ only by ASCII case collide on "
                    "case-insensitive file systems)")};
        }
        if (entry.size > max_entry_bytes) {
            return std::unexpected{make_error(
                error_code::parse_error,
                "extract: archive entry '" + entry.name + "' declares " +
                    std::to_string(entry.size) + " bytes, above the " +
                    std::to_string(max_entry_bytes) + "-byte safety limit")};
        }
        auto category{std::string{category_of(entry.name)}};
        categories.insert(category);
        manifest.entries.push_back(manifest_entry{
            .path = entry.name,
            .size = entry.size,
            .compressed_size = entry.compressed_size,
            .crc32 = entry.crc32,
            .category = std::move(category),
        });
        // Checked accumulation: a forged archive must not wrap the total.
        if (manifest.total_bytes >
            std::numeric_limits<std::uint64_t>::max() - entry.size) {
            return std::unexpected{make_error(
                error_code::parse_error,
                "extract: archive entry sizes overflow the manifest total")};
        }
        manifest.total_bytes += entry.size;
    }

    std::sort(manifest.entries.begin(), manifest.entries.end(),
              [](const manifest_entry &lhs, const manifest_entry &rhs) {
                  return lhs.path < rhs.path;
              });
    manifest.categories.assign(categories.begin(), categories.end());
    std::sort(manifest.categories.begin(), manifest.categories.end());
    manifest.total_files = manifest.entries.size();
    return manifest;
}

} // namespace refrain::archive

// ---------------------------------------------------------------------------
// JSON (simdjson builder)
// ---------------------------------------------------------------------------

namespace refrain::archive {

auto manifest_to_json(const data_manifest &manifest) -> result<std::string> {
    // simdjson owns the JSON layer (ADR update): the builder writes JSON
    // without a schema/reflection dependency, keeping the output stable and
    // allocation-light. Field order is part of the manifest format.
    simdjson::builder::string_builder builder;
    builder.start_object();
    builder.append_key_value("schema", manifest.schema);
    builder.append_comma();
    builder.append_key_value("source", manifest.source);
    builder.append_comma();
    builder.append_key_value("source_size", manifest.source_size);
    builder.append_comma();
    builder.append_key_value("generator", manifest.generator);
    builder.append_comma();
    builder.append_key_value("total_entries", manifest.total_entries);
    builder.append_comma();
    builder.append_key_value("total_files", manifest.total_files);
    builder.append_comma();
    builder.append_key_value("total_bytes", manifest.total_bytes);
    builder.append_comma();

    builder.escape_and_append_with_quotes("categories");
    builder.append_colon();
    builder.start_array();
    for (std::size_t index = 0; index < manifest.categories.size(); ++index) {
        builder.escape_and_append_with_quotes(manifest.categories[index]);
        if (index + 1 < manifest.categories.size()) {
            builder.append_comma();
        }
    }
    builder.end_array();
    builder.append_comma();

    builder.escape_and_append_with_quotes("entries");
    builder.append_colon();
    builder.start_array();
    for (std::size_t index = 0; index < manifest.entries.size(); ++index) {
        const auto &entry{manifest.entries[index]};
        builder.start_object();
        builder.append_key_value("path", entry.path);
        builder.append_comma();
        builder.append_key_value("size", entry.size);
        builder.append_comma();
        builder.append_key_value("compressed_size", entry.compressed_size);
        builder.append_comma();
        builder.append_key_value("crc32", entry.crc32);
        builder.append_comma();
        builder.append_key_value("category", entry.category);
        builder.end_object();
        if (index + 1 < manifest.entries.size()) {
            builder.append_comma();
        }
    }
    builder.end_array();
    builder.end_object();

    if (!builder.validate_unicode()) {
        return std::unexpected{make_error(
            error_code::parse_error,
            "extract: manifest JSON contains invalid UTF-8 (unsafe entry "
            "name)")};
    }
    std::string_view text{};
    if (auto error = builder.view().get(text); error != simdjson::SUCCESS) {
        return std::unexpected{
            make_error(error_code::out_of_memory,
                       std::string{"extract: JSON serialization failed: "} +
                           simdjson::error_message(error))};
    }
    return std::string{text};
}

} // namespace refrain::archive
