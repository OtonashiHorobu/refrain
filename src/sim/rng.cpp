// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/sim/rng.hpp>

#include <cstdint>

namespace refrain::sim {
namespace {

[[nodiscard]] constexpr auto rotl(std::uint64_t value, int amount) noexcept
    -> std::uint64_t {
    return (value << amount) | (value >> (64 - amount));
}

/// SplitMix64 is the standard seeder for xoshiro/xoroshiro state; it expands
/// one 64-bit seed into two well-distributed non-zero words.
[[nodiscard]] auto splitmix64(std::uint64_t &state) noexcept -> std::uint64_t {
    state += 0x9E3779B97F4A7C15ULL;
    auto value{state};
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

} // namespace

rng::rng(std::uint64_t seed) noexcept { this->seed(seed); }

void rng::seed(std::uint64_t value) noexcept {
    auto state{value};
    s0_ = splitmix64(state);
    s1_ = splitmix64(state);
    if (s0_ == 0 && s1_ == 0) {
        // The all-zero state is a fixed point of the xoshiro transition.
        s0_ = 0x9E3779B97F4A7C15ULL;
        s1_ = 0xBF58476D1CE4E5B9ULL;
    }
}

auto rng::next_u64() noexcept -> std::uint64_t {
    const auto result{rotl(s0_ + s1_, 17) + s0_};
    s1_ ^= s0_;
    s0_ = rotl(s0_, 49) ^ s1_ ^ (s1_ << 21U);
    s1_ = rotl(s1_, 28);
    return result;
}

auto rng::next_u32() noexcept -> std::uint32_t {
    return static_cast<std::uint32_t>(next_u64() >> 32U);
}

auto rng::next_bounded(std::uint32_t bound) noexcept -> std::uint32_t {
    if (bound == 0) {
        return 0;
    }
    // Lemire-style rejection with a 32-bit threshold removes modulo bias.
    const auto threshold{
        static_cast<std::uint32_t>(-static_cast<std::uint32_t>(bound)) % bound};
    for (;;) {
        const auto value{next_u32()};
        if (value >= threshold) {
            return value % bound;
        }
    }
}

auto rng::next_float() noexcept -> float {
    // 24 high bits -> exactly representable [0, 1) mantissa.
    return static_cast<float>(next_u64() >> 40U) * 0x1.0p-24F;
}

} // namespace refrain::sim
