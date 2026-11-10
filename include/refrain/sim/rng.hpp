// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <cstdint>

namespace refrain::sim {

/// Small deterministic PRNG for the simulation (xoshiro128++).
///
/// Reproducibility is the requirement here: the same world seed and input
/// script must produce the same tick hash. Vanilla's per-subsystem
/// random streams (legacy LCG / Xoroshiro) and exact call ordering are
/// not reproduced yet; this generator is isolated so replacing it does not
/// touch the world model.
///
/// Not thread-safe: the sim thread owns the instance.
class rng {
  public:
    explicit rng(std::uint64_t seed = 0) noexcept;

    void seed(std::uint64_t value) noexcept;

    [[nodiscard]] auto next_u64() noexcept -> std::uint64_t;
    [[nodiscard]] auto next_u32() noexcept -> std::uint32_t;

    /// Uniform value in `[0, bound)`; `bound == 0` returns 0.
    [[nodiscard]] auto next_bounded(std::uint32_t bound) noexcept
        -> std::uint32_t;

    /// Uniform float in `[0, 1)` with 24 bits of mantissa.
    [[nodiscard]] auto next_float() noexcept -> float;

    [[nodiscard]] auto state0() const noexcept -> std::uint64_t { return s0_; }
    [[nodiscard]] auto state1() const noexcept -> std::uint64_t { return s1_; }

  private:
    std::uint64_t s0_{0};
    std::uint64_t s1_{0};
};

} // namespace refrain::sim
