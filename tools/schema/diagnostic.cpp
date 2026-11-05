// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <schema/diagnostic.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <string>

namespace refrain::schema {

auto format_location(const source_location &location) -> std::string {
    return std::format("{}:{}:{}", location.file, location.line,
                       location.column);
}

void diagnostics::error(source_location location, std::string message) {
    if (entries_.size() >= max_entries) {
        ++dropped_;
        return;
    }
    entries_.push_back(diagnostic{std::move(location), std::move(message)});
}

auto diagnostics::format() const -> std::string {
    constexpr std::size_t max_lines = 24;
    auto text{std::string{}};
    const auto limit{std::min(entries_.size(), max_lines)};
    for (std::size_t i = 0; i < limit; ++i) {
        const auto &entry{entries_[i]};
        if (!text.empty()) {
            text += '\n';
        }
        text += std::format("{}: error: {}", format_location(entry.location),
                            entry.message);
    }
    const auto total{size()};
    if (total > limit) {
        text += std::format("\n... and {} more error(s)", total - limit);
    }
    return text;
}

} // namespace refrain::schema
