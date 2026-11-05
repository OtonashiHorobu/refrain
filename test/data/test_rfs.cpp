// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/io.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/rfs.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;
namespace fs = std::filesystem;

using refrain::data::blake3;
using refrain::data::build_rfs;
using refrain::data::rfs_entry;
using refrain::data::rfs_format_version;
using refrain::data::rfs_header_bytes;
using refrain::data::rfs_input;
using refrain::data::rfs_reader;
using refrain::data::rfs_record_bytes;
using refrain::data::rfs_schema_hash;
using refrain::data::rfs_write_options;
using refrain::data::to_hex;
using refrain::data::write_rfs;

constexpr std::size_t header_schema_hash_offset{8};
constexpr std::size_t header_record_count_offset{24};
constexpr std::size_t header_reserved_28_offset{28};
constexpr std::size_t header_total_size_offset{32};
constexpr std::size_t header_data_hash_offset{40};
constexpr std::size_t header_reserved_72_offset{72};
constexpr std::size_t record_key_hash_offset{0};
constexpr std::size_t record_key_offset_offset{8};
constexpr std::size_t record_key_length_offset{12};
constexpr std::size_t record_flags_offset{14};
constexpr std::size_t record_data_offset_offset{16};
constexpr std::size_t record_data_len_offset{24};

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

/// Deterministic pseudo-random bytes: incompressible payloads must be stored
/// verbatim by the writer (zstd cannot make them smaller).
[[nodiscard]] auto incompressible_bytes(std::size_t count)
    -> std::vector<std::byte> {
    std::vector<std::byte> data(count);
    std::uint64_t state{0x9E3779B97F4A7C15ULL};
    for (auto &byte : data) {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        byte = static_cast<std::byte>(state & 0xFFU);
    }
    return data;
}

struct record_content {
    std::string key;
    std::vector<std::byte> data;
};

/// Build the borrowed-input vector; `storage` must outlive the span use.
[[nodiscard]] auto make_inputs(const std::vector<record_content> &storage)
    -> std::vector<rfs_input> {
    std::vector<rfs_input> inputs;
    inputs.reserve(storage.size());
    for (const auto &record : storage) {
        inputs.push_back(rfs_input{.key = record.key, .data = record.data});
    }
    return inputs;
}

[[nodiscard]] auto make_temp_directory() -> fs::path {
    static int counter{0};
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    const auto path{fs::temp_directory_path() /
                    ("refrain_rfs_test_" + std::to_string(stamp) + "_" +
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

// Little-endian accessors used to craft corrupt containers (the reader is
// tested against them, so the test spells the on-disk layout out
// independently of the implementation).
[[nodiscard]] auto read_u16_at(std::span<const std::byte> bytes,
                               std::size_t offset) -> std::uint16_t {
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint16_t>(bytes[offset]) |
        (std::to_integer<std::uint16_t>(bytes[offset + 1U]) << 8U));
}

[[nodiscard]] auto read_u32_at(std::span<const std::byte> bytes,
                               std::size_t offset) -> std::uint32_t {
    std::uint32_t value{0};
    for (std::size_t index{0}; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(
                     std::to_integer<unsigned int>(bytes[offset + index]))
                 << (8U * index);
    }
    return value;
}

[[nodiscard]] auto read_u64_at(std::span<const std::byte> bytes,
                               std::size_t offset) -> std::uint64_t {
    std::uint64_t value{0};
    for (std::size_t index{0}; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(
                     std::to_integer<unsigned int>(bytes[offset + index]))
                 << (8U * index);
    }
    return value;
}

void write_u16_at(std::vector<std::byte> &bytes, std::size_t offset,
                  std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xFFU);
    bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xFFU);
}

void write_u32_at(std::vector<std::byte> &bytes, std::size_t offset,
                  std::uint32_t value) {
    for (std::size_t index{0}; index < 4; ++index) {
        bytes[offset + index] =
            static_cast<std::byte>((value >> (8U * index)) & 0xFFU);
    }
}

void write_u64_at(std::vector<std::byte> &bytes, std::size_t offset,
                  std::uint64_t value) {
    for (std::size_t index{0}; index < 8; ++index) {
        bytes[offset + index] =
            static_cast<std::byte>((value >> (8U * index)) & 0xFFU);
    }
}

/// Recompute the body digest after deliberately mutating the record table, key
/// arena or data region: lets a negative test reach the semantic validation
/// checks instead of being rejected by the data-hash gate.
void reseal(std::vector<std::byte> &container) {
    const auto digest{blake3(
        std::span<const std::byte>{container.data() + rfs_header_bytes,
                                   container.size() - rfs_header_bytes})};
    for (std::size_t index{0}; index < digest.size(); ++index) {
        container[header_data_hash_offset + index] = digest[index];
    }
}

[[nodiscard]] auto record_offset_of(std::size_t index) -> std::size_t {
    return rfs_header_bytes + index * rfs_record_bytes;
}

[[nodiscard]] auto align_up64(std::uint64_t value) -> std::uint64_t {
    return (value + 63U) & ~std::uint64_t{63U};
}

/// Data-region start of a well-formed container (derived from header + keys).
[[nodiscard]] auto data_start_of(std::span<const std::byte> container)
    -> std::uint64_t {
    const auto count{read_u32_at(container, header_record_count_offset)};
    std::uint64_t key_bytes{0};
    for (std::uint32_t index{0}; index < count; ++index) {
        key_bytes += read_u16_at(container, record_offset_of(index) +
                                                record_key_length_offset);
    }
    const auto key_arena_end{
        static_cast<std::uint64_t>(rfs_header_bytes) +
        static_cast<std::uint64_t>(count) * rfs_record_bytes + key_bytes};
    return align_up64(key_arena_end);
}

template <typename Value>
[[nodiscard]] auto error_code_of(const refrain::result<Value> &value) -> int {
    return value.has_value() ? -1 : static_cast<int>(value.error().code);
}

[[nodiscard]] auto make_schema_hash(std::byte seed) -> rfs_schema_hash {
    rfs_schema_hash schema{};
    for (std::size_t index{0}; index < schema.size(); ++index) {
        schema[index] = seed;
    }
    return schema;
}

const suite rfs_tests = [] {
    "rfs/empty container roundtrip"_test = [] {
        const auto schema{make_schema_hash(std::byte{0x5A})};
        const auto built{build_rfs({}, schema)};
        expect(static_cast<bool>(built));
        if (!built) {
            return;
        }
        // Empty body: 128-byte header only, 64-byte-aligned data start.
        expect(built->size() == rfs_header_bytes);
        const auto expect_reader{[&](const std::vector<std::byte> &bytes) {
            auto reader{rfs_reader::open(bytes)};
            expect(static_cast<bool>(reader));
            if (!reader) {
                return;
            }
            expect(reader->valid());
            expect(reader->file_count() == 0U);
            expect(reader->entries().empty());
            expect(reader->file_size() == rfs_header_bytes);
            expect(reader->find("missing") == nullptr);
            expect(!reader->contains("missing"));
            expect(!reader->view("missing"));
            expect(!reader->read("missing"));
            expect(reader->read("missing").error().code ==
                   refrain::error_code::not_found);
            expect(reader->schema_hash() == schema);
            expect(reader->data_hash() == blake3({}));
        }};
        expect_reader(*built);
    };

    "rfs/single stored record roundtrip"_test = [] {
        const auto schema{make_schema_hash(std::byte{0x01})};
        std::vector<record_content> storage{
            {.key = "data/hello.txt", .data = bytes_of("hello rfs1\n")}};
        const auto inputs{make_inputs(storage)};
        const auto built{build_rfs(inputs, schema)};
        expect(static_cast<bool>(built));
        if (!built) {
            return;
        }
        auto reader{rfs_reader::open(*built)};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        expect(reader->file_count() == 1U);
        expect(reader->schema_hash() == schema);
        expect(reader->file_size() == built->size());
        // Body hash is the stable identity: BLAKE3 over bytes[128..size).
        expect(reader->data_hash() == blake3(std::span<const std::byte>{
                                          built->data() + rfs_header_bytes,
                                          built->size() - rfs_header_bytes}));
        const auto *entry{reader->find("data/hello.txt")};
        expect(entry != nullptr);
        if (entry != nullptr) {
            // Tiny payloads never shrink under zstd: stored verbatim.
            expect(!entry->is_compressed());
            expect(entry->flags == 0U);
            expect(entry->data_len == storage[0].data.size());
            expect(entry->stored_len == entry->data_len);
            // The data region is 64-byte aligned; the first payload starts
            // there.
            expect(entry->data_offset == data_start_of(*built));
            expect(entry->data_offset % 64U == 0U);
            expect(text_of(reader->view(*entry).value_or(
                       std::span<const std::byte>{})) == "hello rfs1\n");
            const auto read_back{reader->read(*entry)};
            expect(static_cast<bool>(read_back));
            if (read_back) {
                expect(*read_back == storage[0].data);
            }
        }
        expect(reader->contains("data/hello.txt"));
        expect(reader->find("data/hello.txt/") == nullptr);
        expect(reader->find("DATA/hello.txt") == nullptr);
    };

    "rfs/empty payload records"_test = [] {
        std::vector<record_content> storage{
            {.key = "a/empty", .data = {}},
            {.key = "b/nonempty", .data = bytes_of("x")},
        };
        const auto inputs{make_inputs(storage)};
        const auto built{build_rfs(inputs, {})};
        expect(static_cast<bool>(built));
        if (!built) {
            return;
        }
        auto reader{rfs_reader::open(*built)};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        const auto *empty{reader->find("a/empty")};
        expect(empty != nullptr);
        if (empty != nullptr) {
            expect(empty->data_len == 0U);
            expect(empty->stored_len == 0U);
            expect(!empty->is_compressed());
            const auto view{reader->view(*empty)};
            expect(static_cast<bool>(view));
            if (view) {
                expect(view->empty());
            }
            const auto content{reader->read(*empty)};
            expect(static_cast<bool>(content));
            if (content) {
                expect(content->empty());
            }
        }
    };

    "rfs/many records sorted, roundtrip and deterministic"_test = [] {
        std::vector<record_content> storage;
        for (int index{0}; index < 24; ++index) {
            std::string key{"record/"};
            key += static_cast<char>('a' + index / 10);
            key += static_cast<char>('0' + index % 10);
            std::vector<std::byte> data;
            switch (index % 4) {
            case 0:
                data =
                    incompressible_bytes(static_cast<std::size_t>(300 + index));
                break;
            case 1:
                data = bytes_of(
                    std::string(static_cast<std::size_t>(2048 + index), 'A'));
                break;
            case 2:
                data = {};
                break;
            default:
                data = bytes_of("small-" + std::to_string(index));
                break;
            }
            storage.push_back({.key = std::move(key), .data = std::move(data)});
        }
        const auto inputs{make_inputs(storage)};
        const auto first{build_rfs(inputs, make_schema_hash(std::byte{2}))};
        expect(static_cast<bool>(first));
        if (!first) {
            return;
        }

        // Input order is irrelevant: rebuild from reversed inputs.
        std::vector<record_content> reordered{storage.rbegin(), storage.rend()};
        const auto reversed_inputs{make_inputs(reordered)};
        const auto second{
            build_rfs(reversed_inputs, make_schema_hash(std::byte{2}))};
        expect(static_cast<bool>(second));
        if (!second) {
            return;
        }
        expect(*first == *second);

        auto reader{rfs_reader::open(*first)};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        expect(reader->file_count() == storage.size());
        expect(std::is_sorted(reader->entries().begin(),
                              reader->entries().end(),
                              [](const rfs_entry &lhs, const rfs_entry &rhs) {
                                  return lhs.key < rhs.key;
                              }));
        for (const auto &record : storage) {
            const auto *entry{reader->find(record.key)};
            expect(entry != nullptr) << record.key;
            if (entry == nullptr) {
                continue;
            }
            const auto content{reader->read(*entry)};
            expect(static_cast<bool>(content)) << record.key;
            if (content) {
                expect(*content == record.data) << record.key;
            }
        }
        // A reader copy stays valid after the original goes out of scope-free
        // path (all data owned by the test for borrowed open).
        auto copied{*reader};
        expect(copied.file_count() == reader->file_count());
        expect(copied.data_hash() == reader->data_hash());
    };

    "rfs/compression is used only when it saves space"_test = [] {
        std::vector<record_content> storage{
            {.key = "a/compressible",
             .data = bytes_of(std::string(64U * 1024U, 'C'))},
            {.key = "b/incompressible",
             .data = incompressible_bytes(64U * 1024U)},
            {.key = "c/empty", .data = {}},
            {.key = "d/small", .data = bytes_of(std::string(512, 'D'))},
        };
        const auto inputs{make_inputs(storage)};
        const auto built{build_rfs(inputs, {})};
        expect(static_cast<bool>(built));
        if (!built) {
            return;
        }
        auto reader{rfs_reader::open(*built)};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        const auto *compressible{reader->find("a/compressible")};
        expect(compressible != nullptr);
        if (compressible != nullptr) {
            expect(compressible->is_compressed());
            expect(compressible->stored_len < compressible->data_len);
            // Compressed records have no zero-copy view...
            const auto view{reader->view(*compressible)};
            expect(!view);
            if (!view) {
                expect(view.error().code == refrain::error_code::parse_error);
                expect(view.error().message.find("read()") !=
                       std::string::npos);
            }
            // ...but read() decompresses correctly.
            const auto content{reader->read(*compressible)};
            expect(static_cast<bool>(content));
            if (content) {
                expect(*content == storage[0].data);
            }
        }
        const auto *incompressible{reader->find("b/incompressible")};
        expect(incompressible != nullptr);
        if (incompressible != nullptr) {
            expect(!incompressible->is_compressed());
            expect(incompressible->stored_len == incompressible->data_len);
        }
        // 512 bytes is below the default 1 KiB compression threshold.
        const auto *small{reader->find("d/small")};
        expect(small != nullptr);
        if (small != nullptr) {
            expect(!small->is_compressed());
        }
        for (const auto &record : storage) {
            const auto content{reader->read(record.key)};
            expect(static_cast<bool>(content)) << record.key;
            if (content) {
                expect(*content == record.data) << record.key;
            }
        }
    };

    "rfs/compression options are honoured"_test = [] {
        std::vector<record_content> storage{
            {.key = "a/repeated", .data = bytes_of(std::string(4096, 'E'))}};
        const auto inputs{make_inputs(storage)};

        rfs_write_options plain;
        plain.compress = false;
        const auto uncompressed{build_rfs(inputs, {}, plain)};
        expect(static_cast<bool>(uncompressed));
        if (uncompressed) {
            auto reader{rfs_reader::open(*uncompressed)};
            expect(static_cast<bool>(reader));
            if (reader) {
                const auto *entry{reader->find("a/repeated")};
                expect(entry != nullptr);
                if (entry != nullptr) {
                    expect(!entry->is_compressed());
                    expect(entry->stored_len == entry->data_len);
                }
            }
        }

        rfs_write_options high_threshold;
        high_threshold.compression_threshold = 1U << 20U;
        const auto thresholded{build_rfs(inputs, {}, high_threshold)};
        expect(static_cast<bool>(thresholded));
        if (thresholded) {
            auto reader{rfs_reader::open(*thresholded)};
            expect(static_cast<bool>(reader));
            if (reader) {
                const auto *entry{reader->find("a/repeated")};
                expect(entry != nullptr);
                if (entry != nullptr) {
                    expect(!entry->is_compressed());
                }
            }
        }

        // threshold 0 compresses even tiny highly repetitive payloads (when
        // zstd actually shrinks them).
        std::vector<record_content> tiny_storage{
            {.key = "tiny", .data = bytes_of(std::string(256, 'F'))}};
        const auto tiny_inputs{make_inputs(tiny_storage)};
        rfs_write_options always;
        always.compression_threshold = 0;
        always.zstd_level = 19;
        const auto tiny{build_rfs(tiny_inputs, {}, always)};
        expect(static_cast<bool>(tiny));
        if (tiny) {
            auto reader{rfs_reader::open(*tiny)};
            expect(static_cast<bool>(reader));
            if (reader) {
                expect(reader->read("tiny").value_or(
                           std::vector<std::byte>{}) == tiny_storage[0].data);
            }
        }
    };

    "rfs/large payloads roundtrip"_test = [] {
        std::vector<record_content> storage{
            {.key = "large/compressible",
             .data = bytes_of(std::string(512U * 1024U, 'G'))},
            {.key = "large/incompressible",
             .data = incompressible_bytes(200000)},
        };
        const auto inputs{make_inputs(storage)};
        const auto built{build_rfs(inputs, {})};
        expect(static_cast<bool>(built));
        if (!built) {
            return;
        }
        auto reader{rfs_reader::open(*built)};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        for (const auto &record : storage) {
            const auto content{reader->read(record.key)};
            expect(static_cast<bool>(content)) << record.key;
            if (content) {
                expect(*content == record.data) << record.key;
            }
        }
    };

    "rfs/unicode keys are preserved and byte-sorted"_test = [] {
        std::vector<record_content> storage{
            {.key = "data/\u00e4\u00f6\u00fc.json", .data = bytes_of("umlaut")},
            {.key = "data/\u65e5\u672c\u8a9e.json",
             .data = bytes_of("japanese")},
            {.key = "data/zzz.json", .data = bytes_of("ascii")},
            {.key = "data/a b c.json", .data = bytes_of("spaces")},
        };
        const auto inputs{make_inputs(storage)};
        const auto built{build_rfs(inputs, {})};
        expect(static_cast<bool>(built));
        if (!built) {
            return;
        }
        auto reader{rfs_reader::open(*built)};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        expect(reader->file_count() == storage.size());
        for (const auto &record : storage) {
            expect(reader->find(record.key) != nullptr) << record.key;
            const auto content{reader->read(record.key)};
            expect(static_cast<bool>(content)) << record.key;
            if (content) {
                expect(*content == record.data) << record.key;
            }
        }
        // Keys are ordered by raw UTF-8 bytes, not by locale/case.
        for (std::size_t index{1}; index < reader->entries().size(); ++index) {
            expect(reader->entries()[index - 1U].key <
                   reader->entries()[index].key);
        }
    };

    "rfs/schema hash and body hash stability"_test = [] {
        std::vector<record_content> storage{
            {.key = "x", .data = bytes_of("payload")},
        };
        const auto inputs{make_inputs(storage)};
        const auto schema_a{make_schema_hash(std::byte{0x11})};
        const auto schema_b{make_schema_hash(std::byte{0x22})};
        const auto first{build_rfs(inputs, schema_a)};
        const auto again{build_rfs(inputs, schema_a)};
        const auto other_schema{build_rfs(inputs, schema_b)};
        expect(static_cast<bool>(first));
        expect(static_cast<bool>(again));
        expect(static_cast<bool>(other_schema));
        if (!first || !again || !other_schema) {
            return;
        }
        expect(*first == *again);
        // The schema hash is header-only: the body (and therefore the content
        // identity) does not change with it.
        auto reader_a{rfs_reader::open(*first)};
        auto reader_b{rfs_reader::open(*other_schema)};
        expect(static_cast<bool>(reader_a));
        expect(static_cast<bool>(reader_b));
        if (reader_a && reader_b) {
            expect((*first) != (*other_schema));
            expect(reader_a->data_hash() == reader_b->data_hash());
            expect(reader_a->schema_hash() == schema_a);
            expect(reader_b->schema_hash() == schema_b);
            // Only the 16 schema-hash header bytes differ.
            std::size_t differing{0};
            for (std::size_t index{0}; index < first->size(); ++index) {
                if ((*first)[index] != (*other_schema)[index]) {
                    ++differing;
                }
            }
            expect(differing == schema_a.size());
            expect((*first)[header_schema_hash_offset] == std::byte{0x11});
            expect((*other_schema)[header_schema_hash_offset] ==
                   std::byte{0x22});
        }
        // The BLAKE3 body hash is the stable identity of identical records.
        expect(to_hex(blake3({})) == to_hex(blake3({})));
    };

    "rfs/build rejects invalid inputs"_test = [] {
        // Empty key.
        const auto empty_key{
            build_rfs(std::array{rfs_input{.key = "", .data = {}}}, {})};
        expect(!empty_key);
        expect(empty_key.error().code == refrain::error_code::invalid_argument);

        // Duplicate keys (independent of input order).
        std::vector<record_content> duplicates{
            {.key = "dup", .data = bytes_of("a")},
            {.key = "dup", .data = bytes_of("b")}};
        const auto duplicate_inputs{make_inputs(duplicates)};
        const auto duplicate{build_rfs(duplicate_inputs, {})};
        expect(!duplicate);
        expect(duplicate.error().code == refrain::error_code::invalid_argument);

        // Key above the per-record limit.
        std::vector<record_content> too_long{
            {.key = std::string(4097, 'k'), .data = {}}};
        const auto too_long_inputs{make_inputs(too_long)};
        const auto oversized_key{build_rfs(too_long_inputs, {})};
        expect(!oversized_key);
        expect(oversized_key.error().code ==
               refrain::error_code::invalid_argument);

        // Bad zstd levels.
        const auto no_compress{
            build_rfs(std::array{rfs_input{.key = "a", .data = {}}}, {},
                      rfs_write_options{.compress = true,
                                        .compression_threshold = 0,
                                        .zstd_level = 0})};
        expect(!no_compress);
        expect(no_compress.error().code ==
               refrain::error_code::invalid_argument);
        const auto too_high{
            build_rfs(std::array{rfs_input{.key = "a", .data = {}}}, {},
                      rfs_write_options{.compress = true,
                                        .compression_threshold = 0,
                                        .zstd_level = 20})};
        expect(!too_high);
    };

    "rfs/header corruption is rejected"_test = [] {
        std::vector<record_content> storage{
            {.key = "data/a", .data = bytes_of("payload-a")},
            {.key = "data/b", .data = bytes_of("payload-b")}};
        const auto inputs{make_inputs(storage)};
        const auto built{build_rfs(inputs, make_schema_hash(std::byte{9}))};
        expect(static_cast<bool>(built));
        if (!built) {
            return;
        }
        const std::vector<std::byte> valid{*built};
        const auto expect_rejected = [&valid](const auto &mutate) {
            auto container{valid};
            mutate(container);
            const auto reader{rfs_reader::open(container)};
            expect(!reader);
            if (!reader) {
                expect(reader.error().code == refrain::error_code::parse_error);
            }
        };

        // Short header / bad magic / unsupported version / non-zero flags.
        expect(!rfs_reader::open({}));
        expect_rejected([](auto &container) { container.resize(64); });
        expect_rejected([](auto &container) { container[0] = std::byte{'X'}; });
        expect_rejected([](auto &container) {
            write_u16_at(container, 4,
                         static_cast<std::uint16_t>(rfs_format_version + 1U));
        });
        expect_rejected([](auto &container) { write_u16_at(container, 6, 1); });
        // Reserved header fields must be zero.
        expect_rejected([](auto &container) {
            container[header_reserved_72_offset] = std::byte{1};
        });
        expect_rejected([](auto &container) {
            write_u32_at(container, header_reserved_28_offset, 1);
        });
        // Truncated/trailing bytes and a lying total_size.
        expect_rejected([](auto &container) { container.pop_back(); });
        expect_rejected(
            [](auto &container) { container.push_back(std::byte{0}); });
        expect_rejected([&valid](auto &container) {
            write_u64_at(container, header_total_size_offset,
                         valid.size() + 64U);
        });
        // Record count above the format limit and a table that overruns.
        expect_rejected([](auto &container) {
            write_u32_at(container, header_record_count_offset, 0x0010'0001U);
        });
        expect_rejected([](auto &container) {
            write_u32_at(container, header_record_count_offset, 0x000F'FFFFU);
        });
        // Mutated payload without resealing: the body hash catches it.
        expect_rejected(
            [](auto &container) { container.back() ^= std::byte{0xFF}; });
    };

    "rfs/corrupt record semantics are rejected (resealed)"_test = [] {
        // Key "a" (1 byte) makes the key arena 1 byte past the table, so there
        // are 63 bytes of alignment padding before the data region.
        std::vector<record_content> storage{
            {.key = "a", .data = bytes_of(std::string(4096, 'H'))},
            {.key = "b", .data = bytes_of("second")},
        };
        const auto inputs{make_inputs(storage)};
        const auto built{build_rfs(inputs, {})};
        expect(static_cast<bool>(built));
        if (!built) {
            return;
        }
        const std::vector<std::byte> valid{*built};
        const auto expect_rejected = [&valid](const auto &mutate) {
            auto container{valid};
            mutate(container);
            reseal(container);
            const auto reader{rfs_reader::open(container)};
            expect(!reader);
            if (!reader) {
                expect(reader.error().code == refrain::error_code::parse_error);
            }
        };
        const auto first{record_offset_of(0)};
        const auto second{record_offset_of(1)};

        // Reserved flag bits.
        expect_rejected([second](auto &container) {
            write_u16_at(container, second + record_flags_offset, 0x0002U);
        });
        // Zero key length and over-long key length.
        expect_rejected([first](auto &container) {
            write_u16_at(container, first + record_key_length_offset, 0);
        });
        expect_rejected([first](auto &container) {
            write_u16_at(container, first + record_key_length_offset, 0xFFFFU);
        });
        // Non-canonical key offsets (second key must follow the first).
        expect_rejected([first, second](auto &container) {
            const auto first_key_offset{
                read_u32_at(container, first + record_key_offset_offset)};
            write_u32_at(container, second + record_key_offset_offset,
                         first_key_offset);
        });
        // Wrong stored key hash (table is re-sealed but the fast-reject field
        // must still be BLAKE3(key)[0..8]).
        expect_rejected([first](auto &container) {
            write_u64_at(container, first + record_key_hash_offset, 0U);
        });
        // Duplicate keys in strictly-sorted form: rewrite "b" to "a" and fix
        // its key hash so the sortedness check is what rejects it.
        expect_rejected([&valid, second](auto &container) {
            const auto key_off{
                read_u32_at(valid, second + record_key_offset_offset)};
            const auto key_len{
                read_u16_at(valid, second + record_key_length_offset)};
            const auto table_end{static_cast<std::size_t>(rfs_header_bytes) +
                                 static_cast<std::size_t>(read_u32_at(
                                     valid, header_record_count_offset)) *
                                     rfs_record_bytes};
            auto key{std::string(static_cast<std::size_t>(key_len), 'a')};
            for (std::size_t index{0}; index < key.size(); ++index) {
                container[table_end + key_off + index] =
                    static_cast<std::byte>(key[index]);
            }
            const auto digest{blake3(
                std::as_bytes(std::span<const char>{key.data(), key.size()}))};
            std::uint64_t key_hash{0};
            for (std::size_t index{0}; index < 8; ++index) {
                key_hash |= static_cast<std::uint64_t>(
                                std::to_integer<unsigned int>(digest[index]))
                            << (8U * index);
            }
            write_u64_at(container, second + record_key_hash_offset, key_hash);
        });
        // First record's payload must start at the 64-byte-aligned data start.
        expect_rejected([first, &valid](auto &container) {
            const auto start{data_start_of(valid)};
            write_u64_at(container, first + record_data_offset_offset,
                         start + 64U);
        });
        // Stored (uncompressed) records must stay contiguous with their size.
        expect_rejected([first, second](auto &container) {
            const auto offset{
                read_u64_at(container, first + record_data_offset_offset)};
            const auto length{
                read_u64_at(container, first + record_data_len_offset)};
            write_u64_at(container, second + record_data_offset_offset,
                         offset + length + 1U);
        });
        // Uncompressed record with a mismatched stored size (last record).
        expect_rejected([second](auto &container) {
            const auto length{
                read_u64_at(container, second + record_data_len_offset)};
            write_u64_at(container, second + record_data_len_offset,
                         length + 1U);
        });
        // Data offset beyond the file end.
        expect_rejected([second, &valid](auto &container) {
            write_u64_at(container, second + record_data_offset_offset,
                         valid.size() + 8U);
        });
        // Non-zero key-arena alignment padding.
        expect_rejected([&valid](auto &container) {
            const auto count{
                read_u32_at(container, header_record_count_offset)};
            std::uint64_t key_bytes{0};
            for (std::uint32_t index{0}; index < count; ++index) {
                key_bytes +=
                    read_u16_at(container, record_offset_of(index) +
                                               record_key_length_offset);
            }
            const auto padding_offset{
                static_cast<std::size_t>(rfs_header_bytes) +
                static_cast<std::size_t>(count) * rfs_record_bytes +
                static_cast<std::size_t>(key_bytes)};
            expect(padding_offset < data_start_of(valid));
            container[padding_offset] = std::byte{1};
        });
        // -- data hash gate: an unsealed payload mutation is rejected ---------
        {
            auto container{valid};
            container.back() ^= std::byte{0x01};
            const auto reader{rfs_reader::open(container)};
            expect(!reader);
            if (!reader) {
                expect(reader.error().code == refrain::error_code::parse_error);
            }
        }
    };

    "rfs/compressed record corruption is rejected on read"_test = [] {
        std::vector<record_content> storage{
            {.key = "z/compressible",
             .data = bytes_of(std::string(8192, 'I'))}};
        const auto inputs{make_inputs(storage)};
        const auto built{build_rfs(inputs, {})};
        expect(static_cast<bool>(built));
        if (!built) {
            return;
        }
        auto reader{rfs_reader::open(*built)};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        const auto *entry{reader->find("z/compressible")};
        expect(entry != nullptr);
        if (entry == nullptr) {
            return;
        }
        expect(entry->is_compressed());
        // Corrupt one byte of the stored frame: open() still succeeds (hash is
        // resealed by the mutation helper below), read() must reject the
        // payload.
        auto container{*built};
        container[static_cast<std::size_t>(entry->data_offset)] ^=
            std::byte{0xFF};
        reseal(container);
        auto corrupted{rfs_reader::open(container)};
        expect(static_cast<bool>(corrupted));
        if (corrupted) {
            const auto content{corrupted->read("z/compressible")};
            expect(!content);
            if (!content) {
                expect(content.error().code ==
                       refrain::error_code::parse_error);
            }
            const auto view{corrupted->view("z/compressible")};
            expect(!view);
        }
        // A compressed record forged to declare an empty payload is rejected at
        // open time.
        auto empty_compressed{*built};
        write_u64_at(empty_compressed,
                     record_offset_of(0) + record_data_len_offset, 0U);
        reseal(empty_compressed);
        expect(!rfs_reader::open(empty_compressed));
    };

    "rfs/open_file roundtrip and error mapping"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto path{scratch.path / "sample.rfs"};
        const auto schema{make_schema_hash(std::byte{0x33})};
        std::vector<record_content> storage{
            {.key = "k/a", .data = bytes_of("alpha")},
            {.key = "k/b", .data = bytes_of(std::string(4096, 'J'))},
        };
        const auto inputs{make_inputs(storage)};
        expect(write_rfs(path.string(), inputs, schema).has_value());

        auto owning{rfs_reader::open_file(path.string())};
        expect(static_cast<bool>(owning));
        if (!owning) {
            return;
        }
        expect(owning->schema_hash() == schema);
        expect(owning->file_count() == storage.size());
        expect(owning->read("k/a").value_or(std::vector<std::byte>{}) ==
               storage[0].data);
        expect(owning->read("k/b").value_or(std::vector<std::byte>{}) ==
               storage[1].data);

        // A copy still owns its buffer (the file bytes are not re-read).
        auto copied{*owning};
        expect(copied.file_count() == 2U);
        expect(copied.read("k/a").has_value());

        // A truncated file is a parse error, not an IO error.
        const auto corrupt_path{scratch.path / "corrupt.rfs"};
        const auto valid_bytes{refrain::core::io::read_file(path.string())};
        expect(static_cast<bool>(valid_bytes));
        if (valid_bytes) {
            auto truncated{*valid_bytes};
            truncated.resize(truncated.size() / 2U);
            expect(
                refrain::core::io::write_file(corrupt_path.string(), truncated)
                    .has_value());
            const auto corrupt{rfs_reader::open_file(corrupt_path.string())};
            expect(!corrupt);
            if (!corrupt) {
                expect(corrupt.error().code ==
                       refrain::error_code::parse_error);
            }
        }

        // Missing files surface the underlying IO error.
        const auto missing{
            rfs_reader::open_file((scratch.path / "missing.rfs").string())};
        expect(!missing);
        if (!missing) {
            expect(missing.error().code == refrain::error_code::io_error);
        }
        // An empty/invalid path is an IO-layer argument error, not a crash.
        expect(!rfs_reader::open_file(""));
    };

    "rfs/default reader is inert and rejects queries"_test = [] {
        rfs_reader reader;
        expect(!reader.valid());
        expect(reader.entries().empty());
        expect(reader.find("x") == nullptr);
        // Key lookups on an empty reader are "not found" (the reader owns no
        // records at all) ...
        expect(!reader.view("x"));
        expect(!reader.read("x"));
        if (!reader.view("x")) {
            expect(reader.view("x").error().code ==
                   refrain::error_code::not_found);
        }
        // ... while a structurally invalid entry is rejected as a reader error.
        const rfs_entry bogus{.key = "x"};
        expect(!reader.view(bogus));
        if (!reader.view(bogus)) {
            expect(reader.view(bogus).error().code ==
                   refrain::error_code::parse_error);
        }
        expect(!reader.read(bogus));
        if (!reader.read(bogus)) {
            expect(reader.read(bogus).error().code ==
                   refrain::error_code::parse_error);
        }
    };

    "rfs/data hash is content-sensitive and stable"_test = [] {
        std::vector<record_content> first_storage{
            {.key = "k", .data = bytes_of("payload-one")}};
        std::vector<record_content> second_storage{
            {.key = "k", .data = bytes_of("payload-two")}};
        const auto first_inputs{make_inputs(first_storage)};
        const auto second_inputs{make_inputs(second_storage)};
        const auto first{build_rfs(first_inputs, {})};
        const auto second{build_rfs(second_inputs, {})};
        expect(static_cast<bool>(first));
        expect(static_cast<bool>(second));
        if (!first || !second) {
            return;
        }
        auto reader_a{rfs_reader::open(*first)};
        auto reader_b{rfs_reader::open(*second)};
        expect(static_cast<bool>(reader_a));
        expect(static_cast<bool>(reader_b));
        if (reader_a && reader_b) {
            expect(reader_a->data_hash() != reader_b->data_hash());
            expect(reader_a->read("k").value_or(std::vector<std::byte>{}) ==
                   first_storage[0].data);
            expect(reader_b->read("k").value_or(std::vector<std::byte>{}) ==
                   second_storage[0].data);
        }
        // Same content rebuilt from a fresh input vector is identical.
        const auto rebuilt_storage{first_storage};
        const auto rebuilt_inputs{make_inputs(rebuilt_storage)};
        const auto rebuilt{build_rfs(rebuilt_inputs, {})};
        expect(static_cast<bool>(rebuilt));
        if (rebuilt) {
            expect(*rebuilt == *first);
        }
    };

    "rfs/open_file agrees with a borrowed open"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto path{scratch.path / "agree.rfs"};
        std::vector<record_content> storage{
            {.key = "same/a", .data = bytes_of("alpha")},
            {.key = "same/b", .data = bytes_of(std::string(2048, 'L'))},
        };
        const auto inputs{make_inputs(storage)};
        expect(write_rfs(path.string(), inputs, make_schema_hash(std::byte{7}))
                   .has_value());
        auto owning{rfs_reader::open_file(path.string())};
        const auto bytes{refrain::core::io::read_file(path.string())};
        expect(static_cast<bool>(owning));
        expect(static_cast<bool>(bytes));
        if (owning && bytes) {
            auto borrowed{rfs_reader::open(*bytes)};
            expect(static_cast<bool>(borrowed));
            if (borrowed) {
                expect(owning->file_size() == borrowed->file_size());
                expect(owning->file_count() == borrowed->file_count());
                expect(owning->data_hash() == borrowed->data_hash());
                expect(owning->schema_hash() == borrowed->schema_hash());
                for (const auto &record : storage) {
                    expect(owning->read(record.key)
                               .value_or(std::vector<std::byte>{}) ==
                           record.data);
                    expect(borrowed->read(record.key)
                               .value_or(std::vector<std::byte>{}) ==
                           record.data);
                }
            }
        }
    };

    "rfs/entries expose offsets and sizes matching the layout"_test = [] {
        std::vector<record_content> storage;
        for (int index{0}; index < 8; ++index) {
            storage.push_back(
                {.key = "entry/" + std::to_string(index),
                 .data = bytes_of(std::string(
                     static_cast<std::size_t>(index) * 400U + 10U, 'K'))});
        }
        const auto inputs{make_inputs(storage)};
        const auto built{build_rfs(inputs, {})};
        expect(static_cast<bool>(built));
        if (!built) {
            return;
        }
        auto reader{rfs_reader::open(*built)};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        // The record table sits directly after the header and the table has a
        // fixed 32-byte stride.
        expect(reader->file_count() * rfs_record_bytes + rfs_header_bytes <
               built->size());
        const auto data_start{data_start_of(*built)};
        expect(reader->entries().front().data_offset == data_start);
        for (std::size_t index{0}; index < reader->entries().size(); ++index) {
            const auto &entry{reader->entries()[index]};
            expect(entry.data_len == storage[index].data.size());
            if (index + 1U < reader->entries().size()) {
                expect(entry.data_offset <
                       reader->entries()[index + 1U].data_offset);
            }
        }
        // Data ranges tile the file exactly (no holes for an all-stored file).
        const auto last{reader->entries().back()};
        expect(last.data_offset + last.stored_len == built->size());
    };
};

} // namespace
