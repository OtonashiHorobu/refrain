// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/command/reader.hpp>
#include <refrain/command/source.hpp>
#include <refrain/command/suggestions.hpp>
#include <refrain/command/tree.hpp>
#include <refrain/data/resource_location.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace refrain::command {

/// One parsed argument value.
///
/// Unquoted words and greedy strings are borrowed views into the command input;
/// quoted strings and enum values own their bytes; numbers and booleans are
/// stored without allocation. The duplicated string alternatives are what make
/// the borrowed form possible: `std::string_view` means "slice of the input"
/// and `std::string` means "decoded or canonicalized copy".
using argument_value =
    std::variant<std::monostate, bool, std::int64_t, double, std::string_view,
                 std::string, data::resource_location>;

/// Extensible argument parser. A parser reads one token from `in` (which is
/// positioned at the first non-whitespace byte of the argument), returns a
/// value, and optionally offers completions for a partial token.
///
/// The dispatcher owns a `unique_ptr<const argument_parser>` per argument node
/// and calls `parse()` during execution; `parse()` may read through the end of
/// the input for greedy parsers. Boundary handling (a token must be followed by
/// whitespace or end of input) is enforced by the dispatcher, not repeated in
/// every parser, so a parser must not consume the following separator.
///
/// No method may throw: failures are `refrain::result` values.
class argument_parser {
  public:
    argument_parser() = default;
    virtual ~argument_parser() = default;
    argument_parser(const argument_parser &) = delete;
    argument_parser &operator=(const argument_parser &) = delete;
    argument_parser(argument_parser &&) = default;
    argument_parser &operator=(argument_parser &&) = default;

    /// Tree properties for this parser. Called only while a tree snapshot is
    /// built, so returning a fresh descriptor by value is acceptable.
    [[nodiscard]] virtual auto descriptor() const -> parser_descriptor = 0;

    /// Parse one argument. The reader is placed at the argument start.
    [[nodiscard]] virtual auto parse(reader &in) const
        -> result<argument_value> = 0;

    /// Offer completions for the byte range `[token_start, cursor)` selected by
    /// the dispatcher. `prefix` is that slice of the input. The default
    /// implementation has no suggestions.
    virtual void suggest(std::string_view, const source &,
                         suggestion_builder &) const {}
};

} // namespace refrain::command
