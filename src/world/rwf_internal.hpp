// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Private RWF encoding helpers shared by the region/record/blob
// implementations. Not installed: the byte layout is reachable only through
// the public `rwf.hpp` container API.

#pragma once

#include <refrain/core/bytes.hpp>
#include <refrain/world/rwf.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace refrain::world::rwf::detail {

/// Little-endian scalar access: thin aliases over the repository-wide
/// `core::bytes` helpers (single implementation; callers guarantee bounds).
inline constexpr auto put_u8 = core::bytes::write_le<std::uint8_t>;
inline constexpr auto put_u16 = core::bytes::write_le<std::uint16_t>;
inline constexpr auto put_u32 = core::bytes::write_le<std::uint32_t>;
inline constexpr auto put_u64 = core::bytes::write_le<std::uint64_t>;
inline constexpr auto put_i32 = core::bytes::write_le<std::int32_t>;
inline constexpr auto get_u8 = core::bytes::read_le<std::uint8_t>;
inline constexpr auto get_u16 = core::bytes::read_le<std::uint16_t>;
inline constexpr auto get_u32 = core::bytes::read_le<std::uint32_t>;
inline constexpr auto get_u64 = core::bytes::read_le<std::uint64_t>;
inline constexpr auto get_i32 = core::bytes::read_le<std::int32_t>;
inline constexpr auto align_up = core::bytes::align_up;

/// Region file header (4096 bytes on disk).
struct file_header {
    std::int32_t x{0};
    std::int32_t z{0};
    std::uint32_t active_slot{0};
    std::uint64_t generation{0};
    std::uint64_t data_tail{data_offset};
    std::uint64_t record_count{0};
    std::uint64_t dictionary_id{0};
};

[[nodiscard]] auto encode_file_header(const file_header &header)
    -> std::vector<std::byte>;
[[nodiscard]] auto decode_file_header(std::span<const std::byte> bytes)
    -> result<file_header>;

/// One index slot (36864 bytes on disk).
struct index_slot {
    std::uint64_t generation{0};
    std::array<region_entry, index_slots> entries{};
};

[[nodiscard]] auto encode_index_slot(const index_slot &slot)
    -> std::vector<std::byte>;
[[nodiscard]] auto decode_index_slot(std::span<const std::byte> bytes)
    -> result<index_slot>;

/// Record frame (region data area).
struct frame_header {
    chunk_pos pos{};
    chunk_status status{chunk_status::empty};
    compression codec{compression::none};
    std::uint32_t stored_size{0};
    /// CRC32C over the 32-byte frame header (this field zeroed) followed by the
    /// stored payload, so a damaged `chunk_x/z`/status cannot be mistaken for a
    /// valid record during index rebuilds.
    std::uint32_t record_crc32c{0};
    std::uint32_t uncompressed_size{0};
};

/// CRC32C of a complete record frame with the CRC field zeroed.
[[nodiscard]] auto frame_crc32c(std::span<const std::byte> frame) noexcept
    -> std::uint32_t;

/// Build a complete record frame (32-byte header + stored payload). Uses zstd
/// only when it makes the payload smaller; otherwise stores it raw.
[[nodiscard]] auto build_frame(chunk_pos pos, chunk_status status,
                               compression codec, int zstd_level,
                               std::span<const std::byte> payload,
                               const zstd_dictionary *dictionary = nullptr)
    -> result<std::vector<std::byte>>;

[[nodiscard]] auto decode_frame_header(std::span<const std::byte> frame)
    -> result<frame_header>;

/// Verify a full frame (CRC + header) and return the decompressed payload.
[[nodiscard]] auto decode_frame(std::span<const std::byte> frame,
                                const zstd_dictionary *dictionary = nullptr)
    -> result<std::vector<std::byte>>;

/// Record-file header (4096 bytes on disk).
struct record_file_info {
    std::uint32_t stored_size{0};
    std::uint32_t uncompressed_size{0};
    compression codec{compression::none};
};

[[nodiscard]] auto encode_record_file_header(std::uint32_t stored_size,
                                             std::uint32_t uncompressed_size,
                                             compression codec)
    -> std::vector<std::byte>;
[[nodiscard]] auto decode_record_file_header(std::span<const std::byte> bytes)
    -> result<record_file_info>;

/// Compression primitives with an explicit output bound.
[[nodiscard]] auto compress(compression codec, int zstd_level,
                            std::span<const std::byte> input,
                            const zstd_dictionary *dictionary = nullptr)
    -> result<std::vector<std::byte>>;
[[nodiscard]] auto decompress(compression codec,
                              std::span<const std::byte> input,
                              std::size_t expected_size,
                              const zstd_dictionary *dictionary = nullptr)
    -> result<std::vector<std::byte>>;

} // namespace refrain::world::rwf::detail
