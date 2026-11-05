// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/content_pack.hpp>

#include <refrain/core/io.hpp>
#include <refrain/data/content_index.hpp>
#include <refrain/data/content_store.hpp>
#include <refrain/data/path.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::data {
namespace {

[[nodiscard]] auto fail(error_code code, std::string message) -> error {
    return make_error(code, message);
}

/// Prefix the diagnostic of `failure` while keeping its code (parse failures
/// must stay `parse_error`/`version_mismatch` for callers to branch on).
[[nodiscard]] auto wrap_error(error failure, std::string_view context)
    -> error {
    failure.message = std::string{context} + ": " + failure.message;
    return failure;
}

/// A composition id doubles as the resolution index file name, so it is
/// restricted to a safe, dot-prefix-free file name (no '/' or '\\', so `../`
/// cannot escape `content/resolutions`).
[[nodiscard]] auto is_safe_composition_name(std::string_view name) noexcept
    -> bool {
    if (name.empty() || name.size() > max_content_composition_bytes ||
        name.front() == '.') {
        return false;
    }
    for (const char character : name) {
        const bool allowed{(character >= 'A' && character <= 'Z') ||
                           (character >= 'a' && character <= 'z') ||
                           (character >= '0' && character <= '9') ||
                           character == '.' || character == '_' ||
                           character == '-'};
        if (!allowed) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] auto blob_table_path(std::string_view store_root) -> std::string {
    return join_path(join_path(store_root, "content"),
                     content_blob_table_filename);
}

[[nodiscard]] auto resolution_index_path(std::string_view store_root,
                                         std::string_view composition)
    -> std::string {
    return join_path(join_path(store_root, content_resolutions_directory),
                     std::string{composition} + ".idx");
}

/// Exact binary search over the path-sorted entry vector.
[[nodiscard]] auto find_entry(std::span<const pack_file_info> entries,
                              std::string_view path) noexcept
    -> const pack_file_info * {
    const auto match{
        std::lower_bound(entries.begin(), entries.end(), path,
                         [](const pack_file_info &entry, std::string_view key) {
                             return std::string_view{entry.path} < key;
                         })};
    if (match == entries.end() || std::string_view{match->path} != path) {
        return nullptr;
    }
    return &*match;
}

/// One composition of the content store exposed as a VFS pack.
///
/// `entries_`/`locations_` are parallel and sorted by path bytes; `read()`
/// re-reads the blob through its RFBT location (file-backed or in-segment), so
/// the pack owns no blob bytes.
class content_pack final : public pack {
  public:
    content_pack(content_store store, std::string label,
                 std::vector<pack_file_info> entries,
                 std::vector<blob_location> locations)
        : store_{std::move(store)}, label_{std::move(label)},
          entries_{std::move(entries)}, locations_{std::move(locations)} {}

    [[nodiscard]] auto format() const noexcept -> pack_format override {
        return pack_format::content;
    }
    [[nodiscard]] auto label() const noexcept -> std::string_view override {
        return label_;
    }
    [[nodiscard]] auto file_count() const noexcept -> std::size_t override {
        return entries_.size();
    }
    [[nodiscard]] auto entries() const noexcept
        -> std::span<const pack_file_info> override {
        return entries_;
    }
    [[nodiscard]] auto find(std::string_view path) const noexcept
        -> const pack_file_info * override {
        return find_entry(entries_, path);
    }
    [[nodiscard]] auto read(std::string_view path) const
        -> result<std::vector<std::byte>> override {
        const auto *entry{find(path)};
        if (entry == nullptr) {
            return std::unexpected{
                fail(error_code::not_found, "content pack '" + label_ +
                                                "' has no file '" +
                                                std::string{path} + "'")};
        }
        const auto index{static_cast<std::size_t>(entry - entries_.data())};
        auto bytes{store_.read(locations_[index])};
        if (!bytes) {
            auto error{std::move(bytes.error())};
            error.message = "content pack '" + label_ + "' file '" +
                            std::string{path} + "': " + error.message;
            // Keep store-level corruption as `parse_error`; everything else
            // (missing blob, host read failure) is an I/O failure for the
            // pack contract.
            if (error.code != error_code::parse_error) {
                error.code = error_code::io_error;
            }
            return std::unexpected{std::move(error)};
        }
        return bytes;
    }

  private:
    content_store store_;
    std::string label_;
    std::vector<pack_file_info> entries_;
    std::vector<blob_location> locations_;
};

/// Winner record of one location: where to read it and its logical size.
/// `record` is owned because a mapped index decodes records on demand rather
/// than exposing pointers into the mapping.
struct winner_entry {
    resolution_entry record{};
    blob_location location{};
    std::uint64_t size{0};
};

} // namespace

auto is_valid_content_composition(std::string_view composition) noexcept
    -> bool {
    return is_safe_composition_name(composition);
}

auto open_content_pack(std::string_view store_root,
                       std::string_view composition, std::string_view label)
    -> result<std::unique_ptr<pack>> {
    if (store_root.empty()) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "content pack store root must not be empty")};
    }
    if (!is_valid_utf8(store_root)) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "content pack store root is not valid UTF-8")};
    }
    if (!is_safe_composition_name(composition)) {
        return std::unexpected{fail(
            error_code::invalid_argument,
            "content pack composition must be a safe file name of at most " +
                std::to_string(max_content_composition_bytes) +
                " bytes ([A-Za-z0-9._-], not starting with '.')")};
    }

    auto store{content_store::open(store_root)};
    if (!store) {
        return std::unexpected{std::move(store.error())};
    }

    // The resolution index binds itself to the exact bytes of the blob table
    // (BLAKE3 prefix in its header), so the pair is validated fail-closed.
    // Both files are mapped read-only and validated eagerly by the mapped
    // views; the pack itself keeps only the winner records, so the mappings
    // (and the page cache) are released as soon as this function returns.
    const auto table_path{blob_table_path(store_root)};
    if (!core::io::exists(table_path)) {
        return std::unexpected{
            fail(error_code::not_found,
                 "content index does not exist: '" + table_path + "'")};
    }
    auto table{mapped_blob_table::open(table_path)};
    if (!table) {
        return std::unexpected{
            wrap_error(std::move(table.error()),
                       "content blob table '" + table_path + "'")};
    }

    const auto index_path{resolution_index_path(store_root, composition)};
    if (!core::io::exists(index_path)) {
        return std::unexpected{
            fail(error_code::not_found,
                 "content index does not exist: '" + index_path + "'")};
    }
    auto index{mapped_resolution_index::open(index_path, *table)};
    if (!index) {
        return std::unexpected{
            wrap_error(std::move(index.error()),
                       "content resolution index '" + index_path + "'")};
    }
    if (index->size() > vfs_max_files) {
        return std::unexpected{fail(error_code::parse_error,
                                    "content resolution index '" + index_path +
                                        "' has more than " +
                                        std::to_string(vfs_max_files) +
                                        " entries (refused before indexing)")};
    }

    // The pack exposes the resolved runtime view: exactly one winner per
    // location, selected by `resolution_precedes()` (the same order as
    // `resolve()`: higher priority, then smaller `blob_index`, then
    // lexicographically smaller source). RFRX is sorted by (location_hash,
    // priority desc, source_index, path), whose key does not include the blob,
    // so legacy/manual same-priority ties may list any blob first; this
    // comparison makes the pack winner independent of that record order.
    // `std::map` keeps the winners keyed by location, which also yields
    // byte-lexicographic path order for the pack entries (unlike
    // location_hash order).
    std::map<std::string, winner_entry> winners;
    for (std::size_t position{0}; position < index->size(); ++position) {
        auto record{index->at(position)};
        if (record.blob_index >= table->size()) {
            // Unreachable for a successfully validated index; kept as a guard
            // so the lookup below never reads past the table.
            return std::unexpected{
                fail(error_code::parse_error,
                     "content resolution index '" + index_path +
                         "' references a missing blob table record")};
        }
        const auto blob{table->at(record.blob_index)};
        const winner_entry candidate{
            .record = std::move(record),
            .location = blob_location_from_table_entry(blob),
            .size = blob.logical_size,
        };
        const auto current{winners.find(candidate.record.location)};
        if (current == winners.end()) {
            winners.emplace(candidate.record.location, candidate);
        } else if (resolution_precedes(candidate.record,
                                       current->second.record)) {
            current->second = candidate;
        }
    }

    std::vector<pack_file_info> entries;
    std::vector<blob_location> locations;
    entries.reserve(winners.size());
    locations.reserve(winners.size());
    for (auto &[path, winner] : winners) {
        entries.push_back(pack_file_info{.path = path, .size = winner.size});
        locations.push_back(winner.location);
    }

    std::string pack_label{};
    if (label.empty()) {
        pack_label = "content:";
        pack_label.append(composition);
    } else {
        pack_label.assign(label);
    }
    return std::unique_ptr<pack>{std::make_unique<content_pack>(
        std::move(*store), std::move(pack_label), std::move(entries),
        std::move(locations))};
}

} // namespace refrain::data
