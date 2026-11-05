// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include "anvil_lz4.hpp"

#include <fmt/format.h>

#include <lz4.h>
#include <xxhash.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace refrain::world::anvil::detail {
namespace {

constexpr std::array<std::byte, 8> magic{
    std::byte{'L'}, std::byte{'Z'}, std::byte{'4'}, std::byte{'B'},
    std::byte{'l'}, std::byte{'o'}, std::byte{'c'}, std::byte{'k'}};
constexpr std::size_t block_header_bytes{magic.size() + 1 + 4 + 4 + 4};
constexpr std::uint8_t method_raw{16};
constexpr std::uint8_t method_lz4{32};
constexpr XXH32_hash_t checksum_seed{0x9747B28CU};
/// lz4-java's `StreamingXXHash32.asChecksum()` masks the digest with
/// `0x0FFFFFFF` before `LZ4BlockOutputStream` writes it, so real lz4-java
/// streams (including vanilla 26.3 `region-file-compression=lz4`) carry a
/// 28-bit checksum. Mask the computed digest to stay byte-compatible with
/// those files; the reader in lz4-java compares against the same masked value.
constexpr XXH32_hash_t checksum_mask{0x0FFFFFFFU};

[[nodiscard]] auto corrupt(std::string message) -> std::unexpected<error> {
    return std::unexpected{
        make_error(error_code::parse_error, std::move(message))};
}

[[nodiscard]] auto read_le32(std::span<const std::byte> input,
                             std::size_t offset) noexcept -> std::uint32_t {
    std::uint32_t value{0};
    for (std::size_t index{0}; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(
                     std::to_integer<std::uint8_t>(input[offset + index]))
                 << (index * 8U);
    }
    return value;
}

} // namespace

auto decode_lz4_block_stream(std::span<const std::byte> input,
                             std::size_t max_output)
    -> result<std::vector<std::byte>> {
    if (max_output == 0) {
        return corrupt("LZ4 output limit is zero");
    }
    std::vector<std::byte> output;
    std::size_t position{0};
    std::size_t block_index{0};
    while (true) {
        if (input.size() - position < block_header_bytes) {
            return corrupt(
                fmt::format("LZ4 block {} header is truncated", block_index));
        }
        if (!std::equal(magic.begin(), magic.end(),
                        input.begin() +
                            static_cast<std::ptrdiff_t>(position))) {
            return corrupt(
                fmt::format("LZ4 block {} magic mismatch", block_index));
        }
        const auto token{
            std::to_integer<std::uint8_t>(input[position + magic.size()])};
        const auto method{static_cast<std::uint8_t>(token & 0xF0U)};
        const auto compression_level{
            static_cast<unsigned>(10U + (token & 0x0FU))};
        const auto compressed_length{
            read_le32(input, position + magic.size() + 1)};
        const auto original_length{
            read_le32(input, position + magic.size() + 5)};
        const auto expected_checksum{
            read_le32(input, position + magic.size() + 9)};
        position += block_header_bytes;

        if (method != method_raw && method != method_lz4) {
            return corrupt(
                fmt::format("LZ4 block {} has invalid compression method {}",
                            block_index, method));
        }
        if (compressed_length > input.size() - position) {
            return corrupt(fmt::format(
                "LZ4 block {} compressed length {} exceeds the {} remaining "
                "bytes",
                block_index, compressed_length, input.size() - position));
        }
        const auto block_limit{std::size_t{1}
                               << std::min(compression_level, 25U)};
        if (original_length > block_limit) {
            return corrupt(fmt::format(
                "LZ4 block {} original length {} exceeds the {} byte block "
                "limit",
                block_index, original_length, block_limit));
        }
        if (original_length == 0) {
            if (compressed_length != 0 || expected_checksum != 0) {
                return corrupt(
                    fmt::format("LZ4 block {} has a malformed terminal marker",
                                block_index));
            }
            if (position != input.size()) {
                return corrupt("LZ4 stream has trailing data after the "
                               "terminal block");
            }
            return output;
        }
        if (compressed_length == 0) {
            return corrupt(fmt::format(
                "LZ4 block {} has zero compressed length", block_index));
        }
        if (original_length > max_output - output.size()) {
            return corrupt(fmt::format(
                "LZ4 stream exceeds the {} byte output limit", max_output));
        }

        std::vector<std::byte> block(original_length);
        if (method == method_raw) {
            if (compressed_length != original_length) {
                return corrupt(fmt::format(
                    "LZ4 block {} raw lengths disagree ({} != {})", block_index,
                    compressed_length, original_length));
            }
            std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(position),
                        static_cast<std::ptrdiff_t>(compressed_length),
                        block.begin());
        } else {
            if (compressed_length > static_cast<std::uint32_t>(
                                        std::numeric_limits<int>::max()) ||
                original_length > static_cast<std::uint32_t>(
                                      std::numeric_limits<int>::max())) {
                return corrupt(fmt::format(
                    "LZ4 block {} is too large for the decoder", block_index));
            }
            const auto decoded{LZ4_decompress_safe(
                reinterpret_cast<const char *>(input.data() + position),
                reinterpret_cast<char *>(block.data()),
                static_cast<int>(compressed_length),
                static_cast<int>(original_length))};
            if (decoded < 0 ||
                static_cast<std::uint32_t>(decoded) != original_length) {
                return corrupt(fmt::format(
                    "LZ4 block {} does not decode to its declared length",
                    block_index));
            }
        }
        position += compressed_length;

        const auto checksum{XXH32(block.data(), block.size(), checksum_seed) &
                            checksum_mask};
        if (checksum != expected_checksum) {
            return corrupt(
                fmt::format("LZ4 block {} checksum mismatch", block_index));
        }
        output.insert(output.end(), block.begin(), block.end());
        ++block_index;
    }
}

} // namespace refrain::world::anvil::detail
