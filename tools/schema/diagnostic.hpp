// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Diagnostics for the RSC/RIDL schema toolchain. Every parser/sema error is
// reported with file/line/column so schema authors can fix it without reading
// generator internals.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::schema {

struct source_location {
    std::string file;
    std::uint32_t line{1};
    std::uint32_t column{1};
};

[[nodiscard]] auto format_location(const source_location &location)
    -> std::string;

struct diagnostic {
    source_location location;
    std::string message;
};

/// Accumulates diagnostics in source order. Formatting is bounded so a broken
/// schema cannot flood CI logs, and storage is bounded so a pathological schema
/// cannot make the tool's memory use grow without limit either.
class diagnostics {
  public:
    /// Upper bound on stored entries. Errors beyond this are only counted.
    static constexpr std::size_t max_entries{256};

    void error(source_location location, std::string message);

    [[nodiscard]] auto empty() const noexcept -> bool {
        return entries_.empty() && dropped_ == 0;
    }

    /// Total number of reported errors, including dropped ones.
    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return entries_.size() + dropped_;
    }

    /// Errors not stored because `max_entries` was already reached.
    [[nodiscard]] auto dropped() const noexcept -> std::size_t {
        return dropped_;
    }

    [[nodiscard]] auto entries() const noexcept
        -> const std::vector<diagnostic> & {
        return entries_;
    }

    /// Multi-line "file:line:column: error: message" summary.
    [[nodiscard]] auto format() const -> std::string;

  private:
    std::vector<diagnostic> entries_;
    std::size_t dropped_{0};
};

} // namespace refrain::schema
