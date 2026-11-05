// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/error.hpp>

#include <algorithm>
#include <array>
#include <compare>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::core {

/// A SemVer 2.0.0 version value.
///
/// The engine declares all of its own artifact/contract versions as SemVer;
/// third-party package versions (Conan, vanilla DataVersion, pack formats)
/// stay with their upstream conventions and are explicitly excluded.
/// Build metadata is parsed/preserved but ignored for precedence.
///
/// This header is intentionally self-contained (header-only, no engine
/// library dependency) so the schema toolchain and the runtime share the exact
/// same version/range semantics.
struct semver {
    std::uint32_t major{0};
    std::uint32_t minor{0};
    std::uint32_t patch{0};
    /// Dot-separated prerelease identifiers, without the leading '-'.
    /// Empty means a release version.
    std::string prerelease{};
    /// Dot-separated build metadata identifiers, without the leading '+'.
    std::string build{};

    /// `major.minor.patch[-prerelease][+build]`.
    [[nodiscard]] auto text() const -> std::string {
        auto out{std::to_string(major) + '.' + std::to_string(minor) + '.' +
                 std::to_string(patch)};
        if (!prerelease.empty()) {
            out += '-' + prerelease;
        }
        if (!build.empty()) {
            out += '+' + build;
        }
        return out;
    }

    [[nodiscard]] auto is_prerelease() const noexcept -> bool {
        return !prerelease.empty();
    }

    friend auto operator==(const semver &, const semver &) -> bool = default;

    /// Strict SemVer 2.0.0 parse. Rejects leading zeros (prerelease only, as
    /// the spec requires: numeric build identifiers may carry them), partial
    /// versions, empty identifiers, and values above 2^32-1. `+build` is
    /// preserved.
    [[nodiscard]] static auto parse(std::string_view text) -> result<semver>;
};

/// SemVer precedence. Declared before the detail helpers so range evaluation
/// can reuse it (defined below).
[[nodiscard]] auto compare(const semver &lhs, const semver &rhs)
    -> std::strong_ordering;

enum class range_operator : std::uint8_t {
    equal,
    greater,
    greater_equal,
    less,
    less_equal,
    caret,
    tilde,
};

struct version_comparator {
    range_operator kind{range_operator::equal};
    semver bound{};
};

/// One or more AND groups separated by `||`.
struct semver_range {
    std::vector<std::vector<version_comparator>> alternatives{};
};

namespace semver_detail {

inline constexpr std::size_t k_max_text_length{160};
inline constexpr std::size_t k_max_component_length{10};
inline constexpr std::uint32_t k_max_component_value{
    std::numeric_limits<std::uint32_t>::max()};

[[nodiscard]] inline auto fail(error_code code, std::string message) -> error {
    return make_error(code, std::move(message));
}

[[nodiscard]] inline auto is_digit(char value) noexcept -> bool {
    return value >= '0' && value <= '9';
}

[[nodiscard]] inline auto is_identifier_char(char value) noexcept -> bool {
    return is_digit(value) || (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') || value == '-';
}

[[nodiscard]] inline auto all_digits(std::string_view text) noexcept -> bool {
    return !text.empty() && std::all_of(text.begin(), text.end(), is_digit);
}

[[nodiscard]] inline auto trim(std::string_view text) noexcept
    -> std::string_view {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

/// Numeric component with SemVer's "no leading zeros" rule.
[[nodiscard]] inline auto parse_component(std::string_view text,
                                          std::uint32_t &out,
                                          std::string_view what) -> status {
    if (text.empty()) {
        return std::unexpected{
            fail(error_code::parse_error,
                 "version: empty " + std::string{what} + " component")};
    }
    if (text.size() > k_max_component_length) {
        return std::unexpected{
            fail(error_code::parse_error,
                 "version: " + std::string{what} + " component is too long")};
    }
    if (text.size() > 1 && text.front() == '0') {
        return std::unexpected{
            fail(error_code::parse_error, "version: " + std::string{what} +
                                              " component has a leading zero")};
    }
    std::uint64_t value{0};
    for (const char character : text) {
        if (!is_digit(character)) {
            return std::unexpected{
                fail(error_code::parse_error, "version: " + std::string{what} +
                                                  " component is not numeric")};
        }
        value = value * 10U + static_cast<std::uint64_t>(character - '0');
        if (value > k_max_component_value) {
            return std::unexpected{fail(error_code::parse_error,
                                        "version: " + std::string{what} +
                                            " component is out of range")};
        }
    }
    out = static_cast<std::uint32_t>(value);
    return status_ok();
}

/// Split `text` on `'.'`, rejecting empty identifiers.
///
/// `allow_leading_zeros` must be true only for build metadata: SemVer §10
/// restricts the "numeric identifiers MUST NOT include leading zeroes" rule to
/// the prerelease, while a build identifier may be any `<digits>` (`1.0.0+001`
/// is valid).
[[nodiscard]] inline auto split_identifiers(std::string_view text,
                                            std::string_view what,
                                            bool allow_leading_zeros = false)
    -> result<std::vector<std::string_view>> {
    std::vector<std::string_view> parts{};
    std::size_t start{0};
    while (true) {
        const auto dot{text.find('.', start)};
        const auto part{text.substr(
            start, dot == std::string_view::npos ? dot : dot - start)};
        if (part.empty()) {
            return std::unexpected{
                fail(error_code::parse_error, "version: " + std::string{what} +
                                                  " has an empty identifier")};
        }
        for (const char character : part) {
            if (!is_identifier_char(character)) {
                return std::unexpected{fail(error_code::parse_error,
                                            "version: " + std::string{what} +
                                                " has an invalid character")};
            }
        }
        if (!allow_leading_zeros && all_digits(part) && part.size() > 1 &&
            part.front() == '0') {
            return std::unexpected{
                fail(error_code::parse_error,
                     "version: " + std::string{what} +
                         " has a numeric identifier with a leading zero")};
        }
        parts.push_back(part);
        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
        if (start >= text.size()) {
            return std::unexpected{
                fail(error_code::parse_error,
                     "version: " + std::string{what} + " has a trailing dot")};
        }
    }
    return parts;
}

/// Compare one prerelease identifier (numeric vs alphanumeric rules).
[[nodiscard]] inline auto compare_identifier(std::string_view lhs,
                                             std::string_view rhs)
    -> std::strong_ordering {
    const bool lhs_numeric{all_digits(lhs)};
    const bool rhs_numeric{all_digits(rhs)};
    if (lhs_numeric && rhs_numeric) {
        // Numeric identifiers never have leading zeros; a longer number is
        // larger.
        if (lhs.size() != rhs.size()) {
            return lhs.size() < rhs.size() ? std::strong_ordering::less
                                           : std::strong_ordering::greater;
        }
        return lhs.compare(rhs) <=> 0;
    }
    if (lhs_numeric != rhs_numeric) {
        // Numeric identifiers have lower precedence than alphanumeric ones.
        return lhs_numeric ? std::strong_ordering::less
                           : std::strong_ordering::greater;
    }
    return lhs.compare(rhs) <=> 0;
}

[[nodiscard]] inline auto compare_prerelease(std::string_view lhs,
                                             std::string_view rhs)
    -> std::strong_ordering {
    if (lhs.empty() || rhs.empty()) {
        // Release > prerelease.
        if (lhs.empty() && rhs.empty()) {
            return std::strong_ordering::equal;
        }
        return lhs.empty() ? std::strong_ordering::greater
                           : std::strong_ordering::less;
    }
    const auto lhs_parts{split_identifiers(lhs, "prerelease")};
    const auto rhs_parts{split_identifiers(rhs, "prerelease")};
    if (!lhs_parts || !rhs_parts) {
        // semver::parse() validated both, so this is unreachable; compare
        // textually as a defensive fallback.
        return lhs.compare(rhs) <=> 0;
    }
    const auto count{std::min(lhs_parts->size(), rhs_parts->size())};
    for (std::size_t i = 0; i < count; ++i) {
        const auto order{compare_identifier((*lhs_parts)[i], (*rhs_parts)[i])};
        if (order != std::strong_ordering::equal) {
            return order;
        }
    }
    if (lhs_parts->size() == rhs_parts->size()) {
        return std::strong_ordering::equal;
    }
    return lhs_parts->size() < rhs_parts->size()
               ? std::strong_ordering::less
               : std::strong_ordering::greater;
}

/// Upper bound of `^bound` (npm caret semantics). Only call when
/// `caret_has_upper(bound)` holds.
[[nodiscard]] inline auto caret_upper(const semver &bound) -> semver {
    if (bound.major != 0) {
        return semver{.major = bound.major + 1};
    }
    if (bound.minor != 0) {
        return semver{.minor = bound.minor + 1};
    }
    return semver{.patch = bound.patch + 1};
}

/// Upper bound of `~bound`. Only call when `tilde_has_upper(bound)` holds.
[[nodiscard]] inline auto tilde_upper(const semver &bound) -> semver {
    return semver{.major = bound.major, .minor = bound.minor + 1};
}

/// True when `^bound`'s exclusive upper bound is representable. Incrementing a
/// component of `UINT32_MAX` wraps to 0, which would turn the range into an
/// empty set (even the bound itself would stop matching).
[[nodiscard]] inline auto caret_has_upper(const semver &bound) noexcept
    -> bool {
    if (bound.major != 0) {
        return bound.major != k_max_component_value;
    }
    if (bound.minor != 0) {
        return bound.minor != k_max_component_value;
    }
    return bound.patch != k_max_component_value;
}

/// True when `~bound`'s exclusive upper bound is representable.
[[nodiscard]] inline auto tilde_has_upper(const semver &bound) noexcept
    -> bool {
    return bound.minor != k_max_component_value;
}

[[nodiscard]] inline auto eval_comparator(const semver &value,
                                          const version_comparator &comparator)
    -> bool {
    const auto order{compare(value, comparator.bound)};
    switch (comparator.kind) {
    case range_operator::equal:
        return order == std::strong_ordering::equal;
    case range_operator::greater:
        return order == std::strong_ordering::greater;
    case range_operator::greater_equal:
        return order != std::strong_ordering::less;
    case range_operator::less:
        return order == std::strong_ordering::less;
    case range_operator::less_equal:
        return order != std::strong_ordering::greater;
    case range_operator::caret:
        // With no representable upper bound the range degrades to its lower
        // bound ("`>= bound`") instead of wrapping into an empty set.
        return order != std::strong_ordering::less &&
               (!caret_has_upper(comparator.bound) ||
                compare(value, caret_upper(comparator.bound)) ==
                    std::strong_ordering::less);
    case range_operator::tilde:
        return order != std::strong_ordering::less &&
               (!tilde_has_upper(comparator.bound) ||
                compare(value, tilde_upper(comparator.bound)) ==
                    std::strong_ordering::less);
    }
    return false;
}

} // namespace semver_detail

/// SemVer precedence (-1 / 0 / +1 as a three-way comparison). Build metadata
/// does not participate.
[[nodiscard]] inline auto compare(const semver &lhs, const semver &rhs)
    -> std::strong_ordering {
    if (lhs.major != rhs.major) {
        return lhs.major < rhs.major ? std::strong_ordering::less
                                     : std::strong_ordering::greater;
    }
    if (lhs.minor != rhs.minor) {
        return lhs.minor < rhs.minor ? std::strong_ordering::less
                                     : std::strong_ordering::greater;
    }
    if (lhs.patch != rhs.patch) {
        return lhs.patch < rhs.patch ? std::strong_ordering::less
                                     : std::strong_ordering::greater;
    }
    return semver_detail::compare_prerelease(lhs.prerelease, rhs.prerelease);
}

inline auto semver::parse(std::string_view text) -> result<semver> {
    using namespace semver_detail;
    if (text.empty()) {
        return std::unexpected{
            fail(error_code::parse_error, "version: empty string")};
    }
    if (text.size() > k_max_text_length) {
        return std::unexpected{
            fail(error_code::parse_error, "version: too long")};
    }

    // Build metadata (`+build`) is ignored for precedence but preserved.
    auto core_and_pre{text};
    std::string build{};
    if (const auto plus{text.find('+')}; plus != std::string_view::npos) {
        if (text.find('+', plus + 1) != std::string_view::npos) {
            return std::unexpected{
                fail(error_code::parse_error, "version: more than one '+'")};
        }
        core_and_pre = text.substr(0, plus);
        const auto build_text{text.substr(plus + 1)};
        // SemVer §10 permits leading zeros in numeric build identifiers.
        const auto parts{split_identifiers(build_text, "build metadata", true)};
        if (!parts) {
            return std::unexpected{parts.error()};
        }
        build = std::string{build_text};
    }

    auto core{core_and_pre};
    std::string prerelease{};
    if (const auto dash{core_and_pre.find('-')};
        dash != std::string_view::npos) {
        core = core_and_pre.substr(0, dash);
        const auto pre_text{core_and_pre.substr(dash + 1)};
        const auto parts{split_identifiers(pre_text, "prerelease")};
        if (!parts) {
            return std::unexpected{parts.error()};
        }
        prerelease = std::string{pre_text};
    }

    std::array<std::string_view, 3> components{};
    std::size_t count{0};
    std::size_t start{0};
    while (true) {
        if (count == components.size()) {
            return std::unexpected{
                fail(error_code::parse_error,
                     "version: expected exactly major.minor.patch")};
        }
        const auto dot{core.find('.', start)};
        components[count++] = core.substr(
            start, dot == std::string_view::npos ? dot : dot - start);
        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }
    if (count != 3) {
        return std::unexpected{
            fail(error_code::parse_error,
                 "version: expected exactly major.minor.patch")};
    }

    semver parsed{};
    if (auto status{parse_component(components[0], parsed.major, "major")};
        !status) {
        return std::unexpected{status.error()};
    }
    if (auto status{parse_component(components[1], parsed.minor, "minor")};
        !status) {
        return std::unexpected{status.error()};
    }
    if (auto status{parse_component(components[2], parsed.patch, "patch")};
        !status) {
        return std::unexpected{status.error()};
    }
    parsed.prerelease = std::move(prerelease);
    parsed.build = std::move(build);
    return parsed;
}

/// Parse a range: `=1.2.3`, `>=1.2.3`, `>1.2.3`, `<1.2.3`, `<=1.2.3`,
/// `^1.2.3`, `~1.2.3`, `>=1.2.3 <2.0.0` (space = AND), `A || B` (OR).
/// Full SemVer triples are required after every operator.
[[nodiscard]] inline auto parse_range(std::string_view text)
    -> result<semver_range> {
    using namespace semver_detail;
    text = trim(text);
    if (text.empty()) {
        return std::unexpected{
            fail(error_code::parse_error, "version range: empty")};
    }
    if (text.size() > k_max_text_length * 4) {
        return std::unexpected{
            fail(error_code::parse_error, "version range: too long")};
    }

    semver_range range{};
    std::size_t start{0};
    while (true) {
        const auto separator{text.find("||", start)};
        const auto group_text{trim(text.substr(
            start, separator == std::string_view::npos ? separator
                                                       : separator - start))};
        if (group_text.empty()) {
            return std::unexpected{
                fail(error_code::parse_error, "version range: empty group")};
        }

        std::vector<version_comparator> comparators{};
        std::size_t token_start{0};
        while (token_start < group_text.size()) {
            while (token_start < group_text.size() &&
                   (group_text[token_start] == ' ' ||
                    group_text[token_start] == '\t')) {
                ++token_start;
            }
            if (token_start >= group_text.size()) {
                break;
            }
            auto token_end{token_start};
            while (token_end < group_text.size() &&
                   group_text[token_end] != ' ' &&
                   group_text[token_end] != '\t') {
                ++token_end;
            }
            auto token{group_text.substr(token_start, token_end - token_start)};

            range_operator op{range_operator::equal};
            std::size_t op_length{0};
            if (token.starts_with(">=")) {
                op = range_operator::greater_equal;
                op_length = 2;
            } else if (token.starts_with("<=")) {
                op = range_operator::less_equal;
                op_length = 2;
            } else if (token.starts_with(">")) {
                op = range_operator::greater;
                op_length = 1;
            } else if (token.starts_with("<")) {
                op = range_operator::less;
                op_length = 1;
            } else if (token.starts_with("=")) {
                op = range_operator::equal;
                op_length = 1;
            } else if (token.starts_with("^")) {
                op = range_operator::caret;
                op_length = 1;
            } else if (token.starts_with("~")) {
                op = range_operator::tilde;
                op_length = 1;
            }
            token.remove_prefix(op_length);
            if (token.empty()) {
                return std::unexpected{
                    fail(error_code::parse_error,
                         "version range: operator without a version")};
            }
            auto bound{semver::parse(token)};
            if (!bound) {
                return std::unexpected{
                    fail(error_code::parse_error,
                         "version range: " + bound.error().message)};
            }
            comparators.push_back(
                version_comparator{.kind = op, .bound = std::move(*bound)});
            token_start = token_end;
        }
        if (comparators.empty()) {
            return std::unexpected{
                fail(error_code::parse_error, "version range: empty group")};
        }
        range.alternatives.push_back(std::move(comparators));
        if (separator == std::string_view::npos) {
            break;
        }
        start = separator + 2;
        if (start >= text.size()) {
            return std::unexpected{
                fail(error_code::parse_error, "version range: trailing '||'")};
        }
    }
    return range;
}

/// SemVer range match. Prerelease versions only match when at least one
/// comparator in the matching AND group has a prerelease with the same
/// major.minor.patch (npm-style rule).
[[nodiscard]] inline auto satisfies(const semver &value,
                                    const semver_range &range) -> bool {
    for (const auto &group : range.alternatives) {
        bool group_matches{true};
        for (const auto &comparator : group) {
            if (!semver_detail::eval_comparator(value, comparator)) {
                group_matches = false;
                break;
            }
        }
        if (!group_matches) {
            continue;
        }
        if (!value.is_prerelease()) {
            return true;
        }
        // npm-style prerelease exclusion: a prerelease only matches when a
        // comparator targets the same major.minor.patch with a prerelease.
        const bool allowed = std::any_of(
            group.begin(), group.end(), [&value](const auto &comparator) {
                return comparator.bound.is_prerelease() &&
                       comparator.bound.major == value.major &&
                       comparator.bound.minor == value.minor &&
                       comparator.bound.patch == value.patch;
            });
        if (allowed) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline auto satisfies(std::string_view version_text,
                                    std::string_view range_text)
    -> result<bool> {
    const auto value{semver::parse(version_text)};
    if (!value) {
        return std::unexpected{value.error()};
    }
    const auto range{parse_range(range_text)};
    if (!range) {
        return std::unexpected{range.error()};
    }
    return satisfies(*value, *range);
}

} // namespace refrain::core
