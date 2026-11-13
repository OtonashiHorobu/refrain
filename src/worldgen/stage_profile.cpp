// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/worldgen/stage_profile.hpp>

namespace refrain::worldgen {

auto stage_profile::stage_entry(std::string_view stage) -> entry & {
    for (auto &row : stages_) {
        if (row.name == stage) {
            return row;
        }
    }
    stages_.push_back(entry{.name = std::string{stage}});
    return stages_.back();
}

auto stage_profile::feature_entry(std::string_view type) -> entry & {
    if (const auto found = features_.find(type); found != features_.end()) {
        return found->second;
    }
    return features_
        .emplace(std::string{type}, entry{.name = std::string{type}})
        .first->second;
}

void stage_profile::add(std::string_view stage, std::uint64_t nanoseconds) {
    auto &row = stage_entry(stage);
    row.nanoseconds += nanoseconds;
    ++row.samples;
}

void stage_profile::add_feature(std::string_view type,
                                std::uint64_t nanoseconds) {
    auto &row = feature_entry(type);
    row.nanoseconds += nanoseconds;
    ++row.samples;
}

void stage_profile::clear() noexcept {
    stages_.clear();
    features_.clear();
}

} // namespace refrain::worldgen
