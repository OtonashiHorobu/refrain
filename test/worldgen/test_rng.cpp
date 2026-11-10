// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Frozen random-source vectors.
//
// Every expected value below comes from the real 26.3 classes, not from a
// second implementation: the probe in `tools/worldgen/vectors` runs the game's
// own `LegacyRandomSource`, `XoroshiroRandomSource`, `WorldgenRandom`,
// `RandomSupport` and `Mth` and prints the draws. Regenerate with
// `tools/worldgen/vectors/run.sh` after a version bump and review the diff.

#include <refrain/worldgen/core/md5.hpp>
#include <refrain/worldgen/core/rng.hpp>

#include <boost/ut.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <memory>
#include <string_view>

namespace {

using namespace boost::ut;
using namespace refrain::worldgen;

constexpr std::array<jlong, 6> k_legacy_next_long = {
    static_cast<jlong>(0xba419d350dfe8af7ULL),
    static_cast<jlong>(0xaee7bbe10c45c028ULL),
    static_cast<jlong>(0x4f083ce3f12bbb4bULL),
    static_cast<jlong>(0x46ee9d82b52c856dULL),
    static_cast<jlong>(0xaa616abe17610c9aULL),
    static_cast<jlong>(0xe74367bd738597dcULL)};
constexpr std::array<jint, 6> k_legacy_next_int = {
    -1170105035, 234785527, -1360544799, 205897768, 1325939940, -248792245};
constexpr std::array<jint, 6> k_legacy_next_int_1000 = {130, 763, 248,
                                                        884, 970, 525};
constexpr std::array<std::uint32_t, 4> k_legacy_next_float = {
    0x3f3a419dU, 0x3d5fe8a0U, 0x3f2ee7bbU, 0x3d445c00U};
constexpr std::array<std::uint64_t, 4> k_legacy_next_double = {
    0x3fe74833a06ff457ULL, 0x3fe5dcf778622e01ULL, 0x3fd3c20f3f12bbb4ULL,
    0x3fd1bba76b52c856ULL};
constexpr jint k_legacy_boolean_bits = 5;

constexpr std::array<jlong, 6> k_xoro_next_long = {
    static_cast<jlong>(0xbed4a3d469c5d91fULL),
    static_cast<jlong>(0x65e301cb50e8f4abULL),
    static_cast<jlong>(0x9752d3d4db9a2abdULL),
    static_cast<jlong>(0x43d8d3137b6e0186ULL),
    static_cast<jlong>(0x74d89c01aa1097cbULL),
    static_cast<jlong>(0xa5e803ec4953c139ULL)};
constexpr std::array<jint, 6> k_xoro_next_int = {
    1774573855, 1357444267, -610653507, 2070806918, -1441753141, 1230225721};
constexpr std::array<jint, 6> k_xoro_next_int_1000 = {413, 316, 857,
                                                      482, 664, 286};
constexpr std::array<std::uint32_t, 4> k_xoro_next_float = {
    0x3f3ed4a3U, 0x3ecbc602U, 0x3f1752d3U, 0x3e87b1a6U};
constexpr std::array<std::uint64_t, 4> k_xoro_next_double = {
    0x3fe7da947a8d38bbULL, 0x3fd978c072d43a3cULL, 0x3fe2ea5a7a9b7345ULL,
    0x3fd0f634c4dedb80ULL};

constexpr jlong k_upgrade_lo = static_cast<jlong>(0x5d4520bed6c96db9ULL);
constexpr jlong k_upgrade_hi = static_cast<jlong>(0xd86e1008eac15bc5ULL);
constexpr std::array<jlong, 3> k_mix = {
    static_cast<jlong>(0x0000000000000000ULL),
    static_cast<jlong>(0x5692161d100b05e5ULL),
    static_cast<jlong>(0xe220a8397b1dcdafULL),
};
constexpr jlong k_hash_aquifer_lo = static_cast<jlong>(0x7bb15cc403c6ace6ULL);
constexpr jlong k_hash_aquifer_hi = static_cast<jlong>(0x0bdd56bc9d232691ULL);
constexpr jlong k_hash_ore_lo = static_cast<jlong>(0x9b88124de600116dULL);
constexpr jlong k_hash_ore_hi = static_cast<jlong>(0x2ae68055aa4a7761ULL);

constexpr std::array<jlong, 3> k_get_seed = {
    static_cast<jlong>(0xffffe15fa16d39f8ULL),
    static_cast<jlong>(0x00005ba13e7cb35aULL),
    static_cast<jlong>(0x0000000000000000ULL),
};

constexpr jint k_legacy_at_int = -1062610619;
constexpr jint k_legacy_at_int_100 = 38;
constexpr jint k_legacy_hash_int = -151679290;
constexpr jint k_xoro_at_int = 1942892291;
constexpr jint k_xoro_at_int_100 = 45;
constexpr jint k_xoro_at_neg_int = 362394548;
constexpr jint k_xoro_hash_int = -93725375;

constexpr jlong k_decoration_seed = static_cast<jlong>(0x2587f26946ab06e9ULL);
constexpr jint k_decoration_int = -442956178;
constexpr jint k_feature_int = -144646999;
constexpr jint k_large_int = -1659321407;
constexpr jint k_salt_int = 1673968363;
constexpr jint k_large_legacy_int = 2002653025;

} // namespace

suite<"worldgen/rng"> rng_tests = [] {
    "legacy lcg reproduces java draws"_test = [] {
        legacy_random_source source{42};
        for (const jlong expected : k_legacy_next_long) {
            expect(source.next_long() == expected);
        }

        legacy_random_source ints{42};
        for (const jint expected : k_legacy_next_int) {
            expect(ints.next_int() == expected);
        }

        legacy_random_source bounded{42};
        for (const jint expected : k_legacy_next_int_1000) {
            expect(bounded.next_int(1000) == expected);
        }

        legacy_random_source floats{42};
        for (const std::uint32_t expected : k_legacy_next_float) {
            expect(std::bit_cast<std::uint32_t>(floats.next_float()) ==
                   expected);
        }

        legacy_random_source doubles{42};
        for (const std::uint64_t expected : k_legacy_next_double) {
            expect(std::bit_cast<std::uint64_t>(doubles.next_double()) ==
                   expected);
        }

        legacy_random_source bools{42};
        const jint bits =
            (bools.next_boolean() ? 1 : 0) | (bools.next_boolean() ? 2 : 0) |
            (bools.next_boolean() ? 4 : 0) | (bools.next_boolean() ? 8 : 0);
        expect(bits == k_legacy_boolean_bits);
    };

    "xoroshiro reproduces java draws"_test = [] {
        xoroshiro_random_source source{42};
        for (const jlong expected : k_xoro_next_long) {
            expect(source.next_long() == expected);
        }

        xoroshiro_random_source ints{42};
        for (const jint expected : k_xoro_next_int) {
            expect(ints.next_int() == expected);
        }

        xoroshiro_random_source bounded{42};
        for (const jint expected : k_xoro_next_int_1000) {
            expect(bounded.next_int(1000) == expected);
        }

        xoroshiro_random_source floats{42};
        for (const std::uint32_t expected : k_xoro_next_float) {
            expect(std::bit_cast<std::uint32_t>(floats.next_float()) ==
                   expected);
        }

        xoroshiro_random_source doubles{42};
        for (const std::uint64_t expected : k_xoro_next_double) {
            expect(std::bit_cast<std::uint64_t>(doubles.next_double()) ==
                   expected);
        }
    };

    "seed mixing matches RandomSupport"_test = [] {
        const seed128bit upgraded = random_support::upgrade_seed_to_128bit(42);
        expect(upgraded.seed_lo == k_upgrade_lo);
        expect(upgraded.seed_hi == k_upgrade_hi);
        expect(random_support::mix_stafford13(0) == k_mix[0]);
        expect(random_support::mix_stafford13(1) == k_mix[1]);
        expect(random_support::mix_stafford13(
                   random_support::golden_ratio_64) == k_mix[2]);
    };

    "md5 derived seeds match the game"_test = [] {
        const seed128bit aquifer =
            random_support::seed_from_hash_of("minecraft:aquifer");
        expect(aquifer.seed_lo == k_hash_aquifer_lo);
        expect(aquifer.seed_hi == k_hash_aquifer_hi);

        const seed128bit ore =
            random_support::seed_from_hash_of("minecraft:ore");
        expect(ore.seed_lo == k_hash_ore_lo);
        expect(ore.seed_hi == k_hash_ore_hi);
    };

    "md5 known answers"_test = [] {
        const auto empty = md5(std::string_view{});
        const auto abc = md5(std::string_view{"abc"});
        expect(empty[0] == 0xd4U && empty[1] == 0x1dU && empty[15] == 0x7eU);
        expect(abc[0] == 0x90U && abc[1] == 0x01U && abc[15] == 0x72U);
    };

    "positional hash matches Mth.getSeed"_test = [] {
        expect(mth_get_seed(1, 2, 3) == k_get_seed[0]);
        expect(mth_get_seed(-5, 64, 7) == k_get_seed[1]);
        expect(mth_get_seed(0, 0, 0) == k_get_seed[2]);
    };

    "positional factories reproduce staged draws"_test = [] {
        const auto legacy = new_positional_factory(algorithm::legacy, 42);
        expect(legacy->at(1, 2, 3)->next_int() == k_legacy_at_int);
        expect(legacy->at(1, 2, 3)->next_int(100) == k_legacy_at_int_100);
        expect(legacy->from_hash_of("minecraft:aquifer")->next_int() ==
               k_legacy_hash_int);

        const auto xoro = new_positional_factory(algorithm::xoroshiro, 42);
        expect(xoro->at(1, 2, 3)->next_int() == k_xoro_at_int);
        expect(xoro->at(1, 2, 3)->next_int(100) == k_xoro_at_int_100);
        expect(xoro->at(-5, 64, 7)->next_int() == k_xoro_at_neg_int);
        expect(xoro->from_hash_of("minecraft:aquifer")->next_int() ==
               k_xoro_hash_int);
    };

    "worldgen random seed formulas match"_test = [] {
        worldgen_random decoration{
            std::make_unique<xoroshiro_random_source>(0)};
        expect(decoration.set_decoration_seed(12345, 16, 32) ==
               k_decoration_seed);
        expect(decoration.next_int() == k_decoration_int);

        worldgen_random feature{std::make_unique<xoroshiro_random_source>(0)};
        feature.set_feature_seed(777, 5, 3);
        expect(feature.next_int() == k_feature_int);

        worldgen_random large{std::make_unique<xoroshiro_random_source>(0)};
        large.set_large_feature_seed(12345, -3, 7);
        expect(large.next_int() == k_large_int);

        worldgen_random salt{std::make_unique<xoroshiro_random_source>(0)};
        salt.set_large_feature_with_salt(12345, -3, 7, 10387312);
        expect(salt.next_int() == k_salt_int);

        worldgen_random gaussian{std::make_unique<legacy_random_source>(12345)};
        expect(std::abs(gaussian.next_gaussian() - (-0.187808989658912)) <
               1e-15);
        expect(std::abs(gaussian.next_gaussian() - 0.5884363051154796) < 1e-15);
        expect(std::abs(gaussian.next_gaussian() - 0.9488047804400426) < 1e-15);
        worldgen_random gaussian2{std::make_unique<legacy_random_source>(1)};
        expect(std::abs(gaussian2.next_gaussian() - 1.561581040188955) < 1e-15);
        expect(std::abs(gaussian2.next_gaussian() - (-0.6081826070068602)) <
               1e-15);

        worldgen_random legacy_delegate{
            std::make_unique<legacy_random_source>(0)};
        legacy_delegate.set_large_feature_seed(12345, -3, 7);
        expect(legacy_delegate.next_int() == k_large_legacy_int);
    };
};
