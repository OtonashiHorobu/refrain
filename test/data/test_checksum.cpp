// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/data/checksum.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace boost::ut;
using refrain::data::blake3;
using refrain::data::crc32c;
using refrain::data::crc32c_update;
using refrain::data::digest32;
using refrain::data::parse_digest;
using refrain::data::to_hex;

/// Published unkeyed BLAKE3 test vectors.
///
/// * empty input and "abc" are the documented BLAKE3/b3sum examples;
/// * the length-indexed vectors come from the upstream
///   `test_vectors/test_vectors.json` shipped with BLAKE3 1.8.5, whose input
///   pattern is `byte[i] = i % 251`.
constexpr std::string_view blake3_empty_hex{
    "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"};
constexpr std::string_view blake3_abc_hex{
    "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85"};

struct blake3_vector {
    std::size_t length;
    std::string_view hex;
};

constexpr std::array<blake3_vector, 4> blake3_length_vectors{{
    {.length = 3,
     .hex = "e1be4d7a8ab5560aa4199eea339849ba8e293d55ca0a81006726d184519e6"
            "47f"},
    {.length = 64,
     .hex = "4eed7141ea4a5cd4b788606bd23f46e212af9cacebacdc7d1f4c6dc7f2511b"
            "98"},
    {.length = 1024,
     .hex = "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855"
            "af7"},
    {.length = 1025,
     .hex = "d00278ae47eb27b34faecf67b4fe263f82d5412916c1ffd97c8cb7fb814b8"
            "444"},
}};

/// `test_vectors.json` input pattern: byte `i` is `i % 251`.
[[nodiscard]] auto vector_input(std::size_t length) -> std::vector<std::byte> {
    std::vector<std::byte> data(length);
    for (std::size_t index{0}; index < data.size(); ++index) {
        data[index] = static_cast<std::byte>(index % 251U);
    }
    return data;
}

[[nodiscard]] auto bytes_of(std::string_view text) -> std::vector<std::byte> {
    const auto bytes{
        std::as_bytes(std::span<const char>{text.data(), text.size()})};
    return std::vector<std::byte>{bytes.begin(), bytes.end()};
}

[[nodiscard]] auto error_code_of(const refrain::result<digest32> &value)
    -> int {
    return value.has_value() ? -1 : static_cast<int>(value.error().code);
}

const suite checksum_tests = [] {
    "checksum/blake3 matches the published test vectors"_test = [] {
        expect(to_hex(blake3(std::span<const std::byte>{})) ==
               blake3_empty_hex);
        expect(to_hex(blake3(bytes_of("abc"))) == blake3_abc_hex);

        for (const auto &vector : blake3_length_vectors) {
            const auto digest{blake3(vector_input(vector.length))};
            expect(to_hex(digest) == vector.hex)
                << "BLAKE3 official vector for length " << vector.length;
        }

        // Some bytes at either side of BLAKE3's 1024-byte chunk boundary still
        // hash deterministically; the digest of the same buffer is stable.
        std::vector<std::byte> large(3U * 1024U, std::byte{0x5A});
        const auto first{blake3(large)};
        const auto second{blake3(large)};
        expect(first == second);
        expect(to_hex(first) == to_hex(first));

        // BLAKE3 is not vulnerable to length-extension by construction, but a
        // changed byte must still change the digest.
        large.back() = std::byte{0x5B};
        expect(blake3(large) != first);
    };

    "checksum/blake3 empty input is the known digest"_test = [] {
        const auto digest{blake3(std::span<const std::byte>{})};
        expect(to_hex(digest) == blake3_empty_hex);
        const auto parsed{parse_digest(blake3_empty_hex)};
        expect(static_cast<bool>(parsed));
        if (parsed) {
            expect(*parsed == digest);
        }
    };

    "checksum/to_hex and parse_digest round-trip"_test = [] {
        digest32 digest{};
        for (std::size_t index{0}; index < digest.size(); ++index) {
            digest[index] = static_cast<std::byte>(index);
        }
        const auto hex{to_hex(digest)};
        expect(hex.size() == 64U);
        expect(hex.starts_with("000102030405060708090a0b0c0d0e0f"));
        expect(hex.ends_with("101112131415161718191a1b1c1d1e1f"));

        const auto parsed{parse_digest(hex)};
        expect(static_cast<bool>(parsed));
        if (parsed) {
            expect(*parsed == digest);
        }

        // Uppercase input is accepted and yields the same value.
        std::string upper{hex};
        for (auto &character : upper) {
            if (character >= 'a' && character <= 'f') {
                character = static_cast<char>(character - 'a' + 'A');
            }
        }
        expect(parse_digest(upper).has_value());
        if (const auto reparsed{parse_digest(upper)}) {
            expect(*reparsed == digest);
        }
    };

    "checksum/parse_digest rejects invalid hex"_test = [] {
        expect(error_code_of(parse_digest("")) ==
               static_cast<int>(refrain::error_code::invalid_argument));
        expect(error_code_of(parse_digest(std::string(63, 'a'))) ==
               static_cast<int>(refrain::error_code::invalid_argument));
        expect(error_code_of(parse_digest(std::string(65, 'a'))) ==
               static_cast<int>(refrain::error_code::invalid_argument));
        expect(error_code_of(parse_digest(std::string(64, 'g'))) ==
               static_cast<int>(refrain::error_code::invalid_argument));
        expect(error_code_of(parse_digest(
                   "zz1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae"
                   "41f3262")) ==
               static_cast<int>(refrain::error_code::invalid_argument));
    };

    "checksum/to_hex formats arbitrary bytes"_test = [] {
        expect(to_hex(std::span<const std::byte>{}).empty());
        const std::array<std::byte, 3> raw{std::byte{0x00}, std::byte{0x0F},
                                           std::byte{0xFF}};
        expect(to_hex(raw) == "000fff");
    };

    "checksum/crc32c matches the Castagnoli known vector"_test = [] {
        // The standard CRC-32C check value for "123456789" is 0xE3069283.
        expect(crc32c(bytes_of("123456789")) == 0xE3069283U);
        expect(crc32c(std::span<const std::byte>{}) == 0U);

        // Deterministic and length-dependent.
        expect(crc32c(bytes_of("123456789")) == crc32c(bytes_of("123456789")));
        expect(crc32c(bytes_of("123456789")) != crc32c(bytes_of("123456788")));
    };

    "checksum/crc32c incremental chunks match the one-shot value"_test = [] {
        expect(crc32c_update(0, std::span<const std::byte>{}) == 0U);
        expect(crc32c_update(0, bytes_of("123456789")) == 0xE3069283U);

        const auto first{crc32c_update(0, bytes_of("1234"))};
        const auto second{crc32c_update(first, bytes_of("56789"))};
        expect(second == 0xE3069283U);
        expect(second == crc32c(bytes_of("123456789")));

        // Arbitrary chunk boundaries (including empty chunks) are equivalent to
        // the single-buffer computation.
        const auto data{
            bytes_of("The quick brown fox jumps over the lazy dog")};
        std::uint32_t chunked{0};
        std::size_t offset{0};
        const std::array<std::size_t, 4> chunk_sizes{0, 7, 16, 1000};
        for (const auto size : chunk_sizes) {
            const auto remaining{data.size() - offset};
            const auto take{std::min(size, remaining)};
            const std::span<const std::byte> chunk{data.data() + offset, take};
            chunked = crc32c_update(chunked, chunk);
            offset += take;
        }
        expect(offset == data.size());
        expect(chunked == crc32c(data));
    };

    "checksum/crc32c matches the RFC 3720 vectors"_test = [] {
        // RFC 3720 Appendix B.4 defines the iSCSI CRC32C over four 32-byte
        // patterns; these also cross-check the library's seed/complement
        // convention against a published, non-empty set of vectors.
        const auto zeros{std::vector<std::byte>(32, std::byte{0x00})};
        const auto ones{std::vector<std::byte>(32, std::byte{0xFF})};
        std::vector<std::byte> ascending(32);
        std::vector<std::byte> descending(32);
        for (std::size_t index{0}; index < ascending.size(); ++index) {
            ascending[index] = static_cast<std::byte>(index);
            descending[index] = static_cast<std::byte>(31U - index);
        }
        expect(crc32c(zeros) == 0x8A9136AAU);
        expect(crc32c(ones) == 0x62A8AB43U);
        expect(crc32c(ascending) == 0x46DD794EU);
        expect(crc32c(descending) == 0x113FDB5CU);

        // RFC 3720 Appendix B.4: the CRC32C of a zero-length buffer is zero.
        expect(crc32c(std::span<const std::byte>{}) == 0U);
    };

    "checksum/crc32c chunking is equivalent for every boundary"_test = [] {
        // One MiB plus a partial tail, so the largest chunk size below does not
        // divide the input and every chunk size exercises a short last chunk.
        const auto data{vector_input((1U << 20U) + 3U)};
        const auto expected{crc32c(data)};

        constexpr std::array<std::size_t, 7> chunk_sizes{
            1U, 2U, 3U, 7U, 16U, 4096U, 1U << 20U};
        for (const auto chunk_size : chunk_sizes) {
            std::uint32_t state{0};
            std::size_t offset{0};
            while (offset < data.size()) {
                const auto take{std::min(chunk_size, data.size() - offset)};
                const std::span<const std::byte> chunk{data.data() + offset,
                                                       take};
                state = crc32c_update(state, chunk);
                offset += take;
            }
            expect(offset == data.size());
            expect(state == expected) << "chunk size " << chunk_size;
        }

        // Empty chunks are no-ops: they neither seed nor perturb the state.
        expect(crc32c_update(0, std::span<const std::byte>{}) == 0U);
        expect(crc32c_update(expected, std::span<const std::byte>{}) ==
               expected);
        expect(crc32c(std::span<const std::byte>{}) ==
               crc32c_update(0, std::span<const std::byte>{}));
    };
};

} // namespace
