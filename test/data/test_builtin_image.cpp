// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/error.hpp>
#include <refrain/data/builtin_image.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/content_index.hpp>
#include <refrain/data/vfs.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;

using refrain::error_code;
using refrain::data::blake3;
using refrain::data::build_builtin_image;
using refrain::data::builtin_image;
using refrain::data::builtin_image_input;
using refrain::data::crc32c;
using refrain::data::open_builtin_image_pack;
using refrain::data::pack_format;
using refrain::data::parse_blob_table;
using refrain::data::parse_resolution_index;
using refrain::data::vfs;

// Byte offsets of the documented RFSI v1 header. The layout test pins them;
// corruption tests reuse them to tamper with a specific field without depending
// on any internal constant.
constexpr std::size_t header_bytes{128};
constexpr std::size_t segment_entry_bytes{24};
constexpr std::size_t blob_entry_bytes{48};
constexpr std::size_t table_offset_field{8};
constexpr std::size_t table_bytes_field{16};
constexpr std::size_t index_offset_field{24};
constexpr std::size_t index_bytes_field{32};
constexpr std::size_t segment_dir_offset_field{40};
constexpr std::size_t segment_count_field{48};
constexpr std::size_t blob_dir_offset_field{56};
constexpr std::size_t blob_count_field{64};
constexpr std::size_t file_digest_field{72};
constexpr std::size_t header_crc_field{104};

constexpr std::size_t packed_size{200000};
constexpr std::size_t duplicate_size{18}; // "duplicate payload\n"

/// Byte-vector equality through a plain bool: `expect(a == b)` on two
/// non-const `std::vector<std::byte>` lvalues would bind boost::ut's range
/// comparison operator, which cannot stream `std::byte` on failure.
[[nodiscard]] auto same_bytes(const std::vector<std::byte> &lhs,
                              const std::vector<std::byte> &rhs) -> bool {
    return lhs == rhs;
}

[[nodiscard]] auto bytes_of(std::string_view text) -> std::vector<std::byte> {
    const auto bytes{
        std::as_bytes(std::span<const char>{text.data(), text.size()})};
    return std::vector<std::byte>{bytes.begin(), bytes.end()};
}

/// Deterministic 0..255 walk; larger than 64 KiB, so the generator keeps it as
/// a standalone packed blob.
[[nodiscard]] auto binary_blob(std::size_t count) -> std::vector<std::byte> {
    std::vector<std::byte> data(count);
    for (std::size_t index{0}; index < count; ++index) {
        data[index] = static_cast<std::byte>(index % 256U);
    }
    return data;
}

[[nodiscard]] constexpr auto read_u16_le(std::span<const std::byte> bytes,
                                         std::size_t offset) -> std::uint16_t {
    return static_cast<std::uint16_t>(
        static_cast<std::uint32_t>(
            std::to_integer<unsigned int>(bytes[offset])) |
        (static_cast<std::uint32_t>(
             std::to_integer<unsigned int>(bytes[offset + 1U]))
         << 8U));
}

[[nodiscard]] constexpr auto read_u32_le(std::span<const std::byte> bytes,
                                         std::size_t offset) -> std::uint32_t {
    std::uint32_t value{0};
    for (std::size_t index{0}; index < 4U; ++index) {
        value |= static_cast<std::uint32_t>(
                     std::to_integer<unsigned int>(bytes[offset + index]))
                 << (8U * index);
    }
    return value;
}

[[nodiscard]] constexpr auto read_u64_le(std::span<const std::byte> bytes,
                                         std::size_t offset) -> std::uint64_t {
    std::uint64_t value{0};
    for (std::size_t index{0}; index < 8U; ++index) {
        value |= static_cast<std::uint64_t>(
                     std::to_integer<unsigned int>(bytes[offset + index]))
                 << (8U * index);
    }
    return value;
}

constexpr void write_u32_le(std::span<std::byte> bytes, std::size_t offset,
                            std::uint32_t value) {
    for (std::size_t index{0}; index < 4U; ++index) {
        bytes[offset + index] =
            static_cast<std::byte>((value >> (8U * index)) & 0xFFU);
    }
}

/// Recompute the image-level checksums after tampering, so a test reaches the
/// validation it targets instead of failing on `file_blake3` first.
void refresh_image_checksums(std::vector<std::byte> &image) {
    const auto digest{
        blake3(std::span<const std::byte>{image}.subspan(header_bytes))};
    std::copy(digest.begin(), digest.end(),
              image.begin() + static_cast<std::ptrdiff_t>(file_digest_field));
    write_u32_le(
        image, header_crc_field,
        crc32c(std::span<const std::byte>{image}.first(header_crc_field)));
}

/// Four files exercising every storage class: two raw blobs sharing one
/// content segment (one of them zero bytes) and one > 64 KiB packed blob on
/// its own, with two locations pointing at the duplicated raw blob.
[[nodiscard]] auto make_inputs() -> std::vector<builtin_image_input> {
    std::vector<builtin_image_input> files;
    files.push_back(builtin_image_input{
        .path = "data/demo/two.txt", .data = bytes_of("duplicate payload\n")});
    files.push_back(builtin_image_input{.path = "assets/demo/nested/data.bin",
                                        .data = binary_blob(packed_size)});
    files.push_back(builtin_image_input{.path = "data/demo/one.txt",
                                        .data = bytes_of("duplicate "
                                                         "payload\n")});
    files.push_back(
        builtin_image_input{.path = "assets/demo/empty.bin", .data = {}});
    return files;
}

[[nodiscard]] auto build(std::span<const builtin_image_input> files)
    -> std::optional<std::vector<std::byte>> {
    auto bytes{build_builtin_image(files)};
    expect(static_cast<bool>(bytes)) << "the generator must build this input";
    if (!bytes) {
        return std::nullopt;
    }
    return std::move(*bytes);
}

void expect_open_fails(std::span<const std::byte> image, error_code code,
                       std::string_view context) {
    auto opened{builtin_image::open(image)};
    expect(!opened) << "expected rejection: " << context;
    if (!opened) {
        expect(opened.error().code == code) << "wrong code: " << context;
    }
}

/// Expected bytes of `path` from the fixture inputs.
[[nodiscard]] auto expected_bytes(std::span<const builtin_image_input> files,
                                  std::string_view path)
    -> std::vector<std::byte> {
    for (const auto &file : files) {
        if (file.path == path) {
            return file.data;
        }
    }
    return {};
}

const suite builtin_image_tests = [] {
    "image/build roundtrips raw, packed, empty and duplicated files"_test = [] {
        auto files{make_inputs()};
        auto image_bytes{build(files)};
        if (!image_bytes) {
            return;
        }

        auto image{builtin_image::open(*image_bytes)};
        expect(static_cast<bool>(image));
        if (!image) {
            return;
        }
        expect(image->file_count() == files.size());
        const auto entries{image->entries()};
        expect(entries.size() == files.size());
        if (entries.size() != files.size()) {
            return;
        }
        // Winners sorted by path bytes, regardless of the input order.
        expect(entries[0].path == "assets/demo/empty.bin");
        expect(entries[1].path == "assets/demo/nested/data.bin");
        expect(entries[2].path == "data/demo/one.txt");
        expect(entries[3].path == "data/demo/two.txt");
        for (std::size_t index{1}; index < entries.size(); ++index) {
            expect(entries[index - 1U].path < entries[index].path);
        }
        expect(entries[0].size == 0U);
        expect(entries[1].size == packed_size);
        expect(entries[2].size == duplicate_size);
        expect(entries[3].size == duplicate_size);

        for (const auto &entry : entries) {
            const auto *found{image->find(entry.path)};
            expect(found != nullptr);
            if (found != nullptr) {
                expect(found->path == entry.path);
                expect(found->size == entry.size);
            }
            auto read{image->read(entry.path)};
            expect(static_cast<bool>(read)) << "read: " << entry.path;
            if (read) {
                expect(same_bytes(*read, expected_bytes(files, entry.path)));
                expect(read->size() == entry.size);
            }
        }
        expect(image->find("data/does-not-exist.txt") == nullptr);
        expect(image->find("") == nullptr);
        auto missing{image->read("data/does-not-exist.txt")};
        expect(!missing);
        if (!missing) {
            expect(missing.error().code == error_code::not_found);
        }

        expect(image->digest() ==
               blake3(std::span<const std::byte>{*image_bytes}.subspan(
                   header_bytes)));
    };

    "image/header, directories and index bytes pin the RFSI v1 layout"_test =
        [] {
            auto files{make_inputs()};
            auto image_bytes{build(files)};
            if (!image_bytes) {
                return;
            }
            const std::span<const std::byte> bytes{*image_bytes};
            expect(bytes.size() > header_bytes);

            // Magic, version and flags.
            expect(std::to_integer<char>(bytes[0]) == 'R');
            expect(std::to_integer<char>(bytes[1]) == 'F');
            expect(std::to_integer<char>(bytes[2]) == 'S');
            expect(std::to_integer<char>(bytes[3]) == 'I');
            expect(read_u16_le(bytes, 4) == 1U);
            expect(read_u16_le(bytes, 6) == 0U);
            expect(read_u32_le(bytes, 52) == 0U);
            expect(read_u32_le(bytes, 68) == 0U);
            for (std::size_t offset{108}; offset < header_bytes; ++offset) {
                expect(bytes[offset] == std::byte{0});
            }

            const auto table_offset{read_u64_le(bytes, table_offset_field)};
            const auto table_bytes{read_u64_le(bytes, table_bytes_field)};
            const auto index_offset{read_u64_le(bytes, index_offset_field)};
            const auto index_bytes{read_u64_le(bytes, index_bytes_field)};
            const auto segment_dir_offset{
                read_u64_le(bytes, segment_dir_offset_field)};
            const auto segment_count{read_u32_le(bytes, segment_count_field)};
            const auto blob_dir_offset{
                read_u64_le(bytes, blob_dir_offset_field)};
            const auto blob_count{read_u32_le(bytes, blob_count_field)};

            // One segment (the duplicated raw blob and the empty file) and one
            // standalone packed blob.
            expect(segment_count == 1U);
            expect(blob_count == 1U);
            expect(segment_dir_offset == header_bytes);
            expect(blob_dir_offset % 8U == 0U);
            expect(table_offset % 8U == 0U);
            expect(index_offset % 8U == 0U);
            expect(table_bytes ==
                   refrain::data::blob_table_header_bytes + 3U * 80U);
            expect(index_bytes >= refrain::data::resolution_index_header_bytes);
            expect(table_offset + table_bytes <= bytes.size());
            expect(index_offset + index_bytes <= bytes.size());
            expect(table_offset + table_bytes <= index_offset);
            expect(index_offset + index_bytes <= bytes.size());

            // Segment/blob directory entries point at 8-byte aligned regions
            // inside the image.
            const auto segment_region{
                bytes.subspan(static_cast<std::size_t>(segment_dir_offset),
                              segment_count * segment_entry_bytes)};
            const auto segment_offset{read_u64_le(segment_region, 8)};
            const auto segment_size{read_u64_le(segment_region, 16)};
            expect(segment_offset % 64U == 0U);
            expect(segment_size >= 80U);
            expect(segment_offset + segment_size <= bytes.size());
            expect(std::to_integer<char>(
                       bytes[static_cast<std::size_t>(segment_offset)]) == 'R');
            expect(read_u16_le(bytes, static_cast<std::size_t>(segment_offset) +
                                          4U) == 1U);

            const auto blob_region{
                bytes.subspan(static_cast<std::size_t>(blob_dir_offset),
                              blob_count * blob_entry_bytes)};
            const auto blob_offset{read_u64_le(blob_region, 32)};
            const auto blob_size{read_u32_le(blob_region, 40)};
            expect(read_u32_le(blob_region, 44) == 0U);
            expect(blob_offset % 64U == 0U);
            expect(blob_size > 0U);
            expect(blob_offset + blob_size <= bytes.size());

            // Header checksums.
            expect(read_u32_le(bytes, header_crc_field) ==
                   crc32c(bytes.first(header_crc_field)));
            expect(std::equal(bytes.begin() + file_digest_field,
                              bytes.begin() + file_digest_field + 32,
                              blake3(bytes.subspan(header_bytes)).begin()));

            // The embedded RFBT/RFRX still parse as their own formats.
            auto table{parse_blob_table(
                bytes.subspan(static_cast<std::size_t>(table_offset),
                              static_cast<std::size_t>(table_bytes)))};
            expect(static_cast<bool>(table));
            if (!table) {
                return;
            }
            expect(table->entries.size() == 3U);
            auto index{parse_resolution_index(
                bytes.subspan(static_cast<std::size_t>(index_offset),
                              static_cast<std::size_t>(index_bytes)),
                *table)};
            expect(static_cast<bool>(index));
            if (index) {
                expect(index->entries.size() == 4U);
            }
        };

    "image/build is deterministic for the same and reordered input"_test = [] {
        auto files{make_inputs()};
        auto first{build(files)};
        auto second{build(files)};
        if (!first || !second) {
            return;
        }
        expect(same_bytes(*first, *second));
        auto reordered{files};
        std::reverse(reordered.begin(), reordered.end());
        auto third{build(reordered)};
        if (!third) {
            return;
        }
        expect(same_bytes(*third, *first));

        auto image{builtin_image::open(*first)};
        expect(static_cast<bool>(image));
        if (image) {
            expect(image->digest() ==
                   blake3(std::span<const std::byte>{*first}.subspan(
                       header_bytes)));
        }
    };

    "image/empty input is a valid empty image that vfs refuses to mount"_test =
        [] {
            const std::span<const builtin_image_input> none{};
            auto image_bytes{build(none)};
            if (!image_bytes) {
                return;
            }
            auto image{builtin_image::open(*image_bytes)};
            expect(static_cast<bool>(image));
            if (!image) {
                return;
            }
            expect(image->file_count() == 0U);
            expect(image->entries().empty());
            expect(image->find("") == nullptr);
            // Empty RFBT + empty RFRX are still embedded.
            expect(read_u64_le(*image_bytes, table_bytes_field) == 128U);
            expect(read_u64_le(*image_bytes, index_bytes_field) == 128U);
            expect(read_u32_le(*image_bytes, segment_count_field) == 0U);
            expect(read_u32_le(*image_bytes, blob_count_field) == 0U);

            auto pack{open_builtin_image_pack(*image_bytes, "")};
            expect(static_cast<bool>(pack));
            if (!pack) {
                return;
            }
            expect((*pack)->file_count() == 0U);
            vfs view;
            auto mounted{view.mount(std::move(*pack))};
            expect(!mounted);
            if (!mounted) {
                expect(mounted.error().code == error_code::invalid_argument);
            }
        };

    "image/duplicate and unsafe input paths are invalid_argument"_test = [] {
        std::vector<builtin_image_input> duplicate{
            builtin_image_input{.path = "data/a.txt", .data = bytes_of("x")},
            builtin_image_input{.path = "data/a.txt", .data = bytes_of("y")}};
        auto duplicated{build_builtin_image(duplicate)};
        expect(!duplicated);
        if (!duplicated) {
            expect(duplicated.error().code == error_code::invalid_argument);
        }

        const std::vector<std::string> unsafe_paths{
            "",     "/absolute", "../escape",           "a/../b",
            "a//b", "a\\b",      std::string(1025, 'a')};
        for (const auto &path : unsafe_paths) {
            std::vector<builtin_image_input> files{
                builtin_image_input{.path = path, .data = {}}};
            auto built{build_builtin_image(files)};
            expect(!built) << "expected reject: " << path;
            if (!built) {
                expect(built.error().code == error_code::invalid_argument)
                    << "wrong code: " << path;
            }
        }

        // Stray continuation byte: not valid UTF-8.
        std::vector<builtin_image_input> invalid_utf8{builtin_image_input{
            .path = std::string{"data/\xFF.bin"}, .data = {}}};
        auto built{build_builtin_image(invalid_utf8)};
        expect(!built);
        if (!built) {
            expect(built.error().code == error_code::invalid_argument);
        }
    };

    "image/truncated image and bad magic are parse_error"_test = [] {
        auto files{make_inputs()};
        auto image_bytes{build(files)};
        if (!image_bytes) {
            return;
        }
        auto truncated{*image_bytes};
        truncated.resize(64);
        expect_open_fails(truncated, error_code::parse_error, "truncated");

        auto bad_magic{*image_bytes};
        bad_magic[0] = static_cast<std::byte>('X');
        expect_open_fails(bad_magic, error_code::parse_error, "bad magic");

        auto bad_version{*image_bytes};
        bad_version[4] = static_cast<std::byte>(2);
        auto opened{builtin_image::open(bad_version)};
        expect(!opened);
        if (!opened) {
            expect(opened.error().code == error_code::version_mismatch);
        }
    };

    "image/header crc and file hash are checked"_test = [] {
        auto files{make_inputs()};
        auto image_bytes{build(files)};
        if (!image_bytes) {
            return;
        }
        auto bad_crc{*image_bytes};
        bad_crc[header_crc_field] ^= std::byte{0xFF};
        expect_open_fails(bad_crc, error_code::parse_error, "header crc");

        auto bad_body{*image_bytes};
        bad_body.back() ^= std::byte{0xFF};
        expect_open_fails(bad_body, error_code::parse_error, "file hash");
    };

    "image/rfrx prefix must match the embedded blob table"_test = [] {
        auto files{make_inputs()};
        auto image_bytes{build(files)};
        if (!image_bytes) {
            return;
        }
        auto tampered{*image_bytes};
        const auto index_offset{static_cast<std::size_t>(
            read_u64_le(tampered, index_offset_field))};
        // RFRX byte 84 is the first byte of the RFBT BLAKE3 prefix.
        tampered[index_offset + 84U] ^= std::byte{0xFF};
        refresh_image_checksums(tampered);
        expect_open_fails(tampered, error_code::parse_error, "rfrx prefix");
        auto opened{builtin_image::open(tampered)};
        if (!opened) {
            expect(opened.error().message.find("prefix") != std::string::npos);
        }
    };

    "image/in-segment records need a segment directory entry"_test = [] {
        auto files{make_inputs()};
        auto image_bytes{build(files)};
        if (!image_bytes) {
            return;
        }
        auto tampered{*image_bytes};
        // Drop the segment directory (the bytes stay in the image, but the
        // RFBT in-segment record can no longer be resolved).
        write_u32_le(tampered, segment_count_field, 0);
        refresh_image_checksums(tampered);
        expect_open_fails(tampered, error_code::parse_error, "missing segment");
        auto opened{builtin_image::open(tampered)};
        if (!opened) {
            expect(opened.error().message.find("segment directory") !=
                   std::string::npos);
        }
    };

    "image/standalone records need a blob directory entry"_test = [] {
        auto files{make_inputs()};
        auto image_bytes{build(files)};
        if (!image_bytes) {
            return;
        }
        auto tampered{*image_bytes};
        write_u32_le(tampered, blob_count_field, 0);
        refresh_image_checksums(tampered);
        expect_open_fails(tampered, error_code::parse_error, "missing blob");
        auto opened{builtin_image::open(tampered)};
        if (!opened) {
            expect(opened.error().message.find("blob directory") !=
                   std::string::npos);
        }
    };

    "image/pack borrows the image and mounts into a frozen vfs"_test = [] {
        auto files{make_inputs()};
        auto image_bytes{build(files)};
        if (!image_bytes) {
            return;
        }
        auto pack{open_builtin_image_pack(*image_bytes, "")};
        expect(static_cast<bool>(pack));
        if (!pack) {
            return;
        }
        expect((*pack)->format() == pack_format::memory);
        expect((*pack)->label() == "builtin.image");
        expect((*pack)->file_count() == files.size());
        auto custom{open_builtin_image_pack(*image_bytes, "builtin:test")};
        expect(static_cast<bool>(custom));
        if (custom) {
            expect((*custom)->label() == "builtin:test");
        }
        auto missing{(*pack)->read("data/nope.txt")};
        expect(!missing);
        if (!missing) {
            expect(missing.error().code == error_code::not_found);
        }

        vfs view;
        expect(view.mount(std::move(*pack)).has_value());
        view.freeze();
        expect(view.file_count() == files.size());
        expect(view.exists("data/demo/one.txt"));
        expect(view.find("data/demo/one.txt") != nullptr);
        auto text{view.read_text("data/demo/two.txt")};
        expect(static_cast<bool>(text));
        if (text) {
            expect(*text == "duplicate payload\n");
        }
        auto big{view.read("assets/demo/nested/data.bin")};
        expect(static_cast<bool>(big));
        if (big) {
            expect(same_bytes(
                *big, expected_bytes(files, "assets/demo/nested/data.bin")));
        }
        auto empty{view.read("assets/demo/empty.bin")};
        expect(static_cast<bool>(empty));
        if (empty) {
            expect(empty->empty());
        }
    };
};

} // namespace
