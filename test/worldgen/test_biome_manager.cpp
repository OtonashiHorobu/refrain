// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// `BiomeManager`: seed obfuscation (SHA-256) and the eight-corner fuzzy biome
// lookup. The resolver records the chosen quart coordinate and returns a fixed
// id, so no biome registry is involved.

#include <refrain/worldgen/biome_manager.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstdint>
#include <string>

namespace {

using namespace refrain::worldgen;

class recording_resolver final : public biome_resolver {
  public:
    [[nodiscard]] auto get_noise_biome(jint quart_x, jint quart_y, jint quart_z)
        -> std::uint16_t override {
        x = quart_x;
        y = quart_y;
        z = quart_z;
        return 0;
    }

    jint x{0};
    jint y{0};
    jint z{0};
};

auto fnv1a(std::uint64_t hash, const std::string &text) -> std::uint64_t {
    for (const char character : text) {
        hash ^=
            static_cast<std::uint64_t>(static_cast<unsigned char>(character));
        hash *= 0x100000001B3ULL;
    }
    return hash;
}

constexpr std::array<jlong, 2> seeds{0, 12345};
constexpr std::array<jint, 12> xs{-3, -2, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8};
constexpr std::array<jint, 3> ys{-2, 0, 1};
constexpr std::array<jint, 4> zs{-3, 0, 3, 4};

} // namespace

using namespace boost::ut;

suite<"worldgen/biome-manager"> biome_manager_tests = [] {
    "seed obfuscation matches the reference SHA-256 derivation"_test = [] {
        expect(biome_manager::obfuscate_seed(0) ==
               static_cast<jlong>(0x7a0b81a1f57055afULL));
        expect(biome_manager::obfuscate_seed(1) ==
               static_cast<jlong>(0xa63f41d436a19f7cULL));
        expect(biome_manager::obfuscate_seed(12345) ==
               static_cast<jlong>(0x04139124513554e1ULL));
        expect(biome_manager::obfuscate_seed(-1) ==
               static_cast<jlong>(0x5dce615644aea312ULL));
        expect(biome_manager::obfuscate_seed(987654321) ==
               static_cast<jlong>(0xec575322085641ddULL));
    };

    "the fuzzy lookup selects the same quart corners as the reference"_test =
        [] {
            std::uint64_t hash = 0xCBF29CE484222325ULL;
            std::size_t count = 0;
            for (const jlong seed : seeds) {
                recording_resolver resolver;
                biome_manager manager{resolver,
                                      biome_manager::obfuscate_seed(seed)};
                for (const jint x : xs) {
                    for (const jint y : ys) {
                        for (const jint z : zs) {
                            expect(manager.get_biome(x, y, z) == 0);
                            hash = fnv1a(hash,
                                         std::to_string(resolver.x) + "," +
                                             std::to_string(resolver.y) + "," +
                                             std::to_string(resolver.z) + "\n");
                            ++count;
                        }
                    }
                }
            }
            expect(count == 288U);
            expect(hash == 0x77d75ee218d24eafULL);
        };
};
