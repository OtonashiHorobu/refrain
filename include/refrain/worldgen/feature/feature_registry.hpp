// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Builds the runtime feature objects and placed-feature table from the data
// set. Object lifetimes are owned here; placed entries point into `features`.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/worldgen/chunk/block_state_registry.hpp>
#include <refrain/worldgen/feature/feature.hpp>
#include <refrain/worldgen/feature/feature_data.hpp>
#include <refrain/worldgen/feature/feature_stage.hpp>

#include <memory>
#include <string>
#include <unordered_map>

namespace refrain::worldgen::feature {

class feature_registry {
  public:
    explicit feature_registry(const block_state_registry &states)
        : states_{&states} {}

    [[nodiscard]] auto load(const feature_data_set &data) -> status;

    [[nodiscard]] auto placed() const
        -> const std::unordered_map<std::string, placed_feature_runtime> & {
        return placed_;
    }

  private:
    const block_state_registry *states_{nullptr};
    std::unordered_map<std::string, std::unique_ptr<feature>> features_{};
    std::unordered_map<std::string, placed_feature_runtime> placed_{};
};

} // namespace refrain::worldgen::feature
