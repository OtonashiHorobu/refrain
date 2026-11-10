// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <cstdint>

namespace refrain::net {

/// Per-connection packet rate limiter. Disabled when the configured rate is
/// zero or negative; enabled limiters allow a one-second burst and refill
/// linearly.
class token_bucket {
  public:
    token_bucket() = default;
    explicit token_bucket(std::int32_t rate_per_second) {
        configure(rate_per_second);
    }

    void configure(std::int32_t rate_per_second) noexcept {
        rate_ = rate_per_second;
        tokens_ = rate_ > 0 ? static_cast<double>(rate_) : 0.0;
        last_ns_ = 0;
        initialized_ = false;
    }

    [[nodiscard]] auto enabled() const noexcept -> bool { return rate_ > 0; }

    /// Consume one token at `now_ns` (monotonic). A disabled limiter always
    /// allows.
    [[nodiscard]] auto allow(std::int64_t now_ns) noexcept -> bool {
        if (rate_ <= 0) {
            return true;
        }
        const auto capacity{static_cast<double>(rate_)};
        if (!initialized_) {
            initialized_ = true;
            last_ns_ = now_ns;
        } else if (now_ns > last_ns_) {
            const auto elapsed_seconds{static_cast<double>(now_ns - last_ns_) /
                                       1'000'000'000.0};
            tokens_ += elapsed_seconds * static_cast<double>(rate_);
            if (tokens_ > capacity) {
                tokens_ = capacity;
            }
            last_ns_ = now_ns;
        }
        if (tokens_ < 1.0) {
            return false;
        }
        tokens_ -= 1.0;
        return true;
    }

  private:
    std::int32_t rate_{0};
    double tokens_{0.0};
    std::int64_t last_ns_{0};
    bool initialized_{false};
};

/// True when a connection that last sent a packet at `last_ns` has been idle
/// for longer than `timeout_ns` at `now_ns`. A non-positive timeout disables
/// the check.
[[nodiscard]] inline auto idle_timed_out(std::int64_t last_ns,
                                         std::int64_t now_ns,
                                         std::int64_t timeout_ns) noexcept
    -> bool {
    return timeout_ns > 0 && now_ns - last_ns > timeout_ns;
}

} // namespace refrain::net
