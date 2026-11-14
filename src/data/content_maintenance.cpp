// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/content_maintenance.hpp>

#include <refrain/core/io.hpp>
#include <refrain/data/content_pack.hpp>
#include <refrain/data/content_segment.hpp>
#include <refrain/data/content_store.hpp>
#include <refrain/data/json.hpp>
#include <refrain/data/path.hpp>
#include <refrain/platform/random_access_file.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace refrain::data {
namespace {

namespace io = core::io;

[[nodiscard]] auto wrap_error(error failure, std::string_view context)
    -> error {
    failure.message = std::string{context} + ": " + failure.message;
    return failure;
}

[[nodiscard]] auto content_directory(std::string_view store_root)
    -> std::string {
    return join_path(store_root, "content");
}

[[nodiscard]] auto content_blobs_directory(std::string_view store_root)
    -> std::string {
    return join_path(content_directory(store_root), "blobs");
}

[[nodiscard]] auto content_segments_directory(std::string_view store_root)
    -> std::string {
    return join_path(store_root, content_segment_directory);
}

/// `<id>` rendered as the 16 lowercase hex digits used by segment file names.
[[nodiscard]] auto segment_id_text(std::uint64_t id) -> std::string {
    return fmt::format("{:016x}", id);
}

/// Parse a 16-digit hex segment id (the file-name stem). Returns `nullopt` for
/// anything else (staging `.part` files and foreign entries are ignored).
[[nodiscard]] auto parse_segment_id(std::string_view hex)
    -> std::optional<std::uint64_t> {
    if (hex.size() != 16U) {
        return std::nullopt;
    }
    std::uint64_t value{0};
    for (const char character : hex) {
        std::uint64_t digit{0};
        if (character >= '0' && character <= '9') {
            digit = static_cast<std::uint64_t>(character - '0');
        } else if (character >= 'a' && character <= 'f') {
            digit = static_cast<std::uint64_t>(character - 'a') + 10U;
        } else {
            return std::nullopt;
        }
        value = (value << 4U) | digit;
    }
    return value;
}

/// Validate a composition id (also used as a file name) so it cannot escape the
/// resolutions directory or be empty. `content build` validates its own CLI
/// value with command-tailored messages; this is the library's defence in depth
/// for `publish_resolution`, which may also be called by the startup
/// bootstrap. The predicate is shared with the runtime open paths
/// (`content_pack`/`content_stream`), so a composition that `content build`
/// accepts can always be opened again.
[[nodiscard]] auto validate_composition(std::string_view composition)
    -> status {
    if (is_valid_content_composition(composition)) {
        return status_ok();
    }
    return std::unexpected{make_error(
        error_code::invalid_argument,
        fmt::format("the content composition must be a safe file name of at "
                    "most {} bytes made of letters, digits, '.', '_' and '-', "
                    "and must not start with '.'",
                    max_content_composition_bytes))};
}

// ---------------------------------------------------------------------------
// Persistent pending-delete queue
// ---------------------------------------------------------------------------

constexpr std::string_view pending_delete_queue_relative{
    "content/gc/pending.json"};
constexpr std::string_view pending_delete_schema{"refrain.content-pending/1"};
constexpr std::string_view pending_blob_prefix{"content/blobs/"};
constexpr std::string_view pending_segment_prefix{"content/segments/"};

/// Hard byte cap of `pending.json`. `max_pending_delete_entries` keeps a valid
/// queue around a few MiB; this second bound protects the loader from a
/// hostile file before JSON parsing allocates a DOM.
constexpr std::uint64_t max_pending_delete_queue_bytes{64ULL * 1024ULL *
                                                       1024ULL};

[[nodiscard]] auto pending_delete_queue_path(std::string_view store_root)
    -> std::string {
    return join_path(store_root, pending_delete_queue_relative);
}

[[nodiscard]] auto pending_kind_name(pending_delete::pending_delete_kind kind)
    -> std::string_view {
    switch (kind) {
    case pending_delete::pending_delete_kind::blob:
        return "blob";
    case pending_delete::pending_delete_kind::segment:
        return "segment";
    }
    return "blob";
}

/// Store-relative path of a standalone blob (`content/blobs/<hh>/<digest>`),
/// the same layout `content_store::path_for` writes.
[[nodiscard]] auto pending_blob_path(const digest32 &digest) -> std::string {
    const auto hex{to_hex(digest)};
    return join_path("content/blobs", join_path(hex.substr(0, 2), hex));
}

/// Store-relative path of a sealed segment (`content/segments/<id16>.rfseg`).
[[nodiscard]] auto pending_segment_path(std::uint64_t id) -> std::string {
    return join_path(content_segment_directory,
                     segment_id_text(id) +
                         std::string{content_segment_extension});
}

/// Validate a queue entry before it is written or after it was parsed. The
/// prefix/kind agreement is what keeps a queue from turning into a
/// delete-anything primitive; `is_safe_relative_path` rejects `..`, absolute
/// paths, backslashes, control bytes and empty segments.
[[nodiscard]] auto validate_pending_delete(const pending_delete &entry)
    -> status {
    if (!is_valid_utf8(entry.path)) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            fmt::format("path '{}' is not valid UTF-8", entry.path))};
    }
    if (!is_safe_relative_path(entry.path)) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            fmt::format("path '{}' is not a safe store-relative path",
                        entry.path))};
    }
    switch (entry.kind) {
    case pending_delete::pending_delete_kind::blob:
        if (!entry.path.starts_with(pending_blob_prefix)) {
            return std::unexpected{
                make_error(error_code::invalid_argument,
                           fmt::format("blob path '{}' is not under '{}'",
                                       entry.path, pending_blob_prefix))};
        }
        break;
    case pending_delete::pending_delete_kind::segment:
        if (!entry.path.starts_with(pending_segment_prefix)) {
            return std::unexpected{
                make_error(error_code::invalid_argument,
                           fmt::format("segment path '{}' is not under '{}'",
                                       entry.path, pending_segment_prefix))};
        }
        break;
    default:
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "unknown pending-delete kind")};
    }
    if (entry.size >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       fmt::format("path '{}' has an unrepresentable size {}",
                                   entry.path, entry.size))};
    }
    return status_ok();
}

[[nodiscard]] auto pending_queue_error(std::string_view queue_path,
                                       std::string detail) -> error {
    return make_error(
        error_code::parse_error,
        fmt::format("pending delete queue '{}': {}", queue_path, detail));
}

/// Deterministic UTF-8 JSON: sorted by path (the vector is sorted by the
/// caller), object keys sorted by the DOM codec, no timestamps.
[[nodiscard]] auto
pending_queue_text(const std::vector<pending_delete> &entries) -> std::string {
    json::value::array_type array;
    array.reserve(entries.size());
    for (const auto &entry : entries) {
        json::value::object_type object;
        object.emplace("attempts", json::value{entry.attempts});
        object.emplace("kind", json::value{pending_kind_name(entry.kind)});
        object.emplace("path", json::value{entry.path});
        object.emplace("size",
                       json::value{static_cast<std::int64_t>(entry.size)});
        array.emplace_back(json::value{std::move(object)});
    }
    json::value::object_type root;
    root.emplace("entries", json::value{std::move(array)});
    root.emplace("schema", json::value{pending_delete_schema});
    return json::value{std::move(root)}.dump_pretty(2) + "\n";
}

[[nodiscard]] auto parse_pending_queue(std::string_view text,
                                       std::string_view queue_path)
    -> result<std::vector<pending_delete>> {
    auto document{json::value::parse(text)};
    if (!document) {
        return std::unexpected{
            pending_queue_error(queue_path, document.error().message)};
    }
    if (!document->is_object()) {
        return std::unexpected{
            pending_queue_error(queue_path, "the document is not an object")};
    }
    auto root{document->as_object()};
    const auto &members{root->get()};
    if (members.size() != 2U || !members.contains("schema") ||
        !members.contains("entries")) {
        return std::unexpected{pending_queue_error(
            queue_path,
            "expected an object with exactly 'schema' and 'entries'")};
    }
    const auto &schema{members.find("schema")->second};
    auto schema_text{schema.as_string()};
    if (!schema_text) {
        return std::unexpected{
            pending_queue_error(queue_path, "'schema' is not a string")};
    }
    if (*schema_text != pending_delete_schema) {
        return std::unexpected{pending_queue_error(
            queue_path, fmt::format("unsupported schema '{}', expected '{}'",
                                    *schema_text, pending_delete_schema))};
    }
    const auto &entries_node{members.find("entries")->second};
    auto entries{entries_node.as_array()};
    if (!entries) {
        return std::unexpected{
            pending_queue_error(queue_path, "'entries' is not an array")};
    }
    if (entries->size() > max_pending_delete_entries) {
        return std::unexpected{pending_queue_error(
            queue_path,
            fmt::format("{} entries exceed the limit of {}", entries->size(),
                        max_pending_delete_entries))};
    }

    std::vector<pending_delete> result;
    result.reserve(entries->size());
    std::size_t index{0};
    for (const auto &element : *entries) {
        const auto origin{fmt::format("entry {}", index)};
        if (!element.is_object()) {
            return std::unexpected{
                pending_queue_error(queue_path, origin + " is not an object")};
        }
        auto object{element.as_object()};
        const auto &fields{object->get()};
        if (fields.size() != 4U || !fields.contains("path") ||
            !fields.contains("kind") || !fields.contains("size") ||
            !fields.contains("attempts")) {
            return std::unexpected{pending_queue_error(
                queue_path, origin +
                                " must contain exactly 'path', 'kind', 'size' "
                                "and 'attempts'")};
        }

        auto path{fields.find("path")->second.as_string()};
        if (!path) {
            return std::unexpected{pending_queue_error(
                queue_path, origin + ": 'path' is not a string")};
        }
        auto kind_text{fields.find("kind")->second.as_string()};
        if (!kind_text) {
            return std::unexpected{pending_queue_error(
                queue_path, origin + ": 'kind' is not a string")};
        }
        pending_delete entry{};
        entry.path = std::string{path->data(), path->size()};
        if (*kind_text == "blob") {
            entry.kind = pending_delete::pending_delete_kind::blob;
        } else if (*kind_text == "segment") {
            entry.kind = pending_delete::pending_delete_kind::segment;
        } else {
            return std::unexpected{pending_queue_error(
                queue_path, origin + ": 'kind' must be 'blob' or 'segment'")};
        }
        if (auto valid{validate_pending_delete(entry)}; !valid) {
            return std::unexpected{pending_queue_error(
                queue_path,
                fmt::format("{}: {}", origin, valid.error().message))};
        }

        auto size{fields.find("size")->second.as_int64()};
        if (!size || *size < 0) {
            return std::unexpected{pending_queue_error(
                queue_path,
                origin + ": 'size' must be a non-negative integer")};
        }
        entry.size = static_cast<std::uint64_t>(*size);
        auto attempts{fields.find("attempts")->second.as_int64()};
        if (!attempts || *attempts < 0 ||
            *attempts > static_cast<std::int64_t>(
                            std::numeric_limits<std::uint32_t>::max())) {
            return std::unexpected{pending_queue_error(
                queue_path,
                origin + ": 'attempts' must be a 32-bit unsigned integer")};
        }
        entry.attempts = static_cast<std::uint32_t>(*attempts);

        if (!result.empty() && !(result.back().path < entry.path)) {
            return std::unexpected{pending_queue_error(
                queue_path,
                origin + ": entries must be sorted by path bytes and unique")};
        }
        result.push_back(std::move(entry));
        ++index;
    }
    return result;
}

/// Live targets currently recorded by `blobs.idx`. A drain skips an entry that
/// the table references again (a concurrent install/build can re-add a blob
/// after it was queued); such a queue entry is obsolete and is dropped. A
/// missing table means "nothing is recorded", so every queued target is fair
/// game. A damaged table fails the drain closed before any unlink.
struct pending_delete_guard {
    std::set<std::string> blob_paths{};
    std::set<std::uint64_t> segment_ids{};
};

[[nodiscard]] auto load_pending_delete_guard(std::string_view store_root)
    -> result<std::optional<pending_delete_guard>> {
    if (!io::exists(content_blob_table_path(store_root))) {
        return std::nullopt;
    }
    auto table{load_blob_table(store_root)};
    if (!table) {
        return std::unexpected{std::move(table.error())};
    }
    pending_delete_guard guard;
    for (const auto &entry : table->entries) {
        if ((entry.flags & blob_flags_in_segment) != 0U) {
            guard.segment_ids.insert(entry.segment_id);
        } else {
            guard.blob_paths.insert(pending_blob_path(entry.digest));
        }
    }
    return guard;
}

[[nodiscard]] auto pending_entry_is_live(const pending_delete_guard &guard,
                                         const pending_delete &entry) -> bool {
    if (entry.kind == pending_delete::pending_delete_kind::blob) {
        return guard.blob_paths.contains(entry.path);
    }
    const auto name{filename(entry.path)};
    if (!name.ends_with(content_segment_extension)) {
        return false;
    }
    const auto id{parse_segment_id(
        name.substr(0, name.size() - content_segment_extension.size()))};
    return id && guard.segment_ids.contains(*id);
}

/// Remove one queued target. Missing targets are success (idempotent retry
/// after a crash between unlink and queue update); directories are refused so a
/// foreign filesystem object at a queued path can never be recursively
/// removed.
[[nodiscard]] auto remove_pending_target(std::string_view store_root,
                                         const pending_delete &entry)
    -> status {
    const auto path{join_path(store_root, entry.path)};
    auto info{io::status_at(path)};
    if (!info) {
        return std::unexpected{std::move(info.error())};
    }
    if (!info->exists) {
        return status_ok();
    }
    if (info->directory) {
        return std::unexpected{make_error(
            error_code::io_error,
            fmt::format("refusing to delete '{}' because it is a directory",
                        path))};
    }
    std::error_code error;
    const auto removed{
        std::filesystem::remove(std::filesystem::path{path}, error)};
    if (error) {
        return std::unexpected{
            make_error(error_code::io_error,
                       fmt::format("'{}': {}", path, error.message()))};
    }
    // `removed == false` without an error means another drainer already
    // unlinked it; the target is gone, which is the desired state.
    (void)removed;
    return status_ok();
}

struct physical_blob {
    digest32 digest{};
    std::string path{};
    std::uint64_t size{0};
    std::filesystem::file_time_type modified{};
};

[[nodiscard]] auto scan_physical_blobs(std::string_view store_root)
    -> result<std::vector<physical_blob>> {
    std::vector<physical_blob> blobs;
    const auto root{content_blobs_directory(store_root)};
    if (!io::is_directory(root)) {
        return blobs;
    }
    auto shards{io::list_directory(root)};
    if (!shards) {
        return std::unexpected{shards.error()};
    }
    for (const auto &shard : *shards) {
        if (shard.size() != 2U) {
            continue;
        }
        const auto directory{join_path(root, shard)};
        if (!io::is_directory(directory)) {
            continue;
        }
        auto names{io::list_directory(directory)};
        if (!names) {
            return std::unexpected{names.error()};
        }
        for (const auto &name : *names) {
            auto digest{parse_digest(name)};
            if (!digest) {
                continue; // stale temporary/unknown file; only CAS names count
            }
            const auto path{join_path(directory, name)};
            if (!io::is_regular_file(path)) {
                continue;
            }
            auto size{io::file_size(path)};
            if (!size) {
                return std::unexpected{size.error()};
            }
            std::error_code error;
            const auto modified{std::filesystem::last_write_time(
                std::filesystem::path{path}, error)};
            blobs.push_back(physical_blob{
                .digest = *digest,
                .path = path,
                .size = *size,
                .modified =
                    error ? std::filesystem::file_time_type::max() : modified,
            });
        }
    }
    std::sort(blobs.begin(), blobs.end(),
              [](const physical_blob &lhs, const physical_blob &rhs) {
                  return lhs.digest < rhs.digest;
              });
    return blobs;
}

/// Rewrite the live blobs of the candidate segments into fresh segments. Each
/// live blob is read through its RFBT record (full digest verification) and
/// appended unchanged to a new segment, using the same 32 MiB soft target and
/// 8-byte alignment as `batch`; zero-byte blobs are preserved. Old segments
/// are left untouched: the caller publishes the new table/indexes first and
/// only then removes them.
///
/// On success `relocated` maps every moved digest to its new
/// `(segment id, body offset)` and the returned count is the number of new
/// segments. On failure nothing is published (the caller has not touched index
/// files) and any sealed new segment is an orphan for the next GC.
[[nodiscard]] auto compact_content_segments(
    const content_store &store,
    const std::vector<const content_segment_file *> &candidates,
    const std::map<std::uint64_t, std::vector<const blob_table_entry *>>
        &live_entries,
    std::map<digest32, std::pair<std::uint64_t, std::uint32_t>> &relocated)
    -> result<std::size_t> {
    const auto segments_dir{content_segments_directory(store.root())};
    std::optional<segment_writer> writer;
    std::vector<std::pair<digest32, std::uint32_t>> pending;
    std::size_t published{0};

    const auto open_writer = [&writer, &segments_dir]() -> status {
        auto created{
            segment_writer::create(segments_dir, content_segment_target_bytes)};
        if (!created) {
            return std::unexpected{created.error()};
        }
        writer.emplace(std::move(*created));
        return status_ok();
    };
    // Sealing is the point where the new segment id (a hash of its body) is
    // known, so the pending records are stamped here.
    const auto seal_pending = [&]() -> status {
        if (!writer) {
            return status_ok();
        }
        auto header{writer->seal()};
        if (!header) {
            return std::unexpected{std::move(header.error())};
        }
        for (const auto &[digest, offset] : pending) {
            relocated[digest] = {header->id, offset};
        }
        pending.clear();
        writer.reset();
        ++published;
        return status_ok();
    };

    for (const auto *segment : candidates) {
        const auto found{live_entries.find(segment->id)};
        if (found == live_entries.end()) {
            continue;
        }
        auto ordered{found->second};
        std::sort(ordered.begin(), ordered.end(),
                  [](const blob_table_entry *lhs, const blob_table_entry *rhs) {
                      return lhs->segment_offset < rhs->segment_offset;
                  });
        for (const auto *entry : ordered) {
            if ((entry->flags & blob_flags_packed) != 0U) {
                return std::unexpected{make_error(
                    error_code::parse_error,
                    "failed to compact the gc segment " +
                        segment_id_text(segment->id) + " because blob " +
                        to_hex(entry->digest).substr(0, 16) + " is packed")};
            }
            auto bytes{store.read(blob_location_from_table_entry(*entry))};
            if (!bytes) {
                return std::unexpected{wrap_error(
                    bytes.error(), "compacting blob " + to_hex(entry->digest))};
            }
            if (bytes->size() != entry->stored_size) {
                return std::unexpected{make_error(
                    error_code::parse_error,
                    "failed to compact blob " +
                        to_hex(entry->digest).substr(0, 16) +
                        " because it read " + std::to_string(bytes->size()) +
                        " bytes but the record says " +
                        std::to_string(entry->stored_size))};
            }
            if (!writer) {
                if (auto opened{open_writer()}; !opened) {
                    return std::unexpected{std::move(opened.error())};
                }
            } else if (writer->blob_count() != 0U) {
                const auto remainder{writer->data_bytes() %
                                     content_segment_alignment};
                const auto padding{remainder == 0U
                                       ? 0U
                                       : content_segment_alignment - remainder};
                const auto projected{writer->data_bytes() + padding +
                                     static_cast<std::uint64_t>(bytes->size())};
                if (projected > content_segment_target_bytes) {
                    if (auto sealed{seal_pending()}; !sealed) {
                        return std::unexpected{std::move(sealed.error())};
                    }
                    if (auto opened{open_writer()}; !opened) {
                        return std::unexpected{std::move(opened.error())};
                    }
                }
            }
            auto offset{writer->append(*bytes)};
            if (!offset) {
                return std::unexpected{std::move(offset.error())};
            }
            pending.emplace_back(entry->digest, *offset);
        }
    }
    if (auto sealed{seal_pending()}; !sealed) {
        return std::unexpected{std::move(sealed.error())};
    }
    return published;
}

} // namespace

auto content_blob_table_path(std::string_view store_root) -> std::string {
    return join_path(content_directory(store_root),
                     content_blob_table_filename);
}

auto content_resolution_path(std::string_view store_root,
                             std::string_view composition) -> std::string {
    return join_path(join_path(store_root, content_resolutions_directory),
                     std::string{composition} + ".idx");
}

auto list_resolution_paths(std::string_view store_root)
    -> result<std::vector<std::string>> {
    std::vector<std::string> paths;
    const auto directory{join_path(store_root, content_resolutions_directory)};
    if (!io::is_directory(directory)) {
        return paths;
    }
    auto names{io::list_directory(directory)};
    if (!names) {
        return std::unexpected{names.error()};
    }
    for (const auto &name : *names) {
        if (name.size() <= 4U ||
            name.compare(name.size() - 4U, 4U, ".idx") != 0) {
            continue;
        }
        paths.push_back(join_path(directory, name));
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

auto load_blob_table(std::string_view store_root) -> result<blob_table_view> {
    const auto path{content_blob_table_path(store_root)};
    auto bytes{io::read_file(path)};
    if (!bytes) {
        return std::unexpected{wrap_error(std::move(bytes.error()), path)};
    }
    auto parsed{parse_blob_table(*bytes)};
    if (!parsed) {
        return std::unexpected{wrap_error(std::move(parsed.error()), path)};
    }
    return parsed;
}

auto load_resolution_indexes(std::string_view store_root,
                             const blob_table_view &table,
                             std::string_view skip_path)
    -> result<std::vector<resolution_index_file>> {
    auto paths{list_resolution_paths(store_root)};
    if (!paths) {
        return std::unexpected{paths.error()};
    }
    std::vector<resolution_index_file> indexes;
    indexes.reserve(paths->size());
    for (const auto &path : *paths) {
        if (path == skip_path) {
            continue;
        }
        auto bytes{io::read_file(path)};
        if (!bytes) {
            return std::unexpected{wrap_error(std::move(bytes.error()), path)};
        }
        auto parsed{parse_resolution_index(*bytes, table)};
        if (!parsed) {
            return std::unexpected{wrap_error(std::move(parsed.error()), path)};
        }
        indexes.push_back(
            resolution_index_file{.path = path, .view = std::move(*parsed)});
    }
    return indexes;
}

auto scan_content_segments(std::string_view store_root)
    -> result<std::vector<content_segment_file>> {
    std::vector<content_segment_file> segments;
    const auto directory{content_segments_directory(store_root)};
    if (!io::is_directory(directory)) {
        return segments;
    }
    auto names{io::list_directory(directory)};
    if (!names) {
        return std::unexpected{names.error()};
    }
    for (const auto &name : *names) {
        if (!name.ends_with(content_segment_extension)) {
            continue;
        }
        auto id{parse_segment_id(std::string_view{name}.substr(
            0, name.size() - content_segment_extension.size()))};
        if (!id) {
            continue;
        }
        const auto path{join_path(directory, name)};
        if (!io::is_regular_file(path)) {
            continue;
        }
        auto size{io::file_size(path)};
        if (!size) {
            return std::unexpected{size.error()};
        }
        content_segment_file segment{
            .id = *id,
            .path = path,
            .file_size = *size,
        };
        std::error_code error;
        segment.modified = std::filesystem::last_write_time(
            std::filesystem::path{path}, error);
        if (error) {
            segment.modified = std::filesystem::file_time_type::max();
        }
        auto reader{segment_reader::open(path)};
        if (reader) {
            segment.header_ok = true;
            segment.data_bytes = reader->header().data_bytes;
            segment.blob_count = reader->header().blob_count;
        }
        segments.push_back(std::move(segment));
    }
    std::sort(segments.begin(), segments.end(),
              [](const content_segment_file &lhs,
                 const content_segment_file &rhs) { return lhs.id < rhs.id; });
    return segments;
}

auto load_pending_deletes(std::string_view store_root)
    -> result<std::vector<pending_delete>> {
    if (store_root.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "the content store root must not be empty")};
    }
    const auto queue_path{pending_delete_queue_path(store_root)};
    auto info{io::status_at(queue_path)};
    if (!info) {
        return std::unexpected{std::move(info.error())};
    }
    if (info->symlink || (info->exists && !info->regular)) {
        return std::unexpected{pending_queue_error(
            queue_path,
            "the path is not a regular file, symlinks and directories are "
            "refused")};
    }
    if (!info->exists) {
        return std::vector<pending_delete>{};
    }
    auto size{io::file_size(queue_path)};
    if (!size) {
        return std::unexpected{std::move(size.error())};
    }
    if (*size > max_pending_delete_queue_bytes) {
        return std::unexpected{pending_queue_error(
            queue_path, fmt::format("{} bytes exceed the {} byte limit", *size,
                                    max_pending_delete_queue_bytes))};
    }
    auto text{
        io::read_text(queue_path, {.max_bytes = max_pending_delete_queue_bytes,
                                   .allow_symlink = false})};
    if (!text) {
        return std::unexpected{wrap_error(std::move(text.error()), queue_path)};
    }
    return parse_pending_queue(*text, queue_path);
}

auto enqueue_pending_deletes(std::string_view store_root,
                             std::span<const pending_delete> entries)
    -> status {
    if (store_root.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "the content store root must not be empty")};
    }
    // A corrupt queue must never be replaced by a partial merge: load fails
    // closed and the original file stays for diagnosis/manual repair.
    auto existing{load_pending_deletes(store_root)};
    if (!existing) {
        return std::unexpected{std::move(existing.error())};
    }
    std::map<std::string, pending_delete> merged;
    for (auto &entry : *existing) {
        merged.emplace(entry.path, std::move(entry));
    }
    bool changed{false};
    for (const auto &entry : entries) {
        if (auto valid{validate_pending_delete(entry)}; !valid) {
            auto failure{std::move(valid.error())};
            failure.message =
                "invalid pending delete entry: " + failure.message;
            return std::unexpected{std::move(failure)};
        }
        auto found{merged.find(entry.path)};
        if (found == merged.end()) {
            merged.emplace(entry.path, entry);
            changed = true;
            continue;
        }
        // Re-planning an already queued path must not reset its retry counter;
        // max() also makes duplicates inside one batch deterministic.
        auto &current{found->second};
        if (entry.attempts > current.attempts) {
            current.attempts = entry.attempts;
            changed = true;
        }
        if (entry.size > current.size) {
            current.size = entry.size;
            changed = true;
        }
    }
    if (merged.empty()) {
        return status_ok();
    }
    if (merged.size() > max_pending_delete_entries) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            fmt::format("the pending delete queue would hold {} entries, over "
                        "the limit of {}",
                        merged.size(), max_pending_delete_entries))};
    }
    // No-op when the queue already holds exactly these entries; this avoids
    // rewriting/fsyncing an unchanged queue on every mark-sweep.
    if (!changed && !existing->empty()) {
        return status_ok();
    }

    std::vector<pending_delete> sorted;
    sorted.reserve(merged.size());
    for (auto &pair : merged) {
        sorted.push_back(std::move(pair.second));
    }
    const auto queue_path{pending_delete_queue_path(store_root)};
    const auto parent{parent_path(queue_path)};
    if (!parent.empty()) {
        if (auto created{io::create_directories(std::string{parent})};
            !created) {
            return std::unexpected{std::move(created.error())};
        }
    }
    return io::atomic_write_text(queue_path, pending_queue_text(sorted));
}

auto drain_pending_deletes(std::string_view store_root, std::uint64_t budget_ms,
                           const std::function<void(std::string_view)> &log)
    -> result<pending_delete_report> {
    if (store_root.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "the content store root must not be empty")};
    }
    auto loaded{load_pending_deletes(store_root)};
    if (!loaded) {
        return std::unexpected{std::move(loaded.error())};
    }
    pending_delete_report report;
    report.remaining = loaded->size();
    if (loaded->empty()) {
        return report;
    }
    // Never delete a path the current table records again. The guard is read
    // once per pass (a later install can still race the unlink; the server is
    // expected to own the store while running).
    auto guard{load_pending_delete_guard(store_root)};
    if (!guard) {
        return std::unexpected{std::move(guard.error())};
    }
    const pending_delete_guard *live_guard{guard->has_value() ? &**guard
                                                              : nullptr};

    const auto emit = [&log](std::string line) {
        if (log) {
            log(line);
        }
    };
    const auto started{std::chrono::steady_clock::now()};
    const auto budget_expired = [budget_ms, started] {
        if (budget_ms == 0) {
            return false;
        }
        const auto elapsed{std::chrono::steady_clock::now() - started};
        return std::chrono::duration<double, std::milli>(elapsed).count() >=
               static_cast<double>(budget_ms);
    };

    std::vector<pending_delete> remaining;
    remaining.reserve(loaded->size());
    bool changed{false};
    bool exhausted{false};
    for (const auto &entry : *loaded) {
        const bool is_blob{entry.kind ==
                           pending_delete::pending_delete_kind::blob};
        if (live_guard != nullptr &&
            pending_entry_is_live(*live_guard, entry)) {
            ++report.skipped_referenced;
            changed = true;
            emit(fmt::format(
                "failed to delete '{}' because blobs.idx references it again, "
                "dropping the stale queue entry",
                entry.path));
            continue;
        }
        if (entry.attempts >= max_pending_delete_attempts) {
            ++report.failed_stuck;
            remaining.push_back(entry);
            continue;
        }
        if (exhausted || budget_expired()) {
            exhausted = true;
            report.budget_exhausted = true;
            ++report.deferred;
            report.deferred_bytes += entry.size;
            if (is_blob) {
                ++report.deferred_blobs;
                report.deferred_blob_bytes += entry.size;
            } else {
                ++report.deferred_segments;
                report.deferred_segment_bytes += entry.size;
            }
            remaining.push_back(entry);
            continue;
        }
        auto removed{remove_pending_target(store_root, entry)};
        if (!removed) {
            ++report.failed;
            report.failed_bytes += entry.size;
            if (is_blob) {
                ++report.failed_blobs;
                report.failed_blob_bytes += entry.size;
            } else {
                ++report.failed_segments;
                report.failed_segment_bytes += entry.size;
            }
            auto kept{entry};
            if (kept.attempts < max_pending_delete_attempts) {
                ++kept.attempts;
            }
            if (kept.attempts >= max_pending_delete_attempts) {
                ++report.failed_stuck;
            }
            remaining.push_back(std::move(kept));
            changed = true;
            emit(fmt::format("failed to delete '{}': {}", entry.path,
                             removed.error().message));
            continue;
        }
        ++report.deleted;
        report.deleted_bytes += entry.size;
        if (is_blob) {
            ++report.deleted_blobs;
            report.deleted_blob_bytes += entry.size;
        } else {
            ++report.deleted_segments;
            report.deleted_segment_bytes += entry.size;
        }
        changed = true;
    }
    report.remaining = remaining.size();
    if (report.skipped_referenced != 0U) {
        emit(fmt::format("dropped {} stale pending deletes",
                         report.skipped_referenced));
    }
    if (report.failed_stuck != 0U) {
        emit(fmt::format(
            "failed to delete {} pending deletes because they reached the "
            "retry limit of {}, kept for manual repair",
            report.failed_stuck, max_pending_delete_attempts));
    }
    if (report.deleted != 0U) {
        emit(fmt::format("deleted {} pending files totaling {} bytes",
                         report.deleted, report.deleted_bytes));
    }
    if (report.deferred != 0U) {
        emit(fmt::format(
            "deferred {} pending files totaling {} bytes because the gc "
            "budget ran out",
            report.deferred, report.deferred_bytes));
    }
    if (report.failed != 0U) {
        emit(fmt::format(
            "kept {} pending files totaling {} bytes for retry because "
            "deleting them failed",
            report.failed, report.failed_bytes));
    }

    const auto queue_path{pending_delete_queue_path(store_root)};
    if (remaining.empty()) {
        if (changed) {
            std::error_code error;
            const auto removed{std::filesystem::remove(
                std::filesystem::path{queue_path}, error)};
            if (error) {
                emit(fmt::format("failed to remove the empty pending delete "
                                 "queue '{}': {}",
                                 queue_path, error.message()));
            } else if (removed) {
                // The rename/create fsync in atomic_write_file covers queue
                // updates; the empty-queue unlink needs its own directory
                // flush. Best effort: the queue is already logically empty.
                const auto directory{parent_path(queue_path)};
                if (!directory.empty()) {
                    if (auto synced{platform::file::sync_directory(
                            std::string{directory})};
                        !synced) {
                        emit(fmt::format(
                            "failed to flush the directory '{}': {}", directory,
                            synced.error().message));
                    }
                }
            }
        }
        return report;
    }
    if (changed) {
        if (auto written{io::atomic_write_text(queue_path,
                                               pending_queue_text(remaining))};
            !written) {
            return std::unexpected{
                wrap_error(std::move(written.error()), queue_path)};
        }
    }
    return report;
}

auto publish_resolution(std::string_view store_root,
                        const content_publish_options &options) -> status {
    if (store_root.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "the content store root must not be empty")};
    }
    if (auto valid{validate_composition(options.composition)}; !valid) {
        return valid;
    }
    auto table{load_blob_table(store_root)};
    if (!table) {
        return std::unexpected{std::move(table.error())};
    }
    const auto index_path{
        content_resolution_path(store_root, options.composition)};

    // A published index owns its RFBT refcounts. Changing a refcount changes
    // the `blobs.idx` bytes, and every RFRX binds to those bytes (BLAKE3
    // prefix), so publishing one composition must re-encode the table and
    // every other index in the same transaction. Leaving refcounts stale would
    // let the table report 0 references while an index references the blobs.
    auto others{load_resolution_indexes(store_root, *table, index_path)};
    if (!others) {
        return std::unexpected{std::move(others.error())};
    }
    std::map<digest32, std::uint64_t> counts;
    for (const auto &index : *others) {
        for (const auto &entry : index.view.entries) {
            ++counts[table->entries[entry.blob_index].digest];
        }
    }
    for (const auto &entry : options.entries) {
        ++counts[entry.blob];
    }
    std::vector<blob_table_entry> next_entries{table->entries.begin(),
                                               table->entries.end()};
    for (auto &entry : next_entries) {
        const auto found{counts.find(entry.digest)};
        const auto count{found == counts.end() ? 0U : found->second};
        if (count > std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected{
                make_error(error_code::invalid_argument,
                           "the blob refcount exceeds the 32-bit field")};
        }
        entry.refcount = static_cast<std::uint32_t>(count);
    }
    auto next_table_bytes{build_blob_table(next_entries)};
    if (!next_table_bytes) {
        return std::unexpected{std::move(next_table_bytes.error())};
    }
    auto next_table{parse_blob_table(*next_table_bytes)};
    if (!next_table) {
        return std::unexpected{std::move(next_table.error())};
    }

    // Encode every index against the new table first (all bytes in memory),
    // then publish the table and the indexes: a crash in between leaves the
    // old indexes fail-closed on the prefix check, never a wrong resolution.
    std::vector<std::pair<std::string, std::vector<std::byte>>> rebuilt;
    rebuilt.reserve(others->size());
    for (const auto &index : *others) {
        std::vector<resolution_input> inputs;
        inputs.reserve(index.view.entries.size());
        for (const auto &entry : index.view.entries) {
            inputs.push_back(resolution_input{
                .location = entry.location,
                .source = entry.source,
                .priority = entry.priority,
                .blob = table->entries[entry.blob_index].digest,
                .flags = 0,
            });
        }
        auto bytes{build_resolution_index(inputs, *next_table)};
        if (!bytes) {
            return std::unexpected{std::move(bytes.error())};
        }
        rebuilt.emplace_back(index.path, std::move(*bytes));
    }
    auto bytes{build_resolution_index(options.entries, *next_table)};
    if (!bytes) {
        return std::unexpected{std::move(bytes.error())};
    }

    const auto table_path{content_blob_table_path(store_root)};
    if (auto written{io::atomic_write_file(table_path, *next_table_bytes)};
        !written) {
        return std::unexpected{std::move(written.error())};
    }
    for (const auto &[path, index_bytes] : rebuilt) {
        if (auto written{publish_resolution_bytes(path, index_bytes)};
            !written) {
            return std::unexpected{std::move(written.error())};
        }
    }
    return publish_resolution_bytes(index_path, *bytes);
}

auto publish_resolution_bytes(std::string_view index_path,
                              std::span<const std::byte> bytes) -> status {
    if (index_path.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "the resolution index path must not be empty")};
    }
    const auto parent{parent_path(index_path)};
    if (!parent.empty()) {
        if (auto created{io::create_directories(parent)}; !created) {
            return std::unexpected{std::move(created.error())};
        }
    }
    // Fingerprint skip: publishing the same bytes is a common no-op (startup
    // rebuilds the same composition). Avoid rewriting/re-fsyncing it; keep the
    // existing file as the source of truth.
    if (io::exists(index_path)) {
        auto existing{io::read_file(index_path)};
        if (!existing) {
            return std::unexpected{std::move(existing.error())};
        }
        if (std::ranges::equal(*existing, bytes)) {
            return status_ok();
        }
    }
    return io::atomic_write_file(index_path, bytes);
}

auto collect_garbage(std::string_view store_root, const gc_options &options,
                     const std::function<void(std::string_view)> &log)
    -> result<gc_report> {
    if (store_root.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "the content store root must not be empty")};
    }
    if (options.keep_days < 0) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "--keep-days must not be negative")};
    }
    // Reject rather than clamp: `now - hours(24 * keep_days)` overflows the
    // filesystem clock's int64 nanoseconds above ~106,751 days (signed-overflow
    // UB) and the wrapped cutoff silently disables the retention window.
    if (options.keep_days > max_gc_keep_days) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       fmt::format("--keep-days {} exceeds the maximum of {}",
                                   options.keep_days, max_gc_keep_days))};
    }
    const auto emit = [&log](std::string line) {
        if (log) {
            log(line);
        }
    };
    gc_report report;
    report.applied = options.apply;

    // An apply is a queue writer. Validate the existing queue before publishing
    // any table/index (or sealing new compaction segments): a corrupt/foreign
    // queue fails closed without touching the store, and the operator keeps the
    // damaged file for diagnosis. The dry run does not need the queue.
    if (options.apply) {
        auto existing_queue{load_pending_deletes(store_root)};
        if (!existing_queue) {
            return std::unexpected{std::move(existing_queue.error())};
        }
    }

    auto table{load_blob_table(store_root)};
    if (!table) {
        return std::unexpected{std::move(table.error())};
    }
    report.table_blobs = table->entries.size();
    auto indexes{load_resolution_indexes(store_root, *table)};
    if (!indexes) {
        return std::unexpected{std::move(indexes.error())};
    }

    // Root set: every RFRX reference plus the caller-supplied roots (receipt
    // files for the CLI). The stored refcount is only a hint; this mark phase
    // is authoritative.
    std::map<digest32, std::uint64_t> root_counts;
    for (const auto &index : *indexes) {
        for (const auto &entry : index.view.entries) {
            ++root_counts[table->entries[entry.blob_index].digest];
            ++report.root_index_references;
        }
    }
    for (const auto &digest : options.extra_roots) {
        ++root_counts[digest];
        ++report.root_extra;
    }
    report.root_blobs = root_counts.size();
    emit(
        fmt::format("the gc roots are {} index references and {} receipt files",
                    report.root_index_references, report.root_extra));

    // No root at all would make every record look unreferenced; an --apply
    // rebuild would wipe a store whose indexes/receipts were never published.
    // Fail closed instead (a receipt alone is enough to permit apply).
    if (options.apply && root_counts.empty() && !table->entries.empty()) {
        return std::unexpected{make_error(
            error_code::not_found,
            "failed to rebuild the blobs.idx because there are no gc roots, "
            "no resolution indexes and no install receipts")};
    }
    // Opening a content_store ensures <root>/content/blobs exists. A dry run
    // must not create anything, so it only opens the handle when the directory
    // is already there (opening then has no side effect) or when applying; a
    // dry run without the directory has no standalone blob to inspect, and an
    // in-segment root cannot appear without a table record (RFBT is a
    // segment's only directory).
    std::optional<content_store> store;
    if (options.apply ||
        io::is_directory(content_blobs_directory(store_root))) {
        auto opened{content_store::open(store_root)};
        if (!opened) {
            return std::unexpected{opened.error()};
        }
        store.emplace(std::move(*opened));
    }

    // The rebuilt table keeps exactly the root blobs (RFRX + receipt files),
    // with refcounts recomputed from that set; the stored refcount is only a
    // hint. A root digest without a table record falls back to the physical
    // probe below.
    std::map<digest32, blob_table_entry> previous_entries;
    for (const auto &entry : table->entries) {
        previous_entries.emplace(entry.digest, entry);
    }
    std::vector<blob_table_entry> kept_entries;
    kept_entries.reserve(root_counts.size());
    std::vector<digest32> missing_blobs;
    for (const auto &[digest, count] : root_counts) {
        if (count > std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected{
                make_error(error_code::invalid_argument,
                           "the blob refcount exceeds the 32-bit field")};
        }
        blob_table_entry entry{};
        entry.digest = digest;
        entry.refcount = static_cast<std::uint32_t>(count);
        const auto previous{previous_entries.find(digest)};
        if (previous != previous_entries.end()) {
            entry = previous->second;
            entry.refcount = static_cast<std::uint32_t>(count);
        } else {
            // Physical-probe fallback (slow path): a referenced blob with no
            // table record can only be a standalone file; re-probe its
            // physical layout.
            if (!store) {
                missing_blobs.push_back(digest);
                continue;
            }
            auto layout{store->layout(digest)};
            auto stored{io::file_size(store->path_for(digest))};
            if (!layout || !stored ||
                *stored > std::numeric_limits<std::uint32_t>::max()) {
                missing_blobs.push_back(digest);
                continue;
            }
            entry.logical_size = layout->logical_size;
            entry.block_count = layout->block_count;
            if (layout->kind == blob_kind::packed) {
                entry.flags = blob_flags_packed;
                if (layout->all_raw_blocks) {
                    entry.flags |= blob_flags_all_raw_blocks;
                }
            }
            entry.stored_size = static_cast<std::uint32_t>(*stored);
            entry.kind = blob_kind_content;
        }
        kept_entries.push_back(entry);
    }

    // Segment-backed records: live bytes and the records that own them. A
    // blob outside a segment is checked by file existence (as in the
    // fallback path).
    std::map<std::uint64_t, std::uint64_t> live_bytes;
    std::map<std::uint64_t, std::vector<const blob_table_entry *>> live_entries;
    for (const auto &entry : kept_entries) {
        if ((entry.flags & blob_flags_in_segment) == 0U) {
            if (!store || !store->contains(entry.digest)) {
                missing_blobs.push_back(entry.digest);
            }
            continue;
        }
        live_bytes[entry.segment_id] += entry.stored_size;
        live_entries[entry.segment_id].push_back(&entry);
    }

    auto blobs{scan_physical_blobs(store_root)};
    if (!blobs) {
        return std::unexpected{blobs.error()};
    }
    auto segments{scan_content_segments(store_root)};
    if (!segments) {
        return std::unexpected{segments.error()};
    }
    std::map<std::uint64_t, const content_segment_file *> segment_by_id;
    std::set<std::uint64_t> referenced_segment_ids;
    for (const auto &segment : *segments) {
        segment_by_id.emplace(segment.id, &segment);
    }
    for (const auto &entry : table->entries) {
        if ((entry.flags & blob_flags_in_segment) != 0U) {
            referenced_segment_ids.insert(entry.segment_id);
        }
    }

    // Fail-closed segment checks: a referenced in-segment blob needs an
    // existing segment with a readable header and an aligned in-body span.
    std::vector<std::string> invalid_segments;
    for (const auto &[id, entries] : live_entries) {
        const auto found{segment_by_id.find(id)};
        if (found == segment_by_id.end()) {
            invalid_segments.push_back("failed to find the gc segment " +
                                       segment_id_text(id));
            continue;
        }
        const auto &segment{*found->second};
        if (!segment.header_ok) {
            invalid_segments.push_back(
                "failed to read the header of the gc segment " +
                segment_id_text(id));
            continue;
        }
        std::uint64_t total{0};
        for (const auto *entry : entries) {
            const auto start{static_cast<std::uint64_t>(entry->segment_offset)};
            const auto size{static_cast<std::uint64_t>(entry->stored_size)};
            if (start % content_segment_alignment != 0U) {
                invalid_segments.push_back("failed to validate blob " +
                                           to_hex(entry->digest).substr(0, 16) +
                                           " of the gc segment " +
                                           segment_id_text(id) +
                                           ", it is not 8-byte aligned");
            } else if (start > segment.data_bytes ||
                       size > segment.data_bytes - start) {
                invalid_segments.push_back(
                    "failed to validate blob " +
                    to_hex(entry->digest).substr(0, 16) +
                    " of the gc segment " + segment_id_text(id) +
                    fmt::format(", the span {}+{} exceeds the body of {} bytes",
                                start, size, segment.data_bytes));
            }
            total += size;
        }
        if (total > segment.data_bytes) {
            invalid_segments.push_back(
                "failed to validate the gc segment " + segment_id_text(id) +
                fmt::format(", the live bytes {} exceed the body of {} bytes",
                            total, segment.data_bytes));
        }
    }

    const auto cutoff{
        std::filesystem::file_time_type::clock::now() -
        std::chrono::hours(24LL * static_cast<long long>(options.keep_days))};
    std::vector<const physical_blob *> removable;
    std::uint64_t removable_bytes{0};
    for (const auto &blob : *blobs) {
        if (root_counts.contains(blob.digest)) {
            continue;
        }
        if (options.keep_days > 0 && blob.modified >= cutoff) {
            continue;
        }
        removable.push_back(&blob);
        removable_bytes += blob.size;
    }
    std::vector<const content_segment_file *> orphan_segments;
    std::uint64_t orphan_bytes{0};
    for (const auto &segment : *segments) {
        if (referenced_segment_ids.contains(segment.id)) {
            continue;
        }
        if (options.keep_days > 0 && segment.modified >= cutoff) {
            continue;
        }
        orphan_segments.push_back(&segment);
        orphan_bytes += segment.file_size;
    }

    // Dead bytes = body bytes - live blob bytes; alignment padding counts as
    // dead. Segments at/above 50% dead are compaction candidates.
    std::vector<const content_segment_file *> candidates;
    std::uint64_t candidate_live_bytes{0};
    std::uint64_t candidate_dead_bytes{0};
    for (const auto &[id, live] : live_bytes) {
        const auto found{segment_by_id.find(id)};
        if (found == segment_by_id.end() || !found->second->header_ok) {
            continue; // reported through invalid_segments on --apply
        }
        const auto &segment{*found->second};
        if (segment.data_bytes == 0U || live > segment.data_bytes) {
            continue;
        }
        const auto dead{segment.data_bytes - live};
        if (dead * 2U < segment.data_bytes) {
            continue;
        }
        candidates.push_back(&segment);
        candidate_live_bytes += live;
        candidate_dead_bytes += dead;
    }

    report.unreferenced_blobs = removable.size();
    report.unreferenced_bytes = removable_bytes;
    report.orphan_segments = orphan_segments.size();
    report.orphan_bytes = orphan_bytes;
    report.compact_candidates = candidates.size();
    report.compact_live_bytes = candidate_live_bytes;
    report.compact_dead_bytes = candidate_dead_bytes;

    if (!options.apply) {
        emit(fmt::format("found {} unreferenced blobs totaling {} bytes",
                         removable.size(), removable_bytes));
        emit(fmt::format("found {} orphan segments totaling {} bytes",
                         orphan_segments.size(), orphan_bytes));
        if (options.compact) {
            emit(fmt::format(
                "found {} segments to compact holding {} live bytes and {} "
                "dead bytes",
                candidates.size(), candidate_live_bytes, candidate_dead_bytes));
        }
        for (const auto &problem : invalid_segments) {
            emit(problem);
        }
        if (!missing_blobs.empty()) {
            emit(fmt::format(
                "failed to find {} referenced blobs because they are missing "
                "from the store",
                missing_blobs.size()));
        }
        return report;
    }

    // Fail closed before touching anything: dropping a record shifts every
    // later blob_index and changes the table BLAKE3 prefix, invalidating all
    // RFRX indexes; a dangling segment reference is equally unrecoverable.
    if (!missing_blobs.empty()) {
        std::string sample;
        for (std::size_t index{0}; index < missing_blobs.size() && index < 3U;
             ++index) {
            if (!sample.empty()) {
                sample += ", ";
            }
            sample += to_hex(missing_blobs[index]);
        }
        return std::unexpected{make_error(
            error_code::not_found,
            "failed to rebuild the content index because " +
                std::to_string(missing_blobs.size()) +
                " referenced blobs are missing, including " + sample)};
    }
    if (!invalid_segments.empty()) {
        std::string sample{invalid_segments.front()};
        if (invalid_segments.size() > 1U) {
            sample += ", ...";
        }
        return std::unexpected{make_error(
            error_code::parse_error,
            "failed to rebuild the content index because " +
                std::to_string(invalid_segments.size()) +
                " referenced segments are invalid, including " + sample)};
    }

    // Compaction reads the live blobs of each candidate and writes them into
    // fresh segments. It publishes nothing: the new table/index bytes are
    // built below and the commit point stays the blobs.idx rename.
    std::map<digest32, std::pair<std::uint64_t, std::uint32_t>> relocated;
    std::size_t new_segment_count{0};
    if (options.compact && !candidates.empty()) {
        auto compacted{compact_content_segments(*store, candidates,
                                                live_entries, relocated)};
        if (!compacted) {
            return std::unexpected{std::move(compacted.error())};
        }
        new_segment_count = *compacted;
    }

    std::vector<blob_table_entry> table_entries;
    table_entries.reserve(kept_entries.size());
    for (const auto &entry : kept_entries) {
        blob_table_entry updated{entry};
        const auto moved{relocated.find(entry.digest)};
        if (moved != relocated.end()) {
            updated.segment_id = moved->second.first;
            updated.segment_offset = moved->second.second;
        }
        table_entries.push_back(updated);
    }
    auto table_bytes{build_blob_table(table_entries)};
    if (!table_bytes) {
        return std::unexpected{std::move(table_bytes.error())};
    }
    auto table_view{parse_blob_table(*table_bytes)};
    if (!table_view) {
        return std::unexpected{std::move(table_view.error())};
    }
    // Every RFRX is bound to the blobs.idx BLAKE3 prefix, so all of them are
    // re-encoded in memory before the table is published.
    std::vector<std::pair<std::string, std::vector<std::byte>>> rebuilt_indexes;
    rebuilt_indexes.reserve(indexes->size());
    for (const auto &index : *indexes) {
        std::vector<resolution_input> inputs;
        inputs.reserve(index.view.entries.size());
        for (const auto &entry : index.view.entries) {
            inputs.push_back(resolution_input{
                .location = entry.location,
                .source = entry.source,
                .priority = entry.priority,
                .blob = table->entries[entry.blob_index].digest,
                .flags = 0,
            });
        }
        auto rebuilt{build_resolution_index(inputs, *table_view)};
        if (!rebuilt) {
            return std::unexpected{std::move(rebuilt.error())};
        }
        rebuilt_indexes.emplace_back(index.path, std::move(*rebuilt));
    }

    const auto table_path{content_blob_table_path(store_root)};
    if (auto written{io::atomic_write_file(table_path, *table_bytes)};
        !written) {
        return std::unexpected{std::move(written.error())};
    }
    for (const auto &[path, bytes] : rebuilt_indexes) {
        if (auto written{publish_resolution_bytes(path, bytes)}; !written) {
            return std::unexpected{std::move(written.error())};
        }
    }
    report.table_blobs = table_entries.size();
    emit(fmt::format("published the blobs.idx with {} blobs",
                     table_entries.size()));
    if (options.compact && !candidates.empty()) {
        report.compacted_segments = candidates.size();
        report.new_segments = new_segment_count;
        emit(fmt::format(
            "compacted {} segments into {} new segments holding {} live bytes "
            "and {} dead bytes",
            candidates.size(), new_segment_count, candidate_live_bytes,
            candidate_dead_bytes));
    }

    // Apply never unlinks inline. The plan is persisted to the pending-delete
    // queue first (a crash in between leaves orphan files that the next
    // mark-sweep re-plans, never a table pointing at deleted bytes), and every
    // physical deletion goes through drain_pending_deletes. With
    // `defer_deletes` the server's `gc-delete` worker owns the drain; otherwise
    // this call drains synchronously, bounded by `budget_ms`.
    std::set<std::uint64_t> live_segment_ids;
    for (const auto &entry : table_entries) {
        if ((entry.flags & blob_flags_in_segment) != 0U) {
            live_segment_ids.insert(entry.segment_id);
        }
    }
    // A segment that lost its last reference in this rebuild (compaction or
    // dropped records) is collected now; a segment that was already an orphan
    // respects --keep-days.
    std::vector<const content_segment_file *> removable_segments;
    for (const auto &segment : *segments) {
        if (live_segment_ids.contains(segment.id)) {
            continue;
        }
        const auto was_orphan{!referenced_segment_ids.contains(segment.id)};
        if (was_orphan && options.keep_days > 0 && segment.modified >= cutoff) {
            continue;
        }
        removable_segments.push_back(&segment);
    }

    std::vector<pending_delete> queued;
    queued.reserve(removable.size() + removable_segments.size());
    for (const auto *blob : removable) {
        queued.push_back(pending_delete{
            .path = pending_blob_path(blob->digest),
            .kind = pending_delete::pending_delete_kind::blob,
            .size = blob->size,
            .attempts = 0,
        });
    }
    std::uint64_t queued_segment_bytes{0};
    for (const auto *segment : removable_segments) {
        queued.push_back(pending_delete{
            .path = pending_segment_path(segment->id),
            .kind = pending_delete::pending_delete_kind::segment,
            .size = segment->file_size,
            .attempts = 0,
        });
        queued_segment_bytes += segment->file_size;
    }
    if (!queued.empty()) {
        if (auto enqueued{enqueue_pending_deletes(store_root, queued)};
            !enqueued) {
            return std::unexpected{
                wrap_error(std::move(enqueued.error()),
                           "failed to persist the pending delete queue")};
        }
    }
    report.queued_blobs = removable.size();
    report.queued_blob_bytes = removable_bytes;
    report.queued_segments = removable_segments.size();
    report.queued_segment_bytes = queued_segment_bytes;

    if (options.defer_deletes) {
        emit(fmt::format("queued {} blobs totaling {} bytes",
                         report.queued_blobs, report.queued_blob_bytes));
        emit(fmt::format("queued {} segments totaling {} bytes",
                         report.queued_segments, report.queued_segment_bytes));
        return report;
    }

    // Synchronous drain (foreground CLI/startup): the drain summary lines would
    // duplicate the stable blob/segment lines below, so forward its failure
    // lines only and map the counters into `gc_report`.
    auto drained{drain_pending_deletes(
        store_root, options.budget_ms, [&log](std::string_view line) {
            if (log && line.starts_with("failed to ")) {
                log(line);
            }
        })};
    if (!drained) {
        return std::unexpected{std::move(drained.error())};
    }
    report.deleted_blobs = drained->deleted_blobs;
    report.deleted_blob_bytes = drained->deleted_blob_bytes;
    report.deleted_segments = drained->deleted_segments;
    report.deleted_segment_bytes = drained->deleted_segment_bytes;
    report.deferred_blobs = drained->deferred_blobs;
    report.deferred_blob_bytes = drained->deferred_blob_bytes;
    report.deferred_segments = drained->deferred_segments;
    report.deferred_segment_bytes = drained->deferred_segment_bytes;
    report.failed_blobs = drained->failed_blobs;
    report.failed_blob_bytes = drained->failed_blob_bytes;
    report.failed_segments = drained->failed_segments;
    report.failed_segment_bytes = drained->failed_segment_bytes;
    report.failed_stuck = drained->failed_stuck;
    report.skipped_referenced = drained->skipped_referenced;
    report.budget_exhausted = drained->budget_exhausted;

    emit(fmt::format("deleted {} blobs totaling {} bytes", report.deleted_blobs,
                     report.deleted_blob_bytes));
    if (report.deferred_blobs != 0U) {
        emit(fmt::format(
            "deferred {} blobs totaling {} bytes because the gc budget ran out",
            report.deferred_blobs, report.deferred_blob_bytes));
    }
    emit(fmt::format("deleted {} segments totaling {} bytes",
                     report.deleted_segments, report.deleted_segment_bytes));
    if (report.deferred_segments != 0U) {
        emit(fmt::format(
            "deferred {} segments totaling {} bytes because the gc budget ran "
            "out",
            report.deferred_segments, report.deferred_segment_bytes));
    }
    if (report.failed_blobs != 0U || report.failed_segments != 0U) {
        emit(fmt::format(
            "kept {} failed blobs totaling {} bytes and {} failed segments "
            "totaling {} bytes in the pending delete queue",
            report.failed_blobs, report.failed_blob_bytes,
            report.failed_segments, report.failed_segment_bytes));
    }
    return report;
}

} // namespace refrain::data
