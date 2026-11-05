// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

// JSON DOM codec (`refrain::data::json`).
//
// This is the JSON reader/writer of the datapack/content pipeline: a small,
// owned DOM with deterministic serialization. Both backends (simdjson's DOM for
// parsing and simdjson's `builder::string_builder` for writing) are
// implementation details of src/data/json.cpp; this header never includes
// simdjson, so the public surface stays dependency-free, RTTI-free and
// replaceable.
//
// Semantics (strict RFC 8259):
//  - no comments, no trailing commas, no trailing garbage, one document only;
//  - input must be UTF-8; a single UTF-8 BOM at the start is accepted, UTF-16
//    input is reported as an encoding error;
//  - `\uXXXX` escapes (including surrogate pairs) are decoded; lone surrogates
//    are malformed;
//  - integer literals stay `integer` while they fit `std::int64_t`; an integer
//    literal outside that range is a `parse_error` (never silently becomes a
//    real), while literals with a fraction/exponent become `real` (`-0.0`
//    keeps its sign);
//  - duplicate object keys: the last occurrence wins (JSON permits the
//    duplicate; the DOM cannot represent both);
//  - nesting deeper than `value::max_depth` (128) is rejected.
//
// Serialization: object members are written in sorted key order
// (`std::flat_map` with `std::less<>`), so `dump()`/`dump_pretty()` output is
// deterministic and byte-identical for equal DOMs. JSON syntax, separators and
// string escaping are owned by simdjson's `builder::string_builder`; this codec
// only walks the DOM and supplies the compact/indented layout. Reals use the
// builder's shortest round-trip representation, which always carries a fraction
// (`.0`) or an exponent, so a serialized `real` re-parses as `real` (never as
// `integer`) and `-0.0` keeps its sign. Strings are emitted as UTF-8 with
// control characters and `"`/`\` escaped (everything else passes through; the
// parser guarantees valid UTF-8 for parsed documents, values built from
// arbitrary `std::string` are the caller's responsibility). Non-finite doubles
// (`NaN`/`±Inf`) are not representable in JSON and are serialized as `null`
// (documented choice); parsing can never produce them.
//
// Errors are values (`refrain::result`): malformed input is `parse_error`,
// failed `parse_file()` I/O keeps the `io_error` from core::io, type-mismatched
// accessors report `invalid_argument`. The simdjson DOM backend does not expose
// byte offsets for syntax errors; this codec adds byte offsets where it can
// determine them itself (empty input, BOM/encoding, trailing content) and
// reports a JSON path (e.g. `$.foo[2]`) for structural conversion errors.
//
// Exception boundary: all logical failures are values. Allocation failure
// while building the DOM is the usual hard-OOM boundary and is not translated
// (`std::bad_alloc` propagates from the containers). `dump()`/`dump_pretty()`
// have no error channel by contract, so a simdjson builder allocation failure
// is translated into `std::bad_alloc` as well; no other exceptions are thrown
// by the codec.
//
// Thread safety: `value` is a plain value type; `parse*` functions are
// reentrant (each call owns its backend parser).

#include <refrain/core/error.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <flat_map>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace refrain::data::json {

namespace detail {
class writer;
} // namespace detail

/// JSON value kind.
enum class type : std::uint8_t {
    null,
    boolean,
    integer,
    real,
    string,
    array,
    object,
};

/// Stable diagnostic name for a kind ("null", "boolean", "integer", "real",
/// "string", "array", "object").
[[nodiscard]] auto to_string(type kind) noexcept -> std::string_view;

/// Owned JSON DOM node.
///
/// A `value` is copyable/movable like any standard container; objects keep
/// their members sorted by key, arrays keep insertion order.
class value {
  public:
    using array_type = std::vector<value>;
    using object_type = std::flat_map<std::string, value, std::less<>>;

    /// Nesting limit applied by `parse()`. Depth counts container levels: the
    /// root value has depth 1 and a scalar nested inside N arrays/objects has
    /// depth N+1, so at most `max_depth - 1` containers may enclose the
    /// innermost scalar. Deeper input fails with `parse_error` instead of
    /// risking unbounded recursion (the limit is also enforced inside the
    /// parsing backend).
    static constexpr std::size_t max_depth = 128;

    /// Null value.
    value() noexcept = default;
    explicit value(std::nullptr_t /*unused*/) noexcept {}
    explicit value(bool boolean) noexcept : data_{boolean} {}
    explicit value(std::int64_t integer) noexcept : data_{integer} {}
    explicit value(double number) noexcept : data_{number} {}

    /// Other integral types convert to `std::int64_t` (the DOM's integer
    /// representation) as long as the conversion cannot silently wrap:
    /// unsigned types wider than 63 bits (`std::uint64_t`, `std::size_t` on
    /// 64-bit hosts) are rejected at compile time; cast explicitly to
    /// `std::int64_t` after a range check when such a value is known to fit.
    template <std::integral Integer>
        requires(!std::same_as<std::remove_cvref_t<Integer>, bool> &&
                 (std::numeric_limits<Integer>::digits <= 63))
    explicit value(Integer integer) noexcept
        : data_{static_cast<std::int64_t>(integer)} {}

    /// UTF-8 string (copied). A null `const char *` constructs a null value
    /// (matching JSON's `null`, unlike the empty-string interpretation).
    explicit value(std::string text) : data_{std::move(text)} {}
    explicit value(std::string_view text) : data_{std::string{text}} {}
    explicit value(const char *text) : data_{} {
        if (text != nullptr) {
            data_ = std::string{text};
        }
    }

    explicit value(array_type elements) : data_{std::move(elements)} {}
    explicit value(object_type members) : data_{std::move(members)} {}

    // ------------------------------------------------------------------
    // Parsing
    // ------------------------------------------------------------------

    /// Parse one JSON document from UTF-8 text. Returns `parse_error` for
    /// malformed input or integer literals outside the `std::int64_t` range.
    [[nodiscard]] static auto parse(std::string_view text) -> result<value>;

    /// Parse one JSON document from bytes; the bytes must be UTF-8.
    [[nodiscard]] static auto parse(std::span<const std::byte> bytes)
        -> result<value>;

    /// Read `path` with core::io and parse it. I/O failures keep their
    /// `io_error`; malformed content reports `parse_error` naming the path.
    [[nodiscard]] static auto parse_file(std::string_view path)
        -> result<value>;

    // ------------------------------------------------------------------
    // Queries
    // ------------------------------------------------------------------

    /// Kind of this node.
    [[nodiscard]] auto kind() const noexcept -> type;

    [[nodiscard]] auto is_null() const noexcept -> bool;
    [[nodiscard]] auto is_boolean() const noexcept -> bool;
    [[nodiscard]] auto is_integer() const noexcept -> bool;
    [[nodiscard]] auto is_real() const noexcept -> bool;
    /// True for `integer` and `real`.
    [[nodiscard]] auto is_number() const noexcept -> bool;
    [[nodiscard]] auto is_string() const noexcept -> bool;
    [[nodiscard]] auto is_array() const noexcept -> bool;
    [[nodiscard]] auto is_object() const noexcept -> bool;

    // ------------------------------------------------------------------
    // Typed accessors (value or error; no throwing/aborting accessors)
    // ------------------------------------------------------------------

    /// `boolean` only.
    [[nodiscard]] auto as_bool() const -> result<bool>;

    /// `integer` only (`real` is not truncated; use `as_double()`).
    [[nodiscard]] auto as_int64() const -> result<std::int64_t>;

    /// `real`, or `integer` converted to double (documented int→double).
    [[nodiscard]] auto as_double() const -> result<double>;

    /// `string` only. The view is valid while this value is alive and not
    /// modified; embedded NUL bytes are preserved.
    [[nodiscard]] auto as_string() const -> result<std::string_view>;

    /// `array` only; span over the owned elements (stable while alive).
    [[nodiscard]] auto as_array() const -> result<std::span<const value>>;

    /// `object` only; the sorted map (stable while alive).
    [[nodiscard]] auto as_object() const
        -> result<std::reference_wrapper<const object_type>>;

    // ------------------------------------------------------------------
    // Object navigation (`null` value reference for missing keys)
    // ------------------------------------------------------------------

    /// True when this is an object containing `key`.
    [[nodiscard]] auto contains(std::string_view key) const noexcept -> bool;

    /// Member for `key`, or nullptr when this is not an object / key missing.
    [[nodiscard]] auto find(std::string_view key) const noexcept
        -> const value *;

    /// Strict member lookup: `invalid_argument` when this is not an object,
    /// `not_found` when the key is missing.
    [[nodiscard]] auto at(std::string_view key) const
        -> result<std::reference_wrapper<const value>>;

    /// Lenient member lookup: returns the member for `key`; a missing key (or
    /// a non-object receiver) yields an immutable null value reference. Never
    /// inserts, never throws.
    [[nodiscard]] auto operator[](std::string_view key) const noexcept
        -> const value &;

    /// Element count for arrays/objects, byte length for strings, 0 otherwise.
    [[nodiscard]] auto size() const noexcept -> std::size_t;

    // ------------------------------------------------------------------
    // Serialization (deterministic; sorted object keys)
    // ------------------------------------------------------------------

    /// Compact single-line JSON. No trailing newline.
    [[nodiscard]] auto dump() const -> std::string;

    /// Indented multi-line JSON (objects/arrays only; empty containers stay
    /// `{}`/`[]`). `indent` spaces per nesting level (clamped to 1..16). No
    /// trailing newline.
    [[nodiscard]] auto dump_pretty(std::size_t indent = 2) const -> std::string;

    /// Structural equality (NaN never appears in parsed documents).
    [[nodiscard]] friend auto operator==(const value &lhs, const value &rhs)
        -> bool = default;

  private:
    using storage = std::variant<std::monostate, bool, std::int64_t, double,
                                 std::string, array_type, object_type>;

    storage data_{};

    /// Serialization driver defined in src/data/json.cpp: walks the DOM and
    /// feeds simdjson's `string_builder` (JSON syntax/escaping live there).
    /// Keeping the traversal out of the header keeps simdjson private.
    friend class detail::writer;
};

} // namespace refrain::data::json
