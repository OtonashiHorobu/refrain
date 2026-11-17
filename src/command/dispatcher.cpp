// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/command/dispatcher.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <cassert>
#include <utility>

namespace refrain::command {

namespace {

[[nodiscard]] auto contains_whitespace(std::string_view text) noexcept -> bool {
    return std::ranges::any_of(
        text, [](char character) { return reader::is_whitespace(character); });
}

} // namespace

dispatcher::dispatcher() { nodes_.push_back(node_entry{}); }

dispatcher::~dispatcher() = default;

auto dispatcher::revision() const noexcept -> std::uint64_t {
    return revision_.load(std::memory_order_relaxed);
}

auto dispatcher::node_count() const noexcept -> std::size_t {
    return nodes_.size();
}

auto dispatcher::permission_of(std::string_view command) const
    -> node_permission {
    if (command.empty() || nodes_.empty()) {
        return {};
    }
    for (const auto child : nodes_[0].children) {
        const auto &entry = nodes_[child];
        if (entry.kind == node_kind::literal && entry.name == command) {
            return node_permission{entry.permission_node,
                                   entry.permission_fallback};
        }
    }
    return {};
}

auto dispatcher::root() noexcept -> node_ref { return node_ref{this, 0}; }

auto dispatcher::literal(std::string_view name) -> node_ref {
    if (name.empty() || contains_whitespace(name)) {
        assert(false && "a command literal needs a non-empty name without "
                        "whitespace");
        return node_ref{};
    }
    const auto index = add_node(node_kind::literal, name);
    attach(0, index);
    return node_ref{this, index};
}

auto dispatcher::argument(std::string_view name,
                          std::unique_ptr<const argument_parser> parser)
    -> node_ref {
    return add_argument(name, std::move(parser));
}

auto dispatcher::add_argument(std::string_view name,
                              std::unique_ptr<const argument_parser> parser)
    -> node_ref {
    if (name.empty() || contains_whitespace(name) || parser == nullptr) {
        assert(false && "a command argument needs a non-empty name without "
                        "whitespace and a parser");
        return node_ref{};
    }
    const auto index = add_node(node_kind::argument, name);
    nodes_[index].parser = std::move(parser);
    attach(0, index);
    return node_ref{this, index};
}

auto dispatcher::add_node(node_kind kind, std::string_view name)
    -> std::size_t {
    const auto index = nodes_.size();
    node_entry entry{};
    entry.kind = kind;
    entry.name = std::string{name};
    nodes_.push_back(std::move(entry));
    bump_revision();
    return index;
}

void dispatcher::attach(std::size_t parent, std::size_t child) {
    if (parent >= nodes_.size() || child >= nodes_.size() || parent == child) {
        assert(false && "a command node cannot be attached to itself or to a "
                        "missing node");
        return;
    }
    for (auto current = parent; current != no_node;
         current = nodes_[current].parent) {
        if (current == child) {
            assert(false &&
                   "attaching this child would create a command cycle");
            return;
        }
    }
    if (nodes_[child].parent == parent) {
        const auto &children = nodes_[parent].children;
        if (std::ranges::find(children, child) != children.end()) {
            return;
        }
    }
    detach(child);
    nodes_[parent].children.push_back(child);
    nodes_[child].parent = parent;
    bump_revision();
}

void dispatcher::detach(std::size_t child) {
    if (child >= nodes_.size() || nodes_[child].parent == no_node) {
        return;
    }
    const auto parent = nodes_[child].parent;
    auto &children = nodes_[parent].children;
    const auto match = std::ranges::find(children, child);
    if (match != children.end()) {
        children.erase(match);
    }
    nodes_[child].parent = no_node;
    bump_revision();
}

void dispatcher::set_requirement(
    std::size_t index, std::function<bool(const source &)> predicate) {
    if (index >= nodes_.size()) {
        assert(false && "requirement set on a missing command node");
        return;
    }
    nodes_[index].requirement = std::move(predicate);
    bump_revision();
}

void dispatcher::set_permission_filter(permission_filter filter) {
    permission_filter_ = std::move(filter);
    bump_revision();
}

void dispatcher::set_permission(std::size_t index, std::string_view node,
                                permission_level fallback) {
    if (index >= nodes_.size()) {
        assert(false && "permission set on a missing command node");
        return;
    }
    auto &entry = nodes_[index];
    entry.permission_node = std::string{node};
    entry.permission_fallback = fallback;
    bump_revision();
}

void dispatcher::set_executor(std::size_t index,
                              std::function<result<int>(context &)> fn) {
    if (index >= nodes_.size()) {
        assert(false && "executor set on a missing command node");
        return;
    }
    nodes_[index].executor = std::move(fn);
    bump_revision();
}

void dispatcher::set_tooltip(std::size_t index, std::string_view text) {
    if (index >= nodes_.size()) {
        assert(false && "tooltip set on a missing command node");
        return;
    }
    nodes_[index].tooltip = std::string{text};
    bump_revision();
}

void dispatcher::set_redirect(std::size_t index, std::size_t target) {
    if (index >= nodes_.size() || target >= nodes_.size() || index == target) {
        assert(false && "redirect needs two distinct command nodes");
        return;
    }
    nodes_[index].redirect = target;
    bump_revision();
}

void dispatcher::bump_revision() noexcept {
    revision_.fetch_add(1, std::memory_order_relaxed);
}

auto dispatcher::is_visible(std::size_t index, const source &src) const
    -> bool {
    const auto &entry = nodes_[index];
    if (entry.requirement != nullptr && !entry.requirement(src)) {
        return false;
    }
    if (entry.permission_node.empty()) {
        return true;
    }
    if (permission_filter_) {
        const auto decision{permission_filter_(
            src,
            node_permission{entry.permission_node, entry.permission_fallback})};
        if (decision == permission_decision::allowed) {
            return true;
        }
        if (decision == permission_decision::denied) {
            return false;
        }
    }
    // No filter, or the provider has no opinion: the declared vanilla level is
    // the fallback, which is the whole policy of a composition without a
    // permission service.
    return src.has_permission(entry.permission_fallback);
}

auto dispatcher::parse_command(std::string_view input, const source &src,
                               bool allow_incomplete) const
    -> std::expected<parsed_command, parse_failure> {
    parsed_command result{};
    result.node = 0;
    if (input.size() > max_command_bytes) {
        return std::unexpected{parse_failure{0, "command is too long"}};
    }
    if (!is_visible(0, src)) {
        return std::unexpected{parse_failure{0, "unknown command"}};
    }

    reader in{input};
    std::size_t current{0};
    std::size_t redirects{0};
    while (true) {
        in.skip_whitespace();
        while (nodes_[current].redirect != no_node) {
            if (++redirects > max_redirect_depth) {
                return std::unexpected{
                    parse_failure{in.cursor(), "too many command redirects"}};
            }
            const auto target = nodes_[current].redirect;
            if (!is_visible(target, src)) {
                return std::unexpected{
                    parse_failure{in.cursor(), "command is unavailable"}};
            }
            current = target;
        }
        if (!in.can_read()) {
            if (allow_incomplete || nodes_[current].executor) {
                result.node = current;
                return result;
            }
            return std::unexpected{parse_failure{
                in.cursor(), nodes_[current].kind == node_kind::root
                                 ? "unknown command"
                                 : "expected argument"}};
        }

        const auto token_start = in.cursor();
        bool matched{false};
        std::size_t best_cursor{token_start};
        std::string best_message{};
        for (const auto child : nodes_[current].children) {
            if (!is_visible(child, src)) {
                continue;
            }
            const auto &entry = nodes_[child];
            in.set_cursor(token_start);
            if (entry.kind == node_kind::literal) {
                if (in.read_word() == entry.name) {
                    current = child;
                    matched = true;
                    break;
                }
                continue;
            }
            if (entry.parser == nullptr) {
                continue;
            }
            auto value = entry.parser->parse(in);
            if (!value) {
                // Prefer the failure that got furthest into the input; parsers
                // report the offset of the token that actually failed.
                const auto failure =
                    std::min(token_start + value.error().offset, input.size());
                if (best_message.empty() || failure > best_cursor) {
                    best_cursor = failure;
                    best_message = value.error().message;
                }
                continue;
            }
            const auto value_end = in.cursor();
            if (value_end == token_start) {
                continue;
            }
            if (in.can_read() && !in.at_whitespace()) {
                if (best_message.empty() || value_end > best_cursor) {
                    best_cursor = value_end;
                    best_message = fmt::format(
                        "unexpected character '{}' after the argument",
                        in.peek());
                }
                continue;
            }
            if (result.arguments.size() >= max_arguments) {
                return std::unexpected{
                    parse_failure{token_start, "too many command arguments"}};
            }
            result.arguments.push_back(parsed_argument{child, std::move(*value),
                                                       token_start, value_end});
            current = child;
            matched = true;
            break;
        }
        if (!matched) {
            if (best_message.empty()) {
                if (nodes_[current].kind == node_kind::root) {
                    best_message = "unknown command";
                } else if (nodes_[current].executor &&
                           nodes_[current].children.empty()) {
                    best_message = "unexpected trailing input";
                } else {
                    best_message = "incorrect argument";
                }
                best_cursor = token_start;
            }
            return std::unexpected{
                parse_failure{best_cursor, std::move(best_message)}};
        }
    }
}

auto dispatcher::locate_suggestion(std::string_view input,
                                   const source &src) const
    -> std::optional<suggestion_context> {
    // One entry per completed node of the parse, in command order. Completion
    // re-enters the node whose byte range contains the cursor, exactly like the
    // reference: typing the last byte of an argument completes that argument,
    // while a trailing separator moves completion to its children.
    struct parsed_node {
        std::size_t node{0};
        std::size_t parent{0};
        std::size_t start{0};
        std::size_t end{0};
    };
    std::vector<parsed_node> parsed{};
    parsed.reserve(16);

    reader in{input};
    std::size_t current{0};
    std::size_t redirects{0};
    while (true) {
        in.skip_whitespace();
        while (nodes_[current].redirect != no_node) {
            if (++redirects > max_redirect_depth) {
                return std::nullopt;
            }
            const auto target = nodes_[current].redirect;
            if (!is_visible(target, src)) {
                return std::nullopt;
            }
            current = target;
        }
        const auto token_start = in.cursor();
        if (!in.can_read()) {
            const auto cursor = input.size();
            for (auto entry = parsed.rbegin(); entry != parsed.rend();
                 ++entry) {
                if (entry->start <= cursor && cursor <= entry->end) {
                    return suggestion_context{entry->parent, entry->start};
                }
            }
            return suggestion_context{current, token_start};
        }
        bool matched{false};
        for (const auto child : nodes_[current].children) {
            if (!is_visible(child, src)) {
                continue;
            }
            const auto &entry = nodes_[child];
            const auto parent = current;
            in.set_cursor(token_start);
            if (entry.kind == node_kind::literal) {
                if (in.read_word() == entry.name) {
                    parsed.push_back(
                        parsed_node{child, parent, token_start, in.cursor()});
                    current = child;
                    matched = true;
                    break;
                }
                continue;
            }
            if (entry.parser == nullptr) {
                continue;
            }
            auto value = entry.parser->parse(in);
            if (!value) {
                continue;
            }
            const auto value_end = in.cursor();
            if (value_end == token_start) {
                continue;
            }
            if (in.can_read() && !in.at_whitespace()) {
                continue;
            }
            parsed.push_back(
                parsed_node{child, parent, token_start, value_end});
            current = child;
            matched = true;
            break;
        }
        if (!matched) {
            // The token being typed cannot complete the parse; its parent's
            // children are the completion candidates and the argument range
            // starts at this token (a multi-token argument reports its whole
            // partial text).
            return suggestion_context{current, token_start};
        }
    }
}

auto dispatcher::execute(std::string_view input, source &src) const
    -> result<execution> {
    if (input.size() > max_command_bytes) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            fmt::format("command exceeds {} bytes", max_command_bytes))};
    }
    auto parsed = parse_command(input, src, false);
    if (!parsed) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       fmt::format("{} at position {}", parsed.error().message,
                                   parsed.error().cursor))};
    }
    const auto node_index = parsed->node;
    if (node_index >= nodes_.size() || !nodes_[node_index].executor) {
        return std::unexpected{
            make_error(error_code::parse_error, "command is not executable")};
    }

    context ctx{src, input};
    ctx.arguments_.reserve(parsed->arguments.size());
    for (auto &argument : parsed->arguments) {
        ctx.arguments_.push_back(argument_entry{nodes_[argument.node].name,
                                                std::move(argument.value),
                                                argument.start, argument.end});
    }
    auto value = nodes_[node_index].executor(ctx);
    if (!value) {
        return std::unexpected{std::move(value.error())};
    }

    execution outcome{};
    outcome.value = *value;
    outcome.success = !ctx.failed_;
    outcome.consumed_start = 0;
    outcome.consumed_end = input.size();
    outcome.failure = std::string{ctx.failure_};
    outcome.feedback = std::move(ctx.feedback_);
    return outcome;
}

auto dispatcher::suggest(std::string_view input, std::size_t cursor,
                         const source &src) const -> suggestions {
    const auto clamped = std::min(cursor, input.size());
    if (input.size() > max_command_bytes) {
        return suggestions{clamped, clamped, {}, false};
    }
    const auto truncated = input.substr(0, clamped);
    const auto located = locate_suggestion(truncated, src);
    if (!located) {
        return suggestions{clamped, clamped, {}, false};
    }
    const auto start = std::min(located->argument_start, clamped);
    const auto remaining = truncated.substr(start);
    suggestion_builder builder{start, clamped};
    for (const auto child : nodes_[located->node].children) {
        if (!is_visible(child, src)) {
            continue;
        }
        const auto &entry = nodes_[child];
        if (entry.kind == node_kind::literal) {
            if (entry.name.starts_with(remaining)) {
                builder.add(entry.name, entry.tooltip);
            }
            continue;
        }
        if (entry.parser != nullptr) {
            entry.parser->suggest(remaining, src, builder);
        }
    }
    return std::move(builder).build();
}

node_ref::node_ref(dispatcher *owner, std::size_t index) noexcept
    : owner_{owner}, index_{index} {}

auto node_ref::valid() const noexcept -> bool {
    return owner_ != nullptr && index_ < owner_->nodes_.size();
}

auto node_ref::index() const noexcept -> std::size_t { return index_; }

auto node_ref::name() const noexcept -> std::string_view {
    return valid() ? std::string_view{owner_->nodes_[index_].name}
                   : std::string_view{};
}

auto node_ref::require(permission_level required) -> node_ref {
    return require(
        std::function<bool(const source &)>{[required](const source &src) {
            return src.has_permission(required);
        }});
}

auto node_ref::require(std::function<bool(const source &)> predicate)
    -> node_ref {
    if (valid()) {
        owner_->set_requirement(index_, std::move(predicate));
    }
    return *this;
}

auto node_ref::require_node(std::string_view node, permission_level fallback)
    -> node_ref {
    if (valid()) {
        owner_->set_permission(index_, node, fallback);
    }
    return *this;
}

auto node_ref::tooltip(std::string_view text) -> node_ref {
    if (valid()) {
        owner_->set_tooltip(index_, text);
    }
    return *this;
}

auto node_ref::then(node_ref child) -> node_ref {
    if (!valid() || !child.valid() || child.owner_ != owner_) {
        assert(false && "a command child must come from the same dispatcher");
        return *this;
    }
    owner_->attach(index_, child.index_);
    return *this;
}

auto node_ref::redirect(node_ref target) -> node_ref {
    if (!valid() || !target.valid() || target.owner_ != owner_) {
        assert(false && "a redirect target must come from the same dispatcher");
        return *this;
    }
    owner_->set_redirect(index_, target.index_);
    return *this;
}

} // namespace refrain::command
