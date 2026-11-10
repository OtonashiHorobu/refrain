// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/command/dispatcher.hpp>

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

auto dispatcher::suggest(std::string_view input, std::size_t cursor,
                         const source &src) const -> suggestions {
    const auto clamped = std::min(cursor, input.size());
    if (input.size() > max_command_bytes) {
        return suggestions{clamped, clamped, {}, false};
    }

    auto token_start = clamped;
    while (token_start > 0 && !reader::is_whitespace(input[token_start - 1])) {
        --token_start;
    }
    const auto prefix = input.substr(0, token_start);
    auto parsed = parse_command(prefix, src, true);
    if (!parsed) {
        return suggestions{clamped, clamped, {}, false};
    }

    const auto token = input.substr(token_start, clamped - token_start);
    suggestion_builder builder{token_start, clamped};
    for (const auto child : nodes_[parsed->node].children) {
        if (!is_allowed(child, src)) {
            continue;
        }
        const auto &entry = nodes_[child];
        if (entry.kind == node_kind::literal) {
            if (entry.name.starts_with(token)) {
                builder.add(entry.name, entry.tooltip);
            }
            continue;
        }
        if (entry.parser != nullptr) {
            entry.parser->suggest(token, src, builder);
        }
    }
    return std::move(builder).build();
}

} // namespace refrain::command
