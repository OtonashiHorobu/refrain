// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/command/dispatcher.hpp>

#include <limits>
#include <memory>
#include <utility>

namespace refrain::command {

auto to_string(node_kind kind) noexcept -> std::string_view {
    switch (kind) {
    case node_kind::root:
        return "root";
    case node_kind::literal:
        return "literal";
    case node_kind::argument:
        return "argument";
    }
    return "unknown";
}

auto to_string(parser_kind kind) noexcept -> std::string_view {
    switch (kind) {
    case parser_kind::none:
        return "none";
    case parser_kind::boolean:
        return "boolean";
    case parser_kind::integer:
        return "integer";
    case parser_kind::long_integer:
        return "long_integer";
    case parser_kind::float_number:
        return "float_number";
    case parser_kind::double_number:
        return "double_number";
    case parser_kind::string:
        return "string";
    case parser_kind::enum_values:
        return "enum_values";
    case parser_kind::resource_location:
        return "resource_location";
    }
    return "unknown";
}

auto to_string(string_mode mode) noexcept -> std::string_view {
    switch (mode) {
    case string_mode::single_word:
        return "single_word";
    case string_mode::quotable_phrase:
        return "quotable_phrase";
    case string_mode::greedy_phrase:
        return "greedy_phrase";
    }
    return "unknown";
}

auto dispatcher::tree(const source &src) const
    -> std::shared_ptr<const refrain::command::tree> {
    auto out = std::make_shared<refrain::command::tree>();
    out->revision = revision();
    out->root = 0;

    constexpr auto invalid = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> mapping(nodes_.size(), invalid);
    std::vector<std::size_t> source_order{};

    // Preorder walk of the permitted nodes, then a second pass that resolves
    // child and redirect indices through the mapping. The root is always
    // present, even when a root requirement hides the whole graph; callers
    // always receive a valid root node to serialize.
    const auto root_allowed = is_allowed(0, src);
    std::vector<std::size_t> stack{};
    stack.push_back(0);
    while (!stack.empty()) {
        const auto index = stack.back();
        stack.pop_back();
        if (mapping[index] != invalid) {
            continue;
        }
        mapping[index] = static_cast<std::uint32_t>(out->nodes.size());
        source_order.push_back(index);

        tree_node node{};
        node.kind = nodes_[index].kind;
        node.name = nodes_[index].name;
        node.executable = static_cast<bool>(nodes_[index].executor);
        if (nodes_[index].parser != nullptr) {
            node.parser = nodes_[index].parser->descriptor();
        }
        out->nodes.push_back(std::move(node));

        if (index == 0 && !root_allowed) {
            continue;
        }
        const auto &children = nodes_[index].children;
        for (auto child = children.rbegin(); child != children.rend();
             ++child) {
            if (is_allowed(*child, src)) {
                stack.push_back(*child);
            }
        }
    }

    for (std::size_t position = 0; position < source_order.size(); ++position) {
        const auto source_index = source_order[position];
        auto &node = out->nodes[position];
        node.children.reserve(nodes_[source_index].children.size());
        for (const auto child : nodes_[source_index].children) {
            if (mapping[child] != invalid) {
                node.children.push_back(mapping[child]);
            }
        }
        const auto target = nodes_[source_index].redirect;
        if (target != no_node && mapping[target] != invalid) {
            node.redirect = true;
            node.redirect_index = mapping[target];
        }
    }
    return out;
}

} // namespace refrain::command
