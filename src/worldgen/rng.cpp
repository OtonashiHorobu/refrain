// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/rng.hpp>

#include <refrain/worldgen/md5.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <string_view>

namespace refrain::worldgen {
namespace {

constexpr int float_denominator_bits = 24;
constexpr float float_multiplier = 5.9604645E-8F;            // 2^-24
constexpr double double_multiplier = 1.1102230246251565E-16; // 2^-53
constexpr jlong mask_32 = 0xFFFFFFFFLL;

[[nodiscard]] auto be64(const std::uint8_t *bytes) noexcept -> jlong {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8U) | bytes[i];
    }
    return static_cast<jlong>(value);
}

} // namespace

// --- random_source defaults (BitRandomSource) ------------------------------

auto random_source::next_int() -> jint { return next(32); }

auto random_source::next_int(jint bound) -> jint {
    assert(bound > 0);
    if ((bound & (bound - 1)) == 0) {
        return static_cast<jint>((static_cast<jlong>(bound) * next(31)) >> 31);
    }
    jint sample = 0;
    jint modulo = 0;
    do {
        sample = next(31);
        modulo = sample % bound;
    } while (wrap_add(wrap_sub(sample, modulo), bound - 1) < 0);
    return modulo;
}

auto random_source::next_long() -> jlong {
    const jint upper = next(32);
    const jint lower = next(32);
    return (static_cast<jlong>(upper) << 32) + static_cast<jlong>(lower);
}

auto random_source::next_float() -> float {
    return static_cast<float>(next(float_denominator_bits)) * float_multiplier;
}

auto random_source::next_double() -> double {
    const jint upper = next(26);
    const jint lower = next(27);
    const jlong combined =
        (static_cast<jlong>(upper) << 27) + static_cast<jlong>(lower);
    return static_cast<double>(combined) * double_multiplier;
}

auto random_source::next_boolean() -> bool { return next(1) != 0; }

void random_source::consume_count(int rounds) {
    for (int i = 0; i < rounds; ++i) {
        (void)next_int();
    }
}

// --- legacy LCG -------------------------------------------------------------

auto legacy_random_source::next(int bits) -> jint {
    const auto next_state =
        static_cast<jlong>((static_cast<std::uint64_t>(state_) *
                                static_cast<std::uint64_t>(multiplier) +
                            static_cast<std::uint64_t>(increment)) &
                           static_cast<std::uint64_t>(mask));
    state_ = next_state;
    return static_cast<jint>(static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(state_) >> (48 - bits)));
}

void legacy_random_source::set_seed(jlong seed) {
    state_ = (seed ^ multiplier) & mask;
}

auto legacy_random_source::fork() -> std::unique_ptr<random_source> {
    return std::make_unique<legacy_random_source>(next_long());
}

// --- xoroshiro --------------------------------------------------------------

auto xoroshiro128pp::next_long() noexcept -> jlong {
    const jlong s0 = seed_lo_;
    jlong s1 = seed_hi_;
    const jlong sum = static_cast<jlong>(static_cast<std::uint64_t>(s0) +
                                         static_cast<std::uint64_t>(s1));
    const jlong result =
        static_cast<jlong>(static_cast<std::uint64_t>(rotate_left(sum, 17)) +
                           static_cast<std::uint64_t>(s0));
    s1 ^= s0;
    seed_lo_ = static_cast<jlong>(
        static_cast<std::uint64_t>(rotate_left(s0, 49) ^ s1) ^
        (static_cast<std::uint64_t>(s1) << 21U));
    seed_hi_ = rotate_left(s1, 28);
    return result;
}

xoroshiro_random_source::xoroshiro_random_source(jlong seed) noexcept {
    set_seed(seed);
}

void xoroshiro_random_source::set_seed(jlong seed) {
    const auto upgraded = random_support::upgrade_seed_to_128bit(seed);
    generator_ = xoroshiro128pp{upgraded.seed_lo, upgraded.seed_hi};
}

auto xoroshiro_random_source::next(int bits) -> jint {
    return static_cast<jint>(next_bits(bits));
}

auto xoroshiro_random_source::next_int() -> jint {
    return static_cast<jint>(generator_.next_long());
}

auto xoroshiro_random_source::next_int(jint bound) -> jint {
    assert(bound > 0);
    jlong bits = static_cast<std::uint32_t>(next_int());
    jlong multiplied = bits * static_cast<jlong>(bound);
    jlong fractional = multiplied & mask_32;
    if (fractional < bound) {
        const jint threshold = remainder_unsigned(wrap_add(~bound, 1), bound);
        while (fractional < threshold) {
            bits = static_cast<std::uint32_t>(next_int());
            multiplied = bits * static_cast<jlong>(bound);
            fractional = multiplied & mask_32;
        }
    }
    return static_cast<jint>(multiplied >> 32);
}

auto xoroshiro_random_source::next_long() -> jlong {
    return generator_.next_long();
}

auto xoroshiro_random_source::next_double() -> double {
    return static_cast<double>(next_bits(53)) * double_multiplier;
}

auto xoroshiro_random_source::fork() -> std::unique_ptr<random_source> {
    const jlong low = generator_.next_long();
    const jlong high = generator_.next_long();
    return std::make_unique<xoroshiro_random_source>(low, high);
}

// --- positional factories ---------------------------------------------------

class legacy_positional_random_factory final
    : public positional_random_factory {
  public:
    explicit legacy_positional_random_factory(jlong seed) noexcept
        : seed_{seed} {}

    [[nodiscard]] auto at(jint x, jint y, jint z)
        -> std::unique_ptr<random_source> override {
        return std::make_unique<legacy_random_source>(mth_get_seed(x, y, z) ^
                                                      seed_);
    }

    [[nodiscard]] auto from_hash_of(std::string_view name)
        -> std::unique_ptr<random_source> override {
        return std::make_unique<legacy_random_source>(
            static_cast<jlong>(java_string_hash(name)) ^ seed_);
    }

    [[nodiscard]] auto from_seed(jlong seed)
        -> std::unique_ptr<random_source> override {
        return std::make_unique<legacy_random_source>(seed);
    }

  private:
    jlong seed_{0};
};

class xoroshiro_positional_random_factory final
    : public positional_random_factory {
  public:
    xoroshiro_positional_random_factory(jlong seed_lo, jlong seed_hi) noexcept
        : seed_lo_{seed_lo}, seed_hi_{seed_hi} {}

    [[nodiscard]] auto at(jint x, jint y, jint z)
        -> std::unique_ptr<random_source> override {
        return std::make_unique<xoroshiro_random_source>(
            mth_get_seed(x, y, z) ^ seed_lo_, seed_hi_);
    }

    [[nodiscard]] auto from_hash_of(std::string_view name)
        -> std::unique_ptr<random_source> override {
        const seed128bit seed = random_support::seed_from_hash_of(name);
        return std::make_unique<xoroshiro_random_source>(
            seed.seed_lo ^ seed_lo_, seed.seed_hi ^ seed_hi_);
    }

    [[nodiscard]] auto from_seed(jlong seed)
        -> std::unique_ptr<random_source> override {
        return std::make_unique<xoroshiro_random_source>(seed ^ seed_lo_,
                                                         seed ^ seed_hi_);
    }

  private:
    jlong seed_lo_{0};
    jlong seed_hi_{0};
};

auto legacy_random_source::fork_positional()
    -> std::unique_ptr<positional_random_factory> {
    return std::make_unique<legacy_positional_random_factory>(next_long());
}

auto xoroshiro_random_source::fork_positional()
    -> std::unique_ptr<positional_random_factory> {
    const jlong low = generator_.next_long();
    const jlong high = generator_.next_long();
    return std::make_unique<xoroshiro_positional_random_factory>(low, high);
}

// --- seed derivation --------------------------------------------------------

namespace random_support {

auto seed_from_hash_of(std::string_view name) noexcept -> seed128bit {
    const auto digest = md5(name);
    return seed128bit{
        .seed_lo = be64(digest.data()),
        .seed_hi = be64(digest.data() + 8),
    };
}

} // namespace random_support

auto mth_get_seed(jint x, jint y, jint z) noexcept -> jlong {
    jlong seed = static_cast<jlong>(wrap_mul(x, 3129871)) ^
                 (static_cast<jlong>(z) * 116129781LL) ^ static_cast<jlong>(y);
    seed =
        static_cast<jlong>(static_cast<std::uint64_t>(seed) *
                               static_cast<std::uint64_t>(seed) * 42317861ULL +
                           static_cast<std::uint64_t>(seed) * 11ULL);
    return seed >> 16;
}

// --- worldgen_random --------------------------------------------------------

auto worldgen_random::next(int bits) -> jint {
    ++count_;
    return source_->next(bits);
}

auto worldgen_random::next_int() -> jint { return next(32); }

auto worldgen_random::next_int(jint bound) -> jint {
    assert(bound > 0);
    if ((bound & (bound - 1)) == 0) {
        return static_cast<jint>((static_cast<jlong>(bound) * next(31)) >> 31);
    }
    jint sample = 0;
    jint modulo = 0;
    do {
        sample = next(31);
        modulo = sample % bound;
    } while (wrap_add(wrap_sub(sample, modulo), bound - 1) < 0);
    return modulo;
}

auto worldgen_random::next_long() -> jlong {
    const jint upper = next(32);
    const jint lower = next(32);
    return (static_cast<jlong>(upper) << 32) + static_cast<jlong>(lower);
}

auto worldgen_random::next_float() -> float {
    return static_cast<float>(next(float_denominator_bits)) * float_multiplier;
}

auto worldgen_random::next_double() -> double {
    const jint upper = next(26);
    const jint lower = next(27);
    const jlong combined =
        (static_cast<jlong>(upper) << 27) + static_cast<jlong>(lower);
    return static_cast<double>(combined) * double_multiplier;
}

auto worldgen_random::next_boolean() -> bool { return next(1) != 0; }

void worldgen_random::set_seed(jlong seed) { source_->set_seed(seed); }

auto worldgen_random::fork() -> std::unique_ptr<random_source> {
    return source_->fork();
}

auto worldgen_random::fork_positional()
    -> std::unique_ptr<positional_random_factory> {
    return source_->fork_positional();
}

auto worldgen_random::set_decoration_seed(jlong seed, jint chunk_x,
                                          jint chunk_z) -> jlong {
    set_seed(seed);
    const jlong x_scale = next_long() | 1;
    const jlong z_scale = next_long() | 1;
    const jlong result = ((static_cast<jlong>(chunk_x) * x_scale) +
                          (static_cast<jlong>(chunk_z) * z_scale)) ^
                         seed;
    set_seed(result);
    return result;
}

void worldgen_random::set_feature_seed(jlong seed, jint index, jint step) {
    set_seed(seed + static_cast<jlong>(index) + (10000LL * step));
}

void worldgen_random::set_large_feature_seed(jlong seed, jint chunk_x,
                                             jint chunk_z) {
    set_seed(seed);
    const jlong x_scale = next_long();
    const jlong z_scale = next_long();
    set_seed((static_cast<jlong>(chunk_x) * x_scale) ^
             (static_cast<jlong>(chunk_z) * z_scale) ^ seed);
}

void worldgen_random::set_large_feature_with_salt(jlong seed, jint x, jint z,
                                                  jint salt) {
    set_seed((static_cast<jlong>(x) * 341873128712LL) +
             (static_cast<jlong>(z) * 132897987541LL) + seed + salt);
}

// --- construction -----------------------------------------------------------

auto new_random_source(algorithm kind, jlong seed)
    -> std::unique_ptr<random_source> {
    if (kind == algorithm::legacy) {
        return std::make_unique<legacy_random_source>(seed);
    }
    return std::make_unique<xoroshiro_random_source>(seed);
}

auto new_positional_factory(algorithm kind, jlong seed)
    -> std::unique_ptr<positional_random_factory> {
    return new_random_source(kind, seed)->fork_positional();
}

} // namespace refrain::worldgen
