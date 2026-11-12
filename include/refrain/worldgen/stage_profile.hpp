// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Per-stage wall-clock accounting for chunk generation. A generator that is
// given a profile records every stage it runs into it; production generation
// leaves the pointer null and pays one branch per stage. A profile is not
// thread-safe, so each worker of a batch needs its own instance.

#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::worldgen {

/// Accumulated nanoseconds of the generation stages a profile observed. Stage
/// names are stable literals (`terrain.fill`), so a reader can compare the
/// same name across runs and builds.
class stage_profile {
  public:
    struct entry {
        std::string name{};
        std::uint64_t nanoseconds{0};
        std::uint64_t samples{0};
    };

    /// Adds `nanoseconds` to one stage.
    void add(std::string_view stage, std::uint64_t nanoseconds);

    /// Adds `nanoseconds` to one configured feature type. The feature
    /// breakdown aggregates by the document type (`minecraft:tree`), so two
    /// placed features of the same type share one row.
    void add_feature(std::string_view type, std::uint64_t nanoseconds);

    [[nodiscard]] auto stages() const noexcept -> const std::vector<entry> & {
        return stages_;
    }
    [[nodiscard]] auto features() const noexcept
        -> const std::map<std::string, entry, std::less<>> & {
        return features_;
    }
    void clear() noexcept;

  private:
    /// Linear search: a generation run records a dozen stage names, so a
    /// vector beats a map and keeps the rows in first-observed order.
    [[nodiscard]] auto stage_entry(std::string_view stage) -> entry &;
    [[nodiscard]] auto feature_entry(std::string_view type) -> entry &;

    std::vector<entry> stages_{};
    std::map<std::string, entry, std::less<>> features_{};
};

enum class stage_kind : std::uint8_t {
    /// A named generation stage.
    stage,
    /// A configured feature type in the feature breakdown.
    feature,
};

/// Adds the time between construction and destruction to one row of `profile`.
/// A null profile makes the timer a no-op. `stop` records early, which lets a
/// caller time a prefix of the surrounding scope and continue with another
/// timer.
class stage_timer {
  public:
    stage_timer(stage_profile *profile, std::string_view name,
                stage_kind kind = stage_kind::stage) noexcept
        : profile_{profile}, name_{name}, kind_{kind} {
        if (profile_ != nullptr) {
            begin_ = clock_type::now();
            armed_ = true;
        }
    }
    stage_timer(const stage_timer &) = delete;
    auto operator=(const stage_timer &) -> stage_timer & = delete;
    ~stage_timer() { stop(); }

    void stop() noexcept {
        if (!armed_) {
            return;
        }
        armed_ = false;
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                clock_type::now() - begin_)
                .count();
        const auto nanoseconds = static_cast<std::uint64_t>(elapsed);
        if (kind_ == stage_kind::feature) {
            profile_->add_feature(name_, nanoseconds);
        } else {
            profile_->add(name_, nanoseconds);
        }
    }

  private:
    using clock_type = std::chrono::steady_clock;

    stage_profile *profile_{nullptr};
    std::string_view name_{};
    stage_kind kind_{stage_kind::stage};
    clock_type::time_point begin_{};
    bool armed_{false};
};

} // namespace refrain::worldgen
