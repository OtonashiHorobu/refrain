// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Quoting of one argument for a Windows command line.
//
// `CreateProcessW` takes a single command-line string, and the child's C
// runtime (like `CommandLineToArgvW`) splits it back into arguments with the
// documented backslash rules. The subprocess backend and its round-trip test
// share this header so the rules live in one place and the test can prove the
// built command line yields the arguments the caller passed.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace refrain::platform::windows_detail {

/// Quote `argument` so the child's `CommandLineToArgvW`-style parser yields it
/// unchanged.
///
/// The parser treats backslashes literally except immediately before a double
/// quotation mark. Inside a quoted run, 2n backslashes before a quote become n
/// literal backslashes plus a delimiter, and 2n+1 backslashes before a quote
/// become n literal backslashes plus a literal quote. The closing quote this
/// function appends is a delimiter, so a backslash run that reaches the end of
/// the argument is doubled instead of being left able to escape that quote.
/// An argument with no whitespace and no quote needs no quoting; an empty
/// argument is written as `""`.
[[nodiscard]] inline auto quote_argument(std::string_view argument)
    -> std::string {
    if (!argument.empty() &&
        argument.find_first_of(" \t\"") == std::string_view::npos) {
        return std::string{argument};
    }
    std::string quoted{"\""};
    quoted.reserve(argument.size() + 2);
    for (std::size_t index{0}; index < argument.size();) {
        std::size_t backslashes{0};
        while (index < argument.size() && argument[index] == '\\') {
            ++backslashes;
            ++index;
        }
        if (index == argument.size()) {
            // The closing quote follows: double the run so the parser keeps
            // every backslash literal instead of using the last one to escape
            // the quote.
            quoted.append(backslashes * 2, '\\');
            break;
        }
        if (argument[index] == '"') {
            // 2n+1 backslashes then a quote: n survive and the quote is
            // literal.
            quoted.append(backslashes * 2 + 1, '\\');
            quoted += '"';
            ++index;
            continue;
        }
        quoted.append(backslashes, '\\');
        quoted += argument[index];
        ++index;
    }
    quoted += '"';
    return quoted;
}

} // namespace refrain::platform::windows_detail
