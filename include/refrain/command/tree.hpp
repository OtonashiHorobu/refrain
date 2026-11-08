// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::command {

/// Node role in the immutable command tree.
///
/// The tree is wire-oriented, not a builder graph: the root is always node 0,
/// children refer to nodes by index, and the node order is a preorder walk of
/// the filtered registration graph.
enum class node_kind : std::uint8_t {
    root = 0,
    literal = 1,
    argument = 2,
};

/// Stable, module-local parser taxonomy. These values are intentionally not
/// protocol parser ids: the network adapter owns that mapping, so the command
/// module can evolve without renumbering itself when the protocol changes.
enum class parser_kind : std::uint8_t {
    none = 0,
    boolean = 1,
    integer = 2,
    long_integer = 3,
    float_number = 4,
    double_number = 5,
    string = 6,
    /// A fixed set of names (for example game modes). Wire adapters normally
    /// serialize this as a string parser plus server-side suggestions.
    enum_values = 7,
    resource_location = 8,
};

/// String parser shape. These values match the vanilla `brigadier:string`
/// string types, so the network adapter can copy `string_mode` directly.
enum class string_mode : std::uint8_t {
    single_word = 0,
    quotable_phrase = 1,
    greedy_phrase = 2,
};

/// Parser properties carried by an argument node of the tree.
///
/// Which numeric fields are meaningful depends on `kind`:
/// - integer, long_integer: `integer_min` / `integer_max` (guarded by
///   `has_min` / `has_max`);
/// - float_number, double_number: `number_min` / `number_max` (guarded by the
///   same flags);
/// - string: `string`;
/// - enum_values: `enum_values` holds the canonical names, `suggestion_type`
///   optionally names a protocol suggestion provider (empty means that the
///   client asks the server, which is what `dispatcher::suggest` serves);
/// - resource_location: no additional properties.
struct parser_descriptor {
    parser_kind kind{parser_kind::none};
    string_mode string{string_mode::single_word};
    bool has_min{false};
    bool has_max{false};
    double number_min{0.0};
    double number_max{0.0};
    std::int64_t integer_min{0};
    std::int64_t integer_max{0};
    std::vector<std::string> enum_values{};
    std::string suggestion_type{};

    friend auto operator==(const parser_descriptor &, const parser_descriptor &)
        -> bool = default;
};

/// One node of the immutable command tree.
///
/// This is a flat, self-contained value: strings and child vectors are owned
/// by the tree and therefore remain valid after the dispatcher is modified.
struct tree_node {
    node_kind kind{node_kind::root};
    /// A node is executable when at least one registered executor is attached.
    bool executable{false};
    /// When `redirect` is true, `redirect_index` points at the node whose
    /// children and executor continue parsing. The redirect node's own
    /// executor is not used.
    bool redirect{false};
    std::uint32_t redirect_index{0};
    std::vector<std::uint32_t> children{};
    /// Literal name, argument name, or empty for the root.
    std::string name{};
    /// Argument properties; `kind == parser_kind::none` for literals and root.
    parser_descriptor parser{};

    [[nodiscard]] auto is_root() const noexcept -> bool {
        return kind == node_kind::root;
    }
    [[nodiscard]] auto is_literal() const noexcept -> bool {
        return kind == node_kind::literal;
    }
    [[nodiscard]] auto is_argument() const noexcept -> bool {
        return kind == node_kind::argument;
    }

    friend auto operator==(const tree_node &, const tree_node &)
        -> bool = default;
};

/// Immutable snapshot of the whole command tree for one source.
///
/// The network adapter converts this to its protocol packet tree. Filtering by
/// the source requirements happens while the snapshot is built; nodes whose
/// requirement rejects the source, and their whole subtrees, are absent. A
/// redirect whose target was filtered out is dropped (both `redirect` and
/// `redirect_index` reset).
///
/// `revision` is the dispatcher revision at snapshot time. A cache keyed by
/// `(revision, source identity/permissions)` can reuse one snapshot until the
/// dispatcher is modified; note that arbitrary requirement predicates make the
/// command module itself unable to cache per source.
struct tree {
    std::uint32_t root{0};
    std::uint64_t revision{0};
    std::vector<tree_node> nodes{};

    [[nodiscard]] auto root_node() const -> const tree_node & {
        return nodes[root];
    }

    friend auto operator==(const tree &, const tree &) -> bool = default;
};

[[nodiscard]] auto to_string(node_kind kind) noexcept -> std::string_view;
[[nodiscard]] auto to_string(parser_kind kind) noexcept -> std::string_view;
[[nodiscard]] auto to_string(string_mode mode) noexcept -> std::string_view;

} // namespace refrain::command
