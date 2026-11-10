// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::command {

/// Upper bound for one suggestion response. The bound is applied by
/// `suggestion_builder` so every parser shares it.
inline constexpr std::size_t max_suggestions = 1'024;

/// Upper bound for one suggestion entry and one tooltip.
inline constexpr std::size_t max_suggestion_text_bytes = 4'096;

/// One completion entry. `tooltip` is empty when the entry has no tooltip.
struct suggestion {
    std::string text{};
    std::string tooltip{};

    [[nodiscard]] auto has_tooltip() const noexcept -> bool {
        return !tooltip.empty();
    }

    friend auto operator==(const suggestion &, const suggestion &)
        -> bool = default;
};

/// Completion response. `[start, end)` is a byte range in the original input;
/// the protocol adapter sends the range verbatim. `truncated` reports that the
/// builder hit `max_suggestions` and dropped further entries.
struct suggestions {
    std::size_t start{0};
    std::size_t end{0};
    std::vector<suggestion> entries{};
    bool truncated{false};

    [[nodiscard]] auto empty() const noexcept -> bool {
        return entries.empty();
    }
};

/// Bounded, duplicate-free accumulator handed to argument parsers.
class suggestion_builder {
  public:
    suggestion_builder(std::size_t start, std::size_t end) noexcept;

    /// Add one entry. Empty text and duplicate text are ignored; over-long text
    /// is skipped and an over-long tooltip is truncated on a UTF-8 boundary.
    /// Once `max_suggestions` entries exist further additions only set
    /// `truncated`.
    void add(std::string_view text, std::string_view tooltip = {});

    [[nodiscard]] auto size() const noexcept -> std::size_t;
    [[nodiscard]] auto full() const noexcept -> bool;
    [[nodiscard]] auto build() && -> suggestions;

  private:
    suggestions out_{};
};

} // namespace refrain::command
