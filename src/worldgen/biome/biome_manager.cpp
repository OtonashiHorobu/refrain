// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/biome/biome_manager.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace refrain::worldgen {

namespace {

constexpr std::array<std::uint32_t, 64> sha256_k{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
    0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
    0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
    0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
    0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
    0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
    0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
    0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

auto rotr(std::uint32_t value, unsigned shift) noexcept -> std::uint32_t {
    return (value >> shift) | (value << (32U - shift));
}

void compress(std::array<std::uint32_t, 8> &hash,
              const std::uint8_t *block) noexcept {
    std::array<std::uint32_t, 64> schedule{};
    for (std::size_t i = 0; i < 16U; ++i) {
        schedule[i] = (static_cast<std::uint32_t>(block[i * 4U]) << 24U) |
                      (static_cast<std::uint32_t>(block[i * 4U + 1U]) << 16U) |
                      (static_cast<std::uint32_t>(block[i * 4U + 2U]) << 8U) |
                      static_cast<std::uint32_t>(block[i * 4U + 3U]);
    }
    for (std::size_t i = 16U; i < 64U; ++i) {
        const auto s0 = rotr(schedule[i - 15U], 7U) ^
                        rotr(schedule[i - 15U], 18U) ^
                        (schedule[i - 15U] >> 3U);
        const auto s1 = rotr(schedule[i - 2U], 17U) ^
                        rotr(schedule[i - 2U], 19U) ^ (schedule[i - 2U] >> 10U);
        schedule[i] = schedule[i - 16U] + s0 + schedule[i - 7U] + s1;
    }

    auto a = hash[0];
    auto b = hash[1];
    auto c = hash[2];
    auto d = hash[3];
    auto e = hash[4];
    auto f = hash[5];
    auto g = hash[6];
    auto h = hash[7];
    for (std::size_t i = 0; i < 64U; ++i) {
        const auto s1 = rotr(e, 6U) ^ rotr(e, 11U) ^ rotr(e, 25U);
        const auto choice = (e & f) ^ (~e & g);
        const auto temp1 = h + s1 + choice + sha256_k[i] + schedule[i];
        const auto s0 = rotr(a, 2U) ^ rotr(a, 13U) ^ rotr(a, 22U);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto temp2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    hash[0] += a;
    hash[1] += b;
    hash[2] += c;
    hash[3] += d;
    hash[4] += e;
    hash[5] += f;
    hash[6] += g;
    hash[7] += h;
}

auto sha256(std::span<const std::uint8_t> message)
    -> std::array<std::uint8_t, 32> {
    std::array<std::uint32_t, 8> hash{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U,
                                      0xa54ff53aU, 0x510e527fU, 0x9b05688cU,
                                      0x1f83d9abU, 0x5be0cd19U};
    std::vector<std::uint8_t> padded(message.begin(), message.end());
    const auto bit_length = static_cast<std::uint64_t>(message.size()) * 8U;
    padded.push_back(0x80U);
    while (padded.size() % 64U != 56U) {
        padded.push_back(0);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        padded.push_back(static_cast<std::uint8_t>(
            bit_length >> static_cast<unsigned>(shift)));
    }
    for (std::size_t offset = 0; offset < padded.size(); offset += 64U) {
        compress(hash, padded.data() + offset);
    }

    std::array<std::uint8_t, 32> digest{};
    for (std::size_t i = 0; i < hash.size(); ++i) {
        digest[i * 4U] = static_cast<std::uint8_t>(hash[i] >> 24U);
        digest[i * 4U + 1U] = static_cast<std::uint8_t>(hash[i] >> 16U);
        digest[i * 4U + 2U] = static_cast<std::uint8_t>(hash[i] >> 8U);
        digest[i * 4U + 3U] = static_cast<std::uint8_t>(hash[i]);
    }
    return digest;
}

auto next_random(jlong value, jlong addend) noexcept -> jlong {
    // LinearCongruentialGenerator.next: wrapping signed arithmetic.
    const auto product = static_cast<jlong>(static_cast<std::uint64_t>(value) *
                                                6364136223846793005ULL +
                                            1442695040888963407ULL);
    const auto mixed = static_cast<jlong>(static_cast<std::uint64_t>(value) *
                                          static_cast<std::uint64_t>(product));
    return static_cast<jlong>(static_cast<std::uint64_t>(mixed) +
                              static_cast<std::uint64_t>(addend));
}

auto get_fiddle(jlong value) noexcept -> double {
    const auto uniform =
        static_cast<double>(floor_mod(value >> 24, 1024)) / 1024.0;
    return (uniform - 0.5) * 0.9;
}

auto get_fiddled_distance(jlong seed, jint x, jint y, jint z, double distance_x,
                          double distance_y, double distance_z) noexcept
    -> double {
    auto value = next_random(seed, x);
    value = next_random(value, y);
    value = next_random(value, z);
    value = next_random(value, x);
    value = next_random(value, y);
    value = next_random(value, z);
    const double fiddle_x = get_fiddle(value);
    value = next_random(value, seed);
    const double fiddle_y = get_fiddle(value);
    value = next_random(value, seed);
    const double fiddle_z = get_fiddle(value);
    const double dx = distance_x + fiddle_x;
    const double dy = distance_y + fiddle_y;
    const double dz = distance_z + fiddle_z;
    // Java adds the three squares in z, y, x order; the order is observable
    // when two corners are nearly equidistant.
    return dz * dz + dy * dy + dx * dx;
}

} // namespace

auto biome_manager::obfuscate_seed(jlong seed) -> jlong {
    std::array<std::uint8_t, 8> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(seed) >>
                                             (i * 8U));
    }
    const auto digest = sha256(bytes);
    // Guava's `HashCode.asLong()` reads the first eight bytes little-endian.
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8U; ++i) {
        value |= static_cast<std::uint64_t>(digest[i]) << (i * 8U);
    }
    return static_cast<jlong>(value);
}

auto biome_manager::get_biome(jint x, jint y, jint z) -> std::uint16_t {
    const jint abs_x = x - 2;
    const jint abs_y = y - 2;
    const jint abs_z = z - 2;
    const jint parent_x = abs_x >> 2;
    const jint parent_y = abs_y >> 2;
    const jint parent_z = abs_z >> 2;
    const double fract_x = static_cast<double>(abs_x & 3) / 4.0;
    const double fract_y = static_cast<double>(abs_y & 3) / 4.0;
    const double fract_z = static_cast<double>(abs_z & 3) / 4.0;
    jint minimum_index = 0;
    double minimum_distance = std::numeric_limits<double>::infinity();

    for (jint i = 0; i < 8; ++i) {
        const bool even_x = (i & 4) == 0;
        const bool even_y = (i & 2) == 0;
        const bool even_z = (i & 1) == 0;
        const jint corner_x = even_x ? parent_x : parent_x + 1;
        const jint corner_y = even_y ? parent_y : parent_y + 1;
        const jint corner_z = even_z ? parent_z : parent_z + 1;
        const double distance_x = even_x ? fract_x : fract_x - 1.0;
        const double distance_y = even_y ? fract_y : fract_y - 1.0;
        const double distance_z = even_z ? fract_z : fract_z - 1.0;
        const double distance =
            get_fiddled_distance(biome_zoom_seed_, corner_x, corner_y, corner_z,
                                 distance_x, distance_y, distance_z);
        if (minimum_distance > distance) {
            minimum_index = i;
            minimum_distance = distance;
        }
    }

    const jint biome_x = (minimum_index & 4) == 0 ? parent_x : parent_x + 1;
    const jint biome_y = (minimum_index & 2) == 0 ? parent_y : parent_y + 1;
    const jint biome_z = (minimum_index & 1) == 0 ? parent_z : parent_z + 1;
    return resolver_->get_noise_biome(biome_x, biome_y, biome_z);
}

} // namespace refrain::worldgen
