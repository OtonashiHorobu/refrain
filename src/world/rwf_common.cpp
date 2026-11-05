// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include "rwf_internal.hpp"

#include <refrain/data/checksum.hpp>

#include <fmt/format.h>

#include <zstd.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

namespace refrain::world::rwf::detail {
namespace {

constexpr std::array<std::byte, 4> region_magic{std::byte{'R'}, std::byte{'F'},
                                                std::byte{'W'}, std::byte{'R'}};
constexpr std::array<std::byte, 4> index_magic{std::byte{'R'}, std::byte{'F'},
                                               std::byte{'I'}, std::byte{'X'}};
constexpr std::array<std::byte, 4> record_magic{std::byte{'R'}, std::byte{'F'},
                                                std::byte{'C'}, std::byte{'H'}};
constexpr std::array<std::byte, 4> meta_magic{std::byte{'R'}, std::byte{'F'},
                                              std::byte{'M'}, std::byte{'F'}};

constexpr std::size_t file_header_crc_offset = 36;
constexpr std::size_t file_header_tail_offset = 40;
constexpr std::size_t file_header_count_offset = 48;
constexpr std::size_t file_header_generation_offset = 24;
constexpr std::size_t file_header_active_offset = 32;
constexpr std::size_t index_crc_offset = 20;

constexpr std::size_t slot_header_bytes = 4096;
constexpr std::size_t slot_entry_bytes = 32;

[[nodiscard]] auto magic_at(std::span<const std::byte> bytes,
                            std::size_t offset) noexcept
    -> std::array<std::byte, 4> {
    std::array<std::byte, 4> out{};
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), 4,
                out.begin());
    return out;
}

void set_magic(std::span<std::byte> bytes, std::size_t offset,
               const std::array<std::byte, 4> &magic) noexcept {
    std::copy(magic.begin(), magic.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

[[nodiscard]] auto invalid(std::string what) -> std::unexpected<error> {
    return std::unexpected{
        make_error(error_code::parse_error, std::move(what))};
}

} // namespace

auto frame_crc32c(std::span<const std::byte> frame) noexcept -> std::uint32_t {
    if (frame.size() < record_header_bytes) {
        return 0;
    }
    // Bytes 24..27 hold the CRC itself and are excluded (equivalent to zeroing
    // the field before hashing).
    auto state{data::crc32c(frame.first(24))};
    return data::crc32c_update(state, frame.subspan(28));
}

auto encode_file_header(const file_header &header) -> std::vector<std::byte> {
    std::vector<std::byte> bytes(4096, std::byte{0});
    set_magic(bytes, 0, region_magic);
    put_u16(bytes, 4, container_version);
    put_u16(bytes, 6, 0); // flags
    put_i32(bytes, 8, header.x);
    put_i32(bytes, 12, header.z);
    put_u32(bytes, 16, sector_size);
    put_u32(bytes, 20, index_slots);
    put_u64(bytes, file_header_generation_offset, header.generation);
    put_u32(bytes, file_header_active_offset, header.active_slot);
    put_u64(bytes, file_header_tail_offset, header.data_tail);
    put_u64(bytes, file_header_count_offset, header.record_count);
    put_u64(bytes, 56, header.dictionary_id);
    const auto crc{data::crc32c(bytes)};
    put_u32(bytes, file_header_crc_offset, crc);
    return bytes;
}

auto decode_file_header(std::span<const std::byte> bytes)
    -> result<file_header> {
    if (bytes.size() != 4096) {
        return invalid("region header is truncated");
    }
    if (magic_at(bytes, 0) != region_magic) {
        return invalid("region header magic mismatch");
    }
    const auto stored_crc{get_u32(bytes, file_header_crc_offset)};
    std::vector<std::byte> copy{bytes.begin(), bytes.end()};
    put_u32(copy, file_header_crc_offset, 0);
    if (data::crc32c(copy) != stored_crc) {
        return invalid("region header checksum mismatch");
    }
    if (get_u16(bytes, 4) != container_version) {
        return std::unexpected{make_error(
            error_code::version_mismatch,
            fmt::format("region container version {} is not supported "
                        "(expected {})",
                        get_u16(bytes, 4), container_version))};
    }
    if (get_u32(bytes, 16) != sector_size ||
        get_u32(bytes, 20) != index_slots) {
        return invalid("region geometry does not match RWF v1");
    }
    file_header header{};
    header.x = get_i32(bytes, 8);
    header.z = get_i32(bytes, 12);
    header.generation = get_u64(bytes, file_header_generation_offset);
    header.active_slot = get_u32(bytes, file_header_active_offset);
    header.data_tail = get_u64(bytes, file_header_tail_offset);
    header.record_count = get_u64(bytes, file_header_count_offset);
    header.dictionary_id = get_u64(bytes, 56);
    if (header.active_slot > 1) {
        return invalid(fmt::format("region header has invalid active slot {}",
                                   header.active_slot));
    }
    if (header.data_tail < data_offset || header.data_tail % 8 != 0) {
        return invalid(fmt::format("region header has invalid data tail {}",
                                   header.data_tail));
    }
    return header;
}

auto encode_index_slot(const index_slot &slot) -> std::vector<std::byte> {
    std::vector<std::byte> bytes(index_slot_bytes, std::byte{0});
    set_magic(bytes, 0, index_magic);
    put_u16(bytes, 4, container_version);
    put_u16(bytes, 6, 0);
    put_u64(bytes, 8, slot.generation);
    std::size_t count{0};
    for (std::size_t index{0}; index < index_slots; ++index) {
        const auto &entry{slot.entries[index]};
        if (!entry.empty()) {
            ++count;
        }
        const auto base{slot_header_bytes + index * slot_entry_bytes};
        put_u64(bytes, base + 0, entry.offset);
        put_u32(bytes, base + 8, entry.length);
        put_u32(bytes, base + 12, entry.crc32c);
        put_u32(bytes, base + 16, entry.uncompressed);
        put_u8(bytes, base + 20, static_cast<std::uint8_t>(entry.codec));
        put_u8(bytes, base + 21, entry.flags);
        put_u16(bytes, base + 22, 0);
        put_u64(bytes, base + 24, entry.generation);
    }
    put_u32(bytes, 16, static_cast<std::uint32_t>(count));
    // Cover the whole slot, including `generation` at offset 8: a bit flip
    // there used to make the loader silently prefer the older slot.
    put_u32(bytes, index_crc_offset, 0);
    auto state{data::crc32c(
        std::span<const std::byte>{bytes}.first(index_crc_offset))};
    state = data::crc32c_update(
        state, std::span<const std::byte>{bytes}.subspan(index_crc_offset + 4));
    put_u32(bytes, index_crc_offset, state);
    return bytes;
}

auto decode_index_slot(std::span<const std::byte> bytes) -> result<index_slot> {
    if (bytes.size() != index_slot_bytes) {
        return invalid("region index slot is truncated");
    }
    if (magic_at(bytes, 0) != index_magic) {
        return invalid("region index magic mismatch");
    }
    auto state{data::crc32c(bytes.first(index_crc_offset))};
    state = data::crc32c_update(state, bytes.subspan(index_crc_offset + 4));
    if (state != get_u32(bytes, index_crc_offset)) {
        return invalid("region index checksum mismatch");
    }
    if (get_u16(bytes, 4) != container_version) {
        return std::unexpected{
            make_error(error_code::version_mismatch,
                       fmt::format("region index version {} is not supported",
                                   get_u16(bytes, 4)))};
    }
    index_slot slot{};
    slot.generation = get_u64(bytes, 8);
    const auto count{get_u32(bytes, 16)};
    if (count > index_slots) {
        return invalid(fmt::format("region index claims {} entries", count));
    }
    std::uint32_t seen{0};
    for (std::size_t index{0}; index < index_slots; ++index) {
        const auto base{slot_header_bytes + index * slot_entry_bytes};
        auto &entry{slot.entries[index]};
        entry.offset = get_u64(bytes, base + 0);
        entry.length = get_u32(bytes, base + 8);
        entry.crc32c = get_u32(bytes, base + 12);
        entry.uncompressed = get_u32(bytes, base + 16);
        entry.codec = static_cast<compression>(get_u8(bytes, base + 20));
        entry.flags = get_u8(bytes, base + 21);
        entry.generation = get_u64(bytes, base + 24);
        if (static_cast<std::uint8_t>(entry.codec) >
            static_cast<std::uint8_t>(compression::zstd)) {
            return invalid(fmt::format("index slot {} has unknown compression "
                                       "{}",
                                       index,
                                       static_cast<unsigned>(entry.codec)));
        }
        if (!entry.empty()) {
            ++seen;
        }
    }
    if (seen != count) {
        return invalid(fmt::format("region index header claims {} entries but "
                                   "{} are present",
                                   count, seen));
    }
    return slot;
}

auto compress(compression codec, int zstd_level,
              std::span<const std::byte> input,
              const zstd_dictionary *dictionary)
    -> result<std::vector<std::byte>> {
    if (input.size() > max_record_bytes) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            fmt::format("payload of {} bytes exceeds the {} byte limit",
                        input.size(), max_record_bytes))};
    }
    if (codec == compression::none) {
        return std::vector<std::byte>{input.begin(), input.end()};
    }
    const auto bound{ZSTD_compressBound(input.size())};
    std::vector<std::byte> out(bound);
    std::size_t written{0};
    if (dictionary != nullptr) {
        auto *context{ZSTD_createCCtx()};
        if (context == nullptr) {
            return std::unexpected{
                make_error(error_code::io_error,
                           "cannot create a zstd compression context")};
        }
        written = ZSTD_compress_usingDict(
            context, out.data(), out.size(), input.data(), input.size(),
            dictionary->bytes.data(), dictionary->bytes.size(), zstd_level);
        ZSTD_freeCCtx(context);
    } else {
        written = ZSTD_compress(out.data(), out.size(), input.data(),
                                input.size(), zstd_level);
    }
    if (ZSTD_isError(written)) {
        return std::unexpected{make_error(
            error_code::io_error, fmt::format("zstd compression failed: {}",
                                              ZSTD_getErrorName(written)))};
    }
    out.resize(written);
    return out;
}

auto decompress(compression codec, std::span<const std::byte> input,
                std::size_t expected_size, const zstd_dictionary *dictionary)
    -> result<std::vector<std::byte>> {
    if (expected_size > max_record_bytes) {
        return std::unexpected{make_error(
            error_code::parse_error,
            fmt::format("declared payload size {} exceeds the {} byte limit",
                        expected_size, max_record_bytes))};
    }
    if (codec == compression::none) {
        if (input.size() != expected_size) {
            return invalid(fmt::format("stored payload has {} bytes, expected "
                                       "{}",
                                       input.size(), expected_size));
        }
        return std::vector<std::byte>{input.begin(), input.end()};
    }
    std::vector<std::byte> out(expected_size);
    std::size_t written{0};
    if (dictionary != nullptr) {
        auto *context{ZSTD_createDCtx()};
        if (context == nullptr) {
            return invalid("cannot create a zstd decompression context");
        }
        written = ZSTD_decompress_usingDict(
            context, out.data(), out.size(), input.data(), input.size(),
            dictionary->bytes.data(), dictionary->bytes.size());
        ZSTD_freeDCtx(context);
    } else {
        written =
            ZSTD_decompress(out.data(), out.size(), input.data(), input.size());
    }
    if (ZSTD_isError(written)) {
        return invalid(fmt::format("zstd decompression failed: {}",
                                   ZSTD_getErrorName(written)));
    }
    if (written != expected_size) {
        return invalid(fmt::format("zstd produced {} bytes, expected {}",
                                   written, expected_size));
    }
    return out;
}

auto build_frame(chunk_pos pos, chunk_status status, compression codec,
                 int zstd_level, std::span<const std::byte> payload,
                 const zstd_dictionary *dictionary)
    -> result<std::vector<std::byte>> {
    if (payload.size() > max_record_bytes) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            fmt::format("chunk payload of {} bytes exceeds the {} byte limit",
                        payload.size(), max_record_bytes))};
    }
    auto stored{compress(codec, zstd_level, payload, dictionary)};
    if (!stored) {
        return std::unexpected{stored.error()};
    }
    auto used{codec};
    if (used == compression::zstd && stored->size() >= payload.size()) {
        used = compression::none;
        stored->assign(payload.begin(), payload.end());
    }
    std::vector<std::byte> frame(record_header_bytes + stored->size(),
                                 std::byte{0});
    set_magic(frame, 0, record_magic);
    put_u16(frame, 4, container_version);
    put_u16(frame, 6, 0);
    put_i32(frame, 8, pos.x);
    put_i32(frame, 12, pos.z);
    put_u8(frame, 16, static_cast<std::uint8_t>(status));
    put_u8(frame, 17, static_cast<std::uint8_t>(used));
    put_u16(frame, 18, 0);
    put_u32(frame, 20, static_cast<std::uint32_t>(stored->size()));
    put_u32(frame, 28, static_cast<std::uint32_t>(payload.size()));
    std::copy(stored->begin(), stored->end(),
              frame.begin() + static_cast<std::ptrdiff_t>(record_header_bytes));
    put_u32(frame, 24, frame_crc32c(frame));
    return frame;
}

auto decode_frame_header(std::span<const std::byte> frame)
    -> result<frame_header> {
    if (frame.size() < record_header_bytes) {
        return invalid("record frame is truncated");
    }
    if (magic_at(frame, 0) != record_magic) {
        return invalid("record frame magic mismatch");
    }
    if (get_u16(frame, 4) != container_version) {
        return std::unexpected{
            make_error(error_code::version_mismatch,
                       fmt::format("record version {} is not supported",
                                   get_u16(frame, 4)))};
    }
    frame_header header{};
    header.pos.x = get_i32(frame, 8);
    header.pos.z = get_i32(frame, 12);
    header.status = static_cast<chunk_status>(get_u8(frame, 16));
    header.codec = static_cast<compression>(get_u8(frame, 17));
    header.stored_size = get_u32(frame, 20);
    header.record_crc32c = get_u32(frame, 24);
    header.uncompressed_size = get_u32(frame, 28);
    if (static_cast<std::uint8_t>(header.codec) >
        static_cast<std::uint8_t>(compression::zstd)) {
        return invalid(fmt::format("record has unknown compression {}",
                                   static_cast<unsigned>(header.codec)));
    }
    if (header.stored_size > max_record_bytes) {
        return invalid(fmt::format("record stores {} bytes (limit {})",
                                   header.stored_size, max_record_bytes));
    }
    if (header.uncompressed_size > max_record_bytes) {
        return invalid(
            fmt::format("record declares {} payload bytes (limit {})",
                        header.uncompressed_size, max_record_bytes));
    }
    if (frame.size() != record_header_bytes + header.stored_size) {
        return invalid(fmt::format("record frame has {} bytes for a {} byte "
                                   "payload",
                                   frame.size(), header.stored_size));
    }
    return header;
}

auto decode_frame(std::span<const std::byte> frame,
                  const zstd_dictionary *dictionary)
    -> result<std::vector<std::byte>> {
    const auto header{decode_frame_header(frame)};
    if (!header) {
        return std::unexpected{header.error()};
    }
    const auto stored{frame.subspan(record_header_bytes, header->stored_size)};
    if (frame_crc32c(frame) != header->record_crc32c) {
        return invalid("record frame checksum mismatch");
    }
    return decompress(header->codec, stored, header->uncompressed_size,
                      dictionary);
}

auto encode_record_file_header(std::uint32_t stored_size,
                               std::uint32_t uncompressed_size,
                               compression codec) -> std::vector<std::byte> {
    std::vector<std::byte> bytes(record_file_header_bytes, std::byte{0});
    set_magic(bytes, 0, meta_magic);
    put_u16(bytes, 4, container_version);
    put_u16(bytes, 6, 0);
    put_u32(bytes, 8, stored_size);
    put_u32(bytes, 12, 0); // payload CRC filled by the caller
    put_u32(bytes, 16, uncompressed_size);
    put_u8(bytes, 20, static_cast<std::uint8_t>(codec));
    return bytes;
}

auto decode_record_file_header(std::span<const std::byte> bytes)
    -> result<record_file_info> {
    if (bytes.size() != record_file_header_bytes) {
        return invalid("record file header is truncated");
    }
    if (magic_at(bytes, 0) != meta_magic) {
        return invalid("record file magic mismatch");
    }
    const auto stored_crc{get_u32(bytes, 24)};
    std::vector<std::byte> copy{bytes.begin(), bytes.end()};
    put_u32(copy, 24, 0);
    if (data::crc32c(copy) != stored_crc) {
        return invalid("record file header checksum mismatch");
    }
    if (get_u16(bytes, 4) != container_version) {
        return std::unexpected{
            make_error(error_code::version_mismatch,
                       fmt::format("record file version {} is not supported",
                                   get_u16(bytes, 4)))};
    }
    record_file_info info{};
    info.stored_size = get_u32(bytes, 8);
    info.uncompressed_size = get_u32(bytes, 16);
    info.codec = static_cast<compression>(get_u8(bytes, 20));
    if (static_cast<std::uint8_t>(info.codec) >
        static_cast<std::uint8_t>(compression::zstd)) {
        return invalid("record file has unknown compression");
    }
    if (info.stored_size > max_record_bytes ||
        info.uncompressed_size > max_record_bytes) {
        return invalid("record file declares an oversized payload");
    }
    return info;
}

} // namespace refrain::world::rwf::detail

namespace refrain::world::rwf {

auto make_zstd_dictionary(std::span<const std::byte> bytes) -> zstd_dictionary {
    zstd_dictionary dictionary{};
    dictionary.bytes.assign(bytes.begin(), bytes.end());
    const auto digest{data::blake3(bytes)};
    for (std::size_t index{0}; index < 8; ++index) {
        dictionary.id |= static_cast<std::uint64_t>(
                             std::to_integer<unsigned int>(digest[index]))
                         << (8U * index);
    }
    return dictionary;
}

} // namespace refrain::world::rwf
