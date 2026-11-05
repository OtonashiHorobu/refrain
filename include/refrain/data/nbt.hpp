// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Java Edition binary NBT codec (`refrain::data::nbt`).
//
// Used by the world import/export pipeline (level.dat, region chunks, player
// data) and by structure-template loading (`data/.../structure/*.nbt`). The
// wire format is the stock Java Edition layout (big endian, modified UTF-8
// strings, TAG_List element type byte); no SNBT parser lives here.
//
// Format / safety decisions documented up front because they are visible in
// the public API and in round-trip behaviour:
//
// * Root: a NBT file is `<tag id><root name><compound payload>`. `read`
//   requires the root tag to be TAG_Compound and discards the root name;
//   `write` always writes a TAG_Compound root and takes the name explicitly.
// * Compounds are stored as `std::map<std::string, value, std::less<>>`, so
//   encoding is sorted by key and deterministic. A compound decoded from a
//   foreign file therefore round-trips **sorted**, not in the original order
//   (NBT compound order is not semantically significant).
// * Lists are homogeneous: `{ element_type, items }`. The declared element
//   type is preserved across empty-list round-trips (TAG_End is legal only for
//   an empty list). `push()` enforces the element type while building; `write`
//   re-validates the whole tree so a hand-assembled model cannot emit a list
//   that a Java reader would reject.
// * Strings are NBT's modified UTF-8 (`DataOutput.writeUTF`: U+0000 encoded as
//   `C0 80`, supplementary code points encoded as CESU-8 surrogate pairs).
//   Values are exposed as ordinary standard UTF-8 `std::string`; the codec
//   translates in both directions. Reading is lenient towards plain 4-byte
//   UTF-8 written by non-Java tooling; writing always emits modified UTF-8 so
//   the result can be consumed by vanilla Minecraft.
// * Floats/doubles are bit-preserving (`std::bit_cast`): NaN payloads, +/-Inf
//   and -0.0 survive a round-trip unchanged.
// * Untrusted input is bounded by `limits` (defaults in the header): maximum
//   decompressed/raw byte count (zip-bomb and OOM guard, 64 MiB), nesting depth
//   (512, matches vanilla), string byte length (65535, the NBT u16 limit),
//   collection length and total tag count. Negative lengths, unknown tag ids,
//   non-empty TAG_End lists, type-mismatched lists and trailing bytes are
//   rejected with a `refrain::error`; nothing throws and no RTTI is used.
// * `compression::gzip`/`zlib` use zlib-ng's compat API (ZLIB::ZLIB). The
//   public header never includes zlib headers. Compressed input must be a
//   single complete stream (no trailing bytes) and its decompressed size is
//   bounded by `limits::max_bytes`.
//
// Round-trip guarantee: `read(write(v, name), mode) == v` for any valid `v`
// (compound ordering is normalised as documented above). The root name is not
// part of the returned value, so only the payload is compared.

#pragma once

#include <refrain/core/error.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace refrain::data::nbt {

/// NBT tag ids as defined by the Java Edition wire format (and the community
/// `NBT.txt` specification). The numeric values are part of the on-disk
/// format; keep them contiguous starting at 0.
enum class tag_type : std::uint8_t {
    end = 0,         ///< TAG_End: compound terminator / empty-list element type
    byte = 1,        ///< TAG_Byte: 1-byte signed integer
    short_ = 2,      ///< TAG_Short: 2-byte big-endian signed integer
    int_ = 3,        ///< TAG_Int: 4-byte big-endian signed integer
    long_ = 4,       ///< TAG_Long: 8-byte big-endian signed integer
    float_ = 5,      ///< TAG_Float: 4-byte IEEE-754 big-endian
    double_ = 6,     ///< TAG_Double: 8-byte IEEE-754 big-endian
    byte_array = 7,  ///< TAG_Byte_Array: i32 length + raw bytes
    string = 8,      ///< TAG_String: u16 length + modified UTF-8 bytes
    list = 9,        ///< TAG_List: element type byte + i32 length + payloads
    compound = 10,   ///< TAG_Compound: named tags until TAG_End
    int_array = 11,  ///< TAG_Int_Array: i32 length + i32 big-endian elements
    long_array = 12, ///< TAG_Long_Array: i32 length + i64 big-endian elements
};

/// Human-readable tag name for diagnostics (e.g. `"TAG_Compound"`). This is
/// not the wire format; the numeric ids above are.
[[nodiscard]] constexpr auto tag_type_name(tag_type kind) noexcept
    -> std::string_view {
    switch (kind) {
    case tag_type::end:
        return "TAG_End";
    case tag_type::byte:
        return "TAG_Byte";
    case tag_type::short_:
        return "TAG_Short";
    case tag_type::int_:
        return "TAG_Int";
    case tag_type::long_:
        return "TAG_Long";
    case tag_type::float_:
        return "TAG_Float";
    case tag_type::double_:
        return "TAG_Double";
    case tag_type::byte_array:
        return "TAG_Byte_Array";
    case tag_type::string:
        return "TAG_String";
    case tag_type::list:
        return "TAG_List";
    case tag_type::compound:
        return "TAG_Compound";
    case tag_type::int_array:
        return "TAG_Int_Array";
    case tag_type::long_array:
        return "TAG_Long_Array";
    }
    return "TAG_Unknown";
}

/// Default safety limits for decoding untrusted NBT. They are generous for
/// vanilla data (largest known `.nbt` files are well below 1 MiB) while
/// stopping zip-bomb/OOM/stack-exhaustion attacks.
inline constexpr std::size_t default_max_bytes{64ULL * 1024ULL * 1024ULL};
inline constexpr std::uint32_t default_max_depth{512};
inline constexpr std::size_t default_max_string_bytes{65535ULL};
inline constexpr std::size_t default_max_collection_items{1ULL << 20U};
inline constexpr std::size_t default_max_tags{4ULL << 20U};

/// Hard ceiling for `limits::max_depth` regardless of what a caller passes:
/// the codec is a recursive-descent parser, so an unbounded configured depth
/// could still exhaust the stack on a hostile document. This ceiling equals the
/// vanilla limit (512), which is also what sanitizer builds (larger stack
/// frames) can safely recurse through; callers may only lower the limit.
inline constexpr std::uint32_t max_supported_depth{512};

struct limits {
    /// Maximum raw input size / total decompressed size, in bytes.
    std::size_t max_bytes{default_max_bytes};
    /// Maximum nesting of compound/list containers; the root compound is
    /// depth 1 (512 matches vanilla's reader).
    std::uint32_t max_depth{default_max_depth};
    /// Maximum modified-UTF-8 byte length of a name or string value. The NBT
    /// wire length is a u16, hence 65535.
    std::size_t max_string_bytes{default_max_string_bytes};
    /// Maximum element count of a list or an int/long/byte array.
    std::size_t max_collection_items{default_max_collection_items};
    /// Maximum total number of tags in one document (memory-amplification
    /// guard for nested list/compound combinations).
    std::size_t max_tags{default_max_tags};
};

/// Wire compression. `none` is the raw NBT layout; the other two are the
/// standard gzip/zlib wrappers used by level.dat and region files.
enum class compression : std::uint8_t { none, gzip, zlib };

class value;

/// Homogeneous NBT list. `items` must all have `element_type`; an empty list
/// keeps its declared element type (`TAG_End` only when empty). Prefer
/// `make_list()` + `value::push()` when building so the invariant holds.
struct list_value {
    tag_type element_type{tag_type::end};
    std::vector<value> items;

    // Needed for `value`'s defaulted equality (std::variant compares every
    // alternative); a list is equal when the declared type and items match.
    [[nodiscard]] friend auto operator==(const list_value &, const list_value &)
        -> bool = default;
};

using byte_array = std::vector<std::uint8_t>;
using int_array = std::vector<std::int32_t>;
using long_array = std::vector<std::int64_t>;

/// Compound: ordered map so writes are deterministic (sorted by key). Lookups
/// are transparent over `std::string_view`.
using compound = std::map<std::string, value, std::less<>>;

/// One NBT tag value. Scalars are stored as their natural C++ type (the tag
/// type is implied by the variant alternative, see `type()`); arrays/strings
/// keep their element/byte order, and containers are the types above.
///
/// A `value` can never be TAG_End: end is only representable as the declared
/// element type of an empty list.
class value {
  public:
    using storage =
        std::variant<std::int8_t, std::int16_t, std::int32_t, std::int64_t,
                     float, double, byte_array, std::string, list_value,
                     compound, int_array, long_array>;

    value() noexcept : data_{compound{}} {}
    explicit value(storage data) : data_{std::move(data)} {}
    value(const value &) = default;
    value(value &&) noexcept = default;
    auto operator=(const value &) -> value & = default;
    auto operator=(value &&) noexcept -> value & = default;
    ~value() = default;

    /// NBT tag type of the stored alternative (never TAG_End).
    [[nodiscard]] auto type() const noexcept -> tag_type;

    /// Number of elements: compound entries, list items, array elements,
    /// string bytes; scalar tags report 1.
    [[nodiscard]] auto size() const noexcept -> std::size_t;

    [[nodiscard]] auto is(tag_type kind) const noexcept -> bool {
        return type() == kind;
    }

    /// Scalar accessors; `std::nullopt` when the type does not match.
    [[nodiscard]] auto as_byte() const noexcept -> std::optional<std::int8_t>;
    [[nodiscard]] auto as_short() const noexcept -> std::optional<std::int16_t>;
    [[nodiscard]] auto as_int() const noexcept -> std::optional<std::int32_t>;
    [[nodiscard]] auto as_long() const noexcept -> std::optional<std::int64_t>;
    [[nodiscard]] auto as_float() const noexcept -> std::optional<float>;
    [[nodiscard]] auto as_double() const noexcept -> std::optional<double>;

    /// String view (borrowed; valid while this value is alive and unchanged).
    [[nodiscard]] auto as_string() const noexcept
        -> std::optional<std::string_view>;

    /// Array views (borrowed).
    [[nodiscard]] auto as_byte_array() const noexcept
        -> std::optional<std::span<const std::uint8_t>>;
    [[nodiscard]] auto as_int_array() const noexcept
        -> std::optional<std::span<const std::int32_t>>;
    [[nodiscard]] auto as_long_array() const noexcept
        -> std::optional<std::span<const std::int64_t>>;

    /// Container views; `nullptr` when the type does not match.
    [[nodiscard]] auto as_list() const noexcept -> const list_value *;
    [[nodiscard]] auto as_compound() const noexcept -> const compound *;

    /// Declared element type of a list value (`std::nullopt` if not a list).
    [[nodiscard]] auto list_element_type() const noexcept
        -> std::optional<tag_type>;

    /// Compound child lookup. `nullptr` when this is not a compound or the key
    /// is absent. `get()` is non-const so callers can mutate in place.
    [[nodiscard]] auto get(std::string_view key) noexcept -> value *;
    [[nodiscard]] auto get(std::string_view key) const noexcept
        -> const value *;
    [[nodiscard]] auto contains(std::string_view key) const noexcept -> bool;

    /// Insert/replace a child of a compound. Returns an `invalid_argument`
    /// error when this value is not a compound.
    [[nodiscard]] auto set(std::string key, value child) -> status;

    /// Append to a list, enforcing the declared element type. Fails for
    /// non-list values, for an empty-list TAG_End declaration, and for items
    /// whose type differs from the declared element type.
    [[nodiscard]] auto push(value item) -> status;

    [[nodiscard]] friend auto operator==(const value &, const value &)
        -> bool = default;

  private:
    template <typename T>
    [[nodiscard]] auto view() const noexcept -> std::optional<T> {
        if (const auto *stored{std::get_if<T>(&data_)}; stored != nullptr) {
            return *stored;
        }
        return std::nullopt;
    }

    storage data_;
};

// --- builders -------------------------------------------------------------
// Explicit factories keep tag types unambiguous (`value{42}` is TAG_Int, but
// integer narrowing would be error-prone); building is allocation-based, so no
// factory is noexcept.

[[nodiscard]] auto make_byte(std::int8_t item) -> value;
[[nodiscard]] auto make_short(std::int16_t item) -> value;
[[nodiscard]] auto make_int(std::int32_t item) -> value;
[[nodiscard]] auto make_long(std::int64_t item) -> value;
[[nodiscard]] auto make_float(float item) -> value;
[[nodiscard]] auto make_double(double item) -> value;
[[nodiscard]] auto make_string(std::string text) -> value;
[[nodiscard]] auto make_byte_array(byte_array items) -> value;
[[nodiscard]] auto make_int_array(int_array items) -> value;
[[nodiscard]] auto make_long_array(long_array items) -> value;

/// Empty compound (`TAG_Compound` with no children).
[[nodiscard]] auto make_compound() -> value;
/// Compound filled from `entries` (moved into the sorted map).
[[nodiscard]] auto make_compound(compound entries) -> value;

/// Empty list with the declared element type (TAG_End allowed only here, and
/// only while empty).
[[nodiscard]] auto make_list(tag_type element_type) -> value;
/// List with pre-built items; `write()` validates the element type of every
/// item. Prefer `make_list(type)` + `push()` for checked construction.
[[nodiscard]] auto make_list(tag_type element_type, std::vector<value> items)
    -> value;

// --- encoding / decoding --------------------------------------------------

/// Decode one NBT document. The root tag must be a named TAG_Compound (its
/// name is validated/discarded); trailing bytes after the root are an error.
/// `compression::none` treats `data` as raw NBT, gzip/zlib decompress first
/// (bounded by `bounds.max_bytes`).
[[nodiscard]] auto read(std::span<const std::byte> data,
                        compression mode = compression::none,
                        const limits &bounds = limits{}) -> result<value>;

/// Encode `root` (which must be TAG_Compound; the root name is written before
/// its payload). Encoding is deterministic: compounds are sorted by key and
/// gzip output uses a fixed header (mtime 0), so two calls with an equal tree
/// produce byte-identical output. Validation failures (TAG_End as a value,
/// mismatched list element types, strings over the u16 limit, depth/tag/limit
/// violations) are returned as `invalid_argument` errors.
[[nodiscard]] auto write(const value &root, std::string_view root_name,
                         compression mode = compression::none,
                         const limits &bounds = limits{})
    -> result<std::vector<std::byte>>;

/// `core::io::read_file` + `read`.
[[nodiscard]] auto read_file(std::string_view path,
                             compression mode = compression::none,
                             const limits &bounds = limits{}) -> result<value>;

/// `write` + `core::io::atomic_write_file` (readers never observe a torn file).
[[nodiscard]] auto write_file(std::string_view path, const value &root,
                              std::string_view root_name,
                              compression mode = compression::none,
                              const limits &bounds = limits{}) -> status;

} // namespace refrain::data::nbt
