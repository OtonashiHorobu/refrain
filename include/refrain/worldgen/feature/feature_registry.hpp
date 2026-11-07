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
#include <string_view>
#include <unordered_map>
#include <vector>

namespace refrain::worldgen::feature {

class tree_decorator;

class feature_registry {
  public:
    explicit feature_registry(block_state_registry &states)
        : states_{&states} {}

    [[nodiscard]] auto load(const feature_data_set &data) -> status;

    [[nodiscard]] auto placed() const
        -> const std::unordered_map<std::string, placed_feature_runtime> & {
        return placed_;
    }

  private:
    struct nested_placed_entry {
        std::string id{};
        std::string feature{};
        data::json::value placement{};
    };

    [[nodiscard]] auto decode_provider(const data::json::value &document)
        -> result<std::unique_ptr<state_provider>>;
    [[nodiscard]] auto nested_reference(const data::json::value &input)
        -> result<std::string>;
    [[nodiscard]] auto decode_fallen_decorators(const data::json::value &list)
        -> result<std::vector<std::unique_ptr<tree_decorator>>>;

    block_state_registry *states_{nullptr};
    std::unordered_map<std::string, state_provider_document>
        provider_documents_{};
    std::unordered_map<std::string, std::unique_ptr<state_provider>>
        providers_{};
    std::vector<nested_placed_entry> nested_placed_{};
    jint next_nested_{0};
    std::unordered_map<std::string, std::unique_ptr<feature>> features_{};
    std::unordered_map<std::string, placed_feature_runtime> placed_{};
};

} // namespace refrain::worldgen::feature
