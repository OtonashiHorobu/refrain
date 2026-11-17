// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Permission node grammar.
//
// A node is a dot-separated, lowercase path that names one permission, for
// example `refrain.command.teleport`. First-party features live under
// `refrain.<area>`; a third-party mod uses its own reverse-domain root, for
// example `com.example.mymod.home`. Each segment starts with a lowercase letter
// or digit and continues with lowercase letters, digits, `_` or `-`.
//
// A rule set may additionally use a single trailing wildcard:
// `refrain.command.*` matches every node below `refrain.command`, and a bare
// `*` matches every node. Rules may carry a `!` deny prefix; deny wins over
// grant, so a broad grant can be narrowed by a specific deny.
//
// Query nodes are always concrete: the wildcard and the deny prefix are rule
// syntax. A malformed query node is denied by the resolver instead of being
// interpreted.

#pragma once

#include <cstddef>
#include <string_view>

namespace refrain::permissions {

/// Longest accepted node text in bytes. The cap keeps a node cheap to copy and
/// stops a malformed configuration from growing the rule tables without bound.
inline constexpr std::size_t max_node_length = 255;

/// Longest accepted segment name in bytes.
inline constexpr std::size_t max_segment_length = 64;

/// True when `segment` is one lowercase node segment.
[[nodiscard]] constexpr auto valid_segment(std::string_view segment) noexcept
    -> bool {
    if (segment.empty() || segment.size() > max_segment_length) {
        return false;
    }
    const auto start{segment.front()};
    const auto is_lower_digit = [](char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9');
    };
    if (!is_lower_digit(start)) {
        return false;
    }
    for (const auto character : segment) {
        if (!is_lower_digit(character) && character != '_' &&
            character != '-') {
            return false;
        }
    }
    return true;
}

/// True when `text` is a dot-separated path of at least `min_segments` valid
/// segments. No wildcard and no deny prefix are accepted.
[[nodiscard]] constexpr auto valid_node_path(std::string_view text,
                                             std::size_t min_segments) noexcept
    -> bool {
    if (text.empty() || text.size() > max_node_length) {
        return false;
    }
    std::size_t segments{0};
    std::size_t begin{0};
    for (std::size_t index{0}; index <= text.size(); ++index) {
        if (index != text.size() && text[index] != '.') {
            continue;
        }
        if (!valid_segment(text.substr(begin, index - begin))) {
            return false;
        }
        ++segments;
        begin = index + 1;
    }
    return segments >= min_segments;
}

/// True when `text` is a concrete query node: at least `namespace.name`.
[[nodiscard]] constexpr auto valid_node(std::string_view text) noexcept
    -> bool {
    return valid_node_path(text, 2);
}

/// True when `text` is a rule node: a concrete node, a path ending in `.*`, or
/// a bare `*` that matches every node.
[[nodiscard]] constexpr auto valid_rule_node(std::string_view text) noexcept
    -> bool {
    if (text == "*") {
        return true;
    }
    constexpr std::string_view wildcard_suffix{".*"};
    if (text.size() > wildcard_suffix.size() &&
        text.ends_with(wildcard_suffix)) {
        return valid_node_path(
            text.substr(0, text.size() - wildcard_suffix.size()), 1);
    }
    return valid_node(text);
}

/// True when `text` names a single trailing wildcard, so grants below one
/// namespace can be expressed as one rule.
[[nodiscard]] constexpr auto is_wildcard_rule(std::string_view text) noexcept
    -> bool {
    return text == "*" || text.ends_with(".*");
}

/// True when rule `rule` matches the concrete node `node`.
///
/// The wildcard matches descendants only: `refrain.command.*` matches
/// `refrain.command.teleport` and `refrain.command.teleport.world` but not
/// `refrain.command` itself. Both arguments are expected to be valid; an
/// invalid rule matches nothing.
[[nodiscard]] constexpr auto node_matches_rule(std::string_view rule,
                                               std::string_view node) noexcept
    -> bool {
    if (rule == "*") {
        return valid_node(node);
    }
    if (rule.ends_with(".*")) {
        const auto prefix{rule.substr(0, rule.size() - 2)};
        return node.size() > prefix.size() && node.starts_with(prefix) &&
               node[prefix.size()] == '.';
    }
    return rule == node;
}

} // namespace refrain::permissions
