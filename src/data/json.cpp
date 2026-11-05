// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// JSON DOM codec implementation.
//
// simdjson is used as the strict, fast RFC 8259 front end but stays completely
// private to this translation unit: the parser builds a simdjson DOM, this file
// converts it into `refrain::data::json::value` (owned storage) and guarantees
// the documented semantic differences/decisions (int64 overflow rejection,
// duplicate-key last-wins, deterministic sorted object order, path/offset
// diagnostics).
//
// The writing side mirrors that split: `value` is traversed locally (the
// ordering/layout rules are this codec's contract) while JSON syntax, number
// formatting and string escaping are delegated to simdjson's
// `builder::string_builder`. The only formatting decision kept on this side is
// the documented `.0` guarantee for reals, verified after the builder has
// written the number so a `real` can never round-trip as an `integer`.
//
// `parse_unpadded()` is used so the input buffer never needs SIMDJSON_PADDING
// bytes: simdjson only reads the documented `text.size()` bytes, which keeps
// arbitrary std::string_view inputs (potentially pointing into mmapped files or
// exactly-sized buffers) safe.

#include <refrain/data/json.hpp>

#include <refrain/core/io.hpp>

#include <simdjson.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace refrain::data::json {

namespace {

constexpr auto npos{std::string_view::npos};

[[nodiscard]] constexpr auto is_json_whitespace(char character) noexcept
    -> bool {
    return character == ' ' || character == '\t' || character == '\n' ||
           character == '\r';
}

[[nodiscard]] constexpr auto starts_with(std::string_view text,
                                         std::string_view prefix) noexcept
    -> bool {
    return text.size() >= prefix.size() &&
           text.substr(0, prefix.size()) == prefix;
}

constexpr std::string_view utf8_bom{"\xEF\xBB\xBF"};

/// Immutable null value returned by `value::operator[]` for missing keys. A
/// namespace-scope object (rather than a function-local static) keeps the
/// noexcept accessor free of magic-static guards.
const value null_value{};

[[nodiscard]] auto decimal(std::int64_t number) -> std::string {
    std::array<char, 32> buffer{};
    const auto result =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), number);
    return std::string{buffer.data(),
                       static_cast<std::size_t>(result.ptr - buffer.data())};
}

[[nodiscard]] auto decimal(std::uint64_t number) -> std::string {
    std::array<char, 32> buffer{};
    const auto result =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), number);
    return std::string{buffer.data(),
                       static_cast<std::size_t>(result.ptr - buffer.data())};
}

/// Prefix parse diagnostics with a consistent `json: [origin: ]detail` shape.
[[nodiscard]] auto parse_error(std::string_view origin, std::string detail)
    -> error {
    std::string message{"json: "};
    if (!origin.empty()) {
        message += '\'';
        message += origin;
        message += "': ";
    }
    message += std::move(detail);
    return make_error(error_code::parse_error, message);
}

[[nodiscard]] auto parse_error(std::string_view origin, std::string detail,
                               error_code code) -> error {
    error failure{parse_error(origin, std::move(detail))};
    failure.code = code;
    return failure;
}

/// Append one object key to a diagnostic JSON path (`$.foo`, `$["a b"]`).
void append_path_key(std::string &path, std::string_view key) {
    const bool simple =
        !key.empty() && std::all_of(key.begin(), key.end(), [](char character) {
            const auto byte = static_cast<unsigned char>(character);
            return byte >= 0x20U && character != '.' && character != '"' &&
                   character != '\\' && character != '[' && character != ']' &&
                   character != '/' && character != '\'' && character != ' ' &&
                   character != '\t' && character != '\n' && character != '\r';
        });
    if (simple) {
        path.push_back('.');
        path.append(key);
        return;
    }
    path += "[\"";
    path.append(key);
    path += "\"]";
}

/// Locate the byte just past the root JSON value. This is diagnostic-only: it
/// backs the "trailing content at byte N" message because the simdjson DOM
/// backend does not expose token offsets. Returns npos whenever the scan is not
/// confident (the backend error remains the primary diagnosis).
[[nodiscard]] auto root_value_end(std::string_view text) noexcept
    -> std::size_t {
    std::size_t index{0};
    const auto skip_whitespace = [&text, &index]() {
        while (index < text.size() && is_json_whitespace(text[index])) {
            ++index;
        }
    };
    skip_whitespace();
    if (starts_with(text.substr(index), utf8_bom)) {
        index += utf8_bom.size();
        skip_whitespace();
    }
    if (index >= text.size()) {
        return npos;
    }
    const char first{text[index]};
    if (first != '{' && first != '[') {
        // Scalar root: consume the token; JSON separators cannot appear after a
        // root scalar, so whitespace (or EOF) ends it.
        while (index < text.size() && !is_json_whitespace(text[index])) {
            ++index;
        }
        return index;
    }
    std::size_t depth{0};
    bool in_string{false};
    bool escaped{false};
    for (; index < text.size(); ++index) {
        const char character{text[index]};
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == '"') {
                in_string = false;
            }
            continue;
        }
        if (character == '"') {
            in_string = true;
        } else if (character == '{' || character == '[') {
            ++depth;
        } else if (character == '}' || character == ']') {
            if (depth == 0) {
                return npos;
            }
            --depth;
            if (depth == 0) {
                return index + 1;
            }
        }
    }
    return npos;
}

/// Human-readable message for a simdjson backend error that has no location.
[[nodiscard]] auto describe(simdjson::error_code code) -> std::string {
    switch (code) {
    case simdjson::SUCCESS:
        return "no error";
    case simdjson::CAPACITY:
    case simdjson::OUT_OF_CAPACITY:
        return "document is larger than the JSON parser capacity";
    case simdjson::MEMALLOC:
        return "the JSON parser ran out of memory";
    case simdjson::TAPE_ERROR:
        return "malformed JSON structure (missing/extra separators, junk "
               "after the document, or comments are not allowed)";
    case simdjson::DEPTH_ERROR:
        return "nesting depth exceeds the maximum of " +
               decimal(static_cast<std::int64_t>(value::max_depth));
    case simdjson::STRING_ERROR:
        return "malformed JSON string (invalid escape sequence or lone "
               "surrogate)";
    case simdjson::T_ATOM_ERROR:
    case simdjson::F_ATOM_ERROR:
    case simdjson::N_ATOM_ERROR:
        return "malformed JSON literal";
    case simdjson::NUMBER_ERROR:
        return "malformed or out-of-range JSON number";
    case simdjson::BIGINT_ERROR:
        return "integer literal exceeds 64 bits";
    case simdjson::UTF8_ERROR:
        return "input is not valid UTF-8";
    case simdjson::EMPTY:
        return "no JSON value found (empty or whitespace-only input)";
    case simdjson::UNESCAPED_CHARS:
        return "unescaped control character inside a JSON string";
    case simdjson::UNCLOSED_STRING:
        return "unterminated JSON string";
    case simdjson::INCOMPLETE_ARRAY_OR_OBJECT:
        return "JSON document ends inside an array or object";
    case simdjson::TRAILING_CONTENT:
        return "unexpected content after the JSON value";
    case simdjson::INSUFFICIENT_PADDING:
        return "internal error: input buffer padding is insufficient";
    case simdjson::UNSUPPORTED_ARCHITECTURE:
        return "simdjson does not support this CPU architecture";
    case simdjson::INCORRECT_TYPE:
    case simdjson::NUMBER_OUT_OF_RANGE:
    case simdjson::INDEX_OUT_OF_BOUNDS:
    case simdjson::NO_SUCH_FIELD:
    case simdjson::IO_ERROR:
    case simdjson::INVALID_JSON_POINTER:
    case simdjson::INVALID_URI_FRAGMENT:
    case simdjson::UNEXPECTED_ERROR:
    case simdjson::PARSER_IN_USE:
    case simdjson::OUT_OF_ORDER_ITERATION:
    case simdjson::SCALAR_DOCUMENT_AS_VALUE:
    case simdjson::OUT_OF_BOUNDS:
    case simdjson::UNKNOWN_FIELD:
    case simdjson::UNINITIALIZED:
    case simdjson::NUM_ERROR_CODES:
        break;
    }
    return std::string{simdjson::error_message(code)};
}

[[nodiscard]] auto backend_error(simdjson::error_code code,
                                 std::string_view text, std::string_view origin)
    -> error {
    if (code == simdjson::MEMALLOC) {
        return parse_error(origin, "the JSON parser ran out of memory",
                           error_code::out_of_memory);
    }
    std::string detail{describe(code)};
    if (code == simdjson::TAPE_ERROR || code == simdjson::TRAILING_CONTENT ||
        code == simdjson::INCOMPLETE_ARRAY_OR_OBJECT) {
        // The DOM backend reports a generic tape error for trailing content;
        // locate it ourselves so the diagnostic includes a byte offset.
        const auto end = root_value_end(text);
        if (end != npos && end < text.size()) {
            auto trailing{end};
            while (trailing < text.size() &&
                   is_json_whitespace(text[trailing])) {
                ++trailing;
            }
            if (trailing < text.size()) {
                detail = "unexpected trailing content at byte " +
                         decimal(static_cast<std::int64_t>(trailing)) +
                         " (exactly one JSON document is expected)";
            }
        }
    }
    if (code == simdjson::EMPTY) {
        detail += " at byte " + decimal(static_cast<std::int64_t>(text.size()));
    } else if (code == simdjson::UTF8_ERROR) {
        detail += " (byte offset unavailable from the DOM backend)";
    } else {
        detail += " (input " + decimal(static_cast<std::int64_t>(text.size())) +
                  " bytes)";
    }
    return parse_error(origin, std::move(detail));
}

[[nodiscard]] auto unsupported_encoding(std::string_view text,
                                        std::string_view origin)
    -> std::optional<error> {
    if (text.size() >= 2) {
        const auto first = static_cast<unsigned char>(text[0]);
        const auto second = static_cast<unsigned char>(text[1]);
        if ((first == 0xFFU && second == 0xFEU) ||
            (first == 0xFEU && second == 0xFFU)) {
            return parse_error(origin,
                               "UTF-16/UTF-32 input is not supported; JSON "
                               "input must be UTF-8 (byte offset 0)");
        }
    }
    if (text.size() >= 2 && static_cast<unsigned char>(text[0]) == 0xEFU &&
        static_cast<unsigned char>(text[1]) == 0xBBU &&
        !starts_with(text, utf8_bom)) {
        return parse_error(origin, "truncated or invalid UTF-8 BOM at byte 0");
    }
    return std::nullopt;
}

[[nodiscard]] auto convert_element(const simdjson::dom::element &element,
                                   std::string &path) -> result<value>;

/// Convert a DOM array, appending `[index]` to `path` for child diagnostics.
[[nodiscard]] auto convert_array(const simdjson::dom::array &array,
                                 std::string &path) -> result<value> {
    value::array_type elements{};
    const auto count = array.size();
    elements.reserve(count);
    std::size_t index{0};
    for (const auto &child : array) {
        const auto saved{path.size()};
        path.push_back('[');
        path.append(decimal(static_cast<std::int64_t>(index)));
        path.push_back(']');
        auto converted = convert_element(child, path);
        path.resize(saved);
        if (!converted) {
            return std::unexpected{std::move(converted.error())};
        }
        elements.push_back(std::move(*converted));
        ++index;
    }
    return value{std::move(elements)};
}

/// Convert a DOM object; duplicate keys keep the last occurrence (JSON
/// semantics, documented on the header).
[[nodiscard]] auto convert_object(const simdjson::dom::object &object,
                                  std::string &path) -> result<value> {
    // Collect first and sort once: inserting each member into the sorted
    // `flat_map` directly moves O(k) pairs per insertion (O(k^2) total).
    std::vector<std::pair<std::string, value>> fields{};
    fields.reserve(object.size());
    for (const auto &field : object) {
        const auto saved{path.size()};
        append_path_key(path, field.key);
        auto converted = convert_element(field.value, path);
        path.resize(saved);
        if (!converted) {
            return std::unexpected{std::move(converted.error())};
        }
        fields.emplace_back(std::string{field.key}, std::move(*converted));
    }

    // Stable sort keeps equal keys in document order, so the last occurrence
    // of a key is the last element of its equal-key run.
    std::stable_sort(
        fields.begin(), fields.end(),
        [](const auto &lhs, const auto &rhs) { return lhs.first < rhs.first; });

    // Collapse every equal-key run to its last element: the documented
    // last-wins rule. Compacting in place keeps the vector key-ascending.
    std::size_t kept{0};
    for (std::size_t index = 0; index < fields.size(); ++index) {
        const bool last_of_run =
            index + 1U == fields.size() ||
            fields[index].first != fields[index + 1U].first;
        if (!last_of_run) {
            continue;
        }
        if (kept != index) {
            fields[kept] = std::move(fields[index]);
        }
        ++kept;
    }
    fields.resize(kept);

    // Fill in ascending key order; every hinted insert lands at the end, so
    // the fill itself is linear.
    value::object_type members{};
    for (auto &field : fields) {
        members.emplace_hint(members.end(), std::move(field.first),
                             std::move(field.second));
    }
    return value{std::move(members)};
}

[[nodiscard]] auto convert_element(const simdjson::dom::element &element,
                                   std::string &path) -> result<value> {
    using simdjson::dom::element_type;
    switch (element.type()) {
    case element_type::NULL_VALUE:
        return value{};
    case element_type::BOOL:
        return value{element.get_bool().value_unsafe()};
    case element_type::INT64:
        return value{element.get_int64().value_unsafe()};
    case element_type::DOUBLE: {
        const double number = element.get_double().value_unsafe();
        if (!std::isfinite(number)) {
            return std::unexpected{
                parse_error({}, "non-finite number at " + path +
                                    " (JSON numbers must be finite)")};
        }
        return value{number};
    }
    case element_type::STRING:
        return value{element.get_string().value_unsafe()};
    case element_type::ARRAY:
        return convert_array(element.get_array().value_unsafe(), path);
    case element_type::OBJECT:
        return convert_object(element.get_object().value_unsafe(), path);
    case element_type::UINT64:
        // Integer literal in (int64_max, uint64_max]: representation would lose
        // the "integer fits int64" contract, so reject instead of silently
        // turning it into a double (documented).
        return std::unexpected{parse_error(
            {}, "integer literal at " + path +
                    " exceeds the int64 range (value " +
                    decimal(element.get_uint64().value_unsafe()) +
                    "); use a real literal (e.g. 1e18) if loss of precision "
                    "is acceptable")};
    case element_type::BIGINT:
        // Integer literal above the 64-bit range; `number_as_string(true)`
        // preserves the exact digits for the diagnostic.
        return std::unexpected{parse_error(
            {}, "integer literal at " + path +
                    " exceeds the 64-bit integer range (value " +
                    std::string{element.get_bigint().value_unsafe()} + ")")};
    }
    return std::unexpected{parse_error(
        {}, "unsupported JSON element at " + path + " (backend element type " +
                decimal(static_cast<std::int64_t>(element.type())) + ")")};
}

[[nodiscard]] auto parse_impl(std::string_view text, std::string_view origin)
    -> result<value> {
    if (auto encoding_error = unsupported_encoding(text, origin);
        encoding_error.has_value()) {
        return std::unexpected{std::move(*encoding_error)};
    }

    // A simdjson parser owns the staging/tape buffers for one parse; keeping it
    // local makes parse() reentrant and thread-safe by construction.
    simdjson::dom::parser parser;
    // Preserve out-of-int64 integer literals as exact digit strings so the
    // conversion step can report them instead of silently degrading to double.
    parser.number_as_string(true);
    // Enforce the documented nesting limit inside the backend as well as in the
    // conversion (defense in depth: the conversion recursion is then bounded by
    // simdjson's own depth check).
    if (const auto allocate_error =
            parser.allocate(text.size(), value::max_depth);
        allocate_error != simdjson::SUCCESS) {
        return std::unexpected{parse_error(origin, describe(allocate_error),
                                           allocate_error == simdjson::MEMALLOC
                                               ? error_code::out_of_memory
                                               : error_code::parse_error)};
    }

    simdjson::dom::element root;
    if (const auto backend = parser.parse_unpadded(text).get(root);
        backend != simdjson::SUCCESS) {
        return std::unexpected{backend_error(backend, text, origin)};
    }

    std::string path{"$"};
    auto converted = convert_element(root, path);
    if (!converted) {
        // Add the origin (file path) context once, at the boundary.
        if (!origin.empty()) {
            auto failure = std::move(converted.error());
            failure.message =
                "json: '" + std::string{origin} +
                "': " + failure.message.substr(std::string{"json: "}.size());
            return std::unexpected{std::move(failure)};
        }
        return converted;
    }
    return converted;
}

} // namespace

// ---------------------------------------------------------------------------
// type names
// ---------------------------------------------------------------------------

auto to_string(type kind) noexcept -> std::string_view {
    switch (kind) {
    case type::null:
        return "null";
    case type::boolean:
        return "boolean";
    case type::integer:
        return "integer";
    case type::real:
        return "real";
    case type::string:
        return "string";
    case type::array:
        return "array";
    case type::object:
        return "object";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// parsing
// ---------------------------------------------------------------------------

auto value::parse(std::string_view text) -> result<value> {
    return parse_impl(text, {});
}

auto value::parse(std::span<const std::byte> bytes) -> result<value> {
    if (bytes.empty()) {
        return parse_impl({}, {});
    }
    return parse_impl(
        std::string_view{reinterpret_cast<const char *>(bytes.data()), // NOLINT
                         bytes.size()},
        {});
}

auto value::parse_file(std::string_view path) -> result<value> {
    auto text = core::io::read_text(path);
    if (!text) {
        return std::unexpected{std::move(text.error())};
    }
    return parse_impl(*text, path);
}

// ---------------------------------------------------------------------------
// queries and typed accessors
// ---------------------------------------------------------------------------

auto value::kind() const noexcept -> type {
    switch (data_.index()) {
    case 0:
        return type::null;
    case 1:
        return type::boolean;
    case 2:
        return type::integer;
    case 3:
        return type::real;
    case 4:
        return type::string;
    case 5:
        return type::array;
    case 6:
        return type::object;
    default:
        return type::null;
    }
}

auto value::is_null() const noexcept -> bool {
    return std::holds_alternative<std::monostate>(data_);
}

auto value::is_boolean() const noexcept -> bool {
    return std::holds_alternative<bool>(data_);
}

auto value::is_integer() const noexcept -> bool {
    return std::holds_alternative<std::int64_t>(data_);
}

auto value::is_real() const noexcept -> bool {
    return std::holds_alternative<double>(data_);
}

auto value::is_number() const noexcept -> bool {
    return is_integer() || is_real();
}

auto value::is_string() const noexcept -> bool {
    return std::holds_alternative<std::string>(data_);
}

auto value::is_array() const noexcept -> bool {
    return std::holds_alternative<array_type>(data_);
}

auto value::is_object() const noexcept -> bool {
    return std::holds_alternative<object_type>(data_);
}

auto value::as_bool() const -> result<bool> {
    if (const auto *boolean = std::get_if<bool>(&data_); boolean != nullptr) {
        return *boolean;
    }
    return std::unexpected{make_error(error_code::invalid_argument,
                                      "json: expected boolean, got " +
                                          std::string{to_string(kind())})};
}

auto value::as_int64() const -> result<std::int64_t> {
    if (const auto *integer = std::get_if<std::int64_t>(&data_);
        integer != nullptr) {
        return *integer;
    }
    return std::unexpected{make_error(error_code::invalid_argument,
                                      "json: expected integer, got " +
                                          std::string{to_string(kind())})};
}

auto value::as_double() const -> result<double> {
    if (const auto *real = std::get_if<double>(&data_); real != nullptr) {
        return *real;
    }
    if (const auto *integer = std::get_if<std::int64_t>(&data_);
        integer != nullptr) {
        // Documented int->double convenience; values above 2^53 lose
        // precision, exactly like a standard conversion.
        return static_cast<double>(*integer);
    }
    return std::unexpected{make_error(error_code::invalid_argument,
                                      "json: expected real or integer, got " +
                                          std::string{to_string(kind())})};
}

auto value::as_string() const -> result<std::string_view> {
    if (const auto *text = std::get_if<std::string>(&data_); text != nullptr) {
        return std::string_view{*text};
    }
    return std::unexpected{make_error(error_code::invalid_argument,
                                      "json: expected string, got " +
                                          std::string{to_string(kind())})};
}

auto value::as_array() const -> result<std::span<const value>> {
    if (const auto *elements = std::get_if<array_type>(&data_);
        elements != nullptr) {
        return std::span<const value>{elements->data(), elements->size()};
    }
    return std::unexpected{make_error(error_code::invalid_argument,
                                      "json: expected array, got " +
                                          std::string{to_string(kind())})};
}

auto value::as_object() const
    -> result<std::reference_wrapper<const object_type>> {
    if (const auto *members = std::get_if<object_type>(&data_);
        members != nullptr) {
        return std::cref(*members);
    }
    return std::unexpected{make_error(error_code::invalid_argument,
                                      "json: expected object, got " +
                                          std::string{to_string(kind())})};
}

// ---------------------------------------------------------------------------
// object navigation
// ---------------------------------------------------------------------------

auto value::contains(std::string_view key) const noexcept -> bool {
    const auto *members = std::get_if<object_type>(&data_);
    return members != nullptr && members->contains(key);
}

auto value::find(std::string_view key) const noexcept -> const value * {
    const auto *members = std::get_if<object_type>(&data_);
    if (members == nullptr) {
        return nullptr;
    }
    const auto match = members->find(key);
    return match == members->end() ? nullptr : &match->second;
}

auto value::at(std::string_view key) const
    -> result<std::reference_wrapper<const value>> {
    if (!is_object()) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            "json: cannot look up key '" + std::string{key} + "': value is " +
                std::string{to_string(kind())} + ", not object")};
    }
    const auto *member = find(key);
    if (member == nullptr) {
        return std::unexpected{
            make_error(error_code::not_found,
                       "json: key '" + std::string{key} + "' not found")};
    }
    return std::cref(*member);
}

auto value::operator[](std::string_view key) const noexcept -> const value & {
    const auto *member = find(key);
    return member != nullptr ? *member : null_value;
}

auto value::size() const noexcept -> std::size_t {
    if (const auto *elements = std::get_if<array_type>(&data_);
        elements != nullptr) {
        return elements->size();
    }
    if (const auto *members = std::get_if<object_type>(&data_);
        members != nullptr) {
        return members->size();
    }
    if (const auto *text = std::get_if<std::string>(&data_); text != nullptr) {
        return text->size();
    }
    return 0;
}

// ---------------------------------------------------------------------------
// serialization (simdjson builder)
// ---------------------------------------------------------------------------

namespace detail {

/// DOM -> simdjson builder traversal.
///
/// `simdjson::builder::string_builder` owns the JSON syntax and the string
/// escaping (`escape_and_append_with_quotes`, `start_object`, `append_comma`,
/// ...). This driver only decides what is written and in which order, keeping
/// the documented byte contract: sorted object members, type-stable reals,
/// non-finite -> `null`, and the pretty layout (`[\n`, indentation, `": "`
/// between key and value, no trailing newline).
class writer {
  public:
    explicit writer(simdjson::builder::string_builder &builder) noexcept
        : builder_{builder} {}

    /// Compact single-line JSON (no trailing newline).
    void compact(const value &node) { write_compact(node); }

    /// Indented JSON; `indent` spaces per nesting level.
    void pretty(const value &node, std::size_t indent) {
        write_pretty(node, indent, 0);
    }

  private:
    void write_compact(const value &node);
    void write_pretty(const value &node, std::size_t indent, std::size_t level);
    void write_number(double number);
    void append_indent(std::size_t count);

    simdjson::builder::string_builder &builder_;
};

void writer::write_compact(const value &node) {
    switch (node.kind()) {
    case type::null:
        builder_.append_null();
        return;
    case type::boolean:
        builder_.append(std::get<bool>(node.data_));
        return;
    case type::integer:
        builder_.append(std::get<std::int64_t>(node.data_));
        return;
    case type::real:
        write_number(std::get<double>(node.data_));
        return;
    case type::string:
        builder_.escape_and_append_with_quotes(
            std::get<std::string>(node.data_));
        return;
    case type::array: {
        const auto &elements = std::get<value::array_type>(node.data_);
        builder_.start_array();
        for (std::size_t index = 0; index < elements.size(); ++index) {
            if (index != 0) {
                builder_.append_comma();
            }
            write_compact(elements[index]);
        }
        builder_.end_array();
        return;
    }
    case type::object: {
        const auto &members = std::get<value::object_type>(node.data_);
        builder_.start_object();
        bool first{true};
        // std::flat_map iterates in key order; the output is deterministic.
        for (const auto &[key, member] : members) {
            if (!first) {
                builder_.append_comma();
            }
            first = false;
            builder_.escape_and_append_with_quotes(key);
            builder_.append_colon();
            write_compact(member);
        }
        builder_.end_object();
        return;
    }
    }
}

void writer::write_pretty(const value &node, std::size_t indent,
                          std::size_t level) {
    switch (node.kind()) {
    case type::array: {
        const auto &elements = std::get<value::array_type>(node.data_);
        builder_.start_array();
        if (elements.empty()) {
            builder_.end_array();
            return;
        }
        for (std::size_t index = 0; index < elements.size(); ++index) {
            builder_.append('\n');
            append_indent(indent * (level + 1));
            write_pretty(elements[index], indent, level + 1);
            if (index + 1 < elements.size()) {
                builder_.append_comma();
            }
        }
        builder_.append('\n');
        append_indent(indent * level);
        builder_.end_array();
        return;
    }
    case type::object: {
        const auto &members = std::get<value::object_type>(node.data_);
        builder_.start_object();
        if (members.empty()) {
            builder_.end_object();
            return;
        }
        std::size_t index{0};
        for (const auto &[key, member] : members) {
            builder_.append('\n');
            append_indent(indent * (level + 1));
            builder_.escape_and_append_with_quotes(key);
            builder_.append_raw(": ");
            write_pretty(member, indent, level + 1);
            if (index + 1 < members.size()) {
                builder_.append_comma();
            }
            ++index;
        }
        builder_.append('\n');
        append_indent(indent * level);
        builder_.end_object();
        return;
    }
    default:
        write_compact(node);
        return;
    }
}

void writer::write_number(double number) {
    if (!std::isfinite(number)) {
        // JSON has no NaN/Inf; the documented serialization choice is null.
        builder_.append_null();
        return;
    }
    const auto start = builder_.size();
    // The builder emits the shortest round-trip form; for finite values it
    // always includes a fraction or an exponent, and it preserves the sign of
    // -0.0. Verify that invariant anyway: a real written as "1" would parse
    // back as an integer and silently break the documented type stability.
    builder_.append(number);
    std::string_view text{};
    if (builder_.view().get(text) != simdjson::SUCCESS) {
        return; // builder invalid (OOM); dump()/dump_pretty() report it.
    }
    if (text.size() > start && text.find_first_of(".eE", start) == npos) {
        builder_.append_raw(".0");
    }
}

void writer::append_indent(std::size_t count) {
    static constexpr std::string_view spaces{"                "}; // 16 max
    while (count > spaces.size()) {
        builder_.append_raw(spaces);
        count -= spaces.size();
    }
    builder_.append_raw(spaces.substr(0, count));
}

} // namespace detail

auto value::dump() const -> std::string {
    simdjson::builder::string_builder builder{};
    detail::writer{builder}.compact(*this);
    std::string out{};
    if (builder.move_to(out) != simdjson::SUCCESS) {
        // dump() has no error channel by contract; the builder only fails when
        // its output buffer cannot be allocated, which is the documented
        // hard-OOM boundary (containers propagate std::bad_alloc the same way).
        throw std::bad_alloc{};
    }
    return out;
}

auto value::dump_pretty(std::size_t indent) const -> std::string {
    const auto safe_indent = std::clamp<std::size_t>(indent, 1, 16);
    simdjson::builder::string_builder builder{};
    detail::writer{builder}.pretty(*this, safe_indent);
    std::string out{};
    if (builder.move_to(out) != simdjson::SUCCESS) {
        // Same hard-OOM boundary as dump().
        throw std::bad_alloc{};
    }
    return out;
}

} // namespace refrain::data::json
