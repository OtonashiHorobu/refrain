// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/io.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/content_index.hpp>
#include <refrain/data/content_segment.hpp>
#include <refrain/data/content_store.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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
using refrain::data::blake3;
using refrain::data::blob_flags_all_raw_blocks;
using refrain::data::blob_flags_in_segment;
using refrain::data::blob_flags_packed;
using refrain::data::blob_kind;
using refrain::data::blob_kind_content;
using refrain::data::blob_location;
using refrain::data::blob_location_from_table_entry;
using refrain::data::blob_metadata;
using refrain::data::blob_table_entry;
using refrain::data::blob_table_view;
using refrain::data::build_blob_table;
using refrain::data::content_segment_directory;
using refrain::data::content_store;
using refrain::data::content_store_options;
using refrain::data::crc32c;
using refrain::data::digest32;
using refrain::data::parse_blob_table;
using refrain::data::segment_path;
using refrain::data::to_hex;

[[nodiscard]] auto bytes_of(std::string_view text) -> std::vector<std::byte> {
    const auto bytes{
        std::as_bytes(std::span<const char>{text.data(), text.size()})};
    return std::vector<std::byte>{bytes.begin(), bytes.end()};
}

[[nodiscard]] auto text_of(std::span<const std::byte> bytes) -> std::string {
    if (bytes.empty()) {
        return std::string{};
    }
    return std::string{reinterpret_cast<const char *>(bytes.data()), // NOLINT
                       bytes.size()};
}

/// Deterministic 0..255 walk, used to prove the store is byte-transparent for
/// arbitrary binary payloads.
[[nodiscard]] auto binary_blob(std::size_t count) -> std::vector<std::byte> {
    std::vector<std::byte> data(count);
    for (std::size_t index{0}; index < count; ++index) {
        data[index] = static_cast<std::byte>(index % 256U);
    }
    return data;
}

[[nodiscard]] auto make_temp_directory() -> fs::path {
    static int counter{0};
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    const auto path{fs::temp_directory_path() /
                    ("refrain_cas_test_" + std::to_string(stamp) + "_" +
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

/// `<root>/content/blobs/<hex[0:2]>/<hex>` — spelled out independently of the
/// implementation so the on-disk layout is pinned by the test.
[[nodiscard]] auto expected_path(std::string_view root, const digest32 &digest)
    -> std::string {
    const auto hex{to_hex(digest)};
    std::string path{root};
    path += "/content/blobs/";
    path += hex.substr(0, 2);
    path += '/';
    path += hex;
    return path;
}

[[nodiscard]] auto expected_blobs_root(std::string_view root) -> std::string {
    std::string path{root};
    path += "/content/blobs";
    return path;
}

/// Deterministic incompressible-looking bytes (splitmix64 output), used to pin
/// the "compression did not help, store the block verbatim" path.
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

[[nodiscard]] auto repeated_blob(std::size_t count, std::byte value)
    -> std::vector<std::byte> {
    return std::vector<std::byte>(count, value);
}

/// 64-byte block alignment, spelled out independently of the implementation.
[[nodiscard]] constexpr auto align_up_64(std::uint64_t value) noexcept
    -> std::uint64_t {
    return (value + 63U) & ~std::uint64_t{63U};
}

/// `<bytes>[offset, offset + length)`, spelled out independently of the
/// implementation.
[[nodiscard]] auto slice_of(std::span<const std::byte> bytes,
                            std::size_t offset, std::size_t length)
    -> std::vector<std::byte> {
    const auto part{bytes.subspan(offset, length)};
    return std::vector<std::byte>{part.begin(), part.end()};
}

/// Byte-wise equality that returns a plain bool (avoids boost.ut's range
/// comparison printer, which would try to stream every std::byte).
[[nodiscard]] auto same_bytes(std::span<const std::byte> lhs,
                              std::span<const std::byte> rhs) -> bool {
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin());
}

// Little-endian field accessors for the RFBL layout, written independently of
// the implementation so the binary format is pinned.
[[nodiscard]] auto read_u16_le(std::span<const std::byte> bytes,
                               std::size_t offset) -> std::uint16_t {
    return static_cast<std::uint16_t>(
        std::to_integer<unsigned int>(bytes[offset]) |
        (std::to_integer<unsigned int>(bytes[offset + 1U]) << 8U));
}

[[nodiscard]] auto read_u32_le(std::span<const std::byte> bytes,
                               std::size_t offset) -> std::uint32_t {
    std::uint32_t value{0};
    for (std::size_t index{0}; index < 4U; ++index) {
        value |= std::to_integer<unsigned int>(bytes[offset + index])
                 << (8U * index);
    }
    return value;
}

[[nodiscard]] auto read_u64_le(std::span<const std::byte> bytes,
                               std::size_t offset) -> std::uint64_t {
    std::uint64_t value{0};
    for (std::size_t index{0}; index < 8U; ++index) {
        value |= static_cast<std::uint64_t>(
                     std::to_integer<unsigned int>(bytes[offset + index]))
                 << (8U * index);
    }
    return value;
}

void write_u32_le(std::span<std::byte> bytes, std::size_t offset,
                  std::uint32_t value) {
    for (std::size_t index{0}; index < 4U; ++index) {
        bytes[offset + index] =
            static_cast<std::byte>((value >> (8U * index)) & 0xFFU);
    }
}

[[nodiscard]] auto magic_is_rfbl(std::span<const std::byte> file) -> bool {
    return file.size() >= 4U && text_of(file.first(4)) == "RFBL";
}

/// The 16-byte table hash at offset 64 covers the block table at offset 96.
[[nodiscard]] auto table_digest_matches(std::span<const std::byte> file,
                                        std::size_t table_size) -> bool {
    const auto table_digest{
        refrain::data::blake3(file.subspan(96U, table_size))};
    return std::equal(table_digest.begin(), table_digest.begin() + 16,
                      file.begin() + 64);
}

[[nodiscard]] auto reserved_bytes_zero(std::span<const std::byte> file)
    -> bool {
    return std::all_of(file.begin() + 80, file.begin() + 96,
                       [](std::byte value) { return value == std::byte{0}; });
}

/// `block_blake3` of the table entry at `entry_offset` equals the BLAKE3 of the
/// uncompressed logical block bytes.
[[nodiscard]] auto block_hash_matches(std::span<const std::byte> file,
                                      std::size_t entry_offset,
                                      std::span<const std::byte> logical_block)
    -> bool {
    refrain::data::digest32 stored{};
    std::copy_n(file.begin() + static_cast<std::ptrdiff_t>(entry_offset + 16U),
                stored.size(), stored.begin());
    return stored == refrain::data::blake3(logical_block);
}

[[nodiscard]] auto read_blob_file(const content_store &store,
                                  const digest32 &digest)
    -> std::vector<std::byte> {
    return refrain::core::io::read_file(store.path_for(digest))
        .value_or(std::vector<std::byte>{});
}

/// Flip the lowest bit of one byte of a stored blob (simulated on-disk damage).
[[nodiscard]] auto flip_blob_byte(const content_store &store,
                                  const digest32 &digest, std::size_t offset)
    -> bool {
    auto bytes{refrain::core::io::read_file(store.path_for(digest))};
    if (!bytes || offset >= bytes->size()) {
        return false;
    }
    (*bytes)[offset] = static_cast<std::byte>(
        std::to_integer<unsigned int>((*bytes)[offset]) ^ 0x01U);
    return refrain::core::io::write_file(store.path_for(digest), *bytes)
        .has_value();
}

/// RFBT record for a blob already stored as its own file (large/packed path).
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
        if (layout->all_raw_blocks) {
            flags |= blob_flags_all_raw_blocks;
        }
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

/// Flip one byte of an arbitrary file (simulated segment damage).
[[nodiscard]] auto flip_file_byte(std::string_view path, std::size_t offset)
    -> bool {
    auto bytes{refrain::core::io::read_file(path)};
    if (!bytes || offset >= bytes->size()) {
        return false;
    }
    (*bytes)[offset] = static_cast<std::byte>(
        std::to_integer<unsigned int>((*bytes)[offset]) ^ 0x01U);
    return refrain::core::io::write_file(path, *bytes).has_value();
}

/// RFBT record a batch publisher would build from one `blob_metadata`. The
/// block count is not part of the metadata (packed layout is deterministic):
/// `ceil(logical_size / 65536)` for packed blobs, 0 for raw ones.
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

/// Parse a blob table built from `records`, failing fast on invalid input.
[[nodiscard]] auto view_of(std::vector<blob_table_entry> records)
    -> std::optional<blob_table_view> {
    auto bytes{build_blob_table(records)};
    if (!bytes) {
        return std::nullopt;
    }
    auto parsed{parse_blob_table(*bytes)};
    if (!parsed) {
        return std::nullopt;
    }
    return std::optional<blob_table_view>{std::move(*parsed)};
}

/// Sealed `.rfseg` names under `<root>/content/segments` (sorted).
[[nodiscard]] auto segment_files(std::string_view root)
    -> std::vector<std::string> {
    const auto directory{std::string{root} + "/" +
                         std::string{content_segment_directory}};
    auto names{refrain::core::io::list_directory(directory)};
    if (!names) {
        return {};
    }
    std::vector<std::string> sealed;
    for (auto &name : *names) {
        if (name.ends_with(".rfseg")) {
            sealed.push_back(std::move(name));
        }
    }
    return sealed;
}

/// Every entry name under `<root>/content/segments` (parts included).
[[nodiscard]] auto segment_directory_entries(std::string_view root)
    -> std::vector<std::string> {
    const auto directory{std::string{root} + "/" +
                         std::string{content_segment_directory}};
    auto names{refrain::core::io::list_directory(directory)};
    if (!names) {
        return {};
    }
    return *names;
}

[[nodiscard]] auto location_for(const blob_table_view &table,
                                const digest32 &digest) -> blob_location {
    for (const auto &entry : table.entries) {
        if (entry.digest == digest) {
            return blob_location_from_table_entry(entry);
        }
    }
    return {};
}

const suite content_store_tests = [] {
    "cas/open creates the layout and rejects empty roots"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{store_root(scratch)};

        expect(!content_store::open(""));
        if (!content_store::open("")) {
            expect(content_store::open("").error().code ==
                   refrain::error_code::invalid_argument);
        }

        auto store{content_store::open(root)};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        expect(store->valid());
        expect(store->root() == root);
        expect(store->blobs_root() == expected_blobs_root(root));
        expect(refrain::core::io::is_directory(store->blobs_root()));
        // Opening an existing store is idempotent.
        expect(static_cast<bool>(content_store::open(root)));
    };

    // Read-only diagnostics (`refrain-tools content where`) must not create
    // `<root>/content/blobs` as a side effect of looking at the store.
    "cas/open_existing never creates store directories"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{store_root(scratch)};
        const auto blobs{expected_blobs_root(root)};

        // Missing root: io_error and nothing is materialized.
        const auto missing{content_store::open_existing(root)};
        expect(!missing);
        if (!missing) {
            expect(missing.error().code == refrain::error_code::io_error);
        }
        expect(!refrain::core::io::exists(root));
        expect(!refrain::core::io::exists(blobs));

        // An existing root without content/blobs opens read-only successfully
        // and still does not create the blob directory.
        expect(refrain::core::io::create_directories(root).has_value());
        auto read_only{content_store::open_existing(root)};
        expect(static_cast<bool>(read_only));
        if (read_only) {
            expect(read_only->valid());
            expect(read_only->root() == root);
            expect(read_only->blobs_root() == blobs);
            expect(static_cast<bool>(content_store::open_existing(root)));
        }
        expect(!refrain::core::io::exists(blobs));

        // Control: the write-side open() creates the layout, and that store can
        // be re-opened read-only afterwards.
        expect(static_cast<bool>(content_store::open(root)));
        expect(refrain::core::io::is_directory(blobs));
        expect(static_cast<bool>(content_store::open_existing(root)));

        // A regular file where the store root should be is not a store.
        const auto not_a_directory{(scratch.path / "not-a-dir").string()};
        expect(refrain::core::io::write_text(not_a_directory, "x").has_value());
        const auto file_root{content_store::open_existing(not_a_directory)};
        expect(!file_root);
        if (!file_root) {
            expect(file_root.error().code == refrain::error_code::io_error);
        }

        // Empty roots keep the open() contract.
        expect(!content_store::open_existing(""));
        if (!content_store::open_existing("")) {
            expect(content_store::open_existing("").error().code ==
                   refrain::error_code::invalid_argument);
        }
    };

    "cas/put stores the blob at its BLAKE3 path"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{store_root(scratch)};
        auto store{content_store::open(root)};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        const auto content{bytes_of("content-addressed payload\n")};

        const auto digest{store->put(content)};
        expect(static_cast<bool>(digest));
        if (!digest) {
            return;
        }
        expect(*digest == blake3(content));

        const auto hex{to_hex(*digest)};
        expect(hex.size() == 64U);
        expect(std::all_of(hex.begin(), hex.end(), [](char character) {
            return (character >= '0' && character <= '9') ||
                   (character >= 'a' && character <= 'f');
        }));
        const auto path{store->path_for(*digest)};
        expect(path == expected_path(root, *digest));
        expect(refrain::core::io::is_regular_file(path));
        expect(store->contains(*digest));

        const auto stored{refrain::core::io::read_file(path)};
        expect(static_cast<bool>(stored));
        if (stored) {
            expect(*stored == content);
        }
        const auto read_back{store->read(*digest)};
        expect(static_cast<bool>(read_back));
        if (read_back) {
            expect(*read_back == content);
        }
    };

    "cas/put deduplicates identical content"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{store_root(scratch)};
        auto store{content_store::open(root)};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        const auto content{binary_blob(4096)};
        const auto first{store->put(content)};
        const auto second{store->put(content)};
        expect(static_cast<bool>(first));
        expect(static_cast<bool>(second));
        if (!first || !second) {
            return;
        }
        expect(*first == *second);
        expect(*first == blake3(content));

        const auto prefix_directory{store->blobs_root() + "/" +
                                    to_hex(*first).substr(0, 2)};
        const auto names{refrain::core::io::list_directory(prefix_directory)};
        expect(static_cast<bool>(names));
        if (names) {
            expect(names->size() == 1U);
            expect((*names)[0] == to_hex(*first));
        }
        expect(store->read(*first).value_or(std::vector<std::byte>{}) ==
               content);
    };

    "cas/empty and binary content roundtrip"_test = [] {
        temp_directory scratch{make_temp_directory()};
        auto store{content_store::open(store_root(scratch))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        // Empty blob: valid content, zero-byte file.
        const auto empty_digest{store->put({})};
        expect(static_cast<bool>(empty_digest));
        if (empty_digest) {
            expect(*empty_digest == blake3({}));
            const auto read_back{store->read(*empty_digest)};
            expect(static_cast<bool>(read_back));
            if (read_back) {
                expect(read_back->empty());
            }
            expect(refrain::core::io::file_size(store->path_for(*empty_digest))
                       .value_or(1U) == 0U);
        }
        // Every byte value, including NUL, survives the roundtrip.
        const auto binary{binary_blob(1024)};
        const auto binary_digest{store->put(binary)};
        expect(static_cast<bool>(binary_digest));
        if (binary_digest) {
            expect(store->read(*binary_digest)
                       .value_or(std::vector<std::byte>{}) == binary);
        }
        // UTF-8 content is just bytes to the store.
        const auto unicode{
            bytes_of("content/\u00e4\u00f6\u00fc/\u65e5\u672c\u8a9e")};
        const auto unicode_digest{store->put(unicode)};
        expect(static_cast<bool>(unicode_digest));
        if (unicode_digest) {
            expect(store->read(*unicode_digest)
                       .value_or(std::vector<std::byte>{}) == unicode);
        }
    };

    "cas/put_file matches put and stores the same blob"_test = [] {
        temp_directory scratch{make_temp_directory()};
        auto store{content_store::open(store_root(scratch))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        const auto content{binary_blob(2048)};
        const auto source_path{scratch.path / "source.bin"};
        expect(refrain::core::io::write_file(source_path.string(), content)
                   .has_value());

        const auto from_file{store->put_file(source_path.string())};
        const auto from_memory{store->put(content)};
        expect(static_cast<bool>(from_file));
        expect(static_cast<bool>(from_memory));
        if (from_file && from_memory) {
            expect(*from_file == *from_memory);
            expect(*from_file == blake3(content));
            expect(store->read(*from_file).value_or(std::vector<std::byte>{}) ==
                   content);
        }
        // Missing source files surface the IO error instead of creating a blob.
        const auto missing{
            store->put_file((scratch.path / "nope.bin").string())};
        expect(!missing);
        if (!missing) {
            expect(missing.error().code == refrain::error_code::io_error);
        }
    };

    "cas/missing blobs are reported"_test = [] {
        temp_directory scratch{make_temp_directory()};
        auto store{content_store::open(store_root(scratch))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        const auto absent{blake3(bytes_of("never stored"))};
        expect(!store->contains(absent));
        expect(!store->read(absent));
        if (!store->read(absent)) {
            // There is no digest-only way to tell "never stored" apart from
            // "lives inside a content segment", so both report not_found and
            // the segment hint.
            expect(store->read(absent).error().code ==
                   refrain::error_code::not_found);
            expect(store->read(absent).error().message.find("blob_location") !=
                   std::string::npos);
        }
    };

    "cas/corrupt blobs are rejected on read and on dedupe"_test = [] {
        temp_directory scratch{make_temp_directory()};
        auto store{content_store::open(store_root(scratch))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        const auto content{binary_blob(512)};
        const auto digest{store->put(content)};
        expect(static_cast<bool>(digest));
        if (!digest) {
            return;
        }
        // Simulate on-disk corruption of a blob that keeps its name.
        const auto path{store->path_for(*digest)};
        expect(refrain::core::io::write_file(path, bytes_of("corrupted!"))
                   .has_value());
        const auto read_back{store->read(*digest)};
        expect(!read_back);
        if (!read_back) {
            expect(read_back.error().code == refrain::error_code::parse_error);
            expect(read_back.error().message.find("BLAKE3") !=
                   std::string::npos);
        }
        // Existence is still true (repair/GC is a separate concern) ...
        expect(store->contains(*digest));
        // ... but put() refuses to adopt the corrupt existing blob silently.
        const auto put_again{store->put(content)};
        expect(!put_again);
        if (!put_again) {
            expect(put_again.error().code == refrain::error_code::parse_error);
            expect(put_again.error().message.find("content hash") !=
                   std::string::npos);
        }
    };

    "cas/publish failure leaves no partial blob or temp file"_test = [] {
        temp_directory scratch{make_temp_directory()};
        auto store{content_store::open(store_root(scratch))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        const auto content{bytes_of("will not be published")};
        const auto digest{blake3(content)};
        // Block the `<hash[0:2]>` directory with a regular file so the CAS
        // cannot create the parent of the blob.
        const auto blobs_root{store->blobs_root()};
        const auto blocking_path{blobs_root + "/" +
                                 to_hex(digest).substr(0, 2)};
        expect(refrain::core::io::write_file(blocking_path, bytes_of("block"))
                   .has_value());

        const auto published{store->put(content)};
        expect(!published);
        if (!published) {
            expect(published.error().code == refrain::error_code::io_error);
        }
        expect(!refrain::core::io::exists(store->path_for(digest)));
        // No temporary siblings left behind in the blobs root.
        const auto names{refrain::core::io::list_directory(blobs_root)};
        expect(static_cast<bool>(names));
        if (names) {
            expect(names->size() == 1U);
            expect((*names)[0] == to_hex(digest).substr(0, 2));
        }
    };

    "cas/concurrent puts of the same content agree"_test = [] {
        temp_directory scratch{make_temp_directory()};
        auto store{content_store::open(store_root(scratch))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        const auto content{binary_blob(16384)};
        std::array<refrain::result<digest32>, 2> results{};
        auto first{
            [&store, &content, &results] { results[0] = store->put(content); }};
        auto second{
            [&store, &content, &results] { results[1] = store->put(content); }};
        std::thread thread_a{first};
        std::thread thread_b{second};
        thread_a.join();
        thread_b.join();

        expect(static_cast<bool>(results[0]));
        expect(static_cast<bool>(results[1]));
        if (results[0] && results[1]) {
            // Compare hex strings: boost.ut's range-expression overload would
            // otherwise pick up std::array<std::byte> operator== and try to
            // stream raw bytes on failure.
            expect(to_hex(*results[0]) == to_hex(*results[1]));
            expect(to_hex(*results[0]) == to_hex(blake3(content)));
        }
        expect(
            store->read(blake3(content)).value_or(std::vector<std::byte>{}) ==
            content);
        // Deferred GC: exactly one blob exists for the content.
        const auto blobs_root{store->blobs_root()};
        const auto names{refrain::core::io::list_directory(blobs_root)};
        expect(static_cast<bool>(names));
        if (names) {
            expect(names->size() == 1U);
        }
    };

    "cas/distinct content produces distinct blobs"_test = [] {
        temp_directory scratch{make_temp_directory()};
        auto store{content_store::open(store_root(scratch))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        const auto first{store->put(bytes_of("one"))};
        const auto second{store->put(bytes_of("two"))};
        expect(static_cast<bool>(first));
        expect(static_cast<bool>(second));
        if (first && second) {
            expect(*first != *second);
            expect(store->contains(*first));
            expect(store->contains(*second));
            expect(text_of(store->read(*first).value_or(
                       std::vector<std::byte>{})) == "one");
            expect(text_of(store->read(*second).value_or(
                       std::vector<std::byte>{})) == "two");
        }
    };

    "cas/trailing separators in the root are normalised"_test = [] {
        temp_directory scratch{make_temp_directory()};
        auto store{content_store::open(store_root(scratch) + "///")};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        expect(store->root() == store_root(scratch));
        const auto content{bytes_of("normalised root")};
        const auto digest{store->put(content)};
        expect(static_cast<bool>(digest));
        if (digest) {
            expect(store->path_for(*digest) ==
                   expected_path(store_root(scratch), *digest));
        }
    };

    "cas/two handles over the same root share blobs"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{store_root(scratch)};
        auto writer{content_store::open(root)};
        auto reader{content_store::open(root)};
        expect(static_cast<bool>(writer));
        expect(static_cast<bool>(reader));
        if (!writer || !reader) {
            return;
        }
        const auto content{binary_blob(777)};
        const auto digest{writer->put(content)};
        expect(static_cast<bool>(digest));
        if (!digest) {
            return;
        }
        // A second handle (e.g. another process/tool) sees the published blob
        // immediately and verifies it on read.
        expect(reader->contains(*digest));
        expect(reader->read(*digest).value_or(std::vector<std::byte>{}) ==
               content);
        expect(reader->path_for(*digest) == writer->path_for(*digest));
    };

    "cas/put_file of an empty file stores the empty blob"_test = [] {
        temp_directory scratch{make_temp_directory()};
        auto store{content_store::open(store_root(scratch))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        const auto empty_path{scratch.path / "empty.bin"};
        expect(
            refrain::core::io::write_file(empty_path.string(), {}).has_value());
        const auto from_file{store->put_file(empty_path.string())};
        const auto from_memory{store->put({})};
        expect(static_cast<bool>(from_file));
        expect(static_cast<bool>(from_memory));
        if (from_file && from_memory) {
            expect(*from_file == *from_memory);
            expect(to_hex(*from_file) == to_hex(blake3({})));
        }
    };

    "cas/default store handle is inert"_test = [] {
        const content_store store;
        expect(!store.valid());
        expect(!store.contains(blake3({})));
        const auto put_result{store.put(bytes_of("x"))};
        expect(!put_result);
        if (!put_result) {
            expect(put_result.error().code ==
                   refrain::error_code::not_initialized);
        }
        const auto read_result{store.read(blake3(bytes_of("x")))};
        expect(!read_result);
        if (!read_result) {
            expect(read_result.error().code ==
                   refrain::error_code::not_initialized);
        }
        const auto put_file_result{store.put_file("x")};
        expect(!put_file_result);
        if (!put_file_result) {
            expect(put_file_result.error().code ==
                   refrain::error_code::not_initialized);
        }
    };

    "cas/raw blobs preserve the existing CAS behaviour"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{store_root(scratch)};
        auto store{content_store::open(root)};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        const auto content{bytes_of("raw content")};
        const auto digest{store->put(content)};
        expect(static_cast<bool>(digest));
        if (!digest) {
            return;
        }
        expect(store->contains(*digest));
        expect(store->path_for(*digest) == expected_path(root, *digest));
        const auto layout{store->layout(*digest)};
        expect(static_cast<bool>(layout));
        if (layout) {
            expect(layout->kind == refrain::data::blob_kind::raw);
            expect(layout->block_size == 0U);
            expect(layout->block_count == 0U);
            expect(layout->logical_size == content.size());
        }
        const auto stored{
            refrain::core::io::read_file(store->path_for(*digest))};
        expect(static_cast<bool>(stored));
        if (stored) {
            expect(*stored == content);
        }
        expect(store->read(*digest).value_or(std::vector<std::byte>{}) ==
               content);

        // A second put of the same content keeps exactly one file.
        const auto again{store->put(content)};
        expect(static_cast<bool>(again));
        if (again) {
            expect(to_hex(*again) == to_hex(*digest));
        }
        const auto prefix_directory{store->blobs_root() + "/" +
                                    to_hex(*digest).substr(0, 2)};
        const auto names{refrain::core::io::list_directory(prefix_directory)};
        expect(static_cast<bool>(names));
        if (names) {
            expect(names->size() == 1U);
        }
    };

    "cas/content of exactly 64 KiB stays raw"_test = [] {
        temp_directory scratch{make_temp_directory()};
        auto store{content_store::open(store_root(scratch))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        // The raw limit is inclusive: 65536 bytes is still raw.
        const auto content{repeated_blob(65536, std::byte{0x5A})};
        const auto digest{store->put(content)};
        expect(static_cast<bool>(digest));
        if (!digest) {
            return;
        }
        const auto file{read_blob_file(*store, *digest)};
        expect(file.size() == content.size());
        expect(file == content);
        expect(!magic_is_rfbl(file));
        const auto layout{store->layout(*digest)};
        expect(static_cast<bool>(layout));
        if (layout) {
            expect(layout->kind == refrain::data::blob_kind::raw);
            expect(layout->block_size == 0U);
            expect(layout->block_count == 0U);
            expect(layout->logical_size == 65536U);
        }
        expect(store->read(*digest).value_or(std::vector<std::byte>{}) ==
               content);
    };

    "cas/content above 64 KiB is stored packed"_test = [] {
        temp_directory scratch{make_temp_directory()};
        auto store{content_store::open(store_root(scratch))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        const auto content{repeated_blob(65537, std::byte{0x41})};
        const auto digest{store->put(content)};
        expect(static_cast<bool>(digest));
        if (!digest) {
            return;
        }
        const auto file{read_blob_file(*store, *digest)};
        expect(magic_is_rfbl(file));
        expect(read_u16_le(file, 4U) == 1U);
        // bit0 = packed; bit1 = all blocks raw, but block 0 compresses.
        expect(read_u16_le(file, 6U) == 1U);
        expect(read_u32_le(file, 8U) == 65536U);
        expect(read_u32_le(file, 12U) == 2U);
        expect(read_u64_le(file, 16U) == content.size());
        digest32 header_digest{};
        std::copy_n(file.begin() + 24, header_digest.size(),
                    header_digest.begin());
        expect(header_digest == *digest);
        expect(read_u32_le(file, 56U) == 96U);
        expect(read_u32_le(file, 60U) ==
               crc32c(std::span<const std::byte>{file}.first(60)));
        expect(table_digest_matches(file, 96U));
        expect(reserved_bytes_zero(file));
        // 96-byte header + two 48-byte entries = 192, already 64-aligned. The
        // second block starts at the next 64-byte boundary after block 0.
        expect(read_u64_le(file, 96U) == 192U);
        const auto first_stored_size{read_u32_le(file, 96U + 8U)};
        expect(first_stored_size < 65536U);
        expect(read_u32_le(file, 96U + 12U) == 65536U);
        expect(read_u64_le(file, 96U + 48U) ==
               align_up_64(192U + first_stored_size));
        expect(read_u32_le(file, 96U + 48U + 8U) == 1U);
        expect(read_u32_le(file, 96U + 48U + 12U) == 1U);
        const auto first_hash_ok{block_hash_matches(
            file, 96U, std::span<const std::byte>{content}.first(65536U))};
        expect(first_hash_ok);
        const auto last_hash_ok{block_hash_matches(
            file, 96U + 48U,
            std::span<const std::byte>{content}.subspan(65536U, 1U))};
        expect(last_hash_ok);
        expect(file.size() < content.size());

        const auto layout{store->layout(*digest)};
        expect(static_cast<bool>(layout));
        if (layout) {
            expect(layout->kind == refrain::data::blob_kind::packed);
            expect(layout->block_size == 65536U);
            expect(layout->block_count == 2U);
            expect(layout->logical_size == 65537U);
        }
        expect(store->read(*digest).value_or(std::vector<std::byte>{}) ==
               content);
    };

    "cas/one MiB of compressible data roundtrips packed"_test = [] {
        temp_directory scratch{make_temp_directory()};
        auto store{content_store::open(store_root(scratch))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        const auto content{repeated_blob(1024U * 1024U, std::byte{0x5A})};
        const auto digest{store->put(content)};
        expect(static_cast<bool>(digest));
        if (!digest) {
            return;
        }
        const auto file{read_blob_file(*store, *digest)};
        expect(magic_is_rfbl(file));
        expect(read_u16_le(file, 6U) == 1U);
        expect(read_u32_le(file, 8U) == 65536U);
        expect(read_u32_le(file, 12U) == 16U);
        expect(read_u64_le(file, 16U) == content.size());
        expect(read_u32_le(file, 56U) == 16U * 48U);
        expect(file.size() < content.size());
        for (std::size_t index{0}; index < 16U; ++index) {
            const auto entry{96U + index * 48U};
            expect(read_u64_le(file, entry) % 64U == 0U);
            expect(read_u32_le(file, entry + 8U) <
                   read_u32_le(file, entry + 12U));
            const auto logical_block{
                std::span<const std::byte>{content}.subspan(index * 65536U,
                                                            65536U)};
            const auto hash_ok{block_hash_matches(file, entry, logical_block)};
            expect(hash_ok);
        }
        for (std::size_t index{1}; index < 16U; ++index) {
            const auto previous_entry{96U + (index - 1U) * 48U};
            const auto previous_end{read_u64_le(file, previous_entry) +
                                    read_u32_le(file, previous_entry + 8U)};
            expect(read_u64_le(file, 96U + index * 48U) >= previous_end);
        }
        const auto layout{store->layout(*digest)};
        expect(static_cast<bool>(layout));
        if (layout) {
            expect(layout->kind == refrain::data::blob_kind::packed);
            expect(layout->block_size == 65536U);
            expect(layout->block_count == 16U);
            expect(layout->logical_size == 1024U * 1024U);
        }
        expect(store->read(*digest).value_or(std::vector<std::byte>{}) ==
               content);
    };

    "cas/one MiB of incompressible data roundtrips packed"_test = [] {
        temp_directory scratch{make_temp_directory()};
        auto store{content_store::open(store_root(scratch))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        const auto content{pseudo_random_blob(1024U * 1024U, 42U)};
        const auto digest{store->put(content)};
        expect(static_cast<bool>(digest));
        if (!digest) {
            return;
        }
        const auto file{read_blob_file(*store, *digest)};
        expect(magic_is_rfbl(file));
        // bit0 packed + bit1 all blocks raw: zstd could not beat the input.
        expect(read_u16_le(file, 6U) == 3U);
        expect(read_u32_le(file, 12U) == 16U);
        // 96-byte header + 768-byte table, 64-byte aligned, then 16 raw blocks.
        constexpr std::size_t data_start{896U};
        expect(file.size() == data_start + content.size());
        for (std::size_t index{0}; index < 16U; ++index) {
            const auto entry{96U + index * 48U};
            expect(read_u64_le(file, entry) == data_start + index * 65536U);
            expect(read_u32_le(file, entry + 8U) == 65536U);
            expect(read_u32_le(file, entry + 12U) == 65536U);
        }
        const auto layout{store->layout(*digest)};
        expect(static_cast<bool>(layout));
        if (layout) {
            expect(layout->kind == refrain::data::blob_kind::packed);
            expect(layout->block_size == 65536U);
            expect(layout->block_count == 16U);
            expect(layout->logical_size == 1024U * 1024U);
        }
        expect(store->read(*digest).value_or(std::vector<std::byte>{}) ==
               content);
    };

    "cas/packed blobs are deterministic and deduplicate"_test = [] {
        temp_directory first_scratch{make_temp_directory()};
        temp_directory second_scratch{make_temp_directory()};
        auto first_store{content_store::open(store_root(first_scratch))};
        auto second_store{content_store::open(store_root(second_scratch))};
        expect(static_cast<bool>(first_store));
        expect(static_cast<bool>(second_store));
        if (!first_store || !second_store) {
            return;
        }
        const auto content{pseudo_random_blob(200000U, 7U)};
        const auto first{first_store->put(content)};
        const auto second{second_store->put(content)};
        expect(static_cast<bool>(first));
        expect(static_cast<bool>(second));
        if (!first || !second) {
            return;
        }
        expect(to_hex(*first) == to_hex(*second));
        const auto first_file{read_blob_file(*first_store, *first)};
        const auto second_file{read_blob_file(*second_store, *second)};
        expect(first_file.size() == second_file.size());
        expect(first_file == second_file);
        expect(magic_is_rfbl(first_file));
        expect(first_file.size() > content.size());

        // A second put of the same content reuses the verified blob bytes.
        const auto again{first_store->put(content)};
        expect(static_cast<bool>(again));
        if (again) {
            expect(to_hex(*again) == to_hex(*first));
        }
        const auto file_again{read_blob_file(*first_store, *first)};
        expect(file_again == first_file);
        const auto prefix_directory{first_store->blobs_root() + "/" +
                                    to_hex(*first).substr(0, 2)};
        const auto names{refrain::core::io::list_directory(prefix_directory)};
        expect(static_cast<bool>(names));
        if (names) {
            expect(names->size() == 1U);
        }
    };

    "cas/read_range slices raw and packed blobs"_test = [] {
        temp_directory scratch{make_temp_directory()};
        auto store{content_store::open(store_root(scratch))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        // Large enough that the 65530..65550 range crosses a block border.
        const auto packed{binary_blob(200000)};
        const auto packed_digest{store->put(packed)};
        expect(static_cast<bool>(packed_digest));
        if (!packed_digest) {
            return;
        }
        // 65530 is inside block 0; 20 bytes cross into block 1.
        std::vector<std::byte> crossing(20);
        const auto crossed{store->read_range(*packed_digest, 65530U, crossing)};
        expect(static_cast<bool>(crossed));
        if (crossed) {
            expect(*crossed == crossing.size());
        }
        expect(crossing == slice_of(packed, 65530U, 20U));

        std::vector<std::byte> first(10);
        const auto first_read{store->read_range(*packed_digest, 0U, first)};
        expect(static_cast<bool>(first_read));
        expect(first == slice_of(packed, 0U, 10U));

        std::vector<std::byte> last(10);
        const auto last_read{
            store->read_range(*packed_digest, packed.size() - 10U, last)};
        expect(static_cast<bool>(last_read));
        expect(last == slice_of(packed, packed.size() - 10U, 10U));

        std::vector<std::byte> whole(packed.size());
        const auto whole_read{store->read_range(*packed_digest, 0U, whole)};
        expect(static_cast<bool>(whole_read));
        if (whole_read) {
            expect(*whole_read == packed.size());
        }
        expect(whole == packed);

        std::vector<std::byte> empty;
        const auto at_end{
            store->read_range(*packed_digest, packed.size(), empty)};
        expect(static_cast<bool>(at_end));
        if (at_end) {
            expect(*at_end == 0U);
        }

        // Raw blobs are read whole and sliced identically.
        const auto raw{bytes_of("0123456789")};
        const auto raw_digest{store->put(raw)};
        expect(static_cast<bool>(raw_digest));
        if (!raw_digest) {
            return;
        }
        std::vector<std::byte> middle(3);
        const auto middle_read{store->read_range(*raw_digest, 2U, middle)};
        expect(static_cast<bool>(middle_read));
        expect(middle == slice_of(raw, 2U, 3U));
    };

    "cas/read_range fails closed when a cached packed blob is truncated"_test =
        [] {
            temp_directory scratch{make_temp_directory()};
            auto store{content_store::open(store_root(scratch))};
            expect(static_cast<bool>(store));
            if (!store) {
                return;
            }
            const auto packed{binary_blob(200000)};
            const auto digest{store->put(packed)};
            expect(static_cast<bool>(digest));
            if (!digest) {
                return;
            }
            std::vector<std::byte> first(10);
            expect(static_cast<bool>(store->read_range(*digest, 0U, first)));
            // The parsed block table is now cached. Shrinking the file must be
            // reported as corruption, not read through a stale offset.
            std::error_code error;
            fs::resize_file(fs::path{store->path_for(*digest)}, 128U, error);
            expect(!error);
            std::vector<std::byte> again(10);
            const auto truncated{store->read_range(*digest, 0U, again)};
            expect(!truncated);
            if (!truncated) {
                expect(truncated.error().code ==
                       refrain::error_code::parse_error);
            }
        };

    "cas/read_range rejects out-of-range requests"_test = [] {
        temp_directory scratch{make_temp_directory()};
        auto store{content_store::open(store_root(scratch))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        const auto packed{binary_blob(200000)};
        const auto digest{store->put(packed)};
        expect(static_cast<bool>(digest));
        if (!digest) {
            return;
        }
        std::vector<std::byte> one(1);
        const auto past_end{store->read_range(*digest, 200001U, one)};
        expect(!past_end);
        if (!past_end) {
            expect(past_end.error().code ==
                   refrain::error_code::invalid_argument);
        }
        const auto at_end{store->read_range(*digest, 200000U, one)};
        expect(!at_end);
        if (!at_end) {
            expect(at_end.error().code ==
                   refrain::error_code::invalid_argument);
        }
        std::vector<std::byte> too_long(200001);
        const auto oversized{store->read_range(*digest, 0U, too_long)};
        expect(!oversized);
        if (!oversized) {
            expect(oversized.error().code ==
                   refrain::error_code::invalid_argument);
        }
        // A raw blob follows the same contract.
        const auto raw{bytes_of("0123456789")};
        const auto raw_digest{store->put(raw)};
        expect(static_cast<bool>(raw_digest));
        if (!raw_digest) {
            return;
        }
        const auto raw_past_end{store->read_range(*raw_digest, 11U, one)};
        expect(!raw_past_end);
        if (!raw_past_end) {
            expect(raw_past_end.error().code ==
                   refrain::error_code::invalid_argument);
        }
    };

    "cas/corrupt packed headers are rejected"_test = [] {
        temp_directory scratch{make_temp_directory()};
        auto store{content_store::open(store_root(scratch))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        const auto content{repeated_blob(1024U * 1024U, std::byte{0x5A})};
        const auto digest{store->put(content)};
        expect(static_cast<bool>(digest));
        if (!digest) {
            return;
        }
        const auto path{store->path_for(*digest)};
        // Flip the low byte of block_size: either way the header CRC fails.
        const auto flipped{flip_blob_byte(*store, *digest, 8U)};
        expect(flipped);
        const auto corrupted{refrain::core::io::read_file(path)};
        expect(static_cast<bool>(corrupted));
        const auto read_back{store->read(*digest)};
        expect(!read_back);
        if (!read_back) {
            expect(read_back.error().code == refrain::error_code::parse_error);
        }
        const auto layout{store->layout(*digest)};
        expect(!layout);
        if (!layout) {
            expect(layout.error().code == refrain::error_code::parse_error);
        }
        const auto put_again{store->put(content)};
        expect(!put_again);
        if (!put_again) {
            expect(put_again.error().code == refrain::error_code::parse_error);
            expect(put_again.error().message.find("content hash") !=
                   std::string::npos);
        }
        // put() must not overwrite a corrupt existing blob.
        const auto after{refrain::core::io::read_file(path)};
        expect(static_cast<bool>(after));
        if (after && corrupted) {
            expect(*after == *corrupted);
        }
    };

    "cas/corrupt packed block tables are rejected"_test = [] {
        temp_directory scratch{make_temp_directory()};
        auto store{content_store::open(store_root(scratch))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        const auto content{repeated_blob(1024U * 1024U, std::byte{0x5A})};
        const auto digest{store->put(content)};
        expect(static_cast<bool>(digest));
        if (!digest) {
            return;
        }
        // Byte 96 is the first byte of the first stored_offset; the header is
        // untouched, so only the table BLAKE3 can catch this.
        const auto flipped{flip_blob_byte(*store, *digest, 96U)};
        expect(flipped);
        const auto read_back{store->read(*digest)};
        expect(!read_back);
        if (!read_back) {
            expect(read_back.error().code == refrain::error_code::parse_error);
        }
        const auto layout{store->layout(*digest)};
        expect(!layout);
        if (!layout) {
            expect(layout.error().code == refrain::error_code::parse_error);
        }
        const auto put_again{store->put(content)};
        expect(!put_again);
        if (!put_again) {
            expect(put_again.error().code == refrain::error_code::parse_error);
        }
    };

    "cas/corrupt packed blocks are rejected"_test = [] {
        temp_directory scratch{make_temp_directory()};
        auto store{content_store::open(store_root(scratch))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        // Random bytes are stored verbatim, so the flip cannot be masked by a
        // zstd decoding failure: the block hash/logical digest must catch it.
        const auto content{pseudo_random_blob(1024U * 1024U, 11U)};
        const auto digest{store->put(content)};
        expect(static_cast<bool>(digest));
        if (!digest) {
            return;
        }
        const auto path{store->path_for(*digest)};
        const auto before{refrain::core::io::read_file(path)};
        expect(static_cast<bool>(before));
        if (!before) {
            return;
        }
        const auto first_block_offset{read_u64_le(*before, 96U)};
        const auto flipped{flip_blob_byte(
            *store, *digest,
            static_cast<std::size_t>(first_block_offset) + 100U)};
        expect(flipped);
        const auto read_back{store->read(*digest)};
        expect(!read_back);
        if (!read_back) {
            expect(read_back.error().code == refrain::error_code::parse_error);
        }
        const auto layout{store->layout(*digest)};
        expect(!layout);
        if (!layout) {
            expect(layout.error().code == refrain::error_code::parse_error);
        }
        const auto put_again{store->put(content)};
        expect(!put_again);
        if (!put_again) {
            expect(put_again.error().code == refrain::error_code::parse_error);
        }
    };

    "cas/truncated packed blobs are rejected"_test = [] {
        temp_directory scratch{make_temp_directory()};
        auto store{content_store::open(store_root(scratch))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        const auto content{repeated_blob(1024U * 1024U, std::byte{0x5A})};
        const auto digest{store->put(content)};
        expect(static_cast<bool>(digest));
        if (!digest) {
            return;
        }
        const auto path{store->path_for(*digest)};
        const auto file{read_blob_file(*store, *digest)};
        expect(file.size() > 1024U);
        const std::vector<std::byte> half(
            file.begin(),
            file.begin() + static_cast<std::ptrdiff_t>(file.size() / 2U));
        expect(refrain::core::io::write_file(path, half).has_value());
        const auto read_half{store->read(*digest)};
        expect(!read_half);
        if (!read_half) {
            expect(read_half.error().code == refrain::error_code::parse_error);
        }
        const auto layout_half{store->layout(*digest)};
        expect(!layout_half);
        if (!layout_half) {
            expect(layout_half.error().code ==
                   refrain::error_code::parse_error);
        }

        // Restore, then keep only the 96-byte header (table truncated).
        expect(refrain::core::io::write_file(path, file).has_value());
        const std::vector<std::byte> header_only(file.begin(),
                                                 file.begin() + 96);
        expect(refrain::core::io::write_file(path, header_only).has_value());
        const auto read_header_only{store->read(*digest)};
        expect(!read_header_only);
        if (!read_header_only) {
            expect(read_header_only.error().code ==
                   refrain::error_code::parse_error);
        }
        const auto layout_header_only{store->layout(*digest)};
        expect(!layout_header_only);
        if (!layout_header_only) {
            expect(layout_header_only.error().code ==
                   refrain::error_code::parse_error);
        }
    };

    "cas/existing raw blobs above the limit are reused"_test = [] {
        temp_directory scratch{make_temp_directory()};
        auto store{content_store::open(store_root(scratch))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        // Simulate a legacy store written before the packed layout existed: a
        // >64 KiB raw blob whose file digest matches its name.
        const auto content{repeated_blob(70000U, std::byte{0x33})};
        const auto digest{blake3(content)};
        const auto directory{store->blobs_root() + "/" +
                             to_hex(digest).substr(0, 2)};
        expect(refrain::core::io::create_directories(directory).has_value());
        expect(refrain::core::io::write_file(store->path_for(digest), content)
                   .has_value());
        expect(store->contains(digest));
        const auto layout{store->layout(digest)};
        expect(static_cast<bool>(layout));
        if (layout) {
            expect(layout->kind == refrain::data::blob_kind::raw);
            expect(layout->block_count == 0U);
            expect(layout->logical_size == content.size());
        }
        expect(store->read(digest).value_or(std::vector<std::byte>{}) ==
               content);

        const auto before{read_blob_file(*store, digest)};
        const auto reused{store->put(content)};
        expect(static_cast<bool>(reused));
        if (reused) {
            expect(to_hex(*reused) == to_hex(digest));
        }
        const auto after{read_blob_file(*store, digest)};
        expect(after == before);
        expect(!magic_is_rfbl(after));
    };

    "cas/raw payloads beginning with the RFBL magic roundtrip"_test = [] {
        temp_directory scratch{make_temp_directory()};
        auto store{content_store::open(store_root(scratch))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        // Small enough to be raw, but the magic-prefixed bytes must not be
        // mistaken for (corrupt) packed content; the raw digest disambiguates.
        auto content{bytes_of("RFBL")};
        const auto body{pseudo_random_blob(200U, 3U)};
        content.insert(content.end(), body.begin(), body.end());
        expect(content.size() <= 65536U);
        const auto digest{store->put(content)};
        expect(static_cast<bool>(digest));
        if (!digest) {
            return;
        }
        const auto file{read_blob_file(*store, *digest)};
        expect(file == content);
        const auto layout{store->layout(*digest)};
        expect(static_cast<bool>(layout));
        if (layout) {
            expect(layout->kind == refrain::data::blob_kind::raw);
            expect(layout->logical_size == content.size());
        }
        expect(store->read(*digest).value_or(std::vector<std::byte>{}) ==
               content);
        std::vector<std::byte> head(4);
        const auto ranged{store->read_range(*digest, 0U, head)};
        expect(static_cast<bool>(ranged));
        expect(head == bytes_of("RFBL"));
    };

    "cas/packed blobs with an invalid block count are rejected"_test = [] {
        temp_directory scratch{make_temp_directory()};
        auto store{content_store::open(store_root(scratch))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        const auto content{repeated_blob(1024U * 1024U, std::byte{0x5A})};
        const auto digest{store->put(content)};
        expect(static_cast<bool>(digest));
        if (!digest) {
            return;
        }
        const auto path{store->path_for(*digest)};
        auto file{read_blob_file(*store, *digest)};
        expect(magic_is_rfbl(file));
        // Keep the header CRC valid so the block-count validation (not the CRC
        // or table hash) is what rejects the blob.
        write_u32_le(file, 12U, 0xFFFFFFFFU);
        write_u32_le(file, 60U,
                     crc32c(std::span<const std::byte>{file}.first(60)));
        expect(refrain::core::io::write_file(path, file).has_value());
        const auto read_back{store->read(*digest)};
        expect(!read_back);
        if (!read_back) {
            expect(read_back.error().code == refrain::error_code::parse_error);
        }
        const auto layout{store->layout(*digest)};
        expect(!layout);
        if (!layout) {
            expect(layout.error().code == refrain::error_code::parse_error);
        }
    };

    "batch/small blobs share a segment and read through blob_location"_test =
        [] {
            temp_directory scratch{make_temp_directory()};
            const auto root{store_root(scratch)};
            auto store{content_store::open(root)};
            expect(static_cast<bool>(store));
            if (!store) {
                return;
            }
            const std::vector<std::vector<std::byte>> payloads{
                bytes_of("alpha"), bytes_of("bravo"), binary_blob(100)};

            auto staged{batch::begin(root, nullptr, 4096)};
            expect(static_cast<bool>(staged));
            if (!staged) {
                return;
            }
            for (const auto &payload : payloads) {
                auto digest{staged->put(payload)};
                expect(static_cast<bool>(digest));
                if (digest) {
                    expect(*digest == blake3(payload));
                }
            }
            expect(static_cast<bool>(staged->commit()));
            const auto blobs{staged->new_blobs()};
            expect(blobs.size() == payloads.size());
            if (blobs.size() != payloads.size()) {
                return;
            }

            // alpha(5) at 0, bravo(5) at 8, binary(100) at 16; body = 116.
            const std::array<std::uint32_t, 3> expected_offsets{0, 8, 16};
            for (std::size_t index{0}; index < blobs.size(); ++index) {
                expect(blobs[index].flags == blob_flags_in_segment);
                expect(blobs[index].segment_id != 0U);
                expect(blobs[index].segment_id == blobs[0].segment_id);
                expect(blobs[index].segment_offset == expected_offsets[index]);
                expect(blobs[index].logical_size == payloads[index].size());
                expect(blobs[index].stored_size == payloads[index].size());
            }
            const auto files{segment_files(root)};
            expect(files.size() == 1U);
            const auto sealed_path{segment_path(root, blobs[0].segment_id)};
            expect(refrain::core::io::is_regular_file(sealed_path));
            expect(refrain::core::io::file_size(sealed_path).value_or(0U) ==
                   80U + 116U);

            std::vector<blob_table_entry> records;
            for (const auto &meta : blobs) {
                records.push_back(entry_from_metadata(meta));
            }
            auto view{view_of(std::move(records))};
            expect(view.has_value());
            if (!view) {
                return;
            }
            for (std::size_t index{0}; index < blobs.size(); ++index) {
                const auto location{location_for(*view, blobs[index].digest)};
                expect(location.in_segment);
                expect(location.segment_id == blobs[index].segment_id);
                expect(location.segment_offset == blobs[index].segment_offset);
                auto read_back{store->read(location)};
                expect(static_cast<bool>(read_back));
                if (read_back) {
                    expect(same_bytes(*read_back, payloads[index]));
                }
                auto ranged{
                    store->read_range(location, 1U, std::span<std::byte>{})};
                expect(static_cast<bool>(ranged));
                if (ranged) {
                    expect(*ranged == 0U);
                }
            }
            // A raw in-segment slice is read straight from the body.
            const auto alpha_location{location_for(*view, blobs[0].digest)};
            std::vector<std::byte> middle(3);
            const auto sliced{store->read_range(alpha_location, 2U, middle)};
            expect(static_cast<bool>(sliced));
            expect(text_of(middle) == "pha");
            std::vector<std::byte> past_end(1);
            const auto invalid{store->read_range(alpha_location, 6U, past_end)};
            expect(!invalid);
            if (!invalid) {
                expect(invalid.error().code ==
                       refrain::error_code::invalid_argument);
            }
        };

    "batch/large blobs keep one packed file each"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{store_root(scratch)};
        auto store{content_store::open(root)};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        const auto payload{pseudo_random_blob(200000U, 7U)};
        auto staged{batch::begin(root, nullptr, 64U)};
        expect(static_cast<bool>(staged));
        if (!staged) {
            return;
        }
        auto digest{staged->put(payload)};
        expect(static_cast<bool>(digest));
        if (!digest) {
            return;
        }
        expect(static_cast<bool>(staged->commit()));
        const auto blobs{staged->new_blobs()};
        expect(blobs.size() == 1U);
        if (blobs.size() != 1U) {
            return;
        }
        expect((blobs[0].flags & blob_flags_packed) != 0U);
        expect((blobs[0].flags & blob_flags_in_segment) == 0U);
        expect(blobs[0].segment_id == 0U);
        expect(blobs[0].segment_offset == 0U);
        expect(blobs[0].logical_size == payload.size());
        const auto file_size{
            refrain::core::io::file_size(store->path_for(*digest))};
        expect(file_size.has_value());
        if (file_size) {
            expect(blobs[0].stored_size == *file_size);
        }
        expect(segment_files(root).empty());

        auto entry{entry_from_metadata(blobs[0])};
        auto view{view_of(std::vector<blob_table_entry>{entry})};
        expect(view.has_value());
        if (!view) {
            return;
        }
        const auto location{location_for(*view, *digest)};
        expect(!location.in_segment);
        expect(store->read(location).value_or(std::vector<std::byte>{}) ==
               payload);
        std::vector<std::byte> crossing(20);
        const auto ranged{store->read_range(location, 65530U, crossing)};
        expect(static_cast<bool>(ranged));
        expect(crossing == slice_of(payload, 65530U, 20U));
        // The legacy digest-only path still finds the file-backed blob.
        expect(store->read(*digest).value_or(std::vector<std::byte>{}) ==
               payload);
        expect(store->contains(*digest));
    };

    "batch/dedupes within the batch and against the existing table"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{store_root(scratch)};
        auto store{content_store::open(root)};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        const auto small{bytes_of("deduplicated small blob")};
        const auto large{pseudo_random_blob(120000U, 11U)};
        // A file-backed blob published by the legacy path; the second batch's
        // table must dedupe it just like an in-segment record.
        const auto legacy{pseudo_random_blob(90000U, 13U)};
        const auto legacy_digest{store->put(legacy)};
        expect(static_cast<bool>(legacy_digest));
        if (!legacy_digest) {
            return;
        }
        const auto legacy_entry{entry_from_store(*store, *legacy_digest)};
        expect(legacy_entry.has_value());
        if (!legacy_entry) {
            return;
        }

        auto first{batch::begin(root, nullptr, 4096U)};
        expect(static_cast<bool>(first));
        if (!first) {
            return;
        }
        const auto small_digest{first->put(small)};
        const auto small_again{first->put(small)};
        const auto large_digest{first->put(large)};
        expect(static_cast<bool>(small_digest));
        expect(static_cast<bool>(small_again));
        expect(static_cast<bool>(large_digest));
        if (!small_digest || !small_again || !large_digest) {
            return;
        }
        expect(to_hex(*small_digest) == to_hex(*small_again));
        expect(static_cast<bool>(first->commit()));
        expect(first->new_blobs().size() == 2U);
        const auto before{segment_files(root)};
        expect(before.size() == 1U);

        // A second batch with the previous table must not rewrite any blob.
        std::vector<blob_table_entry> records;
        for (const auto &meta : first->new_blobs()) {
            records.push_back(entry_from_metadata(meta));
        }
        records.push_back(*legacy_entry);
        auto view{view_of(std::move(records))};
        expect(view.has_value());
        if (!view) {
            return;
        }
        auto second{batch::begin(root, &*view, 4096U)};
        expect(static_cast<bool>(second));
        if (!second) {
            return;
        }
        const auto seen_small{second->put(small)};
        const auto seen_large{second->put(large)};
        const auto seen_legacy{second->put(legacy)};
        expect(static_cast<bool>(seen_small));
        expect(static_cast<bool>(seen_large));
        expect(static_cast<bool>(seen_legacy));
        if (seen_small && seen_large && seen_legacy) {
            expect(to_hex(*seen_small) == to_hex(*small_digest));
            expect(to_hex(*seen_large) == to_hex(*large_digest));
            expect(to_hex(*seen_legacy) == to_hex(*legacy_digest));
        }
        expect(static_cast<bool>(second->commit()));
        expect(second->new_blobs().empty());
        expect(segment_files(root).size() == 1U);
    };

    "batch/a small target publishes several segments"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{store_root(scratch)};
        auto store{content_store::open(root)};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        std::vector<std::vector<std::byte>> payloads;
        for (std::size_t index{0}; index < 3U; ++index) {
            payloads.push_back(pseudo_random_blob(32U, 100U + index));
        }
        auto staged{batch::begin(root, nullptr, 64U)};
        expect(static_cast<bool>(staged));
        if (!staged) {
            return;
        }
        std::vector<digest32> digests;
        for (const auto &payload : payloads) {
            auto digest{staged->put(payload)};
            expect(static_cast<bool>(digest));
            if (!digest) {
                return;
            }
            digests.push_back(*digest);
        }
        expect(static_cast<bool>(staged->commit()));
        const auto blobs{staged->new_blobs()};
        expect(blobs.size() == 3U);
        if (blobs.size() != 3U) {
            return;
        }
        // Two 32-byte blobs fit in a 64-byte segment; the third starts a new
        // one (its offset restarts at 0).
        expect(blobs[1].segment_id == blobs[0].segment_id);
        expect(blobs[2].segment_id != blobs[0].segment_id);
        expect(blobs[2].segment_offset == 0U);
        expect(segment_files(root).size() == 2U);

        std::vector<blob_table_entry> records;
        for (const auto &meta : blobs) {
            records.push_back(entry_from_metadata(meta));
        }
        auto view{view_of(std::move(records))};
        expect(view.has_value());
        if (!view) {
            return;
        }
        for (std::size_t index{0}; index < payloads.size(); ++index) {
            const auto location{location_for(*view, digests[index])};
            expect(store->read(location).value_or(std::vector<std::byte>{}) ==
                   payloads[index]);
        }
    };

    "batch/abort removes the staging part and publishes no segment"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{store_root(scratch)};
        {
            auto staged{batch::begin(root, nullptr, 4096U)};
            expect(static_cast<bool>(staged));
            if (!staged) {
                return;
            }
            expect(static_cast<bool>(staged->put(bytes_of("aborted"))));
            const auto staged_entries{segment_directory_entries(root)};
            expect(staged_entries.size() == 1U);
            if (staged_entries.size() == 1U) {
                expect(staged_entries[0].ends_with(".part"));
            }
        }
        expect(segment_directory_entries(root).empty());
        expect(segment_files(root).empty());
    };

    "batch/zero-byte blob lives in a segment at a valid offset"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{store_root(scratch)};
        auto store{content_store::open(root)};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        auto staged{batch::begin(root, nullptr, 4096U)};
        expect(static_cast<bool>(staged));
        if (!staged) {
            return;
        }
        auto digest{staged->put({})};
        expect(static_cast<bool>(digest));
        if (!digest) {
            return;
        }
        expect(*digest == blake3({}));
        expect(static_cast<bool>(staged->commit()));
        const auto blobs{staged->new_blobs()};
        expect(blobs.size() == 1U);
        if (blobs.size() != 1U) {
            return;
        }
        expect(blobs[0].flags == blob_flags_in_segment);
        expect(blobs[0].segment_id != 0U);
        expect(blobs[0].segment_offset == 0U);
        expect(blobs[0].logical_size == 0U);
        expect(blobs[0].stored_size == 0U);
        const auto sealed_path{segment_path(root, blobs[0].segment_id)};
        expect(refrain::core::io::file_size(sealed_path).value_or(1U) == 80U);

        auto view{view_of(
            std::vector<blob_table_entry>{entry_from_metadata(blobs[0])})};
        expect(view.has_value());
        if (!view) {
            return;
        }
        const auto location{location_for(*view, *digest)};
        auto read_back{store->read(location)};
        expect(static_cast<bool>(read_back));
        if (read_back) {
            expect(read_back->empty());
        }
        std::vector<std::byte> empty;
        const auto ranged{store->read_range(location, 0U, empty)};
        expect(static_cast<bool>(ranged));
        if (ranged) {
            expect(*ranged == 0U);
        }
    };

    "batch/interleaved small and large blobs share the open segment"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{store_root(scratch)};
        auto store{content_store::open(root)};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        const auto first_payload{bytes_of("one")};
        const auto large_payload{pseudo_random_blob(150000U, 5U)};
        const auto last_payload{bytes_of("two")};
        auto staged{batch::begin(root, nullptr, 4096U)};
        expect(static_cast<bool>(staged));
        if (!staged) {
            return;
        }
        auto first_digest{staged->put(first_payload)};
        auto large_digest{staged->put(large_payload)};
        auto last_digest{staged->put(last_payload)};
        expect(static_cast<bool>(first_digest));
        expect(static_cast<bool>(large_digest));
        expect(static_cast<bool>(last_digest));
        if (!first_digest || !large_digest || !last_digest) {
            return;
        }
        expect(static_cast<bool>(staged->commit()));
        const auto blobs{staged->new_blobs()};
        expect(blobs.size() == 3U);
        if (blobs.size() != 3U) {
            return;
        }
        expect(blobs[0].flags == blob_flags_in_segment);
        expect((blobs[1].flags & blob_flags_in_segment) == 0U);
        expect(blobs[2].flags == blob_flags_in_segment);
        expect(blobs[2].segment_id == blobs[0].segment_id);
        expect(blobs[2].segment_offset == 8U);
        expect(segment_files(root).size() == 1U);

        std::vector<blob_table_entry> records;
        for (const auto &meta : blobs) {
            records.push_back(entry_from_metadata(meta));
        }
        auto view{view_of(std::move(records))};
        expect(view.has_value());
        if (!view) {
            return;
        }
        expect(store->read(location_for(*view, *first_digest))
                   .value_or(std::vector<std::byte>{}) == first_payload);
        expect(store->read(location_for(*view, *large_digest))
                   .value_or(std::vector<std::byte>{}) == large_payload);
        expect(store->read(location_for(*view, *last_digest))
                   .value_or(std::vector<std::byte>{}) == last_payload);
    };

    "batch/damaged or missing segments fail location reads"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{store_root(scratch)};
        auto store{content_store::open(root)};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        const auto payload{bytes_of("segment payload")};
        auto staged{batch::begin(root, nullptr, 4096U)};
        expect(static_cast<bool>(staged));
        if (!staged) {
            return;
        }
        auto digest{staged->put(payload)};
        expect(static_cast<bool>(digest));
        if (!digest) {
            return;
        }
        expect(static_cast<bool>(staged->commit()));
        const auto blobs{staged->new_blobs()};
        expect(blobs.size() == 1U);
        if (blobs.size() != 1U) {
            return;
        }
        auto view{view_of(
            std::vector<blob_table_entry>{entry_from_metadata(blobs[0])})};
        expect(view.has_value());
        if (!view) {
            return;
        }
        const auto location{location_for(*view, *digest)};
        const auto sealed_path{segment_path(root, blobs[0].segment_id)};

        // The digest-only overloads cannot see in-segment blobs.
        const auto wrong_path{store->read(*digest)};
        expect(!wrong_path);
        if (!wrong_path) {
            expect(wrong_path.error().code == refrain::error_code::not_found);
            expect(wrong_path.error().message.find("blob_location") !=
                   std::string::npos);
        }
        std::vector<std::byte> head(1);
        const auto wrong_range{store->read_range(*digest, 0U, head)};
        expect(!wrong_range);
        if (!wrong_range) {
            expect(wrong_range.error().code == refrain::error_code::not_found);
            expect(wrong_range.error().message.find("blob_location") !=
                   std::string::npos);
        }
        expect(!store->contains(*digest));

        // Flip a body byte: the segment body BLAKE3 check must reject it.
        expect(flip_file_byte(sealed_path, 80U));
        const auto damaged{store->read(location)};
        expect(!damaged);
        if (!damaged) {
            expect(damaged.error().code == refrain::error_code::parse_error);
            expect(damaged.error().message.find("BLAKE3") != std::string::npos);
        }
        std::vector<std::byte> out(payload.size());
        const auto damaged_range{store->read_range(location, 0U, out)};
        expect(!damaged_range);
        if (!damaged_range) {
            expect(damaged_range.error().code ==
                   refrain::error_code::parse_error);
        }

        std::error_code removed;
        expect(fs::remove(fs::path{sealed_path}, removed));
        const auto missing{store->read(location)};
        expect(!missing);
        if (!missing) {
            expect(missing.error().code == refrain::error_code::io_error);
        }
    };

    "cas/large packed blobs read identically through the mapped path"_test =
        [] {
            temp_directory scratch{make_temp_directory()};
            const auto root{store_root(scratch)};
            auto store{content_store::open(root)};
            expect(static_cast<bool>(store));
            if (!store) {
                return;
            }
            // Incompressible payload: the RFBL file itself stays above the
            // 1 MiB mmap threshold, so read()/read_range() take the mapping
            // path instead of read_file().
            const auto payload{
                pseudo_random_blob(2U * 1024U * 1024U + 123U, 21U)};
            const auto digest{store->put(payload)};
            expect(static_cast<bool>(digest));
            if (!digest) {
                return;
            }
            const auto path{store->path_for(*digest)};
            const auto stored_size{refrain::core::io::file_size(path)};
            expect(stored_size.has_value());
            if (stored_size) {
                expect(*stored_size >= 1024U * 1024U);
            }
            expect(magic_is_rfbl(read_blob_file(*store, *digest)));

            const auto read_back{store->read(*digest)};
            expect(static_cast<bool>(read_back));
            if (read_back) {
                expect(*read_back == payload);
            }

            const auto layout{store->layout(*digest)};
            expect(static_cast<bool>(layout));
            if (layout) {
                expect(layout->kind == blob_kind::packed);
                expect(layout->logical_size == payload.size());
            }

            auto entry{entry_from_store(*store, *digest)};
            expect(entry.has_value());
            if (!entry) {
                return;
            }
            auto view{view_of(std::vector<blob_table_entry>{*entry})};
            expect(view.has_value());
            if (!view) {
                return;
            }
            const auto location{location_for(*view, *digest)};
            expect(store->read(location).value_or(std::vector<std::byte>{}) ==
                   payload);

            // Ranges crossing a 64 KiB block boundary, a 1 MiB mapping
            // boundary and the final partial block must all match the source.
            const std::array<std::pair<std::uint64_t, std::size_t>, 4> ranges{
                std::pair{0U, std::size_t{100}},
                std::pair{65530U, std::size_t{20}},
                std::pair{1024U * 1024U - 7U, std::size_t{64}},
                std::pair{2U * 1024U * 1024U - 10U, std::size_t{10}},
            };
            for (const auto &[offset, length] : ranges) {
                std::vector<std::byte> out(length);
                const auto copied{store->read_range(location, offset, out)};
                expect(static_cast<bool>(copied));
                if (copied) {
                    expect(*copied == length);
                    expect(out == slice_of(payload, offset, length));
                }
                std::vector<std::byte> digest_out(length);
                const auto digest_copied{
                    store->read_range(*digest, offset, digest_out)};
                expect(static_cast<bool>(digest_copied));
                if (digest_copied) {
                    expect(digest_out == slice_of(payload, offset, length));
                }
            }
        };

    "batch/large segments read through the mapped segment reader"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{store_root(scratch)};
        auto store{content_store::open(root)};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }

        // 24 raw 64 KiB blobs put the sealed segment above the 1 MiB segment
        // mmap threshold; the store target stays large enough for one segment.
        constexpr std::size_t blob_count{24U};
        constexpr std::size_t blob_size{65536U};
        std::vector<std::vector<std::byte>> payloads;
        payloads.reserve(blob_count);
        auto staged{batch::begin(root, nullptr, 32U * 1024U * 1024U)};
        expect(static_cast<bool>(staged));
        if (!staged) {
            return;
        }
        for (std::size_t index{0}; index < blob_count; ++index) {
            payloads.push_back(pseudo_random_blob(blob_size, 1000U + index));
            const auto digest{staged->put(payloads.back())};
            expect(static_cast<bool>(digest));
            if (!digest) {
                return;
            }
        }
        expect(static_cast<bool>(staged->commit()));
        const auto blobs{staged->new_blobs()};
        expect(blobs.size() == blob_count);
        if (blobs.size() != blob_count) {
            return;
        }
        expect(blobs[0].segment_id != 0U);
        expect(blobs[0].flags == blob_flags_in_segment);
        const auto sealed_path{segment_path(root, blobs[0].segment_id)};
        const auto sealed_size{refrain::core::io::file_size(sealed_path)};
        expect(sealed_size.has_value());
        if (sealed_size) {
            expect(*sealed_size >= 1024U * 1024U);
        }

        std::vector<blob_table_entry> records;
        records.reserve(blobs.size());
        for (const auto &meta : blobs) {
            records.push_back(entry_from_metadata(meta));
        }
        auto view{view_of(std::move(records))};
        expect(view.has_value());
        if (!view) {
            return;
        }

        for (std::size_t index{0}; index < blobs.size(); ++index) {
            const auto location{location_for(*view, blobs[index].digest)};
            expect(location.in_segment);
            const auto read_back{store->read(location)};
            expect(static_cast<bool>(read_back));
            if (read_back) {
                expect(same_bytes(*read_back, payloads[index]));
            }
            // The final 17 bytes of the 64 KiB blob (crossing no block
            // boundary, but exercising the tail of the mapped body).
            std::vector<std::byte> middle(17U);
            const auto copied{store->read_range(location, 65519U, middle)};
            expect(static_cast<bool>(copied));
            if (copied) {
                expect(
                    same_bytes(middle, slice_of(payloads[index], 65519U, 17U)));
            }
        }

        // The reader itself maps the sealed segment; read_at still copies the
        // requested slice out of the mapped body.
        auto reader{refrain::data::segment_reader::open(sealed_path)};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        expect(reader->mapped());
        expect(reader->header().data_bytes == blob_count * blob_size);
        const auto first{reader->read_at(0U, 32U)};
        expect(static_cast<bool>(first));
        if (first) {
            expect(*first == slice_of(payloads[0], 0U, 32U));
        }
        const auto last_offset{
            static_cast<std::uint32_t>((blob_count - 1U) * blob_size)};
        const auto last{reader->read_at(last_offset, 64U)};
        expect(static_cast<bool>(last));
        if (last) {
            expect(*last == slice_of(payloads.back(), 0U, 64U));
        }
        expect(static_cast<bool>(reader->verify()));
    };

    "cas/optional packed block cache preserves range reads"_test = [] {
        // Defaults: block cache off, eight segment cache slots.
        constexpr content_store_options defaults{};
        expect(defaults.block_cache_bytes == 0U);
        expect(defaults.segment_cache_slots == 8U);

        temp_directory scratch{make_temp_directory()};
        const auto root{store_root(scratch)};
        auto store{content_store::open(
            root, content_store_options{.block_cache_bytes = 1024U * 1024U})};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        // > 64 KiB => deterministic packed layout with five 64 KiB blocks
        // (the last one partial), i.e. every range below spans block
        // boundaries.
        const auto payload{pseudo_random_blob(300000U, 61U)};
        const auto digest{store->put(payload)};
        expect(static_cast<bool>(digest));
        if (!digest) {
            return;
        }
        const auto layout{store->layout(*digest)};
        expect(static_cast<bool>(layout));
        if (layout) {
            expect(layout->kind == blob_kind::packed);
            expect(layout->block_count == 5U);
        }
        auto entry{entry_from_store(*store, *digest)};
        expect(entry.has_value());
        if (!entry) {
            return;
        }
        auto view{view_of(std::vector<blob_table_entry>{*entry})};
        expect(view.has_value());
        if (!view) {
            return;
        }
        const auto location{location_for(*view, *digest)};
        expect(!location.in_segment);

        constexpr std::size_t window{128U * 1024U};
        const auto check_window =
            [&payload](const content_store &reader, const blob_location &where,
                       std::uint64_t offset, std::size_t length) {
                std::vector<std::byte> out(length);
                const auto copied{reader.read_range(where, offset, out)};
                expect(static_cast<bool>(copied));
                if (copied) {
                    expect(*copied == length);
                    expect(same_bytes(out, slice_of(payload, offset, length)));
                }
            };
        const auto readable_size{static_cast<std::uint64_t>(payload.size())};

        // Four passes over the blob in 128 KiB windows: each window covers two
        // blocks, so later passes hit the LRU instead of decompressing again.
        for (int pass{0}; pass < 4; ++pass) {
            for (std::uint64_t offset{0}; offset < readable_size;
                 offset += window) {
                const auto length{static_cast<std::size_t>(
                    std::min<std::uint64_t>(window, readable_size - offset))};
                check_window(*store, location, offset, length);
            }
        }
        // One range spanning several blocks (and ending in the partial block).
        std::vector<std::byte> wide(200000U);
        const auto wide_copied{store->read_range(location, 50000U, wide)};
        expect(static_cast<bool>(wide_copied));
        if (wide_copied) {
            expect(*wide_copied == wide.size());
            expect(same_bytes(wide, slice_of(payload, 50000U, wide.size())));
        }
        // A range crossing one 64 KiB block boundary.
        std::vector<std::byte> crossing(64U);
        const auto crossed{store->read_range(location, 65530U, crossing)};
        expect(static_cast<bool>(crossed));
        if (crossed) {
            expect(same_bytes(crossing, slice_of(payload, 65530U, 64U)));
        }
        // The digest-only overload threads the same block cache.
        std::vector<std::byte> digest_out(90U);
        const auto digest_copied{
            store->read_range(*digest, 131000U, digest_out)};
        expect(static_cast<bool>(digest_copied));
        if (digest_copied) {
            expect(same_bytes(digest_out, slice_of(payload, 131000U, 90U)));
        }

        // Cache disabled: identical bytes for every window.
        auto uncached{content_store::open(root)};
        expect(static_cast<bool>(uncached));
        if (!uncached) {
            return;
        }
        for (std::uint64_t offset{0}; offset < readable_size;
             offset += window) {
            const auto length{static_cast<std::size_t>(
                std::min<std::uint64_t>(window, readable_size - offset))};
            check_window(*uncached, location, offset, length);
        }
    };

    "batch/mapped segment readers are shared across concurrent reads"_test =
        [] {
            temp_directory scratch{make_temp_directory()};
            const auto root{store_root(scratch)};
            constexpr std::size_t blob_count{40U};
            constexpr std::size_t blob_size{65536U};
            std::vector<std::vector<std::byte>> payloads;
            payloads.reserve(blob_count);
            auto staged{batch::begin(root, nullptr, 32U * 1024U * 1024U)};
            expect(static_cast<bool>(staged));
            if (!staged) {
                return;
            }
            for (std::size_t index{0}; index < blob_count; ++index) {
                payloads.push_back(
                    pseudo_random_blob(blob_size, 7000U + index));
                const auto digest{staged->put(payloads.back())};
                expect(static_cast<bool>(digest));
                if (!digest) {
                    return;
                }
            }
            expect(static_cast<bool>(staged->commit()));
            const auto blobs{staged->new_blobs()};
            expect(blobs.size() == blob_count);
            if (blobs.size() != blob_count) {
                return;
            }
            expect(blobs[0].flags == blob_flags_in_segment);
            expect(blobs[0].segment_id != 0U);
            const auto sealed_path{segment_path(root, blobs[0].segment_id)};
            const auto sealed_size{refrain::core::io::file_size(sealed_path)};
            expect(sealed_size.has_value());
            if (sealed_size) {
                expect(*sealed_size >= 1024U * 1024U);
            }
            // Above the 1 MiB threshold the reader is mmap-backed, which is the
            // precondition for caching it.
            auto probe{refrain::data::segment_reader::open(sealed_path)};
            expect(static_cast<bool>(probe));
            if (!probe) {
                return;
            }
            expect(probe->mapped());

            std::vector<blob_table_entry> records;
            records.reserve(blobs.size());
            for (const auto &meta : blobs) {
                records.push_back(entry_from_metadata(meta));
            }
            auto view{view_of(std::move(records))};
            expect(view.has_value());
            if (!view) {
                return;
            }
            constexpr std::size_t target{17U};
            const auto location{location_for(*view, blobs[target].digest)};
            expect(location.in_segment);
            expect(location.segment_id == blobs[0].segment_id);

            const auto read_concurrently = [&](const content_store &reader,
                                               const std::vector<std::byte>
                                                   &expected) {
                std::array<std::optional<std::vector<std::byte>>, 4> results{};
                std::vector<std::thread> threads;
                threads.reserve(results.size());
                for (std::size_t index{0}; index < results.size(); ++index) {
                    threads.emplace_back(
                        [&reader, &location, &results, &expected, index] {
                            auto bytes{reader.read(location)};
                            if (bytes) {
                                results[index] = std::move(*bytes);
                            }
                        });
                }
                for (auto &thread : threads) {
                    thread.join();
                }
                for (const auto &bytes : results) {
                    expect(bytes.has_value());
                    if (bytes) {
                        expect(same_bytes(*bytes, expected));
                    }
                }
            };

            // Two slots, four readers: the first pass races on the cold cache
            // (opens happen outside the lock, one insert wins), the second
            // shares the single cached mapping concurrently.
            auto cached{content_store::open(
                root, content_store_options{.segment_cache_slots = 2U})};
            expect(static_cast<bool>(cached));
            if (!cached) {
                return;
            }
            read_concurrently(*cached, payloads[target]);
            read_concurrently(*cached, payloads[target]);

            // Control: cache disabled. Each thread opens its own mapped
            // one-off reader (mapped readers are safe to share, but these are
            // not shared) and must return the same bytes.
            auto uncached{content_store::open(
                root, content_store_options{.segment_cache_slots = 0U})};
            expect(static_cast<bool>(uncached));
            if (!uncached) {
                return;
            }
            read_concurrently(*uncached, payloads[target]);
        };

    "batch/put_file and API guards"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{store_root(scratch)};
        auto store{content_store::open(root)};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        const auto payload{binary_blob(2048)};
        const auto source_path{scratch.path / "batch-source.bin"};
        expect(refrain::core::io::write_file(source_path.string(), payload)
                   .has_value());

        auto staged{batch::begin(root, nullptr, 4096U)};
        expect(static_cast<bool>(staged));
        if (!staged) {
            return;
        }
        const auto missing_path{(scratch.path / "does-not-exist.bin").string()};
        const auto missing{staged->put_file(missing_path)};
        expect(!missing);
        if (!missing) {
            expect(missing.error().code == refrain::error_code::io_error);
        }
        auto digest{staged->put_file(source_path.string())};
        expect(static_cast<bool>(digest));
        if (!digest) {
            return;
        }
        expect(*digest == blake3(payload));
        expect(static_cast<bool>(staged->commit()));
        expect(!staged->commit());
        if (!staged->commit()) {
            expect(staged->commit().error().code ==
                   refrain::error_code::invalid_argument);
        }
        expect(!staged->put(payload));
        if (!staged->put(payload)) {
            expect(staged->put(payload).error().code ==
                   refrain::error_code::invalid_argument);
        }
        const auto blobs{staged->new_blobs()};
        expect(blobs.size() == 1U);
        if (blobs.size() == 1U) {
            auto view{view_of(
                std::vector<blob_table_entry>{entry_from_metadata(blobs[0])})};
            expect(view.has_value());
            if (view) {
                const auto location{location_for(*view, *digest)};
                expect(store->read(location).value_or(
                           std::vector<std::byte>{}) == payload);
            }
        }

        // Inert/default handles and invalid begin arguments.
        batch inert;
        expect(!inert.valid());
        expect(!inert.put(payload));
        if (!inert.put(payload)) {
            expect(inert.put(payload).error().code ==
                   refrain::error_code::not_initialized);
        }
        expect(!inert.commit());
        expect(inert.new_blobs().empty());
        expect(!batch::begin("", nullptr));
        if (!batch::begin("", nullptr)) {
            expect(batch::begin("", nullptr).error().code ==
                   refrain::error_code::invalid_argument);
        }
        expect(!batch::begin(root, nullptr, 0U));
        if (!batch::begin(root, nullptr, 0U)) {
            expect(batch::begin(root, nullptr, 0U).error().code ==
                   refrain::error_code::invalid_argument);
        }
    };

    "cas/open_existing rejects a corrupt published blobs.idx"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{store_root(scratch)};
        const auto content{(fs::path{root} / "content").string()};
        expect(refrain::core::io::create_directories(content).has_value());
        const auto table{(fs::path{content} / "blobs.idx").string()};
        expect(refrain::core::io::write_text(table, "not an RFBT").has_value());
        const auto corrupt{content_store::open_existing(root)};
        expect(!corrupt);
        if (!corrupt) {
            expect(corrupt.error().code == refrain::error_code::parse_error);
        }

        auto valid{refrain::data::build_blob_table({})};
        expect(valid.has_value());
        if (valid) {
            expect(refrain::core::io::atomic_write_file(table, *valid)
                       .has_value());
            expect(static_cast<bool>(content_store::open_existing(root)));
        }
    };
};

} // namespace
