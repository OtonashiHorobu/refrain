// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/config.hpp>

#include <refrain/core/io.hpp>

#include <glaze/glaze.hpp>
#include <glaze/toml.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::core::config {

namespace {

// glaze's TOML reader writes table headers into std::map-rooted documents;
// `glz::generic` itself only supports table headers in map form (glaze 8.4
// limitation). The i64 flavour keeps TOML integers exact (ids, ports, seeds)
// while still storing TOML floats as double.
using config_value = glz::generic_i64;
using config_object = config_value::object_t;
using config_array = config_value::array_t;
using config_table = std::map<std::string, config_value, std::less<>>;

[[nodiscard]] auto copy_fallback(std::span<const std::string_view> fallback)
    -> std::vector<std::string> {
    auto strings{std::vector<std::string>{}};
    strings.reserve(fallback.size());
    for (const auto &value : fallback) {
        strings.emplace_back(value);
    }
    return strings;
}

[[nodiscard]] auto child_of(const config_value &value,
                            std::string_view key) noexcept
    -> const config_value * {
    const auto *object{std::get_if<config_object>(&value.data)};
    if (object == nullptr) {
        return nullptr;
    }
    const auto match{object->find(key)};
    return match == object->end() ? nullptr : &match->second;
}

/// Resolve literal key components: every component is one table key (dots are
/// not separators). Empty paths denote the document root.
[[nodiscard]] auto node_at(const config_table &root,
                           std::span<const std::string_view> path) noexcept
    -> const config_value * {
    if (path.empty()) {
        return nullptr;
    }
    const auto match{root.find(path.front())};
    if (match == root.end()) {
        return nullptr;
    }
    const config_value *current{&match->second};
    for (std::size_t index = 1; index < path.size(); ++index) {
        current = child_of(*current, path[index]);
        if (current == nullptr) {
            return nullptr;
        }
    }
    return current;
}

/// Resolve a dotted key path ("gamepack.id"). Splitting happens here, not in
/// the TOML library: quoted keys containing '.' stay addressable through the
/// literal `*_at()` API.
[[nodiscard]] auto node_for_dotted_key(const config_table &root,
                                       std::string_view key) noexcept
    -> const config_value * {
    if (key.empty()) {
        return nullptr;
    }
    std::size_t start{0};
    const config_value *current{nullptr};
    while (true) {
        const auto dot{key.find('.', start)};
        const auto component{key.substr(
            start, dot == std::string_view::npos ? dot : dot - start)};
        if (current == nullptr) {
            const auto match{root.find(component)};
            if (match == root.end()) {
                return nullptr;
            }
            current = &match->second;
        } else {
            current = child_of(*current, component);
            if (current == nullptr) {
                return nullptr;
            }
        }
        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }
    return current;
}

[[nodiscard]] auto kind_of(const config_value *node) noexcept -> value_kind {
    if (node == nullptr) {
        return value_kind::missing;
    }
    if (std::holds_alternative<bool>(node->data)) {
        return value_kind::boolean;
    }
    if (std::holds_alternative<std::int64_t>(node->data)) {
        return value_kind::integer;
    }
    if (std::holds_alternative<double>(node->data)) {
        return value_kind::floating;
    }
    if (std::holds_alternative<std::string>(node->data)) {
        return value_kind::string;
    }
    if (std::holds_alternative<config_array>(node->data)) {
        return value_kind::array;
    }
    if (std::holds_alternative<config_object>(node->data)) {
        return value_kind::table;
    }
    return value_kind::other;
}

[[nodiscard]] auto render_path(std::span<const std::string_view> path)
    -> std::string {
    auto rendered{std::string{}};
    for (const auto component : path) {
        if (!rendered.empty()) {
            rendered += '.';
        }
        rendered += component;
    }
    return rendered;
}

[[nodiscard]] auto missing_error(std::span<const std::string_view> path)
    -> error {
    return make_error(error_code::not_found,
                      "config: missing key '" + render_path(path) + "'");
}

[[nodiscard]] auto type_error(std::span<const std::string_view> path,
                              std::string_view expected) -> error {
    return make_error(error_code::parse_error,
                      "config: key '" + render_path(path) + "' is not " +
                          std::string{expected});
}

/// Byte offset -> 1-based line/column for diagnostics.
[[nodiscard]] auto line_column_of(std::string_view text,
                                  std::size_t offset) noexcept
    -> std::pair<std::size_t, std::size_t> {
    std::size_t line{1};
    std::size_t column{1};
    const auto limit{std::min(offset, text.size())};
    for (std::size_t index = 0; index < limit; ++index) {
        if (text[index] == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    return {line, column};
}

[[nodiscard]] auto parse_error(const glz::error_ctx &context,
                               std::string_view origin, std::string_view text)
    -> error {
    auto formatted{glz::format_error(context, std::string{text})};
    // glaze reports offset-based errors as "index N: ..." when the offending
    // position has no trailing newline; rewrite that prefix to line:column so
    // diagnostics always carry source coordinates.
    constexpr std::string_view index_prefix{"index "};
    if (formatted.rfind(index_prefix, 0) == 0) {
        const auto colon{formatted.find(':')};
        if (colon != std::string::npos) {
            std::size_t offset{0};
            const auto digits{std::string_view{formatted}.substr(
                index_prefix.size(), colon - index_prefix.size())};
            const auto *begin{digits.data()};
            const auto *end{digits.data() + digits.size()};
            if (std::from_chars(begin, end, offset).ec == std::errc{}) {
                const auto [line, column]{line_column_of(text, offset)};
                formatted = std::to_string(line) + ":" +
                            std::to_string(column) + formatted.substr(colon);
            }
        }
    }
    return make_error(error_code::parse_error,
                      std::string{origin} + ":" + formatted);
}

/// Collect the direct child keys of a document table, sorted.
[[nodiscard]] auto sorted_keys_of(const config_value &value)
    -> std::vector<std::string> {
    const auto *object{std::get_if<config_object>(&value.data)};
    if (object == nullptr) {
        return {};
    }
    auto keys{std::vector<std::string>{}};
    keys.reserve(object->size());
    for (const auto &[key, child] : *object) {
        static_cast<void>(child);
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

} // namespace

struct document::impl {
    config_table table;
    std::string origin;
};

document::document(document &&other) noexcept = default;

auto document::operator=(document &&other) noexcept -> document & = default;

document::document(const document &other) noexcept = default;

auto document::operator=(const document &other) noexcept
    -> document & = default;

document::~document() = default;

document::document(std::shared_ptr<const impl> parsed) noexcept
    : impl_{std::move(parsed)} {}

auto document::parse_file(std::string_view path,
                          const io::read_options &options) -> result<document> {
    auto text{io::read_text(path, options)};
    if (!text) {
        return std::unexpected{std::move(text.error())};
    }
    return parse_text(*text, path);
}

auto document::parse_text(std::string_view text, std::string_view origin)
    -> result<document> {
    auto parsed{std::make_shared<impl>()};
    parsed->origin = std::string{origin};

    const auto context{glz::read_toml(parsed->table, text)};
    if (context) {
        // TOML allows comments/whitespace-only files; glaze reports those as
        // "no input read", which is a valid empty document.
        if (context.ec == glz::error_code::no_read_input) {
            return document{std::move(parsed)};
        }
        return std::unexpected{parse_error(context, origin, text)};
    }
    return document{std::move(parsed)};
}

auto document::origin() const noexcept -> std::string_view {
    if (impl_ == nullptr) {
        return {};
    }
    return impl_->origin;
}

auto document::contains(std::string_view key) const noexcept -> bool {
    if (impl_ == nullptr) {
        return false;
    }
    if (key.empty()) {
        // The dotted-key API has no empty key; every get_*() treats "" as
        // missing, so contains("") must not claim a (nonexistent) key exists.
        // The document root is queried through contains_at({}) / keys("").
        return false;
    }
    return node_for_dotted_key(impl_->table, key) != nullptr;
}

auto document::get_bool(std::string_view key, bool fallback) const -> bool {
    if (impl_ == nullptr) {
        return fallback;
    }
    const auto *node{node_for_dotted_key(impl_->table, key)};
    if (node == nullptr) {
        return fallback;
    }
    const auto *value{std::get_if<bool>(&node->data)};
    return value != nullptr ? *value : fallback;
}

auto document::get_int64(std::string_view key, std::int64_t fallback) const
    -> std::int64_t {
    if (impl_ == nullptr) {
        return fallback;
    }
    const auto *node{node_for_dotted_key(impl_->table, key)};
    if (node == nullptr) {
        return fallback;
    }
    const auto *value{std::get_if<std::int64_t>(&node->data)};
    return value != nullptr ? *value : fallback;
}

auto document::get_double(std::string_view key, double fallback) const
    -> double {
    if (impl_ == nullptr) {
        return fallback;
    }
    const auto *node{node_for_dotted_key(impl_->table, key)};
    if (node == nullptr) {
        return fallback;
    }
    const auto *value{std::get_if<double>(&node->data)};
    return value != nullptr ? *value : fallback;
}

auto document::get_string(std::string_view key, std::string_view fallback) const
    -> std::string {
    if (impl_ == nullptr) {
        return std::string{fallback};
    }
    const auto *node{node_for_dotted_key(impl_->table, key)};
    if (node == nullptr) {
        return std::string{fallback};
    }
    auto *value{std::get_if<std::string>(&node->data)};
    return value != nullptr ? *value : std::string{fallback};
}

auto document::get_strings(std::string_view key,
                           std::span<const std::string_view> fallback) const
    -> std::vector<std::string> {
    if (impl_ == nullptr) {
        return copy_fallback(fallback);
    }
    const auto *node{node_for_dotted_key(impl_->table, key)};
    const auto *array{node != nullptr ? std::get_if<config_array>(&node->data)
                                      : nullptr};
    if (array == nullptr) {
        return copy_fallback(fallback);
    }
    auto strings{std::vector<std::string>{}};
    strings.reserve(array->size());
    for (const auto &element : *array) {
        const auto *value{std::get_if<std::string>(&element.data)};
        if (value == nullptr) {
            // Mixed arrays are not string arrays: fall back wholesale.
            return copy_fallback(fallback);
        }
        strings.push_back(*value);
    }
    return strings;
}

auto document::get_strings_at(std::span<const std::string_view> path) const
    -> result<std::vector<std::string>> {
    if (impl_ == nullptr) {
        return std::unexpected{missing_error(path)};
    }
    const auto *node{node_at(impl_->table, path)};
    if (node == nullptr) {
        return std::unexpected{missing_error(path)};
    }
    const auto *array{std::get_if<config_array>(&node->data)};
    if (array == nullptr) {
        return std::unexpected{type_error(path, "an array of strings")};
    }
    auto strings{std::vector<std::string>{}};
    strings.reserve(array->size());
    for (const auto &element : *array) {
        const auto *value{std::get_if<std::string>(&element.data)};
        if (value == nullptr) {
            return std::unexpected{type_error(path, "an array of strings")};
        }
        strings.push_back(*value);
    }
    return strings;
}

auto document::get_string_at(std::span<const std::string_view> path) const
    -> result<std::string> {
    if (impl_ == nullptr) {
        return std::unexpected{missing_error(path)};
    }
    const auto *node{node_at(impl_->table, path)};
    if (node == nullptr) {
        return std::unexpected{missing_error(path)};
    }
    const auto *value{std::get_if<std::string>(&node->data)};
    if (value == nullptr) {
        return std::unexpected{type_error(path, "a string")};
    }
    return *value;
}

auto document::get_int64_at(std::span<const std::string_view> path) const
    -> result<std::int64_t> {
    if (impl_ == nullptr) {
        return std::unexpected{missing_error(path)};
    }
    const auto *node{node_at(impl_->table, path)};
    if (node == nullptr) {
        return std::unexpected{missing_error(path)};
    }
    const auto *value{std::get_if<std::int64_t>(&node->data)};
    if (value == nullptr) {
        return std::unexpected{type_error(path, "an integer")};
    }
    return *value;
}

auto document::get_bool_at(std::span<const std::string_view> path) const
    -> result<bool> {
    if (impl_ == nullptr) {
        return std::unexpected{missing_error(path)};
    }
    const auto *node{node_at(impl_->table, path)};
    if (node == nullptr) {
        return std::unexpected{missing_error(path)};
    }
    const auto *value{std::get_if<bool>(&node->data)};
    if (value == nullptr) {
        return std::unexpected{type_error(path, "a boolean")};
    }
    return *value;
}

auto document::keys_at(std::span<const std::string_view> path) const
    -> result<std::vector<std::string>> {
    if (impl_ == nullptr) {
        return std::unexpected{missing_error(path)};
    }
    if (path.empty()) {
        return keys("");
    }
    const auto *node{node_at(impl_->table, path)};
    if (node == nullptr) {
        return std::unexpected{missing_error(path)};
    }
    if (std::get_if<config_object>(&node->data) == nullptr) {
        return std::unexpected{type_error(path, "a table")};
    }
    return sorted_keys_of(*node);
}

auto document::kind_at(std::span<const std::string_view> path) const noexcept
    -> value_kind {
    if (impl_ == nullptr) {
        return value_kind::missing;
    }
    if (path.empty()) {
        return value_kind::table;
    }
    return kind_of(node_at(impl_->table, path));
}

auto document::contains_at(
    std::span<const std::string_view> path) const noexcept -> bool {
    if (impl_ == nullptr) {
        return false;
    }
    return path.empty() || node_at(impl_->table, path) != nullptr;
}

auto document::dump_table_at(
    std::span<const std::string_view> path,
    std::span<const std::string_view> excluded_keys) const -> std::string {
    if (impl_ == nullptr) {
        return {};
    }

    auto out{std::string{}};
    if (path.empty()) {
        // Root dump without exclusions: write the table directly instead of
        // deep-copying every nested object/array.
        if (excluded_keys.empty()) {
            if (glz::write_toml(impl_->table, out)) {
                return {};
            }
            return out;
        }
        auto filtered{impl_->table};
        for (const auto key : excluded_keys) {
            filtered.erase(key);
        }
        if (glz::write_toml(filtered, out)) {
            return {};
        }
        return out;
    }

    const auto *node{node_at(impl_->table, path)};
    const auto *object{node != nullptr ? std::get_if<config_object>(&node->data)
                                       : nullptr};
    if (object == nullptr) {
        return {};
    }
    if (excluded_keys.empty()) {
        if (glz::write_toml(*node, out)) {
            return {};
        }
        return out;
    }
    auto filtered{*object};
    for (const auto key : excluded_keys) {
        filtered.erase(key);
    }
    config_value fragment{};
    fragment.data = std::move(filtered);
    if (glz::write_toml(fragment, out)) {
        return {};
    }
    return out;
}

auto document::keys(std::string_view table_key) const
    -> std::vector<std::string> {
    if (impl_ == nullptr) {
        return {};
    }
    if (table_key.empty()) {
        auto keys{std::vector<std::string>{}};
        keys.reserve(impl_->table.size());
        for (const auto &[key, value] : impl_->table) {
            static_cast<void>(value);
            keys.push_back(key);
        }
        std::sort(keys.begin(), keys.end());
        return keys;
    }
    const auto *node{node_for_dotted_key(impl_->table, table_key)};
    if (node == nullptr) {
        return {};
    }
    return sorted_keys_of(*node);
}

auto document::dump() const -> std::string {
    if (impl_ == nullptr) {
        return {};
    }
    auto out{std::string{}};
    if (glz::write_toml(impl_->table, out)) {
        return {};
    }
    return out;
}

} // namespace refrain::core::config
