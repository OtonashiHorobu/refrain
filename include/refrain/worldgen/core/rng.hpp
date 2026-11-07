// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// World generation random sources.
//
// Three generators matter: the 48-bit legacy LCG, xoroshiro128++ and the
// `WorldgenRandom` wrapper that re-seeds the same object for every stage. The
// wrapper is not a thin delegate: it overrides only `next(bits)` and inherits
// `nextLong`, `nextInt`, `nextFloat` and `nextDouble` from the Java
// `BitRandomSource` mixin, so those draws are composed from the overridden
// `next(bits)` even when the wrapped source is xoroshiro. Reproducing the
// composition order is what makes decoration, feature and structure seeds
// identical.
//
// Every value here is defined by the reference implementation; the frozen
// vectors in `test/worldgen/test_rng.cpp` come from the real 26.3 classes.

#pragma once

#include <refrain/worldgen/core/java_compat.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace refrain::worldgen {

class positional_random_factory;

/// Java `RandomSource`: the subset world generation uses.
///
/// Bit-exactness rules:
/// * `next(bits)` returns the top `bits` bits of one state transition.
/// * `next_long` is two `next(32)` draws for the legacy family, but a single
///   64-bit draw for xoroshiro; subclasses override accordingly.
/// * `next_int(bound)` uses modulo rejection for legacy and Lemire's method
///   for xoroshiro.
class random_source {
  public:
    random_source() = default;
    random_source(const random_source &) = delete;
    auto operator=(const random_source &) -> random_source & = delete;
    random_source(random_source &&) = delete;
    auto operator=(random_source &&) -> random_source & = delete;
    virtual ~random_source() = default;

    /// Top `bits` bits of one transition (`bits` in 1..32).
    [[nodiscard]] virtual auto next(int bits) -> jint = 0;

    [[nodiscard]] virtual auto next_int() -> jint;
    [[nodiscard]] virtual auto next_int(jint bound) -> jint;
    [[nodiscard]] virtual auto next_long() -> jlong;
    [[nodiscard]] virtual auto next_float() -> float;
    [[nodiscard]] virtual auto next_double() -> double;
    [[nodiscard]] virtual auto next_boolean() -> bool;

    virtual void set_seed(jlong seed) = 0;

    /// Discards `rounds` draws. Legacy consumes one LCG step per round;
    /// xoroshiro consumes one 64-bit output.
    virtual void consume_count(int rounds);

    [[nodiscard]] virtual auto fork() -> std::unique_ptr<random_source> = 0;
    [[nodiscard]] virtual auto fork_positional()
        -> std::unique_ptr<positional_random_factory> = 0;
};

/// 48-bit LCG (`java.util.Random` compatible).
class legacy_random_source final : public random_source {
  public:
    static constexpr jlong multiplier = 25214903917LL;
    static constexpr jlong increment = 11LL;
    static constexpr jlong mask = 281474976710655LL; // 2^48 - 1

    explicit legacy_random_source(jlong seed) noexcept { set_seed(seed); }

    [[nodiscard]] auto next(int bits) -> jint override;
    void set_seed(jlong seed) override;
    [[nodiscard]] auto fork() -> std::unique_ptr<random_source> override;
    [[nodiscard]] auto fork_positional()
        -> std::unique_ptr<positional_random_factory> override;

    [[nodiscard]] auto state() const noexcept -> jlong { return state_; }

  private:
    jlong state_{0};
};

/// Same algorithm as `legacy_random_source`, without atomics. The reference
/// implementation uses it for carver tunnels, which are per-thread.
using single_threaded_random_source = legacy_random_source;

/// xoroshiro128++.
class xoroshiro128pp {
  public:
    xoroshiro128pp() noexcept = default;
    xoroshiro128pp(jlong seed_lo, jlong seed_hi) noexcept
        : seed_lo_{seed_lo}, seed_hi_{seed_hi} {
        if ((seed_lo_ | seed_hi_) == 0) {
            seed_lo_ = golden_ratio_64;
            seed_hi_ = silver_ratio_64;
        }
    }

    [[nodiscard]] auto next_long() noexcept -> jlong;

  private:
    static constexpr jlong golden_ratio_64 = -7046029254386353131LL;
    static constexpr jlong silver_ratio_64 = 7640891576956012809LL;

    jlong seed_lo_{0};
    jlong seed_hi_{0};
};

/// xoroshiro128++ backed `RandomSource`.
class xoroshiro_random_source final : public random_source {
  public:
    explicit xoroshiro_random_source(jlong seed) noexcept;
    xoroshiro_random_source(jlong seed_lo, jlong seed_hi) noexcept
        : generator_{seed_lo, seed_hi} {}

    [[nodiscard]] auto next(int bits) -> jint override;
    [[nodiscard]] auto next_int() -> jint override;
    [[nodiscard]] auto next_int(jint bound) -> jint override;
    [[nodiscard]] auto next_long() -> jlong override;
    [[nodiscard]] auto next_double() -> double override;
    void set_seed(jlong seed) override;
    [[nodiscard]] auto fork() -> std::unique_ptr<random_source> override;
    [[nodiscard]] auto fork_positional()
        -> std::unique_ptr<positional_random_factory> override;

  private:
    [[nodiscard]] auto next_bits(int bits) noexcept -> jlong {
        return static_cast<jlong>(
            static_cast<std::uint64_t>(generator_.next_long()) >>
            static_cast<unsigned>(64 - bits));
    }

    xoroshiro128pp generator_{};
};

/// 128-bit seed split used by xoroshiro's seeding.
struct seed128bit {
    jlong seed_lo{0};
    jlong seed_hi{0};

    [[nodiscard]] constexpr auto operator==(const seed128bit &) const noexcept
        -> bool = default;
};

/// Java `RandomSupport`: seed mixing and hash-derived seeds.
namespace random_support {

inline constexpr jlong golden_ratio_64 = -7046029254386353131LL;
inline constexpr jlong silver_ratio_64 = 7640891576956012809LL;

[[nodiscard]] constexpr auto mix_stafford13(jlong value) noexcept -> jlong {
    value =
        static_cast<jlong>(static_cast<std::uint64_t>(value ^ ushr(value, 30)) *
                           0xBF58476D1CE4E5B9ULL);
    value =
        static_cast<jlong>(static_cast<std::uint64_t>(value ^ ushr(value, 27)) *
                           0x94D049BB133111EBULL);
    return value ^ ushr(value, 31);
}

[[nodiscard]] constexpr auto upgrade_seed_to_128bit(jlong legacy_seed) noexcept
    -> seed128bit {
    const jlong low = legacy_seed ^ silver_ratio_64;
    const jlong high = low + golden_ratio_64;
    return seed128bit{
        .seed_lo = mix_stafford13(low),
        .seed_hi = mix_stafford13(high),
    };
}

/// MD5 of the UTF-8 name, split into two big-endian 64-bit halves.
[[nodiscard]] auto seed_from_hash_of(std::string_view name) noexcept
    -> seed128bit;

} // namespace random_support

/// `Mth.getSeed(x, y, z)`: the positional hash both factories fold in.
[[nodiscard]] auto mth_get_seed(jint x, jint y, jint z) noexcept -> jlong;

/// Factory for coordinate-derived and name-derived random streams.
class positional_random_factory {
  public:
    positional_random_factory() = default;
    positional_random_factory(const positional_random_factory &) = delete;
    auto operator=(const positional_random_factory &)
        -> positional_random_factory & = delete;
    positional_random_factory(positional_random_factory &&) = delete;
    auto operator=(positional_random_factory &&)
        -> positional_random_factory & = delete;
    virtual ~positional_random_factory() = default;

    [[nodiscard]] virtual auto at(jint x, jint y, jint z)
        -> std::unique_ptr<random_source> = 0;
    [[nodiscard]] virtual auto from_hash_of(std::string_view name)
        -> std::unique_ptr<random_source> = 0;
    [[nodiscard]] virtual auto from_seed(jlong seed)
        -> std::unique_ptr<random_source> = 0;
};

/// `WorldgenRandom`: the re-seedable wrapper used by every stage.
///
/// Only `next(bits)` and `set_seed` delegate to the wrapped source; the rest of
/// the Java surface is the `BitRandomSource` composition over that method.
class worldgen_random {
  public:
    explicit worldgen_random(std::unique_ptr<random_source> source)
        : source_{std::move(source)} {}

    [[nodiscard]] auto next(int bits) -> jint;
    [[nodiscard]] auto next_int() -> jint;
    [[nodiscard]] auto next_int(jint bound) -> jint;
    [[nodiscard]] auto next_long() -> jlong;
    [[nodiscard]] auto next_float() -> float;
    [[nodiscard]] auto next_double() -> double;
    [[nodiscard]] auto next_boolean() -> bool;

    void set_seed(jlong seed);
    [[nodiscard]] auto fork() -> std::unique_ptr<random_source>;
    [[nodiscard]] auto fork_positional()
        -> std::unique_ptr<positional_random_factory>;

    /// `setDecorationSeed`: two odd multipliers derived from `seed`, folded
    /// with the chunk coordinates, and re-seeded. Returns the derived seed.
    auto set_decoration_seed(jlong seed, jint chunk_x, jint chunk_z) -> jlong;

    /// `setFeatureSeed`: `seed + index + 10000 * step`.
    void set_feature_seed(jlong seed, jint index, jint step);

    /// `setLargeFeatureSeed`: two multipliers, xor-combined with the seed.
    void set_large_feature_seed(jlong seed, jint chunk_x, jint chunk_z);

    /// `setLargeFeatureWithSalt`: structure placement's coordinate hash.
    void set_large_feature_with_salt(jlong seed, jint x, jint z, jint salt);

    [[nodiscard]] auto count() const noexcept -> int { return count_; }
    [[nodiscard]] auto source() noexcept -> random_source & { return *source_; }

  private:
    std::unique_ptr<random_source> source_;
    int count_{0};
};

/// The two generators selected by `noise_settings.legacy_random_source`.
enum class algorithm : std::uint8_t {
    legacy = 0,
    xoroshiro = 1,
};

[[nodiscard]] auto new_random_source(algorithm kind, jlong seed)
    -> std::unique_ptr<random_source>;

[[nodiscard]] auto new_positional_factory(algorithm kind, jlong seed)
    -> std::unique_ptr<positional_random_factory>;

} // namespace refrain::worldgen
