// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/io.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/content_index.hpp>
#include <refrain/data/content_pack.hpp>
#include <refrain/data/content_store.hpp>
#include <refrain/data/path.hpp>
#include <refrain/data/vfs.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
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

using refrain::data::batch;
using refrain::data::blake3;
using refrain::data::blob_flags_in_segment;
using refrain::data::blob_flags_packed;
using refrain::data::blob_kind;
using refrain::data::blob_kind_content;
using refrain::data::blob_metadata;
using refrain::data::blob_table_entry;
using refrain::data::blob_table_view;
using refrain::data::build_blob_table;
using refrain::data::build_resolution_index;
using refrain::data::content_blob_table_filename;
using refrain::data::content_resolutions_directory;
using refrain::data::content_store;
using refrain::data::crc32c;
using refrain::data::digest32;
using refrain::data::join_path;
using refrain::data::open_content_pack;
using refrain::data::pack_format;
using refrain::data::pack_source;
using refrain::data::parse_blob_table;
using refrain::data::parse_resolution_index;
using refrain::data::resolution_index_entry_bytes;
using refrain::data::resolution_index_header_bytes;
using refrain::data::resolution_input;
using refrain::data::resolve;
using refrain::data::vfs;

constexpr std::string_view raw_text = R"({"format":1,"value":"raw"})";
constexpr std::size_t packed_size{200000};

// RFRX record/header offsets used to forge a legacy tie. The public header has
// the fixed sizes; the per-entry priority and the header digest/CRC offsets are
// fixed by the RFRX format.
constexpr std::size_t rfrx_priority_offset{8};
constexpr std::size_t rfrx_file_blake3_offset{48};
constexpr std::size_t rfrx_header_crc32c_offset{80};

// `content` is appended to the enum; the existing values are part of the
// diagnostic/API surface and must never shift. Value 2 (`rfpack`) is
// intentionally unused so every existing value keeps its meaning.
static_assert(static_cast<std::uint8_t>(pack_format::directory) == 0U);
static_assert(static_cast<std::uint8_t>(pack_format::zip) == 1U);
static_assert(static_cast<std::uint8_t>(pack_format::memory) == 3U);
static_assert(static_cast<std::uint8_t>(pack_format::content) == 4U);

[[nodiscard]] auto bytes_of(std::string_view text) -> std::vector<std::byte> {
    const auto bytes{
        std::as_bytes(std::span<const char>{text.data(), text.size()})};
    return std::vector<std::byte>{bytes.begin(), bytes.end()};
}

/// Deterministic 0..255 walk; larger than the 64 KiB raw limit, so the store
/// keeps it packed (multiple blocks).
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
                    ("refrain_content_pack_test_" + std::to_string(stamp) +
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

/// One published store used by the tests: two distinct blobs (one raw, one
/// packed, one of them written twice) and a three-location composition where
/// `assets/refrain_test/dup.json` also has a lower-priority fallback blob.
struct fixture {
    temp_directory scratch{make_temp_directory()};
    std::string root{(scratch.path / "store").string()};
    content_store store{};
    std::vector<std::byte> raw_bytes{bytes_of(raw_text)};
    std::vector<std::byte> packed_bytes{binary_blob(packed_size)};
    digest32 raw_digest{};
    digest32 packed_digest{};
    std::vector<std::byte> table_bytes{};
    std::vector<std::byte> index_bytes{};
};

[[nodiscard]] auto table_entry_for(const content_store &store,
                                   const digest32 &digest,
                                   std::uint32_t refcount)
    -> std::optional<blob_table_entry> {
    auto layout{store.layout(digest)};
    if (!layout) {
        return std::nullopt;
    }
    auto stored{refrain::core::io::file_size(store.path_for(digest))};
    if (!stored) {
        return std::nullopt;
    }
    return blob_table_entry{
        .digest = digest,
        .logical_size = layout->logical_size,
        .flags = layout->kind == blob_kind::packed ? blob_flags_packed : 0U,
        .refcount = refcount,
        .kind = blob_kind_content,
        .block_count = layout->block_count,
        .stored_size = static_cast<std::uint32_t>(*stored),
    };
}

[[nodiscard]] auto metadata_entry(const blob_metadata &meta,
                                  std::uint32_t refcount) -> blob_table_entry {
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
        .kind = blob_kind_content,
        .block_count = block_count,
        .stored_size = meta.stored_size,
        .segment_id = meta.segment_id,
        .segment_offset = meta.segment_offset,
    };
}

[[nodiscard]] auto write_table(const fixture &built,
                               std::span<const std::byte> bytes) -> bool {
    return refrain::core::io::atomic_write_file(
               join_path(join_path(built.root, "content"),
                         content_blob_table_filename),
               bytes)
        .has_value();
}

[[nodiscard]] auto write_index(const fixture &built,
                               std::string_view composition,
                               std::span<const std::byte> bytes) -> bool {
    return refrain::core::io::atomic_write_file(
               join_path(join_path(built.root, content_resolutions_directory),
                         std::string{composition} + ".idx"),
               bytes)
        .has_value();
}

void write_u32_le(std::span<std::byte> bytes, std::size_t offset,
                  std::uint32_t value) {
    for (std::size_t index{0}; index < 4U; ++index) {
        bytes[offset + index] =
            static_cast<std::byte>((value >> (8U * index)) & 0xFFU);
    }
}

/// Recompute the RFRX header fields after a test patches its records: the file
/// BLAKE3 covers everything after the 128-byte header, and the header CRC32C
/// covers bytes [0, 80).
void refresh_rfrx(std::vector<std::byte> &bytes) {
    const auto digest{blake3(std::span<const std::byte>{bytes}.subspan(
        resolution_index_header_bytes))};
    std::copy(digest.begin(), digest.end(),
              bytes.begin() +
                  static_cast<std::ptrdiff_t>(rfrx_file_blake3_offset));
    write_u32_le(bytes, rfrx_header_crc32c_offset,
                 crc32c(std::span<const std::byte>{bytes}.first(
                     rfrx_header_crc32c_offset)));
}

/// Position of `digest` in the digest-sorted blob table.
[[nodiscard]] auto table_index(const blob_table_view &table,
                               const digest32 &digest)
    -> std::optional<std::uint32_t> {
    for (std::size_t index{0}; index < table.entries.size(); ++index) {
        if (table.entries[index].digest == digest) {
            return static_cast<std::uint32_t>(index);
        }
    }
    return std::nullopt;
}

[[nodiscard]] auto build_fixture() -> std::unique_ptr<fixture> {
    auto built{std::make_unique<fixture>()};
    auto store{content_store::open(built->root)};
    if (!store) {
        return nullptr;
    }
    built->store = std::move(*store);

    auto raw{built->store.put(built->raw_bytes)};
    auto packed{built->store.put(built->packed_bytes)};
    auto duplicate{built->store.put(bytes_of(raw_text))};
    if (!raw || !packed || !duplicate || *duplicate != *raw) {
        return nullptr;
    }
    built->raw_digest = *raw;
    built->packed_digest = *packed;

    const auto raw_entry{table_entry_for(built->store, built->raw_digest, 2U)};
    const auto packed_entry{
        table_entry_for(built->store, built->packed_digest, 2U)};
    if (!raw_entry || !packed_entry) {
        return nullptr;
    }
    auto table{build_blob_table(
        std::vector<blob_table_entry>{*raw_entry, *packed_entry})};
    if (!table) {
        return nullptr;
    }
    built->table_bytes = std::move(*table);
    auto view{parse_blob_table(built->table_bytes)};
    if (!view) {
        return nullptr;
    }

    const std::vector<resolution_input> inputs{
        resolution_input{.location = "assets/refrain_test/dup.json",
                         .source = "mod:test",
                         .priority = 5,
                         .blob = built->raw_digest},
        resolution_input{.location = "assets/refrain_test/dup.json",
                         .source = "builtin",
                         .priority = 1,
                         .blob = built->packed_digest},
        resolution_input{.location = "data/refrain_test/packed.bin",
                         .source = "mod:test",
                         .priority = 5,
                         .blob = built->packed_digest},
        resolution_input{.location = "data/refrain_test/raw.json",
                         .source = "builtin",
                         .priority = 1,
                         .blob = built->raw_digest},
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
    if (!write_table(*built, built->table_bytes) ||
        !write_index(*built, "test", built->index_bytes)) {
        return nullptr;
    }
    return built;
}

const suite content_pack_tests = [] {
    "content pack metadata, sorted entries and find"_test = [] {
        const auto built{build_fixture()};
        expect(built != nullptr);
        if (built == nullptr) {
            return;
        }
        auto opened{open_content_pack(built->root, "test")};
        expect(opened.has_value());
        if (!opened) {
            return;
        }
        expect((*opened)->format() == pack_format::content);
        expect((*opened)->label() == "content:test");
        expect((*opened)->file_count() == 3U);

        // RFRX is location_hash-ordered; the pack must expose path order.
        const auto entries{(*opened)->entries()};
        expect(entries.size() == 3U);
        if (entries.size() != 3U) {
            return;
        }
        expect(entries[0].path == "assets/refrain_test/dup.json");
        expect(entries[0].size == built->raw_bytes.size());
        expect(entries[1].path == "data/refrain_test/packed.bin");
        expect(entries[1].size == built->packed_bytes.size());
        expect(entries[2].path == "data/refrain_test/raw.json");
        expect(entries[2].size == built->raw_bytes.size());

        const auto *found{(*opened)->find("data/refrain_test/packed.bin")};
        expect(found != nullptr);
        if (found != nullptr) {
            expect(found->size == built->packed_bytes.size());
        }
        expect((*opened)->find("data/refrain_test/missing.json") == nullptr);

        auto custom{open_content_pack(built->root, "test", "custom-label")};
        expect(custom.has_value());
        if (custom) {
            expect((*custom)->label() == "custom-label");
        }
    };

    "content pack reads raw and packed blobs byte for byte"_test = [] {
        const auto built{build_fixture()};
        expect(built != nullptr);
        if (built == nullptr) {
            return;
        }
        auto opened{open_content_pack(built->root, "test")};
        expect(opened.has_value());
        if (!opened) {
            return;
        }
        const auto raw{(*opened)->read("data/refrain_test/raw.json")};
        expect(raw.has_value());
        if (raw) {
            expect(*raw == built->raw_bytes);
        }
        const auto packed{(*opened)->read("data/refrain_test/packed.bin")};
        expect(packed.has_value());
        if (packed) {
            expect(*packed == built->packed_bytes);
        }
        // The higher-priority (raw) record wins over the packed fallback.
        const auto duplicate{(*opened)->read("assets/refrain_test/dup.json")};
        expect(duplicate.has_value());
        if (duplicate) {
            expect(*duplicate == built->raw_bytes);
        }
        const auto missing{(*opened)->read("data/refrain_test/missing.json")};
        expect(!missing);
        if (!missing) {
            expect(missing.error().code == refrain::error_code::not_found);
        }
    };

    "content pack legacy tie picks the resolve() winner in either record "
    "order"_test = [] {
        const auto built{build_fixture()};
        expect(built != nullptr);
        if (built == nullptr) {
            return;
        }
        auto table{parse_blob_table(built->table_bytes)};
        expect(table.has_value());
        if (!table) {
            return;
        }
        const auto raw_index{table_index(*table, built->raw_digest)};
        const auto packed_index{table_index(*table, built->packed_digest)};
        expect(raw_index.has_value());
        expect(packed_index.has_value());
        if (!raw_index || !packed_index) {
            return;
        }
        expect(*raw_index != *packed_index);
        const auto smaller_index{std::min(*raw_index, *packed_index)};
        const auto larger_index{std::max(*raw_index, *packed_index)};
        const bool raw_wins{*raw_index < *packed_index};
        // Publish the larger blob_index at the higher priority, so the first
        // RFRX record becomes the loser once the priorities tie: the old
        // first-record rule would expose it even though resolve() picks the
        // smaller index.
        const auto &first_blob{raw_wins ? built->packed_digest
                                        : built->raw_digest};
        const auto &second_blob{raw_wins ? built->raw_digest
                                         : built->packed_digest};
        const auto &expected_bytes{raw_wins ? built->raw_bytes
                                            : built->packed_bytes};
        constexpr std::string_view location{
            "data/refrain_test/legacy-tie.json"};
        const std::vector<resolution_input> inputs{
            resolution_input{.location = std::string{location},
                             .source = "mod:test",
                             .priority = 5,
                             .blob = first_blob},
            resolution_input{.location = std::string{location},
                             .source = "mod:test",
                             .priority = 1,
                             .blob = second_blob},
        };
        auto built_index{build_resolution_index(inputs, *table)};
        expect(built_index.has_value());
        if (!built_index) {
            return;
        }
        auto legacy{*built_index};
        // Equalize both priorities. The records still share source_index, so
        // the parser accepts the tied pair as one legacy group in either
        // order.
        constexpr std::uint32_t tied_priority{3};
        write_u32_le(legacy,
                     resolution_index_header_bytes + rfrx_priority_offset,
                     tied_priority);
        write_u32_le(legacy,
                     resolution_index_header_bytes +
                         resolution_index_entry_bytes + rfrx_priority_offset,
                     tied_priority);
        refresh_rfrx(legacy);

        const auto check_resolve = [&table, location](
                                       const std::vector<std::byte> &bytes,
                                       std::uint32_t first_blob_index) {
            auto parsed{parse_resolution_index(bytes, *table)};
            expect(parsed.has_value());
            if (!parsed) {
                return std::optional<std::uint32_t>{};
            }
            expect(parsed->entries.size() == 2U);
            if (parsed->entries.size() != 2U) {
                return std::optional<std::uint32_t>{};
            }
            expect(parsed->entries[0].blob_index == first_blob_index);
            auto resolved{resolve(*parsed, location)};
            expect(resolved.has_value());
            if (!resolved || resolved->winner == nullptr) {
                return std::optional<std::uint32_t>{};
            }
            expect(resolved->fallback.size() == 1U);
            return std::optional<std::uint32_t>{resolved->winner->blob_index};
        };
        const auto check_pack = [&built, &expected_bytes, location]() {
            auto opened{open_content_pack(built->root, "legacy")};
            expect(opened.has_value());
            if (!opened) {
                return;
            }
            expect((*opened)->file_count() == 1U);
            expect((*opened)->find(location) != nullptr);
            const auto bytes{(*opened)->read(location)};
            expect(bytes.has_value());
            if (bytes) {
                expect(*bytes == expected_bytes);
            }
        };

        const auto before{check_resolve(legacy, larger_index)};
        expect(before.has_value());
        if (before) {
            expect(*before == smaller_index);
        }
        expect(write_index(*built, "legacy", legacy));
        check_pack();

        // Swap the two 32-byte records: the on-disk winner changes, the pack
        // winner (and resolve()'s) must not.
        const auto first{std::span<std::byte>{legacy}.subspan(
            resolution_index_header_bytes, resolution_index_entry_bytes)};
        const auto second{std::span<std::byte>{legacy}.subspan(
            resolution_index_header_bytes + resolution_index_entry_bytes,
            resolution_index_entry_bytes)};
        std::vector<std::byte> saved(first.begin(), first.end());
        std::copy(second.begin(), second.end(), first.begin());
        std::copy(saved.begin(), saved.end(), second.begin());
        refresh_rfrx(legacy);

        const auto after{check_resolve(legacy, smaller_index)};
        expect(after.has_value());
        if (after) {
            expect(*after == smaller_index);
        }
        expect(write_index(*built, "legacy", legacy));
        check_pack();
    };

    "content pack missing composition is not_found"_test = [] {
        const auto built{build_fixture()};
        expect(built != nullptr);
        if (built == nullptr) {
            return;
        }
        const auto opened{open_content_pack(built->root, "missing")};
        expect(!opened);
        if (!opened) {
            expect(opened.error().code == refrain::error_code::not_found);
        }
    };

    "content pack truncated blob table is parse_error"_test = [] {
        const auto built{build_fixture()};
        expect(built != nullptr);
        if (built == nullptr) {
            return;
        }
        auto truncated{built->table_bytes};
        truncated.resize(64U);
        expect(write_table(*built, truncated));
        const auto opened{open_content_pack(built->root, "test")};
        expect(!opened);
        if (!opened) {
            expect(opened.error().code == refrain::error_code::parse_error);
        }
    };

    "content pack corrupted resolution index is parse_error"_test = [] {
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
        expect(write_index(*built, "test", corrupted));
        const auto opened{open_content_pack(built->root, "test")};
        expect(!opened);
        if (!opened) {
            expect(opened.error().code == refrain::error_code::parse_error);
        }
    };

    "content pack rejects unsafe composition names"_test = [] {
        const auto built{build_fixture()};
        expect(built != nullptr);
        if (built == nullptr) {
            return;
        }
        const std::vector<std::string> compositions{
            "",     "../x", "a/b", ".hidden",
            "a\\b", "a b",  "a:b", std::string(129U, 'a')};
        for (const auto &composition : compositions) {
            const auto opened{open_content_pack(built->root, composition)};
            expect(!opened) << "composition: " << composition;
            if (!opened) {
                expect(opened.error().code ==
                       refrain::error_code::invalid_argument);
            }
        }
        // The length limit itself is inclusive: a valid 128-byte name simply
        // has no published index.
        const auto maximal{
            open_content_pack(built->root, std::string(128U, 'a'))};
        expect(!maximal);
        if (!maximal) {
            expect(maximal.error().code == refrain::error_code::not_found);
        }
    };

    "content pack rejects invalid store roots"_test = [] {
        const auto empty{open_content_pack("", "test")};
        expect(!empty);
        if (!empty) {
            expect(empty.error().code == refrain::error_code::invalid_argument);
        }
        const std::string invalid_utf8{"\xC3\x28", 2};
        const auto bad_root{open_content_pack(invalid_utf8, "test")};
        expect(!bad_root);
        if (!bad_root) {
            expect(bad_root.error().code ==
                   refrain::error_code::invalid_argument);
        }
    };

    "empty resolution index is a legal empty pack"_test = [] {
        const auto built{build_fixture()};
        expect(built != nullptr);
        if (built == nullptr) {
            return;
        }
        auto view{parse_blob_table(built->table_bytes)};
        expect(view.has_value());
        if (!view) {
            return;
        }
        auto empty{refrain::data::build_resolution_index({}, *view)};
        expect(empty.has_value());
        if (!empty) {
            return;
        }
        expect(write_index(*built, "empty", *empty));
        auto opened{open_content_pack(built->root, "empty")};
        expect(opened.has_value());
        if (!opened) {
            return;
        }
        expect((*opened)->file_count() == 0U);
        expect((*opened)->entries().empty());
        expect((*opened)->find("data/refrain_test/raw.json") == nullptr);
        const auto missing{(*opened)->read("data/refrain_test/raw.json")};
        expect(!missing);
        if (!missing) {
            expect(missing.error().code == refrain::error_code::not_found);
        }

        vfs stack;
        const auto mounted{stack.mount(std::move(*opened))};
        expect(!mounted);
        if (!mounted) {
            expect(mounted.error().code ==
                   refrain::error_code::invalid_argument);
        }
    };

    "content pack mounts into vfs as pack_format::content"_test = [] {
        const auto built{build_fixture()};
        expect(built != nullptr);
        if (built == nullptr) {
            return;
        }
        auto opened{open_content_pack(built->root, "test")};
        expect(opened.has_value());
        if (!opened) {
            return;
        }
        vfs stack;
        expect(stack.mount(std::move(*opened)).has_value());
        expect(stack.pack_count() == 1U);
        expect(stack.file_count() == 3U);
        expect(stack.find("data/refrain_test/raw.json") != nullptr);
        expect(stack.find("data/refrain_test/missing.json") == nullptr);

        const auto mounted{stack.mounted()};
        expect(mounted.size() == 1U);
        if (mounted.size() == 1U) {
            expect(mounted[0].label == "content:test");
            expect(mounted[0].format == pack_format::content);
            expect(mounted[0].file_count == 3U);
        }
        const auto size{stack.size("data/refrain_test/packed.bin")};
        expect(size.has_value());
        if (size) {
            expect(*size == built->packed_bytes.size());
        }
        const auto packed{stack.read("data/refrain_test/packed.bin")};
        expect(packed.has_value());
        if (packed) {
            expect(*packed == built->packed_bytes);
        }

        // A content pack is a store+composition pair, not a path: the
        // path-based mount must reject it with a pointer to the factory.
        const auto rejected{stack.mount(
            pack_source{.path = built->root, .format = pack_format::content})};
        expect(!rejected);
        if (!rejected) {
            expect(rejected.error().code ==
                   refrain::error_code::invalid_argument);
            expect(rejected.error().message.find("open_content_pack") !=
                   std::string::npos);
        }
    };

    "content pack read maps missing blobs to io_error and damage to "
    "parse_error"_test = [] {
        const auto built{build_fixture()};
        expect(built != nullptr);
        if (built == nullptr) {
            return;
        }
        auto opened{open_content_pack(built->root, "test")};
        expect(opened.has_value());
        if (!opened) {
            return;
        }

        std::error_code remove_error;
        const auto removed{
            fs::remove(built->store.path_for(built->raw_digest), remove_error)};
        expect(removed);
        expect(!remove_error);
        const auto missing{(*opened)->read("data/refrain_test/raw.json")};
        expect(!missing);
        if (!missing) {
            expect(missing.error().code == refrain::error_code::io_error);
        }

        // Overwriting a blob with different bytes is store corruption, which
        // the store reports as parse_error; the pack keeps that code.
        expect(refrain::core::io::write_file(
                   built->store.path_for(built->packed_digest),
                   bytes_of("not the packed blob"))
                   .has_value());
        const auto damaged{(*opened)->read("data/refrain_test/packed.bin")};
        expect(!damaged);
        if (!damaged) {
            expect(damaged.error().code == refrain::error_code::parse_error);
        }
    };

    "content pack reads blobs that live in content segments"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto root{(scratch.path / "segmented-store").string()};
        const auto first{bytes_of(R"({"segment":1})")};
        const auto second{binary_blob(3000)};
        const auto large{binary_blob(packed_size)};

        auto staged{batch::begin(root, nullptr, 4096U)};
        expect(staged.has_value());
        if (!staged) {
            return;
        }
        auto first_digest{staged->put(first)};
        auto second_digest{staged->put(second)};
        auto large_digest{staged->put(large)};
        expect(first_digest.has_value());
        expect(second_digest.has_value());
        expect(large_digest.has_value());
        if (!first_digest || !second_digest || !large_digest) {
            return;
        }
        expect(staged->commit().has_value());
        const auto blobs{staged->new_blobs()};
        expect(blobs.size() == 3U);
        if (blobs.size() != 3U) {
            return;
        }
        // Two small blobs share a segment; the large one keeps its own file.
        expect((blobs[0].flags & blob_flags_in_segment) != 0U);
        expect((blobs[1].flags & blob_flags_in_segment) != 0U);
        expect(blobs[0].segment_id == blobs[1].segment_id);
        expect((blobs[2].flags & blob_flags_in_segment) == 0U);

        std::vector<blob_table_entry> entries;
        for (const auto &meta : blobs) {
            entries.push_back(metadata_entry(meta, 1U));
        }
        auto table_bytes{build_blob_table(entries)};
        expect(table_bytes.has_value());
        if (!table_bytes) {
            return;
        }
        const auto table_path{
            join_path(join_path(root, "content"), content_blob_table_filename)};
        expect(refrain::core::io::atomic_write_file(table_path, *table_bytes)
                   .has_value());
        auto table{parse_blob_table(*table_bytes)};
        expect(table.has_value());
        if (!table) {
            return;
        }

        const std::vector<resolution_input> inputs{
            resolution_input{.location = "data/refrain_test/segment.json",
                             .source = "mod:test",
                             .priority = 1,
                             .blob = *first_digest},
            resolution_input{.location = "assets/refrain_test/segment.bin",
                             .source = "mod:test",
                             .priority = 1,
                             .blob = *second_digest},
            resolution_input{.location = "data/refrain_test/large.bin",
                             .source = "mod:test",
                             .priority = 1,
                             .blob = *large_digest},
        };
        auto index_bytes{build_resolution_index(inputs, *table)};
        expect(index_bytes.has_value());
        if (!index_bytes) {
            return;
        }
        const auto index_path{join_path(
            join_path(root, content_resolutions_directory), "segmented.idx")};
        expect(refrain::core::io::create_directories(
                   join_path(root, content_resolutions_directory))
                   .has_value());
        expect(refrain::core::io::atomic_write_file(index_path, *index_bytes)
                   .has_value());

        auto opened{open_content_pack(root, "segmented")};
        expect(opened.has_value());
        if (!opened) {
            return;
        }
        expect((*opened)->format() == pack_format::content);
        const auto *raw_entry{
            (*opened)->find("data/refrain_test/segment.json")};
        expect(raw_entry != nullptr);
        if (raw_entry != nullptr) {
            expect(raw_entry->size == first.size());
        }
        expect((*opened)
                   ->read("data/refrain_test/segment.json")
                   .value_or(std::vector<std::byte>{}) == first);
        expect((*opened)
                   ->read("assets/refrain_test/segment.bin")
                   .value_or(std::vector<std::byte>{}) == second);
        expect((*opened)
                   ->read("data/refrain_test/large.bin")
                   .value_or(std::vector<std::byte>{}) == large);

        // A missing segment file is an I/O failure for the pack contract.
        const auto segment_file{
            join_path(join_path(root, "content/segments"),
                      // The name is the id's 16-hex spelling; derive it from
                      // the metadata the same way a publisher would.
                      [&blobs] {
                          constexpr char digits[]{"0123456789abcdef"};
                          std::string name(16, '0');
                          const auto id{blobs[0].segment_id};
                          for (std::size_t index{0}; index < 16U; ++index) {
                              const auto shift{4U * (16U - 1U - index)};
                              name[index] = digits[(id >> shift) & 0xFU];
                          }
                          name.append(".rfseg");
                          return name;
                      }())};
        std::error_code remove_error;
        expect(fs::remove(fs::path{segment_file}, remove_error));
        const auto missing{(*opened)->read("data/refrain_test/segment.json")};
        expect(!missing);
        if (!missing) {
            expect(missing.error().code == refrain::error_code::io_error);
        }
    };
};

} // namespace
