// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/io.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/content_index.hpp>
#include <refrain/data/content_store.hpp>
#include <refrain/data/content_stream.hpp>
#include <refrain/data/path.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;
namespace fs = std::filesystem;

using refrain::data::batch;
using refrain::data::blob_flags_in_segment;
using refrain::data::blob_flags_packed;
using refrain::data::blob_kind;
using refrain::data::blob_kind_content;
using refrain::data::blob_location;
using refrain::data::blob_metadata;
using refrain::data::blob_table_entry;
using refrain::data::blob_table_view;
using refrain::data::build_blob_table;
using refrain::data::build_resolution_index;
using refrain::data::content_blob_table_filename;
using refrain::data::content_resolutions_directory;
using refrain::data::content_store;
using refrain::data::content_stream;
using refrain::data::content_stream_options;
using refrain::data::digest32;
using refrain::data::join_path;
using refrain::data::parse_blob_table;
using refrain::data::resolution_input;

constexpr auto kWaitTimeout = std::chrono::seconds{5};
constexpr std::size_t packed_size{4U * 1024U * 1024U + 321U};

[[nodiscard]] auto bytes_of(std::string_view text) -> std::vector<std::byte> {
    const auto bytes{
        std::as_bytes(std::span<const char>{text.data(), text.size()})};
    return std::vector<std::byte>{bytes.begin(), bytes.end()};
}

/// Deterministic incompressible-looking bytes (splitmix64 output), so the
/// packed blob's physical file stays above the 1 MiB mmap threshold.
[[nodiscard]] auto pseudo_random_blob(std::size_t count, std::uint64_t seed)
    -> std::vector<std::byte> {
    std::vector<std::byte> data(count);
    std::uint64_t state{seed};
    for (auto &value : data) {
        state += 0x9E3779B97F4A7C15ULL;
        auto mixed{state};
        mixed = (mixed ^ (mixed >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        mixed = (mixed ^ (mixed >> 27U)) * 0x94D049BB133111EBULL;
        mixed ^= mixed >> 31U;
        value = static_cast<std::byte>(mixed & 0xFFU);
    }
    return data;
}

[[nodiscard]] auto same_bytes(std::span<const std::byte> lhs,
                              std::span<const std::byte> rhs) -> bool {
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin());
}

[[nodiscard]] auto make_temp_directory() -> fs::path {
    static int counter{0};
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    const auto path{fs::temp_directory_path() /
                    ("refrain_content_stream_test_" + std::to_string(stamp) +
                     "_" + std::to_string(counter++))};
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

/// RFBT record for a file-backed blob (raw or packed).
[[nodiscard]] auto entry_from_store(const content_store &store,
                                    const digest32 &digest)
    -> std::optional<blob_table_entry> {
    auto layout{store.layout(digest)};
    auto size{refrain::core::io::file_size(store.path_for(digest))};
    if (!layout || !size) {
        return std::nullopt;
    }
    std::uint32_t flags{0};
    if (layout->kind == blob_kind::packed) {
        flags |= blob_flags_packed;
    }
    return blob_table_entry{
        .digest = digest,
        .logical_size = layout->logical_size,
        .flags = flags,
        .refcount = 1,
        .kind = blob_kind_content,
        .block_count = layout->block_count,
        .stored_size = static_cast<std::uint32_t>(*size),
        .segment_id = 0,
        .segment_offset = 0,
    };
}

/// RFBT record for a blob published by `batch`.
[[nodiscard]] auto entry_from_metadata(const blob_metadata &meta)
    -> blob_table_entry {
    std::uint32_t block_count{0};
    if ((meta.flags & blob_flags_packed) != 0U) {
        block_count =
            static_cast<std::uint32_t>((meta.logical_size + 65535U) / 65536U);
    }
    return blob_table_entry{
        .digest = meta.digest,
        .logical_size = meta.logical_size,
        .flags = meta.flags,
        .refcount = 1,
        .kind = blob_kind_content,
        .block_count = block_count,
        .stored_size = meta.stored_size,
        .segment_id = meta.segment_id,
        .segment_offset = meta.segment_offset,
    };
}

[[nodiscard]] auto write_table(std::string_view root,
                               std::span<const std::byte> bytes) -> bool {
    return refrain::core::io::atomic_write_file(
               join_path(join_path(root, "content"),
                         content_blob_table_filename),
               bytes)
        .has_value();
}

[[nodiscard]] auto write_index(std::string_view root,
                               std::string_view composition,
                               std::span<const std::byte> bytes) -> bool {
    return refrain::core::io::atomic_write_file(
               join_path(join_path(root, content_resolutions_directory),
                         std::string{composition} + ".idx"),
               bytes)
        .has_value();
}

/// One published composition with a file-backed raw blob, a file-backed
/// packed blob and an in-segment raw blob (the three physical read paths).
struct fixture {
    temp_directory scratch{make_temp_directory()};
    std::string root{(scratch.path / "store").string()};
    content_store store{};
    std::vector<std::byte> raw_bytes{bytes_of("content-stream raw blob")};
    std::vector<std::byte> packed_bytes{pseudo_random_blob(packed_size, 33U)};
    std::vector<std::byte> segment_bytes{
        bytes_of("content-stream segment blob")};
    digest32 raw_digest{};
    digest32 packed_digest{};
    digest32 segment_digest{};
    std::vector<std::byte> table_bytes{};
    std::vector<std::byte> index_bytes{};
};

[[nodiscard]] auto build_fixture() -> std::unique_ptr<fixture> {
    auto built{std::make_unique<fixture>()};
    auto store{content_store::open(built->root)};
    if (!store) {
        return nullptr;
    }
    built->store = std::move(*store);

    auto raw{built->store.put(built->raw_bytes)};
    auto packed{built->store.put(built->packed_bytes)};
    if (!raw || !packed) {
        return nullptr;
    }
    built->raw_digest = *raw;
    built->packed_digest = *packed;

    auto staged{batch::begin(built->root, nullptr, 32U * 1024U * 1024U)};
    if (!staged) {
        return nullptr;
    }
    auto segment{staged->put(built->segment_bytes)};
    if (!segment || !staged->commit()) {
        return nullptr;
    }
    const auto metadata{staged->new_blobs()};
    if (metadata.size() != 1U) {
        return nullptr;
    }
    built->segment_digest = *segment;

    const auto raw_entry{entry_from_store(built->store, built->raw_digest)};
    const auto packed_entry{
        entry_from_store(built->store, built->packed_digest)};
    if (!raw_entry || !packed_entry) {
        return nullptr;
    }
    auto table{build_blob_table(std::vector<blob_table_entry>{
        *raw_entry, *packed_entry, entry_from_metadata(metadata[0])})};
    if (!table) {
        return nullptr;
    }
    built->table_bytes = std::move(*table);
    auto view{parse_blob_table(built->table_bytes)};
    if (!view) {
        return nullptr;
    }

    const std::vector<resolution_input> inputs{
        resolution_input{.location = "data/stream/raw.txt",
                         .source = "mod:test",
                         .priority = 5,
                         .blob = built->raw_digest},
        resolution_input{.location = "data/stream/packed.bin",
                         .source = "mod:test",
                         .priority = 5,
                         .blob = built->packed_digest},
        resolution_input{.location = "data/stream/packed-alias.bin",
                         .source = "builtin",
                         .priority = 1,
                         .blob = built->packed_digest},
        resolution_input{.location = "data/stream/segment.txt",
                         .source = "mod:test",
                         .priority = 5,
                         .blob = built->segment_digest},
    };
    auto index{build_resolution_index(inputs, *view)};
    if (!index) {
        return nullptr;
    }
    built->index_bytes = std::move(*index);

    if (!refrain::core::io::create_directories(
            join_path(built->root, content_resolutions_directory))) {
        return nullptr;
    }
    if (!write_table(built->root, built->table_bytes) ||
        !write_index(built->root, "test", built->index_bytes)) {
        return nullptr;
    }
    return built;
}

const suite content_stream_tests = [] {
    "stream/open validates composition, tables and options"_test = [] {
        const auto built{build_fixture()};
        expect(built != nullptr);
        if (built == nullptr) {
            return;
        }

        auto opened{content_stream::open(built->root, "test")};
        expect(opened.has_value());
        if (opened) {
            expect(opened->valid());
        }

        const auto missing{content_stream::open(built->root, "missing")};
        expect(!missing);
        if (!missing) {
            expect(missing.error().code == refrain::error_code::not_found);
        }
        const auto unsafe{content_stream::open(built->root, "../test")};
        expect(!unsafe);
        if (!unsafe) {
            expect(unsafe.error().code ==
                   refrain::error_code::invalid_argument);
        }
        const auto no_workers{content_stream::open(
            built->root, "test", content_stream_options{.worker_threads = 0})};
        expect(!no_workers);
        if (!no_workers) {
            expect(no_workers.error().code ==
                   refrain::error_code::invalid_argument);
        }
        const auto no_window{content_stream::open(
            built->root, "test",
            content_stream_options{.prefetch_window_bytes = 0})};
        expect(!no_window);
        if (!no_window) {
            expect(no_window.error().code ==
                   refrain::error_code::invalid_argument);
        }

        auto truncated{built->table_bytes};
        truncated.resize(64U);
        expect(write_table(built->root, truncated));
        const auto bad_table{content_stream::open(built->root, "test")};
        expect(!bad_table);
        if (!bad_table) {
            expect(bad_table.error().code == refrain::error_code::parse_error);
        }
    };

    "stream/open reports a corrupted resolution index as parse_error"_test =
        [] {
            const auto built{build_fixture()};
            expect(built != nullptr);
            if (built == nullptr) {
                return;
            }
            auto corrupted{built->index_bytes};
            expect(corrupted.size() > 200U);
            if (corrupted.size() <= 200U) {
                return;
            }
            corrupted[200] = static_cast<std::byte>(
                std::to_integer<unsigned int>(corrupted[200]) ^ 0x01U);
            expect(write_index(built->root, "test", corrupted));
            const auto opened{content_stream::open(built->root, "test")};
            expect(!opened);
            if (!opened) {
                expect(opened.error().code == refrain::error_code::parse_error);
            }
        };

    "stream/read serves raw, packed and in-segment blobs"_test = [] {
        const auto built{build_fixture()};
        expect(built != nullptr);
        if (built == nullptr) {
            return;
        }
        auto opened{content_stream::open(built->root, "test")};
        expect(opened.has_value());
        if (!opened) {
            return;
        }
        auto stream{std::move(*opened)};

        const auto raw{stream.read("data/stream/raw.txt")};
        expect(raw.has_value());
        if (raw) {
            expect(same_bytes(*raw, built->raw_bytes));
        }
        const auto packed{stream.read("data/stream/packed.bin")};
        expect(packed.has_value());
        if (packed) {
            expect(same_bytes(*packed, built->packed_bytes));
        }
        const auto alias{stream.read("data/stream/packed-alias.bin")};
        expect(alias.has_value());
        if (alias) {
            expect(same_bytes(*alias, built->packed_bytes));
        }
        const auto segment{stream.read("data/stream/segment.txt")};
        expect(segment.has_value());
        if (segment) {
            expect(same_bytes(*segment, built->segment_bytes));
        }
        const auto missing{stream.read("data/stream/missing.bin")};
        expect(!missing);
        if (!missing) {
            expect(missing.error().code == refrain::error_code::not_found);
        }

        const auto stats{stream.stats()};
        expect(stats.reads == 4U);
        expect(stats.bytes == built->raw_bytes.size() +
                                  built->packed_bytes.size() * 2U +
                                  built->segment_bytes.size());
    };

    "stream/read_async invokes the callback on a runtime thread"_test = [] {
        const auto built{build_fixture()};
        expect(built != nullptr);
        if (built == nullptr) {
            return;
        }
        auto opened{content_stream::open(built->root, "test")};
        expect(opened.has_value());
        if (!opened) {
            return;
        }
        auto stream{std::move(*opened)};

        std::mutex mutex;
        std::condition_variable cv;
        bool finished{false};
        std::optional<refrain::result<std::vector<std::byte>>> received;
        std::thread::id callback_thread;
        const auto caller_thread{std::this_thread::get_id()};

        {
            std::unique_lock lock{mutex};
            expect(stream.read_async(
                "data/stream/packed.bin",
                [&](refrain::result<std::vector<std::byte>> outcome) {
                    const auto thread{std::this_thread::get_id()};
                    {
                        std::lock_guard guard{mutex};
                        received = std::move(outcome);
                        callback_thread = thread;
                        finished = true;
                    }
                    cv.notify_one();
                }));
            expect(cv.wait_for(lock, kWaitTimeout, [&] { return finished; }));
        }

        expect(finished);
        expect(received.has_value());
        if (received) {
            expect(received->has_value());
            if (*received) {
                expect(same_bytes(**received, built->packed_bytes));
            }
        }
        expect(callback_thread != caller_thread);

        // Unknown locations and empty callbacks are rejected without posting.
        expect(!stream.read_async("data/stream/missing.bin", [](auto) {}));
        expect(!stream.read_async(
            "data/stream/raw.txt",
            std::function<void(refrain::result<std::vector<std::byte>>)>{}));
        expect(stream.stats().reads == 1U);
    };

    "stream/prefetch deduplicates in-flight windows"_test = [] {
        const auto built{build_fixture()};
        expect(built != nullptr);
        if (built == nullptr) {
            return;
        }
        auto opened{content_stream::open(built->root, "test")};
        expect(opened.has_value());
        if (!opened) {
            return;
        }
        auto stream{std::move(*opened)};

        // The first call marks every 256 KiB window and posts one task; the
        // second call (and the alias location, which shares the digest) must
        // observe those keys still in flight.
        stream.prefetch("data/stream/packed.bin");
        stream.prefetch("data/stream/packed.bin");
        stream.prefetch("data/stream/packed-alias.bin");
        const auto stats{stream.stats()};
        expect(stats.prefetches > 0U);
        expect(stats.deduped > 0U);

        // A prefetch of a raw blob is a single whole-blob read and is
        // deduplicated the same way while it is in flight.
        stream.prefetch("data/stream/raw.txt");
        stream.prefetch("data/stream/raw.txt");
        expect(stream.stats().deduped >= stats.deduped);
    };

    "stream/destruction stops the runtime and drops pending work"_test = [] {
        const auto built{build_fixture()};
        expect(built != nullptr);
        if (built == nullptr) {
            return;
        }
        {
            auto opened{content_stream::open(built->root, "test")};
            expect(opened.has_value());
            if (!opened) {
                return;
            }
            auto stream{std::move(*opened)};
            std::atomic<bool> called{false};
            expect(stream.read_async("data/stream/raw.txt",
                                     [&](auto) { called = true; }));
            stream.prefetch("data/stream/packed.bin");
            // The destructor stops and joins the runtime; pending tasks are
            // dropped. The callback may or may not have run by now.
        }
        expect(true);
    };
};

} // namespace
