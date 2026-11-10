// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/config.hpp>

#include <refrain/core/io.hpp>

#include <glaze/glaze.hpp>
#include <glaze/toml.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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

[[nodiscard]] auto parse_table_text(std::string_view text,
                                    std::string_view origin)
    -> result<config_table> {
    config_table table;
    const auto context{glz::read_toml(table, text)};
    if (context) {
        // TOML allows comments/whitespace-only files; glaze reports those as
        // "no input read", which is a valid empty document.
        if (context.ec == glz::error_code::no_read_input) {
            return table;
        }
        return std::unexpected{parse_error(context, origin, text)};
    }
    return table;
}

/// Flat dotted-key representation used by the host store. Nested TOML tables
/// are flattened to leaves so layering is a per-key overlay (a user file that
/// sets `[status] motd` keeps the defaults' other status keys).
using flat_table = std::map<std::string, config_value, std::less<>>;

[[nodiscard]] auto valid_key_path(std::string_view key) noexcept -> bool {
    if (key.empty()) {
        return false;
    }
    std::size_t start{0};
    while (true) {
        const auto dot{key.find('.', start)};
        const auto component{key.substr(
            start, dot == std::string_view::npos ? dot : dot - start)};
        if (component.empty()) {
            return false;
        }
        if (dot == std::string_view::npos) {
            return true;
        }
        start = dot + 1;
    }
}

[[nodiscard]] auto valid_namespace(std::string_view name) noexcept -> bool {
    if (name.empty() || name.front() == '/' || name.back() == '/') {
        return false;
    }
    if (name.find("..") != std::string_view::npos) {
        return false;
    }
    for (const char character : name) {
        const auto byte{static_cast<unsigned char>(character)};
        const auto okay{std::isalnum(byte) != 0 || character == '_' ||
                        character == '-' || character == '.' ||
                        character == '/'};
        if (!okay) {
            return false;
        }
    }
    return true;
}

void flatten_object(const config_object &object, std::string_view prefix,
                    flat_table &out) {
    for (const auto &[key, value] : object) {
        auto path{std::string{prefix}};
        if (!path.empty()) {
            path.push_back('.');
        }
        path += key;
        if (const auto *child{std::get_if<config_object>(&value.data)};
            child != nullptr) {
            flatten_object(*child, path, out);
        } else {
            out.insert_or_assign(std::move(path), value);
        }
    }
}

auto flatten_table(const config_table &table, flat_table &out) -> void {
    for (const auto &[key, value] : table) {
        if (const auto *child{std::get_if<config_object>(&value.data)};
            child != nullptr) {
            flatten_object(*child, key, out);
        } else {
            out.insert_or_assign(key, value);
        }
    }
}

[[nodiscard]] auto config_bool(bool value) -> config_value {
    config_value result{};
    result.data = value;
    return result;
}

[[nodiscard]] auto config_int64(std::int64_t value) -> config_value {
    config_value result{};
    result.data = value;
    return result;
}

[[nodiscard]] auto config_double(double value) -> config_value {
    config_value result{};
    result.data = value;
    return result;
}

[[nodiscard]] auto config_string(std::string_view value) -> config_value {
    config_value result{};
    result.data = std::string{value};
    return result;
}

[[nodiscard]] auto config_strings(std::span<const std::string_view> values)
    -> config_value {
    config_array array;
    array.reserve(values.size());
    for (const auto value : values) {
        array.push_back(config_string(value));
    }
    config_value result{};
    result.data = std::move(array);
    return result;
}

[[nodiscard]] auto flat_node(const flat_table &table, std::string_view key)
    -> const config_value * {
    const auto match{table.find(key)};
    return match == table.end() ? nullptr : &match->second;
}

[[nodiscard]] auto view_bool(const config_value *node, bool fallback) -> bool {
    if (node == nullptr) {
        return fallback;
    }
    const auto *value{std::get_if<bool>(&node->data)};
    return value != nullptr ? *value : fallback;
}

[[nodiscard]] auto view_int64(const config_value *node, std::int64_t fallback)
    -> std::int64_t {
    if (node == nullptr) {
        return fallback;
    }
    const auto *value{std::get_if<std::int64_t>(&node->data)};
    return value != nullptr ? *value : fallback;
}

[[nodiscard]] auto view_double(const config_value *node, double fallback)
    -> double {
    if (node == nullptr) {
        return fallback;
    }
    const auto *value{std::get_if<double>(&node->data)};
    return value != nullptr ? *value : fallback;
}

[[nodiscard]] auto view_string(const config_value *node,
                               std::string_view fallback) -> std::string {
    if (node == nullptr) {
        return std::string{fallback};
    }
    const auto *value{std::get_if<std::string>(&node->data)};
    return value != nullptr ? *value : std::string{fallback};
}

[[nodiscard]] auto view_strings(const config_value *node,
                                std::span<const std::string_view> fallback)
    -> std::vector<std::string> {
    const auto *array{node != nullptr ? std::get_if<config_array>(&node->data)
                                      : nullptr};
    if (array == nullptr) {
        return copy_fallback(fallback);
    }
    auto values{std::vector<std::string>{}};
    values.reserve(array->size());
    for (const auto &element : *array) {
        const auto *text{std::get_if<std::string>(&element.data)};
        if (text == nullptr) {
            return copy_fallback(fallback);
        }
        values.push_back(*text);
    }
    return values;
}

/// Overlay `source` on top of `destination`; leaf keys are replaced.
void merge_flat(flat_table &destination, const flat_table &source) {
    for (const auto &[key, value] : source) {
        destination.insert_or_assign(key, value);
    }
}

[[nodiscard]] auto set_flat(flat_table &table, std::string_view key,
                            config_value value) -> status {
    if (!valid_key_path(key)) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "config: invalid key '" + std::string{key} + "'")};
    }
    // A leaf and its descendants cannot coexist in one flat namespace.
    std::size_t start{0};
    while (true) {
        const auto dot{key.find('.', start)};
        if (dot == std::string_view::npos) {
            break;
        }
        const auto parent{key.substr(0, dot)};
        if (table.contains(parent)) {
            return std::unexpected{make_error(
                error_code::invalid_argument,
                "config: key '" + std::string{key} + "' conflicts with '" +
                    std::string{parent} + "'")};
        }
        start = dot + 1;
    }
    // Setting a parent removes descendants ("status" clears "status.motd").
    const auto prefix{std::string{key} + "."};
    for (auto iterator = table.lower_bound(prefix);
         iterator != table.end() && iterator->first.starts_with(prefix);) {
        iterator = table.erase(iterator);
    }
    table.insert_or_assign(std::string{key}, std::move(value));
    return status_ok();
}

[[nodiscard]] auto erase_flat(flat_table &table, std::string_view key)
    -> status {
    if (!valid_key_path(key)) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "config: invalid key '" + std::string{key} + "'")};
    }
    const auto match{table.find(key)};
    if (match == table.end()) {
        return std::unexpected{
            make_error(error_code::not_found,
                       "config: key '" + std::string{key} + "' is not set")};
    }
    table.erase(match);
    return status_ok();
}

/// Rebuild a nested TOML table from flat keys for serialization.
void insert_object(config_object &object, std::string_view path,
                   const config_value &value) {
    const auto dot{path.find('.')};
    if (dot == std::string_view::npos) {
        object[std::string{path}] = value;
        return;
    }
    auto &child{object[std::string{path.substr(0, dot)}]};
    auto *nested{std::get_if<config_object>(&child.data)};
    if (nested == nullptr) {
        child.data = config_object{};
        nested = std::get_if<config_object>(&child.data);
    }
    insert_object(*nested, path.substr(dot + 1), value);
}

[[nodiscard]] auto unflatten(const flat_table &table) -> config_table {
    config_table root;
    for (const auto &[key, value] : table) {
        const auto dot{key.find('.')};
        if (dot == std::string::npos) {
            root.insert_or_assign(key, value);
            continue;
        }
        const auto first{key.substr(0, dot)};
        auto match{root.find(first)};
        if (match == root.end()) {
            match = root.emplace(std::string{first}, config_value{}).first;
            match->second.data = config_object{};
        }
        auto *nested{std::get_if<config_object>(&match->second.data)};
        if (nested == nullptr) {
            match->second.data = config_object{};
            nested = std::get_if<config_object>(&match->second.data);
        }
        insert_object(*nested, key.substr(dot + 1), value);
    }
    return root;
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
    auto table{parse_table_text(text, origin)};
    if (!table) {
        return std::unexpected{std::move(table.error())};
    }
    auto parsed{std::make_shared<impl>()};
    parsed->origin = std::string{origin};
    parsed->table = std::move(*table);
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

namespace {

[[nodiscard]] auto read_flat_file(std::string_view path, bool required)
    -> result<flat_table> {
    if (path.empty()) {
        return flat_table{};
    }
    if (!required) {
        std::error_code error;
        if (!std::filesystem::exists(path, error)) {
            return flat_table{};
        }
    }
    const io::read_options options{.max_bytes = 4ULL * 1024ULL * 1024ULL,
                                   .allow_symlink = false};
    auto text{io::read_text(path, options)};
    if (!text) {
        if (!required && text.error().code == error_code::not_found) {
            return flat_table{};
        }
        return std::unexpected{make_error(
            text.error().code, "config: cannot read '" + std::string{path} +
                                   "': " + text.error().message)};
    }
    auto table{parse_table_text(*text, path)};
    if (!table) {
        return std::unexpected{std::move(table.error())};
    }
    flat_table flat;
    flatten_table(*table, flat);
    return flat;
}

} // namespace

struct view::impl {
    std::string name;
    flat_table table;
};

view::view() noexcept = default;
view::~view() = default;

view::view(std::shared_ptr<const impl> state) noexcept
    : impl_{std::move(state)} {}

auto view::name() const noexcept -> std::string_view {
    return impl_ != nullptr ? std::string_view{impl_->name}
                            : std::string_view{};
}

auto view::valid() const noexcept -> bool {
    return impl_ != nullptr;
}

auto view::contains(std::string_view key) const noexcept -> bool {
    return impl_ != nullptr && flat_node(impl_->table, key) != nullptr;
}

auto view::kind(std::string_view key) const noexcept -> value_kind {
    if (impl_ == nullptr) {
        return value_kind::missing;
    }
    return kind_of(flat_node(impl_->table, key));
}

auto view::get_bool(std::string_view key, bool fallback) const -> bool {
    if (impl_ == nullptr) {
        return fallback;
    }
    return view_bool(flat_node(impl_->table, key), fallback);
}

auto view::get_int64(std::string_view key, std::int64_t fallback) const
    -> std::int64_t {
    if (impl_ == nullptr) {
        return fallback;
    }
    return view_int64(flat_node(impl_->table, key), fallback);
}

auto view::get_double(std::string_view key, double fallback) const -> double {
    if (impl_ == nullptr) {
        return fallback;
    }
    return view_double(flat_node(impl_->table, key), fallback);
}

auto view::get_string(std::string_view key, std::string_view fallback) const
    -> std::string {
    if (impl_ == nullptr) {
        return std::string{fallback};
    }
    return view_string(flat_node(impl_->table, key), fallback);
}

auto view::get_strings(std::string_view key,
                       std::span<const std::string_view> fallback) const
    -> std::vector<std::string> {
    if (impl_ == nullptr) {
        return copy_fallback(fallback);
    }
    return view_strings(flat_node(impl_->table, key), fallback);
}

struct store::impl {
    struct entry {
        std::string name;
        std::string path;
        std::string defaults_path;
        flat_table defaults;
        flat_table fallback;
        flat_table user;
        flat_table overrides;
        bool dirty{false};
        mutable std::shared_ptr<const view::impl> snapshot;
    };

    std::vector<entry> entries;

    [[nodiscard]] auto find(std::string_view name) noexcept -> entry * {
        for (auto &candidate : entries) {
            if (candidate.name == name) {
                return &candidate;
            }
        }
        return nullptr;
    }

    [[nodiscard]] auto find(std::string_view name) const noexcept
        -> const entry * {
        for (const auto &candidate : entries) {
            if (candidate.name == name) {
                return &candidate;
            }
        }
        return nullptr;
    }

    [[nodiscard]] auto set(std::string_view name, std::string_view key,
                           config_value value, layer target) -> status;

    static void rebuild(entry &value) {
        auto merged{value.defaults};
        merge_flat(merged, value.fallback);
        merge_flat(merged, value.user);
        merge_flat(merged, value.overrides);
        auto state{std::make_shared<view::impl>()};
        state->name = value.name;
        state->table = std::move(merged);
        value.snapshot = std::move(state);
    }
};

store::store() : impl_{std::make_unique<impl>()} {}
store::store(store &&) noexcept = default;
auto store::operator=(store &&) noexcept -> store & = default;
store::~store() = default;

auto store::empty() -> store { return store{}; }

auto store::mount(mount_options options) -> status {
    if (!valid_namespace(options.name)) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "config: invalid namespace '" + options.name + "'")};
    }
    if (options.path.empty()) {
        return std::unexpected{make_error(error_code::invalid_argument,
                                          "config: empty path for namespace '" +
                                              options.name + "'")};
    }
    if (impl_->find(options.name) != nullptr) {
        return std::unexpected{make_error(error_code::already_exists,
                                          "config: namespace '" + options.name +
                                              "' is already mounted")};
    }
    auto &entry{impl_->entries.emplace_back()};
    entry.name = std::move(options.name);
    entry.path = std::move(options.path);
    entry.defaults_path = std::move(options.defaults_path);
    return status_ok();
}

auto store::load_all() -> status {
    for (auto &entry : impl_->entries) {
        auto defaults{
            read_flat_file(entry.defaults_path, !entry.defaults_path.empty())};
        if (!defaults) {
            return std::unexpected{std::move(defaults.error())};
        }
        auto user{read_flat_file(entry.path, false)};
        if (!user) {
            return std::unexpected{std::move(user.error())};
        }
        entry.defaults = std::move(*defaults);
        entry.fallback = flat_table{};
        entry.user = std::move(*user);
        entry.overrides = flat_table{};
        entry.dirty = false;
        impl::rebuild(entry);
    }
    return status_ok();
}

auto store::module(std::string_view name) const -> result<view> {
    const auto *entry{impl_->find(name)};
    if (entry == nullptr) {
        return std::unexpected{make_error(
            error_code::not_found,
            "config: namespace '" + std::string{name} + "' is not mounted")};
    }
    if (entry->snapshot == nullptr) {
        impl::rebuild(*const_cast<impl::entry *>(entry));
    }
    return view{entry->snapshot};
}

auto store::names() const -> std::vector<std::string> {
    auto result{std::vector<std::string>{}};
    result.reserve(impl_->entries.size());
    for (const auto &entry : impl_->entries) {
        result.push_back(entry.name);
    }
    return result;
}

auto store::impl::set(std::string_view name, std::string_view key,
                      config_value value, layer target) -> status {
    auto *target_entry{find(name)};
    if (target_entry == nullptr) {
        return std::unexpected{make_error(
            error_code::not_found,
            "config: namespace '" + std::string{name} + "' is not mounted")};
    }
    auto &table{target == layer::fallback
                    ? target_entry->fallback
                    : (target == layer::user ? target_entry->user
                                             : target_entry->overrides)};
    auto status{set_flat(table, key, std::move(value))};
    if (!status) {
        return status;
    }
    if (target == layer::user) {
        target_entry->dirty = true;
    }
    target_entry->snapshot.reset();
    return status_ok();
}

auto store::set_bool(std::string_view name, std::string_view key, bool value,
                     layer target) -> status {
    return impl_->set(name, key, config_bool(value), target);
}

auto store::set_int64(std::string_view name, std::string_view key,
                      std::int64_t value, layer target) -> status {
    return impl_->set(name, key, config_int64(value), target);
}

auto store::set_double(std::string_view name, std::string_view key,
                       double value, layer target) -> status {
    return impl_->set(name, key, config_double(value), target);
}

auto store::set_string(std::string_view name, std::string_view key,
                       std::string_view value, layer target) -> status {
    return impl_->set(name, key, config_string(value), target);
}

auto store::set_strings(std::string_view name, std::string_view key,
                        std::span<const std::string_view> value, layer target)
    -> status {
    return impl_->set(name, key, config_strings(value), target);
}

auto store::erase(std::string_view name, std::string_view key, layer target)
    -> status {
    auto *entry{impl_->find(name)};
    if (entry == nullptr) {
        return std::unexpected{make_error(
            error_code::not_found,
            "config: namespace '" + std::string{name} + "' is not mounted")};
    }
    auto &table{target == layer::fallback
                    ? entry->fallback
                    : (target == layer::user ? entry->user : entry->overrides)};
    auto status{erase_flat(table, key)};
    if (!status) {
        return status;
    }
    if (target == layer::user) {
        entry->dirty = true;
    }
    entry->snapshot.reset();
    return status_ok();
}

auto store::reload(std::string_view name) -> status {
    auto *entry{impl_->find(name)};
    if (entry == nullptr) {
        return std::unexpected{make_error(
            error_code::not_found,
            "config: namespace '" + std::string{name} + "' is not mounted")};
    }
    auto user{read_flat_file(entry->path, false)};
    if (!user) {
        return std::unexpected{std::move(user.error())};
    }
    entry->user = std::move(*user);
    entry->dirty = false;
    impl::rebuild(*entry);
    return status_ok();
}

auto store::reload_all() -> status {
    for (const auto &entry : impl_->entries) {
        if (auto status{reload(entry.name)}; !status) {
            return status;
        }
    }
    return status_ok();
}

auto store::save(std::string_view name) -> status {
    auto *entry{impl_->find(name)};
    if (entry == nullptr) {
        return std::unexpected{make_error(
            error_code::not_found,
            "config: namespace '" + std::string{name} + "' is not mounted")};
    }
    auto text{std::string{}};
    auto document_value{unflatten(entry->user)};
    if (glz::write_toml(document_value, text)) {
        return std::unexpected{make_error(
            error_code::io_error,
            "config: cannot serialize namespace '" + entry->name + "'")};
    }
    if (auto status{io::atomic_write_text(entry->path, text)}; !status) {
        return status;
    }
    entry->dirty = false;
    return status_ok();
}

auto store::save_all() -> status {
    for (const auto &entry : impl_->entries) {
        if (!entry.dirty) {
            continue;
        }
        if (auto status{save(entry.name)}; !status) {
            return status;
        }
    }
    return status_ok();
}

auto store::dirty(std::string_view name) const -> bool {
    const auto *entry{impl_->find(name)};
    return entry != nullptr && entry->dirty;
}

} // namespace refrain::core::config
