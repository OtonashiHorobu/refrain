// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/io.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/content_index.hpp>
#include <refrain/data/content_maintenance.hpp>
#include <refrain/data/content_segment.hpp>
#include <refrain/data/content_store.hpp>
#include <refrain/data/json.hpp>
#include <refrain/data/path.hpp>

#include <boost/ut.hpp>

#include <fmt/format.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;
namespace fs = std::filesystem;
namespace io = refrain::core::io;

using refrain::data::batch;
using refrain::data::blake3;
using refrain::data::blob_flags_packed;
using refrain::data::blob_location_from_table_entry;
using refrain::data::blob_metadata;
using refrain::data::blob_table_entry;
using refrain::data::blob_table_view;
using refrain::data::build_blob_table;
using refrain::data::collect_garbage;
using refrain::data::content_blob_table_path;
using refrain::data::content_publish_options;
using refrain::data::content_resolution_path;
using refrain::data::content_segment_file;
using refrain::data::content_store;
using refrain::data::digest32;
using refrain::data::drain_pending_deletes;
using refrain::data::enqueue_pending_deletes;
using refrain::data::gc_options;
using refrain::data::gc_report;
using refrain::data::load_pending_deletes;
using refrain::data::max_pending_delete_attempts;
using refrain::data::parse_blob_table;
using refrain::data::parse_resolution_index;
using refrain::data::pending_delete;
using refrain::data::pending_delete_report;
using refrain::data::publish_resolution;
using refrain::data::resolution_index_view;
using refrain::data::resolution_input;
using refrain::data::resolve;
using refrain::data::resolved_blob;
using refrain::data::scan_content_segments;
using refrain::data::segment_path;
using refrain::data::to_hex;

[[nodiscard]] auto bytes_of(std::string_view text) -> std::vector<std::byte> {
    const auto bytes{
        std::as_bytes(std::span<const char>{text.data(), text.size()})};
    return std::vector<std::byte>{bytes.begin(), bytes.end()};
}

[[nodiscard]] auto repeated_blob(std::size_t count, std::byte value)
    -> std::vector<std::byte> {
    return std::vector<std::byte>(count, value);
}

[[nodiscard]] auto make_temp_directory() -> fs::path {
    static int counter{0};
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    const auto path{fs::temp_directory_path() /
                    ("refrain_maintenance_test_" + std::to_string(stamp) + "_" +
                     std::to_string(counter++))};
    std::error_code error;
    fs::create_directories(path, error);
    return path;
}

/// Removes the scratch directory when the test returns (including early exits
/// on failed preconditions).
struct temp_directory {
    fs::path path{};

    explicit temp_directory(fs::path location) : path{std::move(location)} {}
    temp_directory(const temp_directory &) = delete;
    auto operator=(const temp_directory &) -> temp_directory & = delete;
    ~temp_directory() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

[[nodiscard]] auto store_root(const temp_directory &scratch) -> std::string {
    return (scratch.path / "cache").string();
}

/// RFBT record a batch publisher would build from one `blob_metadata`.
[[nodiscard]] auto entry_from(const blob_metadata &meta,
                              std::uint32_t refcount = 1) -> blob_table_entry {
    std::uint32_t block_count{0};
    if ((meta.flags & blob_flags_packed) != 0U) {
        block_count =
            static_cast<std::uint32_t>((meta.logical_size + 65535U) / 65536U);
    }
    return blob_table_entry{
        .digest = meta.digest,
        .logical_size = meta.logical_size,
        .flags = meta.flags,
        .refcount = refcount,
        .kind = refrain::data::blob_kind_content,
        .block_count = block_count,
        .stored_size = meta.stored_size,
        .segment_id = meta.segment_id,
        .segment_offset = meta.segment_offset,
    };
}

[[nodiscard]] auto entries_from(const std::vector<blob_metadata> &blobs)
    -> std::vector<blob_table_entry> {
    std::vector<blob_table_entry> records;
    records.reserve(blobs.size());
    for (const auto &meta : blobs) {
        records.push_back(entry_from(meta));
    }
    return records;
}

/// Publish a table built from `records` (the same write the CLI performs).
[[nodiscard]] auto write_table(std::string_view root,
                               std::span<const blob_table_entry> records)
    -> bool {
    auto bytes{build_blob_table(records)};
    if (!bytes) {
        return false;
    }
    const auto path{content_blob_table_path(root)};
    const auto directory{refrain::data::parent_path(path)};
    if (!directory.empty()) {
        auto created{io::create_directories(std::string{directory})};
        if (!created) {
            return false;
        }
    }
    return static_cast<bool>(io::atomic_write_file(path, *bytes));
}

/// Run one batch and return its new blob records (empty on failure).
[[nodiscard]] auto stage(std::string_view root,
                         std::span<const std::vector<std::byte>> payloads)
    -> std::vector<blob_metadata> {
    auto staged{batch::begin(root, nullptr)};
    if (!staged) {
        return {};
    }
    for (const auto &payload : payloads) {
        if (!staged->put(payload)) {
            return {};
        }
    }
    if (!staged->commit()) {
        return {};
    }
    const auto blobs{staged->new_blobs()};
    return {blobs.begin(), blobs.end()};
}

[[nodiscard]] auto read_table(std::string_view root)
    -> std::optional<blob_table_view> {
    auto bytes{io::read_file(content_blob_table_path(root))};
    if (!bytes) {
        return std::nullopt;
    }
    auto parsed{parse_blob_table(*bytes)};
    if (!parsed) {
        return std::nullopt;
    }
    return std::optional<blob_table_view>{std::move(*parsed)};
}

[[nodiscard]] auto read_index(std::string_view root,
                              std::string_view composition,
                              const blob_table_view &table)
    -> std::optional<resolution_index_view> {
    auto bytes{io::read_file(content_resolution_path(root, composition))};
    if (!bytes) {
        return std::nullopt;
    }
    auto parsed{parse_resolution_index(*bytes, table)};
    if (!parsed) {
        return std::nullopt;
    }
    return std::optional<resolution_index_view>{std::move(*parsed)};
}

[[nodiscard]] auto find_entry(const blob_table_view &table,
                              const digest32 &digest)
    -> const blob_table_entry * {
    for (const auto &entry : table.entries) {
        if (entry.digest == digest) {
            return &entry;
        }
    }
    return nullptr;
}

[[nodiscard]] auto pending_queue_path(std::string_view root) -> std::string {
    return refrain::data::join_path(root, "content/gc/pending.json");
}

/// Store-relative standalone-blob path (`content/blobs/<hh>/<digest>`).
[[nodiscard]] auto pending_blob_path(const digest32 &digest) -> std::string {
    const auto hex{to_hex(digest)};
    return refrain::data::join_path(
        "content/blobs", refrain::data::join_path(hex.substr(0, 2), hex));
}

/// Store-relative sealed-segment path (`content/segments/<id16>.rfseg`).
[[nodiscard]] auto pending_segment_path(std::uint64_t id) -> std::string {
    return fmt::format("content/segments/{:016x}.rfseg", id);
}

/// Overwrite the queue with raw text (used for foreign/corrupt queue tests).
[[nodiscard]] auto write_pending_queue_text(std::string_view root,
                                            std::string_view text) -> bool {
    const auto path{pending_queue_path(root)};
    const auto directory{refrain::data::parent_path(path)};
    if (!directory.empty() && !io::create_directories(std::string{directory})) {
        return false;
    }
    return static_cast<bool>(io::atomic_write_text(path, text));
}

[[nodiscard]] auto pending_blob_entry(std::string path, std::uint64_t size,
                                      std::uint32_t attempts = 0)
    -> pending_delete {
    return pending_delete{
        .path = std::move(path),
        .kind = pending_delete::pending_delete_kind::blob,
        .size = size,
        .attempts = attempts,
    };
}

[[nodiscard]] auto pending_segment_entry(std::string path, std::uint64_t size,
                                         std::uint32_t attempts = 0)
    -> pending_delete {
    return pending_delete{
        .path = std::move(path),
        .kind = pending_delete::pending_delete_kind::segment,
        .size = size,
        .attempts = attempts,
    };
}

/// RFBT record of one standalone raw blob (the layout `content_store::put`
/// writes for payloads <= 64 KiB).
[[nodiscard]] auto standalone_record(const digest32 &digest, std::uint64_t size,
                                     std::uint32_t refcount = 1)
    -> blob_table_entry {
    return blob_table_entry{
        .digest = digest,
        .logical_size = size,
        .flags = 0,
        .refcount = refcount,
        .kind = refrain::data::blob_kind_content,
        .block_count = 0,
        .stored_size = static_cast<std::uint32_t>(size),
        .segment_id = 0,
        .segment_offset = 0,
    };
}

const suite content_maintenance_tests = [] {
    "publish_resolution binds and atomically overwrites an index"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{store_root(scratch)};
        const std::vector<std::vector<std::byte>> payloads{bytes_of("alpha"),
                                                           bytes_of("beta")};
        const auto blobs{stage(root, payloads)};
        expect(blobs.size() == 2U);
        if (blobs.size() != 2U) {
            return;
        }
        expect(write_table(root, entries_from(blobs)));
        auto table{read_table(root)};
        expect(table.has_value());
        if (!table) {
            return;
        }

        content_publish_options options{};
        options.composition = "main";
        options.entries = {
            resolution_input{.location = "data/a.txt",
                             .source = "test",
                             .priority = 1,
                             .blob = blobs[0].digest,
                             .flags = 0},
            resolution_input{.location = "data/b.txt",
                             .source = "test",
                             .priority = 1,
                             .blob = blobs[1].digest,
                             .flags = 0},
        };
        expect(static_cast<bool>(publish_resolution(root, options)));
        // Publishing now also rewrites `blobs.idx` (refcounts), so the RFRX
        // must be parsed against the republished table.
        auto table_after{read_table(root)};
        expect(table_after.has_value());
        if (!table_after) {
            return;
        }
        expect(table_after->entries.size() == 2U);
        for (const auto &entry : table_after->entries) {
            expect(entry.refcount == 1U);
        }
        auto index{read_index(root, "main", *table_after)};
        expect(index.has_value());
        if (index) {
            expect(index->entries.size() == 2U);
            auto resolved{resolve(*index, "data/a.txt")};
            expect(resolved.has_value());
            if (resolved) {
                auto digest{resolved_blob(*resolved, *table_after)};
                expect(digest.has_value());
                if (digest) {
                    expect(*digest == blobs[0].digest);
                }
            }
        }

        // Publishing the same composition again replaces the file (the old
        // entries are not merged by this primitive) and recomputes refcounts:
        // the blob that is no longer referenced drops back to zero.
        options.entries.resize(1U);
        options.entries[0].location = "data/c.txt";
        options.entries[0].blob = blobs[1].digest;
        expect(static_cast<bool>(publish_resolution(root, options)));
        auto table_replaced{read_table(root)};
        expect(table_replaced.has_value());
        if (!table_replaced) {
            return;
        }
        for (const auto &entry : table_replaced->entries) {
            expect(entry.refcount ==
                   (entry.digest == blobs[1].digest ? 1U : 0U));
        }
        auto replaced{read_index(root, "main", *table_replaced)};
        expect(replaced.has_value());
        if (replaced) {
            expect(replaced->entries.size() == 1U);
            if (replaced->entries.size() == 1U) {
                expect(replaced->entries[0].location == "data/c.txt");
            }
        }

        // A digest missing from blobs.idx fails and leaves the published index
        // untouched (no partial rewrite).
        const auto before{io::read_file(content_resolution_path(root, "main"))};
        expect(before.has_value());
        options.entries[0].blob = blake3(bytes_of("not in the table"));
        auto unknown{publish_resolution(root, options)};
        expect(!unknown);
        if (!unknown) {
            expect(unknown.error().code ==
                   refrain::error_code::invalid_argument);
        }
        if (before) {
            expect(io::read_file(content_resolution_path(root, "main"))
                       .value_or(std::vector<std::byte>{}) == *before);
        }
    };

    "publish_resolution rejects a missing/corrupt table and unsafe names"_test =
        [] {
            temp_directory scratch{make_temp_directory()};
            const auto root{store_root(scratch)};
            const std::vector<std::vector<std::byte>> payloads{
                bytes_of("alpha")};
            const auto blobs{stage(root, payloads)};
            expect(blobs.size() == 1U);
            if (blobs.size() != 1U) {
                return;
            }

            content_publish_options options{};
            options.composition = "main";
            options.entries = {resolution_input{.location = "data/a.txt",
                                                .source = "test",
                                                .priority = 0,
                                                .blob = blobs[0].digest,
                                                .flags = 0}};

            // No blobs.idx yet: publishing must not create a resolutions file.
            auto missing{publish_resolution(root, options)};
            expect(!missing);
            if (!missing) {
                expect(missing.error().code == refrain::error_code::io_error);
            }
            expect(!io::exists(content_resolution_path(root, "main")));

            // A damaged table is fail-closed as well.
            const auto table_path{content_blob_table_path(root)};
            const auto directory{refrain::data::parent_path(table_path)};
            expect(static_cast<bool>(
                io::create_directories(std::string{directory})));
            expect(static_cast<bool>(
                io::atomic_write_file(table_path, bytes_of("not a table"))));
            auto corrupt{publish_resolution(root, options)};
            expect(!corrupt);
            if (!corrupt) {
                expect(corrupt.error().code ==
                       refrain::error_code::parse_error);
            }
            expect(!io::exists(content_resolution_path(root, "main")));

            // A composition is a single safe file-name segment.
            expect(write_table(root, entries_from(blobs)));
            options.composition = "nested/name";
            auto unsafe{publish_resolution(root, options)};
            expect(!unsafe);
            if (!unsafe) {
                expect(unsafe.error().code ==
                       refrain::error_code::invalid_argument);
            }
            // Writer and reader share one predicate: names the runtime pack
            // cannot open must be rejected by the writer too, including spaces
            // and 129..200-byte names.
            options.composition = "my pack";
            auto spaced{publish_resolution(root, options)};
            expect(!spaced);
            if (!spaced) {
                expect(spaced.error().code ==
                       refrain::error_code::invalid_argument);
            }
            options.composition = std::string(129U, 'a');
            auto too_long{publish_resolution(root, options)};
            expect(!too_long);
            if (!too_long) {
                expect(too_long.error().code ==
                       refrain::error_code::invalid_argument);
            }
        };

    "collect_garbage rejects a keep-days value that would overflow the clock"_test =
        [] {
            temp_directory scratch{make_temp_directory()};
            const auto root{store_root(scratch)};
            gc_options options{};
            options.keep_days = refrain::data::max_gc_keep_days + 1;
            auto rejected{collect_garbage(root, options)};
            expect(!rejected);
            if (!rejected) {
                expect(rejected.error().code ==
                       refrain::error_code::invalid_argument);
                expect(rejected.error().message.find("--keep-days") !=
                       std::string::npos);
            }
            // The bound itself is accepted (no table -> not_found, not a
            // validation error).
            options.keep_days = refrain::data::max_gc_keep_days;
            auto accepted{collect_garbage(root, options)};
            expect(!accepted);
            if (!accepted) {
                expect(accepted.error().code == refrain::error_code::io_error);
            }
        };

    "collect_garbage keep-days protects recent blobs and drops old ones"_test =
        [] {
            temp_directory scratch{make_temp_directory()};
            const auto root{store_root(scratch)};
            const std::vector<std::vector<std::byte>> payloads{
                repeated_blob(70U * 1024U, std::byte{0x55}),
                repeated_blob(70U * 1024U, std::byte{0x66})};
            const auto blobs{stage(root, payloads)};
            expect(blobs.size() == 2U);
            if (blobs.size() != 2U) {
                return;
            }
            expect(write_table(root, entries_from(blobs)));
            auto store{content_store::open(root)};
            expect(store.has_value());
            if (!store) {
                return;
            }
            const auto path{store->path_for(blobs[1].digest)};

            gc_options options{};
            options.apply = true;
            options.keep_days = 1;
            // An apply needs at least one root; keep the first blob rooted and
            // test retention/deletion on the second one.
            options.extra_roots = {blobs[0].digest};
            // Fresh mtime: inside the one-day window, so it must survive.
            auto kept{collect_garbage(root, options)};
            expect(kept.has_value());
            if (kept) {
                expect(kept->deleted_blobs == 0U);
            }
            expect(io::is_regular_file(path));

            // Older than the window: the same run must delete it.
            std::error_code time_error;
            fs::last_write_time(fs::path{path},
                                fs::file_time_type::clock::now() -
                                    std::chrono::hours{48},
                                time_error);
            expect(!time_error);
            auto removed{collect_garbage(root, options)};
            expect(removed.has_value());
            if (removed) {
                expect(removed->deleted_blobs == 1U);
            }
            expect(!io::is_regular_file(path));
        };

    "collect_garbage keeps extra roots and deletes other blobs"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{store_root(scratch)};
        const std::vector<std::vector<std::byte>> payloads{
            repeated_blob(70U * 1024U, std::byte{0x11}),
            repeated_blob(70U * 1024U, std::byte{0x22})};
        const auto blobs{stage(root, payloads)};
        expect(blobs.size() == 2U);
        if (blobs.size() != 2U) {
            return;
        }
        expect(write_table(root, entries_from(blobs)));

        gc_options options{};
        options.apply = true;
        options.extra_roots = {blobs[1].digest};
        auto report{collect_garbage(root, options)};
        expect(report.has_value());
        if (!report) {
            return;
        }
        expect(report->applied);
        expect(report->root_index_references == 0U);
        expect(report->root_extra == 1U);
        expect(report->root_blobs == 1U);
        expect(report->deleted_blobs == 1U);
        expect(report->table_blobs == 1U);

        auto table{read_table(root)};
        expect(table.has_value());
        if (table) {
            expect(table->entries.size() == 1U);
            if (table->entries.size() == 1U) {
                expect(table->entries[0].digest == blobs[1].digest);
                expect(table->entries[0].refcount == 1U);
            }
        }
        auto store{content_store::open(root)};
        expect(store.has_value());
        if (store) {
            expect(!io::is_regular_file(store->path_for(blobs[0].digest)));
            expect(io::is_regular_file(store->path_for(blobs[1].digest)));
        }
    };

    "collect_garbage dry run never writes and apply drops zero-reference "
    "rows"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{store_root(scratch)};
        const std::vector<std::vector<std::byte>> payloads{
            repeated_blob(70U * 1024U, std::byte{0x33}),
            repeated_blob(70U * 1024U, std::byte{0x44})};
        const auto blobs{stage(root, payloads)};
        expect(blobs.size() == 2U);
        if (blobs.size() != 2U) {
            return;
        }
        expect(write_table(root, entries_from(blobs)));
        content_publish_options publish{};
        publish.composition = "main";
        publish.entries = {resolution_input{.location = "data/keep.txt",
                                            .source = "test",
                                            .priority = 0,
                                            .blob = blobs[0].digest,
                                            .flags = 0}};
        expect(static_cast<bool>(publish_resolution(root, publish)));
        const auto table_before{io::read_file(content_blob_table_path(root))};
        expect(table_before.has_value());
        auto store{content_store::open(root)};
        expect(store.has_value());
        if (!store) {
            return;
        }
        expect(io::is_regular_file(store->path_for(blobs[1].digest)));

        gc_options options{};
        options.compact = true;
        auto planned{collect_garbage(root, options)};
        expect(planned.has_value());
        if (planned) {
            expect(!planned->applied);
            expect(planned->unreferenced_blobs == 1U);
            expect(planned->unreferenced_bytes == blobs[1].stored_size);
            expect(planned->deleted_blobs == 0U);
        }
        if (table_before) {
            expect(io::read_file(content_blob_table_path(root))
                       .value_or(std::vector<std::byte>{}) == *table_before);
        }
        expect(io::is_regular_file(store->path_for(blobs[1].digest)));

        options.apply = true;
        auto applied{collect_garbage(root, options)};
        expect(applied.has_value());
        if (applied) {
            expect(applied->applied);
            expect(applied->deleted_blobs == 1U);
            expect(applied->deleted_blob_bytes == blobs[1].stored_size);
            expect(applied->table_blobs == 1U);
        }
        expect(!io::is_regular_file(store->path_for(blobs[1].digest)));
        auto table{read_table(root)};
        expect(table.has_value());
        if (table) {
            expect(table->entries.size() == 1U);
            expect(find_entry(*table, blobs[1].digest) == nullptr);
        }
    };

    "collect_garbage deletes orphan segments"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{store_root(scratch)};
        const std::vector<std::vector<std::byte>> kept_payload{
            bytes_of("kept")};
        const auto kept{stage(root, kept_payload)};
        const std::vector<std::vector<std::byte>> orphan_payload{
            bytes_of("orphan")};
        const auto orphan{stage(root, orphan_payload)};
        expect(kept.size() == 1U);
        expect(orphan.size() == 1U);
        if (kept.size() != 1U || orphan.size() != 1U) {
            return;
        }
        expect(kept[0].segment_id != orphan[0].segment_id);
        expect(write_table(root, entries_from(kept)));
        content_publish_options publish{};
        publish.composition = "main";
        publish.entries = {resolution_input{.location = "data/kept.txt",
                                            .source = "test",
                                            .priority = 0,
                                            .blob = kept[0].digest,
                                            .flags = 0}};
        expect(static_cast<bool>(publish_resolution(root, publish)));

        gc_options options{};
        options.apply = true;
        auto report{collect_garbage(root, options)};
        expect(report.has_value());
        if (!report) {
            return;
        }
        expect(report->orphan_segments == 1U);
        expect(report->deleted_segments == 1U);
        expect(report->table_blobs == 1U);
        expect(!io::exists(segment_path(root, orphan[0].segment_id)));
        expect(io::exists(segment_path(root, kept[0].segment_id)));
        auto segments{scan_content_segments(root)};
        expect(segments.has_value());
        if (segments) {
            expect(segments->size() == 1U);
        }
    };

    "collect_garbage defers deletions over the budget, next run finishes "
    "them"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{store_root(scratch)};
        auto store{content_store::open(root)};
        expect(store.has_value());
        if (!store) {
            return;
        }
        // One rooted standalone blob plus a large set of unreferenced
        // standalone blobs (`content_store::put`, one raw file each). The table
        // only knows the root; the orphans are physical files the mark-sweep
        // discovers.
        const auto root_payload{bytes_of("rooted blob")};
        const auto root_digest{store->put(root_payload)};
        expect(root_digest.has_value());
        if (!root_digest) {
            return;
        }
        constexpr std::size_t orphan_count{1000};
        bool orphans_ok{true};
        for (std::size_t index{0}; index < orphan_count; ++index) {
            const auto payload{bytes_of("orphan-" + std::to_string(index))};
            if (!store->put(payload)) {
                orphans_ok = false;
                break;
            }
        }
        expect(orphans_ok);
        if (!orphans_ok) {
            return;
        }
        const blob_table_entry root_record{
            .digest = *root_digest,
            .logical_size = root_payload.size(),
            .flags = 0,
            .refcount = 1,
            .kind = refrain::data::blob_kind_content,
            .block_count = 0,
            .stored_size = static_cast<std::uint32_t>(root_payload.size()),
            .segment_id = 0,
            .segment_offset = 0};
        expect(write_table(root, std::span{&root_record, 1U}));

        // The plan is independent of the budget; the dry run needs the root,
        // exactly like the applying run below.
        gc_options plan_options{};
        plan_options.extra_roots = {*root_digest};
        auto planned{collect_garbage(root, plan_options)};
        expect(planned.has_value());
        if (!planned) {
            return;
        }
        expect(planned->unreferenced_blobs == orphan_count);
        expect(planned->deferred_blobs == 0U);
        expect(!planned->budget_exhausted);

        // A 1 ms budget must stop inside the deletion phase: 1000 unlinks take
        // longer than 1 ms on the supported filesystems. The rebuilt table is
        // published before deletion, so the leftover blobs are plain orphans.
        gc_options budget{};
        budget.apply = true;
        budget.budget_ms = 1;
        budget.extra_roots = {*root_digest};
        std::vector<std::string> lines;
        auto first{
            collect_garbage(root, budget, [&lines](std::string_view line) {
                lines.emplace_back(line);
            })};
        expect(first.has_value());
        if (!first) {
            return;
        }
        expect(first->applied);
        expect(first->budget_exhausted);
        expect(first->deferred_blobs > 0U);
        expect(first->deferred_blob_bytes > 0U);
        expect(first->deleted_blobs + first->deferred_blobs == orphan_count);
        expect(first->deferred_segments == 0U);
        bool saw_budget_line{false};
        for (const auto &line : lines) {
            if (line.find("deferred") != std::string::npos &&
                line.find("(gc budget)") != std::string::npos) {
                saw_budget_line = true;
            }
        }
        expect(saw_budget_line);

        auto table{read_table(root)};
        expect(table.has_value());
        if (table) {
            expect(table->entries.size() == 1U);
            if (table->entries.size() == 1U) {
                expect(table->entries[0].digest == *root_digest);
            }
        }
        expect(io::is_regular_file(store->path_for(*root_digest)));

        // Unlimited follow-up: the deferred remainder is re-planned from the
        // physical scan and deleted, leaving a clean store.
        gc_options finish{};
        finish.apply = true;
        finish.extra_roots = {*root_digest};
        auto second{collect_garbage(root, finish)};
        expect(second.has_value());
        if (!second) {
            return;
        }
        expect(!second->budget_exhausted);
        expect(second->deferred_blobs == 0U);
        expect(second->deferred_segments == 0U);
        expect(second->unreferenced_blobs == first->deferred_blobs);
        expect(second->deleted_blobs == first->deferred_blobs);
        gc_options clean_options{};
        clean_options.extra_roots = {*root_digest};
        auto clean{collect_garbage(root, clean_options)};
        expect(clean.has_value());
        if (clean) {
            expect(clean->unreferenced_blobs == 0U);
        }
    };

    "collect_garbage compacts a segment that becomes mostly dead"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{store_root(scratch)};
        const std::vector<std::vector<std::byte>> payloads{
            repeated_blob(200U, std::byte{0x55}),
            repeated_blob(200U, std::byte{0x66})};
        const auto blobs{stage(root, payloads)};
        expect(blobs.size() == 2U);
        if (blobs.size() != 2U) {
            return;
        }
        expect(blobs[0].segment_id == blobs[1].segment_id);
        expect(write_table(root, entries_from(blobs)));
        content_publish_options publish{};
        publish.composition = "main";
        publish.entries = {resolution_input{.location = "data/keep.txt",
                                            .source = "test",
                                            .priority = 0,
                                            .blob = blobs[0].digest,
                                            .flags = 0}};
        expect(static_cast<bool>(publish_resolution(root, publish)));
        const auto old_segment{blobs[0].segment_id};
        const auto table_before{io::read_file(content_blob_table_path(root))};
        expect(table_before.has_value());

        // Dry run with --compact only reports the plan.
        gc_options plan{};
        plan.compact = true;
        auto planned{collect_garbage(root, plan)};
        expect(planned.has_value());
        if (planned) {
            expect(planned->compact_candidates == 1U);
            expect(planned->compacted_segments == 0U);
        }
        if (table_before) {
            expect(io::read_file(content_blob_table_path(root))
                       .value_or(std::vector<std::byte>{}) == *table_before);
        }
        expect(io::exists(segment_path(root, old_segment)));

        gc_options options{};
        options.apply = true;
        options.compact = true;
        auto report{collect_garbage(root, options)};
        expect(report.has_value());
        if (!report) {
            return;
        }
        expect(report->compact_candidates == 1U);
        expect(report->compacted_segments == 1U);
        expect(report->new_segments == 1U);
        expect(report->deleted_segments == 1U);
        expect(report->table_blobs == 1U);
        expect(!io::exists(segment_path(root, old_segment)));

        auto table{read_table(root)};
        expect(table.has_value());
        if (!table) {
            return;
        }
        expect(table->entries.size() == 1U);
        if (table->entries.size() != 1U) {
            return;
        }
        expect(table->entries[0].digest == blobs[0].digest);
        expect(table->entries[0].segment_id != old_segment);
        auto store{content_store::open(root)};
        expect(store.has_value());
        if (store) {
            auto bytes{
                store->read(blob_location_from_table_entry(table->entries[0]))};
            expect(bytes.has_value());
            if (bytes) {
                expect(*bytes == payloads[0]);
            }
        }
        auto index{read_index(root, "main", *table)};
        expect(index.has_value());
    };

    "collect_garbage dry run does not create the blob directory"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{store_root(scratch)};
        const std::vector<std::vector<std::byte>> payloads{
            bytes_of("segment-only")};
        const auto blobs{stage(root, payloads)};
        expect(blobs.size() == 1U);
        if (blobs.size() != 1U) {
            return;
        }
        expect((blobs[0].flags & refrain::data::blob_flags_in_segment) != 0U);
        expect(write_table(root, entries_from(blobs)));
        content_publish_options publish{};
        publish.composition = "main";
        publish.entries = {resolution_input{.location = "data/a.txt",
                                            .source = "test",
                                            .priority = 0,
                                            .blob = blobs[0].digest,
                                            .flags = 0}};
        expect(static_cast<bool>(publish_resolution(root, publish)));

        // A segment-only store may have no content/blobs directory at all; a
        // dry run must not create it.
        const auto blobs_directory{
            refrain::data::join_path(root, "content/blobs")};
        std::error_code remove_error;
        fs::remove_all(fs::path{blobs_directory}, remove_error);
        expect(!io::is_directory(blobs_directory));

        auto report{collect_garbage(root, gc_options{})};
        expect(report.has_value());
        if (report) {
            expect(!report->applied);
            expect(report->root_index_references == 1U);
            expect(report->unreferenced_blobs == 0U);
        }
        expect(!io::is_directory(blobs_directory));
    };

    "collect_garbage refuses an apply with no roots"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{store_root(scratch)};
        const std::vector<std::vector<std::byte>> payloads{
            repeated_blob(70U * 1024U, std::byte{0x77})};
        const auto blobs{stage(root, payloads)};
        expect(blobs.size() == 1U);
        if (blobs.size() != 1U) {
            return;
        }
        expect(write_table(root, entries_from(blobs)));
        const auto table_before{io::read_file(content_blob_table_path(root))};
        expect(table_before.has_value());

        gc_options options{};
        options.apply = true;
        auto refused{collect_garbage(root, options)};
        expect(!refused);
        if (!refused) {
            expect(refused.error().code == refrain::error_code::not_found);
            expect(refused.error().message.find("refusing to rebuild") !=
                   std::string::npos);
        }
        if (table_before) {
            expect(io::read_file(content_blob_table_path(root))
                       .value_or(std::vector<std::byte>{}) == *table_before);
        }
        auto store{content_store::open(root)};
        expect(store.has_value());
        if (store) {
            expect(io::is_regular_file(store->path_for(blobs[0].digest)));
        }

        // A dry run is allowed to plan the deletion.
        auto planned{collect_garbage(root, gc_options{})};
        expect(planned.has_value());
        if (planned) {
            expect(planned->unreferenced_blobs == 1U);
            expect(planned->deleted_blobs == 0U);
        }
    };

    "collect_garbage fails closed when a referenced blob is missing"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{store_root(scratch)};
        const std::vector<std::vector<std::byte>> payloads{
            repeated_blob(70U * 1024U, std::byte{0x88})};
        const auto blobs{stage(root, payloads)};
        expect(blobs.size() == 1U);
        if (blobs.size() != 1U) {
            return;
        }
        expect(write_table(root, entries_from(blobs)));
        content_publish_options publish{};
        publish.composition = "main";
        publish.entries = {resolution_input{.location = "data/a.txt",
                                            .source = "test",
                                            .priority = 0,
                                            .blob = blobs[0].digest,
                                            .flags = 0}};
        expect(static_cast<bool>(publish_resolution(root, publish)));
        auto store{content_store::open(root)};
        expect(store.has_value());
        if (!store) {
            return;
        }
        const auto table_before{io::read_file(content_blob_table_path(root))};
        expect(table_before.has_value());
        expect(io::is_regular_file(store->path_for(blobs[0].digest)));
        std::error_code remove_error;
        expect(fs::remove(fs::path{store->path_for(blobs[0].digest)},
                          remove_error));

        gc_options options{};
        options.apply = true;
        auto refused{collect_garbage(root, options)};
        expect(!refused);
        if (!refused) {
            expect(refused.error().code == refrain::error_code::not_found);
            expect(refused.error().message.find(
                       "referenced blob(s) are missing") != std::string::npos);
        }
        if (table_before) {
            expect(io::read_file(content_blob_table_path(root))
                       .value_or(std::vector<std::byte>{}) == *table_before);
        }

        // A dry run reports the missing root but still succeeds.
        std::vector<std::string> lines;
        auto planned{collect_garbage(
            root, gc_options{},
            [&lines](std::string_view line) { lines.emplace_back(line); })};
        expect(planned.has_value());
        if (planned) {
            expect(planned->root_index_references == 1U);
        }
        bool saw_missing{false};
        for (const auto &line : lines) {
            if (line.find("missing") != std::string::npos) {
                saw_missing = true;
            }
        }
        expect(saw_missing);
    };

    "content/publish skips identical index bytes (fingerprint no-op)"_test =
        [] {
            temp_directory scratch{make_temp_directory()};
            const std::string path{(scratch.path / "index.idx").string()};
            const std::vector<std::byte> bytes{std::byte{1}, std::byte{2},
                                               std::byte{3}};
            expect(io::atomic_write_file(path, bytes).has_value());
            const auto sentinel{fs::file_time_type::clock::now() -
                                std::chrono::hours(24 * 365)};
            std::error_code error;
            fs::last_write_time(path, sentinel, error);
            if (error) {
                return; // filesystem without settable mtimes
            }
            expect(refrain::data::publish_resolution_bytes(path, bytes)
                       .has_value());
            const auto after_same{fs::last_write_time(path, error)};
            expect(!error);
            // Some filesystems store timestamps with second granularity.
            const auto delta{after_same > sentinel ? after_same - sentinel
                                                   : sentinel - after_same};
            expect(delta < std::chrono::seconds{2});

            const std::vector<std::byte> changed{std::byte{9}};
            expect(refrain::data::publish_resolution_bytes(path, changed)
                       .has_value());
            auto reread{io::read_file(path)};
            expect(reread.has_value());
            if (reread) {
                expect(*reread == changed);
            }
        };
    "pending delete queue round-trips atomically, sorted and deduped"_test =
        [] {
            temp_directory scratch{make_temp_directory()};
            const auto root{store_root(scratch)};
            const auto first{std::string{"content/blobs/aa/"} +
                             std::string(64U, 'a')};
            const auto second{std::string{"content/blobs/00/"} +
                              std::string(64U, '0')};
            const auto segment{pending_segment_path(7U)};
            const std::vector<pending_delete> batch{
                pending_blob_entry(first, 20U, 2U),
                pending_blob_entry(second, 10U),
                pending_blob_entry(first, 30U, 5U),
            };
            expect(enqueue_pending_deletes(root, batch).has_value());
            auto loaded{load_pending_deletes(root)};
            expect(loaded.has_value());
            if (!loaded) {
                return;
            }
            expect(loaded->size() == 2U);
            if (loaded->size() != 2U) {
                return;
            }
            expect(loaded->at(0).path == second);
            expect(loaded->at(0).attempts == 0U);
            expect(loaded->at(1).path == first);
            expect(loaded->at(1).attempts == 5U);
            expect(loaded->at(1).size == 30U);

            // Atomic write: exactly pending.json lives in content/gc, and the
            // bytes are valid UTF-8 JSON with the documented schema.
            auto names{io::list_directory(
                refrain::data::join_path(root, "content/gc"))};
            expect(names.has_value());
            if (names) {
                expect(names->size() == 1U);
                if (names->size() == 1U) {
                    expect(names->front() == "pending.json");
                }
            }
            auto text{io::read_text(pending_queue_path(root))};
            expect(text.has_value());
            if (text) {
                auto document{refrain::data::json::value::parse(*text)};
                expect(document.has_value());
                if (document) {
                    const auto *schema{document->find("schema")};
                    expect(schema != nullptr);
                    if (schema != nullptr) {
                        auto schema_text{schema->as_string()};
                        expect(schema_text.has_value());
                        if (schema_text) {
                            expect(*schema_text == "refrain.content-pending/1");
                        }
                    }
                    const auto *entries{document->find("entries")};
                    expect(entries != nullptr);
                    if (entries != nullptr) {
                        auto array{entries->as_array()};
                        expect(array.has_value());
                        if (array) {
                            expect(array->size() == 2U);
                        }
                    }
                }
            }

            // A re-plan must not downgrade the retry counter or the size.
            const std::vector<pending_delete> lower{
                pending_blob_entry(first, 1U, 1U)};
            expect(enqueue_pending_deletes(root, lower).has_value());
            auto preserved{load_pending_deletes(root)};
            expect(preserved.has_value());
            if (preserved && preserved->size() == 2U) {
                expect(preserved->at(1).attempts == 5U);
                expect(preserved->at(1).size == 30U);
            }
            const std::vector<pending_delete> higher{
                pending_blob_entry(first, 40U, 6U)};
            expect(enqueue_pending_deletes(root, higher).has_value());
            auto merged{load_pending_deletes(root)};
            expect(merged.has_value());
            if (merged && merged->size() == 2U) {
                expect(merged->at(1).attempts == 6U);
                expect(merged->at(1).size == 40U);
            }

            // Segment entries sort after blob entries; kinds stay explicit.
            const std::vector<pending_delete> segment_batch{
                pending_segment_entry(segment, 99U, 3U)};
            expect(enqueue_pending_deletes(root, segment_batch).has_value());
            auto all{load_pending_deletes(root)};
            expect(all.has_value());
            if (all) {
                expect(all->size() == 3U);
                if (all->size() == 3U) {
                    expect(all->at(2).path == segment);
                    expect(all->at(2).kind ==
                           pending_delete::pending_delete_kind::segment);
                }
            }
        };

    "pending delete queue rejects unsafe entries and fails closed on damage"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{store_root(scratch)};
        const auto queue{pending_queue_path(root)};

        std::string with_control{"content/blobs/aa/bad"};
        with_control.push_back('\x01');
        const std::vector<pending_delete> invalid{
            pending_blob_entry("", 1U),
            pending_blob_entry("../escape", 1U),
            pending_blob_entry("/absolute/path", 1U),
            pending_blob_entry("content\\blobs\\aa\\file", 1U),
            pending_blob_entry(with_control, 1U),
            pending_blob_entry("content/blobs/../escape", 1U),
            pending_segment_entry("content/blobs/aa/file", 1U),
            pending_blob_entry(pending_segment_path(1U), 1U),
        };
        for (const auto &entry : invalid) {
            auto rejected{enqueue_pending_deletes(root, std::span{&entry, 1U})};
            expect(!rejected);
            if (!rejected) {
                expect(rejected.error().code ==
                       refrain::error_code::invalid_argument);
            }
        }
        expect(!io::exists(queue));

        // Foreign schema / truncated JSON / unsafe path / unsorted /
        // duplicate / bad field: parse_error, original file untouched.
        const std::vector<std::string> damaged{
            R"({"schema":"foreign/1","entries":[]})",
            R"({"schema":"refrain.content-pending/1","entries":[)",
            R"({"schema":"refrain.content-pending/1","entries":[{"path":"../evil","kind":"blob","size":1,"attempts":0}]})",
            R"({"schema":"refrain.content-pending/1","entries":[{"path":"content/blobs/zz/b","kind":"blob","size":1,"attempts":0},{"path":"content/blobs/aa/a","kind":"blob","size":1,"attempts":0}]})",
            R"({"schema":"refrain.content-pending/1","entries":[{"path":"content/blobs/aa/a","kind":"blob","size":1,"attempts":0},{"path":"content/blobs/aa/a","kind":"blob","size":2,"attempts":0}]})",
            R"({"schema":"refrain.content-pending/1","entries":[{"path":"content/blobs/aa/a","kind":"blob","size":-1,"attempts":0}]})",
            R"({"schema":"refrain.content-pending/1","entries":[{"path":"content/blobs/aa/a","kind":"blob","size":1}]})",
        };
        for (const auto &text : damaged) {
            expect(write_pending_queue_text(root, text));
            auto rejected{load_pending_deletes(root)};
            expect(!rejected);
            if (!rejected) {
                expect(rejected.error().code ==
                       refrain::error_code::parse_error);
            }
            // An apply must fail closed before publishing/deleting.
            gc_options options{};
            options.apply = true;
            auto refused{collect_garbage(root, options)};
            expect(!refused);
            if (!refused) {
                expect(refused.error().code ==
                       refrain::error_code::parse_error);
            }
            auto drain{drain_pending_deletes(root, 0)};
            expect(!drain);
            if (!drain) {
                expect(drain.error().code == refrain::error_code::parse_error);
            }
            auto after{io::read_text(queue)};
            expect(after.has_value());
            if (after) {
                expect(*after == text);
            }
        }
    };

    "collect_garbage defer mode queues planned blobs/segments without deleting"_test =
        [] {
            temp_directory scratch{make_temp_directory()};
            const auto root{store_root(scratch)};
            auto store{content_store::open(root)};
            expect(store.has_value());
            if (!store) {
                return;
            }
            const auto root_payload{bytes_of("rooted blob")};
            const auto root_digest{store->put(root_payload)};
            const auto orphan_payload{bytes_of("orphan blob")};
            const auto orphan_digest{store->put(orphan_payload)};
            const std::vector<std::vector<std::byte>> segment_payload{
                bytes_of("orphan segment")};
            const auto segment_blobs{stage(root, segment_payload)};
            expect(root_digest.has_value());
            expect(orphan_digest.has_value());
            expect(segment_blobs.size() == 1U);
            if (!root_digest || !orphan_digest || segment_blobs.size() != 1U) {
                return;
            }
            const auto root_record{
                standalone_record(*root_digest, root_payload.size())};
            expect(write_table(root, std::span{&root_record, 1U}));

            // A dry run with defer_deletes still plans only (no queue file).
            gc_options plan{};
            plan.defer_deletes = true;
            plan.extra_roots = {*root_digest};
            auto planned{collect_garbage(root, plan)};
            expect(planned.has_value());
            if (planned) {
                expect(planned->unreferenced_blobs == 1U);
                expect(planned->orphan_segments == 1U);
                expect(planned->queued_blobs == 0U);
                expect(planned->deleted_blobs == 0U);
            }
            expect(!io::exists(pending_queue_path(root)));

            gc_options options{};
            options.apply = true;
            options.defer_deletes = true;
            options.extra_roots = {*root_digest};
            std::vector<std::string> lines;
            auto report{
                collect_garbage(root, options, [&lines](std::string_view line) {
                    lines.emplace_back(line);
                })};
            expect(report.has_value());
            if (!report) {
                return;
            }
            expect(report->applied);
            expect(report->queued_blobs == 1U);
            expect(report->queued_blob_bytes == orphan_payload.size());
            expect(report->queued_segments == 1U);
            expect(report->queued_segment_bytes > 0U);
            expect(report->deleted_blobs == 0U);
            expect(report->deleted_segments == 0U);
            expect(report->deferred_blobs == 0U);
            bool saw_queued{false};
            for (const auto &line : lines) {
                if (line.find("queued") != std::string::npos) {
                    saw_queued = true;
                }
            }
            expect(saw_queued);

            // Nothing was unlinked.
            expect(io::is_regular_file(store->path_for(*orphan_digest)));
            expect(io::exists(segment_path(root, segment_blobs[0].segment_id)));

            auto queued{load_pending_deletes(root)};
            expect(queued.has_value());
            if (queued) {
                expect(queued->size() == 2U);
                if (queued->size() == 2U) {
                    expect(queued->at(0).path ==
                           pending_blob_path(*orphan_digest));
                    expect(queued->at(0).kind ==
                           pending_delete::pending_delete_kind::blob);
                    expect(queued->at(0).size == orphan_payload.size());
                    expect(queued->at(0).attempts == 0U);
                    expect(queued->at(1).path ==
                           pending_segment_path(segment_blobs[0].segment_id));
                    expect(queued->at(1).kind ==
                           pending_delete::pending_delete_kind::segment);
                }
            }
        };

    "drain deletes queued files, is idempotent and tolerates missing targets"_test =
        [] {
            temp_directory scratch{make_temp_directory()};
            const auto root{store_root(scratch)};
            const auto blob{std::string{"content/blobs/aa/"} +
                            std::string(64U, 'b')};
            const auto segment{pending_segment_path(3U)};
            const auto blob_path{refrain::data::join_path(root, blob)};
            const auto segment_path{refrain::data::join_path(root, segment)};
            expect(io::create_directories(
                       std::string{refrain::data::parent_path(blob_path)})
                       .has_value());
            expect(io::create_directories(
                       std::string{refrain::data::parent_path(segment_path)})
                       .has_value());
            expect(io::write_text(blob_path, "blob").has_value());
            expect(io::write_text(segment_path, "segment").has_value());
            const std::vector<pending_delete> batch{
                pending_blob_entry(blob, 4U),
                pending_segment_entry(segment, 7U),
            };
            expect(enqueue_pending_deletes(root, batch).has_value());

            std::vector<std::string> lines;
            auto report{
                drain_pending_deletes(root, 0, [&lines](std::string_view line) {
                    lines.emplace_back(line);
                })};
            expect(report.has_value());
            if (!report) {
                return;
            }
            expect(report->deleted == 2U);
            expect(report->deleted_bytes == 11U);
            expect(report->deleted_blobs == 1U);
            expect(report->deleted_blob_bytes == 4U);
            expect(report->deleted_segments == 1U);
            expect(report->deleted_segment_bytes == 7U);
            expect(report->failed == 0U);
            expect(report->deferred == 0U);
            expect(report->remaining == 0U);
            expect(!io::exists(blob_path));
            expect(!io::exists(segment_path));
            expect(!io::exists(pending_queue_path(root)));
            bool saw_deleted{false};
            for (const auto &line : lines) {
                if (line.find("deleted 2 pending file(s)") !=
                    std::string::npos) {
                    saw_deleted = true;
                }
            }
            expect(saw_deleted);

            // Idempotent: an empty/missing queue is a successful empty pass.
            auto repeated{drain_pending_deletes(root, 0)};
            expect(repeated.has_value());
            if (repeated) {
                expect(repeated->deleted == 0U);
                expect(repeated->remaining == 0U);
            }

            // A target that vanished before the drain is success, not failure.
            const std::vector<pending_delete> missing{
                pending_blob_entry("content/blobs/aa/already-gone", 5U)};
            expect(enqueue_pending_deletes(root, missing).has_value());
            auto gone{drain_pending_deletes(root, 0)};
            expect(gone.has_value());
            if (gone) {
                expect(gone->deleted == 1U);
                expect(gone->failed == 0U);
                expect(gone->remaining == 0U);
            }
            expect(!io::exists(pending_queue_path(root)));
        };

    "drain failure keeps the entry with attempts and repair finishes it"_test =
        [] {
            temp_directory scratch{make_temp_directory()};
            const auto root{store_root(scratch)};
            const auto blocked{std::string{"content/blobs/cc/"} +
                               std::string(64U, 'c')};
            const auto blocked_path{refrain::data::join_path(root, blocked)};
            // A non-empty directory at the queued path: the drain refuses to
            // remove directories instead of recursively deleting them.
            expect(io::create_directories(blocked_path).has_value());
            expect(io::write_text(
                       refrain::data::join_path(blocked_path, "blocker"), "x")
                       .has_value());
            const std::vector<pending_delete> batch{
                pending_blob_entry(blocked, 11U)};
            expect(enqueue_pending_deletes(root, batch).has_value());

            auto first{drain_pending_deletes(root, 0)};
            expect(first.has_value());
            if (!first) {
                return;
            }
            expect(first->deleted == 0U);
            expect(first->failed == 1U);
            expect(first->failed_bytes == 11U);
            expect(first->remaining == 1U);
            expect(io::is_directory(blocked_path));
            auto kept{load_pending_deletes(root)};
            expect(kept.has_value());
            if (kept && kept->size() == 1U) {
                expect(kept->at(0).attempts == 1U);
                expect(kept->at(0).size == 11U);
            }

            auto second{drain_pending_deletes(root, 0)};
            expect(second.has_value());
            if (second) {
                expect(second->failed == 1U);
                expect(second->remaining == 1U);
            }
            auto retried{load_pending_deletes(root)};
            expect(retried.has_value());
            if (retried && retried->size() == 1U) {
                expect(retried->at(0).attempts == 2U);
            }

            // Repair the filesystem: the next drain removes the entry.
            std::error_code remove_error;
            fs::remove_all(fs::path{blocked_path}, remove_error);
            expect(!remove_error);
            expect(io::write_text(blocked_path, "repaired").has_value());
            auto repaired{drain_pending_deletes(root, 0)};
            expect(repaired.has_value());
            if (repaired) {
                expect(repaired->deleted == 1U);
                expect(repaired->remaining == 0U);
            }
            expect(!io::exists(blocked_path));
            expect(!io::exists(pending_queue_path(root)));

            // Retry limit: an entry at the cap is kept, skipped and reported.
            const auto stuck{std::string{"content/blobs/dd/"} +
                             std::string(64U, 'd')};
            const auto stuck_path{refrain::data::join_path(root, stuck)};
            expect(io::create_directories(
                       std::string{refrain::data::parent_path(stuck_path)})
                       .has_value());
            expect(io::write_text(stuck_path, "stuck").has_value());
            const std::vector<pending_delete> stuck_batch{
                pending_blob_entry(stuck, 5U, max_pending_delete_attempts)};
            expect(enqueue_pending_deletes(root, stuck_batch).has_value());
            std::vector<std::string> lines;
            auto capped{
                drain_pending_deletes(root, 0, [&lines](std::string_view line) {
                    lines.emplace_back(line);
                })};
            expect(capped.has_value());
            if (capped) {
                expect(capped->failed == 0U);
                expect(capped->failed_stuck == 1U);
                expect(capped->remaining == 1U);
            }
            expect(io::is_regular_file(stuck_path));
            auto still_queued{load_pending_deletes(root)};
            expect(still_queued.has_value());
            if (still_queued && still_queued->size() == 1U) {
                expect(still_queued->at(0).attempts ==
                       max_pending_delete_attempts);
            }
            bool saw_limit{false};
            for (const auto &line : lines) {
                if (line.find("retry limit") != std::string::npos) {
                    saw_limit = true;
                }
            }
            expect(saw_limit);
        };

    "drain budget defers entries and the next unlimited pass finishes"_test =
        [] {
            temp_directory scratch{make_temp_directory()};
            const auto root{store_root(scratch)};
            const auto parent{"content/blobs/budget"};
            expect(
                io::create_directories(refrain::data::join_path(root, parent))
                    .has_value());
            constexpr std::size_t file_count{2000};
            std::vector<pending_delete> batch;
            batch.reserve(file_count);
            for (std::size_t index{0}; index < file_count; ++index) {
                const auto relative{
                    fmt::format("{}/file-{:04d}", parent, index)};
                expect(io::write_text(refrain::data::join_path(root, relative),
                                      "x")
                           .has_value());
                batch.push_back(pending_blob_entry(relative, 1U));
            }
            expect(enqueue_pending_deletes(root, batch).has_value());
            auto first{drain_pending_deletes(root, 1)};
            expect(first.has_value());
            if (!first) {
                return;
            }
            expect(first->budget_exhausted);
            expect(first->deferred > 0U);
            expect(first->deleted + first->deferred == file_count);
            expect(first->remaining == first->deferred);
            auto left{load_pending_deletes(root)};
            expect(left.has_value());
            if (left) {
                expect(left->size() == first->deferred);
            }
            auto second{drain_pending_deletes(root, 0)};
            expect(second.has_value());
            if (second) {
                expect(second->deleted == first->deferred);
                expect(second->remaining == 0U);
            }
            expect(!io::exists(pending_queue_path(root)));
        };

    "drain drops a queued path that blobs.idx references again"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{store_root(scratch)};
        auto store{content_store::open(root)};
        expect(store.has_value());
        if (!store) {
            return;
        }
        const auto payload{bytes_of("live again")};
        const auto digest{store->put(payload)};
        expect(digest.has_value());
        if (!digest) {
            return;
        }
        const auto record{standalone_record(*digest, payload.size())};
        expect(write_table(root, std::span{&record, 1U}));
        const auto relative{pending_blob_path(*digest)};
        const std::vector<pending_delete> stale{
            pending_blob_entry(relative, payload.size())};
        expect(enqueue_pending_deletes(root, stale).has_value());

        auto report{drain_pending_deletes(root, 0)};
        expect(report.has_value());
        if (report) {
            expect(report->deleted == 0U);
            expect(report->skipped_referenced == 1U);
            expect(report->remaining == 0U);
        }
        expect(io::is_regular_file(store->path_for(*digest)));
        expect(!io::exists(pending_queue_path(root)));
    };

    "drain fails closed when blobs.idx is corrupt"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{store_root(scratch)};
        const auto relative{std::string{"content/blobs/ee/"} +
                            std::string(64U, 'e')};
        const auto target{refrain::data::join_path(root, relative)};
        expect(io::create_directories(
                   std::string{refrain::data::parent_path(target)})
                   .has_value());
        expect(io::write_text(target, "target").has_value());
        const std::vector<pending_delete> batch{
            pending_blob_entry(relative, 6U)};
        expect(enqueue_pending_deletes(root, batch).has_value());
        expect(io::atomic_write_file(content_blob_table_path(root),
                                     bytes_of("not a table"))
                   .has_value());

        auto report{drain_pending_deletes(root, 0)};
        expect(!report);
        if (!report) {
            expect(report.error().code == refrain::error_code::parse_error);
        }
        // Fail closed: the target and its queue entry stay untouched.
        expect(io::is_regular_file(target));
        auto queued{load_pending_deletes(root)};
        expect(queued.has_value());
        if (queued) {
            expect(queued->size() == 1U);
        }
    };

    "pending queue survives restart, is not a GC root, and crash leftovers "
    "drain"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{store_root(scratch)};
        auto store{content_store::open(root)};
        expect(store.has_value());
        if (!store) {
            return;
        }
        const auto root_payload{bytes_of("rooted blob")};
        const auto root_digest{store->put(root_payload)};
        const auto orphan_payload{bytes_of("orphan blob")};
        const auto orphan_digest{store->put(orphan_payload)};
        expect(root_digest.has_value());
        expect(orphan_digest.has_value());
        if (!root_digest || !orphan_digest) {
            return;
        }
        const auto root_record{
            standalone_record(*root_digest, root_payload.size())};
        expect(write_table(root, std::span{&root_record, 1U}));

        gc_options deferred{};
        deferred.apply = true;
        deferred.defer_deletes = true;
        deferred.extra_roots = {*root_digest};
        auto queued{collect_garbage(root, deferred)};
        expect(queued.has_value());
        if (!queued) {
            return;
        }
        expect(queued->queued_blobs == 1U);
        expect(io::is_regular_file(store->path_for(*orphan_digest)));

        // "Restart": the queue is read back from disk by a fresh call.
        auto survived{load_pending_deletes(root)};
        expect(survived.has_value());
        expect(survived && survived->size() == 1U);

        // The queue is a deletion intent, not a GC root: the next mark-sweep
        // still sees the orphan as unreferenced.
        gc_options plan{};
        plan.extra_roots = {*root_digest};
        auto planned{collect_garbage(root, plan)};
        expect(planned.has_value());
        if (planned) {
            expect(planned->unreferenced_blobs == 1U);
        }

        // Crash after the unlink but before the queue update: the target is
        // gone, the entry is stale, and the next drain clears it as success.
        std::error_code remove_error;
        expect(fs::remove(fs::path{store->path_for(*orphan_digest)},
                          remove_error));
        expect(!remove_error);
        auto drained{drain_pending_deletes(root, 0)};
        expect(drained.has_value());
        if (drained) {
            expect(drained->deleted == 1U);
            expect(drained->remaining == 0U);
        }
        auto cleared{load_pending_deletes(root)};
        expect(cleared.has_value());
        if (cleared) {
            expect(cleared->empty());
        }
    };
};

} // namespace
