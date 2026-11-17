// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/command/suggestions.hpp>

#include <algorithm>
#include <utility>

namespace refrain::command {

namespace {

[[nodiscard]] auto utf8_prefix(std::string_view text, std::size_t limit)
    -> std::string_view {
    if (text.size() <= limit) {
        return text;
    }
    // Move back over continuation bytes so the truncated tooltip stays valid
    // UTF-8. text[limit] is a valid index because text is longer than limit.
    while (limit > 0 &&
           (static_cast<unsigned char>(text[limit]) & 0xC0U) == 0x80U) {
        --limit;
    }
    return text.substr(0, limit);
}

} // namespace

suggestion_builder::suggestion_builder(std::size_t start,
                                       std::size_t end) noexcept {
    out_.start = start;
    out_.end = end;
}

void suggestion_builder::add(std::string_view text, std::string_view tooltip) {
    if (text.empty() || text.size() > max_suggestion_text_bytes) {
        return;
    }
    if (out_.entries.size() >= max_suggestions) {
        out_.truncated = true;
        return;
    }
    const auto duplicate =
        std::ranges::any_of(out_.entries, [text](const suggestion &entry) {
            return entry.text == text;
        });
    if (duplicate) {
        return;
    }
    suggestion entry{};
    entry.text = std::string{text};
    entry.tooltip =
        std::string{utf8_prefix(tooltip, max_suggestion_text_bytes)};
    out_.entries.push_back(std::move(entry));
}

auto suggestion_builder::size() const noexcept -> std::size_t {
    return out_.entries.size();
}

auto suggestion_builder::full() const noexcept -> bool {
    return out_.entries.size() >= max_suggestions;
}

auto suggestion_builder::build() && -> suggestions { return std::move(out_); }

} // namespace refrain::command
