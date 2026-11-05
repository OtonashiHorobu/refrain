// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/io.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/content_segment.hpp>

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
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;
namespace fs = std::filesystem;

using refrain::data::blake3;
using refrain::data::content_segment_alignment;
using refrain::data::content_segment_directory;
using refrain::data::content_segment_extension;
using refrain::data::content_segment_header_bytes;
using refrain::data::crc32c;
using refrain::data::digest32;
using refrain::data::read_segment_header;
using refrain::data::segment_blob_span;
using refrain::data::segment_header;
using refrain::data::segment_path;
using refrain::data::segment_reader;
using refrain::data::segment_writer;
using refrain::data::verify_segment;

constexpr std::size_t header_bytes = content_segment_header_bytes;
constexpr std::uint32_t generous_target = 1U << 20;

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
                    ("refrain_segment_test_" + std::to_string(stamp) + "_" +
                     std::to_string(counter++))};
    std::error_code error;
    fs::create_directories(path, error);
    return path;
}

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

[[nodiscard]] auto segments_dir(const temp_directory &scratch) -> std::string {
    return scratch.path.string() + "/" + std::string{content_segment_directory};
}

[[nodiscard]] auto store_root(const temp_directory &scratch) -> std::string {
    return scratch.path.string();
}

// Independent little-endian accessors so the on-disk layout is pinned by the
// tests rather than by the implementation.
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

void write_u64_le(std::span<std::byte> bytes, std::size_t offset,
                  std::uint64_t value) {
    for (std::size_t index{0}; index < 8U; ++index) {
        bytes[offset + index] =
            static_cast<std::byte>((value >> (8U * index)) & 0xFFU);
    }
}

/// Recompute the header CRC after a deliberate mutation so a test reaches the
/// intended deeper validation.
void refresh_crc(std::vector<std::byte> &file) {
    write_u32_le(file, 64, crc32c(std::span<const std::byte>{file}.first(64)));
}

/// 16 lowercase hex digits of the numeric segment id.
[[nodiscard]] auto id_hex(std::uint64_t id) -> std::string {
    constexpr char digits[]{"0123456789abcdef"};
    std::string text(16, '0');
    for (std::size_t index{0}; index < 16U; ++index) {
        const auto shift{4U * (16U - 1U - index)};
        text[index] = digits[(id >> shift) & 0xFU];
    }
    return text;
}

[[nodiscard]] auto body_id(std::span<const std::byte> body) -> std::uint64_t {
    const auto hash{blake3(body)};
    std::uint64_t id{0};
    for (std::size_t index{0}; index < 8U; ++index) {
        id |= static_cast<std::uint64_t>(
                  std::to_integer<unsigned int>(hash[index]))
              << (8U * index);
    }
    return id;
}

[[nodiscard]] auto same_digest(const digest32 &lhs, const digest32 &rhs)
    -> bool {
    return std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
}

/// Write `payloads` through a fresh writer and seal it.
[[nodiscard]] auto
seal_payloads(std::string_view directory,
              std::span<const std::vector<std::byte>> payloads)
    -> std::optional<segment_header> {
    auto writer{segment_writer::create(directory, generous_target)};
    if (!writer) {
        return std::nullopt;
    }
    for (const auto &payload : payloads) {
        if (!writer->append(payload)) {
            return std::nullopt;
        }
    }
    auto header{writer->seal()};
    if (!header) {
        return std::nullopt;
    }
    return *header;
}

[[nodiscard]] auto read_file(std::string_view path) -> std::vector<std::byte> {
    return refrain::core::io::read_file(path).value_or(
        std::vector<std::byte>{});
}

/// A valid sealed segment used as the base for corruption tests.
struct sealed_fixture {
    std::string directory{};
    segment_header header{};
    std::string path{};
    std::vector<std::byte> bytes{};
};

[[nodiscard]] auto make_fixture(std::string_view directory)
    -> std::optional<sealed_fixture> {
    const std::vector<std::vector<std::byte>> payloads{
        bytes_of("alpha"), bytes_of("bravo"), {}};
    auto header{seal_payloads(directory, payloads)};
    if (!header) {
        return std::nullopt;
    }
    sealed_fixture fixture;
    fixture.directory = std::string{directory};
    fixture.header = *header;
    fixture.path = std::string{directory} + "/" + id_hex(header->id) +
                   std::string{content_segment_extension};
    fixture.bytes = read_file(fixture.path);
    if (fixture.bytes.empty()) {
        return std::nullopt;
    }
    return fixture;
}

const suite content_segment_tests = [] {
    "segment/header layout and body are pinned"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto directory{segments_dir(scratch)};
        const std::vector<std::vector<std::byte>> payloads{
            bytes_of("alpha"), bytes_of("bravo"), {}};
        auto sealed{seal_payloads(directory, payloads)};
        expect(sealed.has_value());
        if (!sealed) {
            return;
        }
        // alpha at 0, bravo at 8, empty at 16 -> body 16 bytes.
        expect(sealed->blob_count == 3U);
        expect(sealed->data_bytes == 16U);
        expect(sealed->id != 0U);

        const auto path{segment_path(store_root(scratch), sealed->id)};
        expect(path == directory + "/" + id_hex(sealed->id) +
                           std::string{content_segment_extension});
        expect(refrain::core::io::is_regular_file(path));
        const auto file{read_file(path)};
        expect(file.size() == header_bytes + 16U);
        if (file.size() != header_bytes + 16U) {
            return;
        }
        expect(text_of(std::span<const std::byte>{file}.first(4)) == "RFSG");
        expect(read_u16_le(file, 4) == 1U);
        expect(read_u16_le(file, 6) == 0U);
        expect(read_u64_le(file, 8) == sealed->id);
        expect(read_u32_le(file, 16) == 3U);
        expect(read_u32_le(file, 20) == 0U);
        expect(read_u64_le(file, 24) == 16U);
        digest32 stored_hash{};
        std::copy_n(file.begin() + 32, stored_hash.size(), stored_hash.begin());
        expect(same_digest(
            stored_hash,
            blake3(std::span<const std::byte>{file}.subspan(header_bytes))));
        expect(read_u32_le(file, 64) ==
               crc32c(std::span<const std::byte>{file}.first(64)));
        for (std::size_t offset = 68; offset < header_bytes; ++offset) {
            expect(std::to_integer<unsigned int>(file[offset]) == 0U);
        }
        // Body: alpha, three zero pad bytes, bravo, then the empty blob's
        // aligned offset (body end).
        expect(text_of(std::span<const std::byte>{file}.subspan(80, 5)) ==
               "alpha");
        for (std::size_t offset = 85; offset < 88; ++offset) {
            expect(std::to_integer<unsigned int>(file[offset]) == 0U);
        }
        expect(text_of(std::span<const std::byte>{file}.subspan(88, 5)) ==
               "bravo");

        auto parsed{read_segment_header(file)};
        expect(parsed.has_value());
        if (parsed) {
            expect(parsed->id == sealed->id);
            expect(parsed->blob_count == sealed->blob_count);
            expect(parsed->data_bytes == sealed->data_bytes);
            expect(same_digest(parsed->body_hash, sealed->body_hash));
        }
        expect(same_digest(
            sealed->body_hash,
            blake3(std::span<const std::byte>{file}.subspan(header_bytes))));
        expect(sealed->id ==
               body_id(std::span<const std::byte>{file}.subspan(header_bytes)));
        expect(static_cast<bool>(verify_segment(file)));

        const std::array<segment_blob_span, 3> spans{
            segment_blob_span{.offset = 0, .size = 5},
            segment_blob_span{.offset = 8, .size = 5},
            segment_blob_span{.offset = 16, .size = 0},
        };
        expect(static_cast<bool>(verify_segment(file, spans)));

        auto reader{segment_reader::open(path)};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        expect(reader->header().id == sealed->id);
        expect(reader->header().blob_count == 3U);
        expect(reader->path() == path);
        auto alpha{reader->read_at(0, 5)};
        expect(static_cast<bool>(alpha));
        if (alpha) {
            expect(text_of(*alpha) == "alpha");
        }
        auto bravo{reader->read_at(8, 5)};
        expect(static_cast<bool>(bravo));
        if (bravo) {
            expect(text_of(*bravo) == "bravo");
        }
        auto empty{reader->read_at(16, 0)};
        expect(static_cast<bool>(empty));
        if (empty) {
            expect(empty->empty());
        }
        auto padding{reader->read_at(5, 3)};
        expect(static_cast<bool>(padding));
        if (padding) {
            expect(std::all_of(
                padding->begin(), padding->end(),
                [](std::byte value) { return value == std::byte{0}; }));
        }
        expect(!reader->read_at(17, 1));
        if (!reader->read_at(17, 1)) {
            expect(reader->read_at(17, 1).error().code ==
                   refrain::error_code::invalid_argument);
        }
        expect(!reader->read_at(16, 1));
        expect(static_cast<bool>(reader->verify(spans)));
    };

    "segment/id is derived from the body and re-sealing is idempotent"_test =
        [] {
            temp_directory scratch{make_temp_directory()};
            const auto directory{segments_dir(scratch)};
            const std::vector<std::vector<std::byte>> payloads{
                bytes_of("first"), bytes_of("second")};
            auto first{seal_payloads(directory, payloads)};
            expect(first.has_value());
            if (!first) {
                return;
            }
            const auto first_path{segment_path(store_root(scratch), first->id)};
            const auto first_bytes{read_file(first_path)};
            const std::vector<std::vector<std::byte>> other{bytes_of("third"),
                                                            bytes_of("fourth")};
            auto second{seal_payloads(directory, other)};
            expect(second.has_value());
            if (!second) {
                return;
            }
            expect(second->id != first->id);
            expect(refrain::core::io::is_regular_file(
                segment_path(store_root(scratch), second->id)));

            // Sealing the same body again reuses the published file.
            auto again{seal_payloads(directory, payloads)};
            expect(again.has_value());
            if (again) {
                expect(again->id == first->id);
                expect(same_digest(again->body_hash, first->body_hash));
                expect(read_file(first_path) == first_bytes);
            }
            std::size_t sealed_count{0};
            const auto names{refrain::core::io::list_directory(directory)};
            expect(names.has_value());
            if (names) {
                for (const auto &name : *names) {
                    expect(!name.ends_with(".part"));
                    if (name.ends_with(".rfseg")) {
                        ++sealed_count;
                    }
                }
            }
            expect(sealed_count == 2U);
        };

    "segment/empty segment is not published"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto directory{segments_dir(scratch)};
        expect(!segment_writer::create("", generous_target));
        expect(!segment_writer::create(directory, 0U));

        auto writer{segment_writer::create(directory, generous_target)};
        expect(static_cast<bool>(writer));
        if (!writer) {
            return;
        }
        expect(writer->valid());
        expect(writer->blob_count() == 0U);
        expect(writer->data_bytes() == 0U);
        const auto part_path{std::string{writer->path()}};
        expect(part_path.ends_with(".part"));
        expect(refrain::core::io::is_regular_file(part_path));
        const auto sealed{writer->seal()};
        expect(!sealed);
        if (!sealed) {
            expect(sealed.error().code ==
                   refrain::error_code::invalid_argument);
        }
        expect(!writer->valid());
        expect(!refrain::core::io::exists(part_path));
        const auto names{refrain::core::io::list_directory(directory)};
        expect(names.has_value());
        if (names) {
            expect(names->empty());
        }
    };

    "segment/abort removes the staging part"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto directory{segments_dir(scratch)};
        {
            auto writer{segment_writer::create(directory, generous_target)};
            expect(static_cast<bool>(writer));
            if (!writer) {
                return;
            }
            expect(static_cast<bool>(writer->append(bytes_of("aborted"))));
            const auto names{refrain::core::io::list_directory(directory)};
            expect(names.has_value());
            if (names) {
                expect(names->size() == 1U);
                if (names->size() == 1U) {
                    expect((*names)[0].ends_with(".part"));
                }
            }
        }
        const auto after{refrain::core::io::list_directory(directory)};
        expect(after.has_value());
        if (after) {
            expect(after->empty());
        }
    };

    "segment/malformed headers are rejected"_test = [] {
        temp_directory scratch{make_temp_directory()};
        auto fixture{make_fixture(segments_dir(scratch))};
        expect(fixture.has_value());
        if (!fixture) {
            return;
        }
        const auto &valid{fixture->bytes};
        const auto expect_parse_error = [](std::span<const std::byte> bytes) {
            auto parsed{read_segment_header(bytes)};
            expect(!parsed);
            if (!parsed) {
                expect(parsed.error().code == refrain::error_code::parse_error);
            }
        };

        // Truncated before the header and a missing body byte.
        expect_parse_error(std::span<const std::byte>{valid}.first(40));
        auto truncated{valid};
        truncated.pop_back();
        expect_parse_error(truncated);

        auto bad_magic{valid};
        bad_magic[0] = static_cast<std::byte>('X');
        expect_parse_error(bad_magic);

        auto bad_version{valid};
        bad_version[4] = std::byte{2};
        refresh_crc(bad_version);
        {
            auto parsed{read_segment_header(bad_version)};
            expect(!parsed);
            if (!parsed) {
                expect(parsed.error().code ==
                       refrain::error_code::version_mismatch);
            }
        }

        auto bad_flags{valid};
        bad_flags[6] = std::byte{1};
        refresh_crc(bad_flags);
        expect_parse_error(bad_flags);

        auto bad_crc{valid};
        bad_crc[64] = static_cast<std::byte>(
            std::to_integer<unsigned int>(bad_crc[64]) ^ 0x01U);
        expect_parse_error(bad_crc);

        auto bad_data_bytes{valid};
        write_u64_le(bad_data_bytes, 24, 15U);
        refresh_crc(bad_data_bytes);
        expect_parse_error(bad_data_bytes);

        auto bad_reserved{valid};
        bad_reserved[20] = std::byte{1};
        refresh_crc(bad_reserved);
        expect_parse_error(bad_reserved);

        auto bad_tail{valid};
        bad_tail[68] = std::byte{1};
        refresh_crc(bad_tail);
        expect_parse_error(bad_tail);

        auto bad_id{valid};
        write_u64_le(bad_id, 8, fixture->header.id ^ 1U);
        refresh_crc(bad_id);
        expect_parse_error(bad_id);

        auto empty_segment{valid};
        write_u32_le(empty_segment, 16, 0U);
        refresh_crc(empty_segment);
        expect_parse_error(empty_segment);

        // The reader applies the same header checks on open.
        auto bad_path{fixture->path + ".copy"};
        expect(refrain::core::io::write_file(bad_path, bad_magic).has_value());
        auto reader{segment_reader::open(bad_path)};
        expect(!reader);
        if (!reader) {
            expect(reader.error().code == refrain::error_code::parse_error);
        }
    };

    "segment/body hash and blob spans are verified"_test = [] {
        temp_directory scratch{make_temp_directory()};
        auto fixture{make_fixture(segments_dir(scratch))};
        expect(fixture.has_value());
        if (!fixture) {
            return;
        }
        const std::array<segment_blob_span, 3> spans{
            segment_blob_span{.offset = 0, .size = 5},
            segment_blob_span{.offset = 8, .size = 5},
            segment_blob_span{.offset = 16, .size = 0},
        };
        expect(static_cast<bool>(verify_segment(fixture->bytes, spans)));

        auto damaged{fixture->bytes};
        damaged[80] = static_cast<std::byte>(
            std::to_integer<unsigned int>(damaged[80]) ^ 0x01U);
        // The header stays valid; only the body hash can see this.
        expect(read_segment_header(damaged).has_value());
        auto verified{verify_segment(damaged, spans)};
        expect(!verified);
        if (!verified) {
            expect(verified.error().code == refrain::error_code::parse_error);
            expect(verified.error().message.find("BLAKE3") !=
                   std::string::npos);
        }

        const auto expect_span_error =
            [&fixture](std::span<const segment_blob_span> bad_spans) {
                auto valid{verify_segment(fixture->bytes, bad_spans)};
                expect(!valid);
                if (!valid) {
                    expect(valid.error().code ==
                           refrain::error_code::parse_error);
                }
            };
        const std::array<segment_blob_span, 1> unaligned{
            segment_blob_span{.offset = 1, .size = 1}};
        expect_span_error(unaligned);
        const std::array<segment_blob_span, 1> past_end{
            segment_blob_span{.offset = 16, .size = 1}};
        expect_span_error(past_end);
        const std::array<segment_blob_span, 1> overflow{
            segment_blob_span{.offset = 17, .size = 0}};
        expect_span_error(overflow);
        const std::array<segment_blob_span, 2> overlap{
            segment_blob_span{.offset = 0, .size = 5},
            segment_blob_span{.offset = 4, .size = 5}};
        expect_span_error(overlap);
        const std::array<segment_blob_span, 1> wrong_count{
            segment_blob_span{.offset = 0, .size = 5}};
        expect_span_error(wrong_count);

        auto reader{segment_reader::open(fixture->path)};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        expect(static_cast<bool>(reader->verify(spans)));
        expect(!reader->verify(unaligned));
        // Damage the published file and re-verify through the reader.
        expect(
            refrain::core::io::write_file(fixture->path, damaged).has_value());
        expect(!reader->verify(spans));
    };

    "segment/re-sealing validates an existing file and rejects collisions"_test =
        [] {
            temp_directory scratch{make_temp_directory()};
            const auto directory{segments_dir(scratch)};
            const std::vector<std::vector<std::byte>> two_blobs{
                bytes_of("alpha"), bytes_of("bravo")};
            auto first{seal_payloads(directory, two_blobs)};
            expect(first.has_value());
            if (!first) {
                return;
            }
            const auto path{segment_path(store_root(scratch), first->id)};

            // Same body bytes, different blob boundaries: the id is identical
            // but the header (blob_count) differs, which must fail closed.
            // alpha(5) + 3 alignment bytes + bravo(5) is one 13-byte blob.
            std::vector<std::byte> merged{bytes_of("alpha")};
            merged.resize(8U);
            const auto bravo{bytes_of("bravo")};
            merged.insert(merged.end(), bravo.begin(), bravo.end());
            auto writer{segment_writer::create(directory, generous_target)};
            expect(static_cast<bool>(writer));
            if (!writer) {
                return;
            }
            const auto part_path{std::string{writer->path()}};
            expect(static_cast<bool>(writer->append(merged)));
            auto collision{writer->seal()};
            expect(!collision);
            if (!collision) {
                expect(collision.error().code ==
                       refrain::error_code::parse_error);
                expect(collision.error().message.find("collision") !=
                       std::string::npos);
            }
            expect(!refrain::core::io::exists(part_path));
            expect(refrain::core::io::is_regular_file(path));
            auto header{read_segment_header(read_file(path))};
            expect(header.has_value());
            if (header) {
                expect(header->blob_count == 2U);
                expect(header->id == first->id);
            }
        };

    "segment/append offsets stay aligned for uneven sizes"_test = [] {
        temp_directory scratch{make_temp_directory()};
        const auto directory{segments_dir(scratch)};
        const std::vector<std::vector<std::byte>> payloads{
            binary_blob(1), binary_blob(7), binary_blob(9), binary_blob(8)};
        auto writer{segment_writer::create(directory, generous_target)};
        expect(static_cast<bool>(writer));
        if (!writer) {
            return;
        }
        const std::array<std::uint32_t, 4> expected{0, 8, 16, 32};
        std::vector<std::uint32_t> offsets;
        for (const auto &payload : payloads) {
            auto offset{writer->append(payload)};
            expect(static_cast<bool>(offset));
            if (!offset) {
                return;
            }
            offsets.push_back(*offset);
        }
        expect(offsets.size() == expected.size());
        if (offsets.size() != expected.size()) {
            return;
        }
        for (std::size_t index{0}; index < expected.size(); ++index) {
            expect(offsets[index] == expected[index]);
            expect(offsets[index] % content_segment_alignment == 0U);
        }
        auto header{writer->seal()};
        expect(header.has_value());
        if (!header) {
            return;
        }
        expect(header->blob_count == payloads.size());
        expect(header->data_bytes == 40U);
        expect(!writer->valid());
        const auto appended_after_seal{writer->append(binary_blob(1))};
        expect(!appended_after_seal);
        if (!appended_after_seal) {
            expect(appended_after_seal.error().code ==
                   refrain::error_code::invalid_argument);
        }
        const auto file{
            read_file(segment_path(store_root(scratch), header->id))};
        expect(static_cast<bool>(verify_segment(file)));
        auto reader{segment_reader::open(
            segment_path(store_root(scratch), header->id))};
        expect(static_cast<bool>(reader));
        if (!reader) {
            return;
        }
        for (std::size_t index{0}; index < payloads.size(); ++index) {
            auto stored{
                reader->read_at(offsets[index], payloads[index].size())};
            expect(static_cast<bool>(stored));
            if (stored) {
                expect(*stored == payloads[index]);
            }
        }
    };

    "segment/reader and writer defaults are inert"_test = [] {
        segment_writer writer;
        expect(!writer.valid());
        expect(!writer.append(bytes_of("x")));
        if (!writer.append(bytes_of("x"))) {
            expect(writer.append(bytes_of("x")).error().code ==
                   refrain::error_code::not_initialized);
        }
        expect(!writer.seal());
        expect(writer.path().empty());

        const segment_reader reader;
        expect(!reader.valid());
        expect(!reader.read_at(0U, 1U));
        if (!reader.read_at(0U, 1U)) {
            expect(reader.read_at(0U, 1U).error().code ==
                   refrain::error_code::not_initialized);
        }
        expect(!reader.verify());
        expect(reader.path().empty());

        expect(!segment_reader::open(""));
        if (!segment_reader::open("")) {
            expect(segment_reader::open("").error().code ==
                   refrain::error_code::invalid_argument);
        }
        expect(!segment_reader::open("/nonexistent/refrain/segment.rfseg"));
        if (!segment_reader::open("/nonexistent/refrain/segment.rfseg")) {
            expect(segment_reader::open("/nonexistent/refrain/segment.rfseg")
                       .error()
                       .code == refrain::error_code::io_error);
        }
        // Paths of different store roots stay relative to the given root.
        expect(segment_path("", 1U) ==
               std::string{content_segment_directory} + "/" + id_hex(1U) +
                   std::string{content_segment_extension});
        expect(segment_path("/store///", 1U) ==
               "/store/" + std::string{content_segment_directory} + "/" +
                   id_hex(1U) + std::string{content_segment_extension});
    };
};

} // namespace
