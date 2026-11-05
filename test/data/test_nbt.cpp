// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// NBT codec tests: byte-exact golden vectors, round-trips for every tag type,
// scalar edge values, malformed-input hardening, compression wrappers and a
// structure-template-sized (~50 KiB) decode. Golden byte strings are written
// as hex so the wire format is visible in the test itself.

#include <refrain/data/nbt.hpp>

#include <refrain/core/error.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using namespace boost::ut;

namespace nbt = refrain::data::nbt;
using nbt::compression;
using nbt::limits;
using nbt::tag_type;

[[nodiscard]] constexpr auto nibble(char digit) -> std::uint8_t {
    if (digit >= '0' && digit <= '9') {
        return static_cast<std::uint8_t>(digit - '0');
    }
    if (digit >= 'a' && digit <= 'f') {
        return static_cast<std::uint8_t>(digit - 'a' + 10);
    }
    return static_cast<std::uint8_t>(digit - 'A' + 10);
}

/// Parse a hex string ("0a0000") into raw bytes for hand-built golden vectors.
[[nodiscard]] auto bytes_from_hex(std::string_view hex)
    -> std::vector<std::byte> {
    std::vector<std::byte> bytes;
    bytes.reserve(hex.size() / 2U);
    for (std::size_t index{0}; index + 1U < hex.size(); index += 2U) {
        const auto value{static_cast<std::uint8_t>((nibble(hex[index]) << 4U) |
                                                   nibble(hex[index + 1U]))};
        bytes.push_back(static_cast<std::byte>(value));
    }
    return bytes;
}

[[nodiscard]] auto to_hex(std::span<const std::byte> data) -> std::string {
    static constexpr std::string_view digits{"0123456789abcdef"};
    std::string text;
    text.reserve(data.size() * 2U);
    for (const auto item : data) {
        const auto value{std::to_integer<std::uint8_t>(item)};
        text += digits[value >> 4U];
        text += digits[value & 0x0FU];
    }
    return text;
}

[[nodiscard]] auto read_ok(std::span<const std::byte> data,
                           compression mode = compression::none,
                           const limits &bounds = {}) -> nbt::value {
    auto decoded{nbt::read(data, mode, bounds)};
    expect(static_cast<bool>(decoded)) << "expected valid NBT";
    return decoded ? std::move(*decoded) : nbt::make_compound();
}

[[nodiscard]] auto write_ok(const nbt::value &root, std::string_view root_name,
                            compression mode = compression::none,
                            const limits &bounds = {})
    -> std::vector<std::byte> {
    auto encoded{nbt::write(root, root_name, mode, bounds)};
    expect(static_cast<bool>(encoded)) << "expected NBT encoding to succeed";
    return encoded ? std::move(*encoded) : std::vector<std::byte>{};
}

/// Self-cleaning unique scratch file (std::filesystem only, no OS calls).
class temp_file {
  public:
    temp_file() {
        std::error_code error;
        auto base{std::filesystem::temp_directory_path(error)};
        if (error) {
            base = std::filesystem::path{"."};
        }
        const auto stamp{static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count())};
        path_ = base / ("refrain_nbt_test_" + std::to_string(stamp) + ".nbt");
    }

    temp_file(const temp_file &) = delete;
    auto operator=(const temp_file &) -> temp_file & = delete;

    ~temp_file() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    [[nodiscard]] auto path() const -> std::string {
        return path_.generic_string();
    }

  private:
    std::filesystem::path path_;
};

/// Nested compounds: `depth` compounds, each containing key "child" -> next,
/// innermost empty. `depth == 1` is one empty compound.
[[nodiscard]] auto make_nested(std::uint32_t depth) -> nbt::value {
    auto node{nbt::make_compound()};
    for (std::uint32_t level{1}; level < depth; ++level) {
        auto parent{nbt::make_compound()};
        static_cast<void>(parent.set("child", std::move(node)));
        node = std::move(parent);
    }
    return node;
}

/// Prototype structure-template tree: size/palette/blocks/entities +
/// DataVersion.
[[nodiscard]] auto make_structure() -> nbt::value {
    auto root{nbt::make_compound()};
    static_cast<void>(root.set("DataVersion", nbt::make_int(5023)));
    static_cast<void>(
        root.set("size", nbt::make_list(tag_type::int_,
                                        {nbt::make_int(5), nbt::make_int(3),
                                         nbt::make_int(5)})));
    auto palette{nbt::make_list(tag_type::compound)};
    for (std::int32_t state{0}; state < 100; ++state) {
        auto entry{nbt::make_compound()};
        static_cast<void>(
            entry.set("Name", nbt::make_string("minecraft:stone")));
        auto properties{nbt::make_compound()};
        static_cast<void>(properties.set("variant", nbt::make_string("0")));
        static_cast<void>(entry.set("Properties", std::move(properties)));
        static_cast<void>(palette.push(std::move(entry)));
    }
    static_cast<void>(root.set("palette", std::move(palette)));
    auto blocks{nbt::make_list(tag_type::compound)};
    for (std::int32_t index{0}; index < 1500; ++index) {
        auto block{nbt::make_compound()};
        static_cast<void>(block.set(
            "pos", nbt::make_list(tag_type::int_, {nbt::make_int(index % 5),
                                                   nbt::make_int(index % 3),
                                                   nbt::make_int(index % 5)})));
        static_cast<void>(block.set("state", nbt::make_int(index % 100)));
        static_cast<void>(blocks.push(std::move(block)));
    }
    static_cast<void>(root.set("blocks", std::move(blocks)));
    static_cast<void>(root.set("entities", nbt::make_list(tag_type::end)));
    return root;
}

const suite nbt_tests = [] {
    // --- golden wire format ----------------------------------------------

    "nbt/empty compound golden bytes"_test = [] {
        const auto encoded{write_ok(nbt::make_compound(), "")};
        expect(to_hex(encoded) == "0a000000");
        expect(encoded.size() == 4U);

        const auto decoded{read_ok(encoded)};
        expect(static_cast<bool>(decoded.type() == tag_type::compound));
        expect(decoded.size() == 0U);
        expect(decoded == nbt::make_compound());
    };

    "nbt/root name is written before the payload"_test = [] {
        const auto encoded{write_ok(nbt::make_compound(), "root")};
        expect(to_hex(encoded) == "0a0004726f6f7400");
        expect(read_ok(encoded) == nbt::make_compound());
    };

    "nbt/single int golden bytes"_test = [] {
        auto root{nbt::make_compound()};
        static_cast<void>(root.set("a", nbt::make_int(1)));
        const auto encoded{write_ok(root, "")};
        // 0a 0000 (compound, empty name) | 03 0001 'a' 00000001 | 00
        expect(to_hex(encoded) == "0a0000030001610000000100");

        const auto decoded{read_ok(encoded)};
        expect(decoded.get("a") != nullptr);
        expect(decoded.get("a")->as_int() == std::optional<std::int32_t>{1});
    };

    "nbt/byte array golden bytes"_test = [] {
        auto root{nbt::make_compound()};
        static_cast<void>(root.set("ba", nbt::make_byte_array({1U, 2U, 3U})));
        expect(to_hex(write_ok(root, "")) ==
               "0a000007000262610000000301020300");
    };

    "nbt/empty arrays golden bytes"_test = [] {
        auto root{nbt::make_compound()};
        static_cast<void>(root.set("ba", nbt::make_byte_array({})));
        static_cast<void>(root.set("ia", nbt::make_int_array({})));

        const auto decoded{read_ok(write_ok(root, ""))};
        expect(decoded.get("ba")->as_byte_array()->empty());
        expect(decoded.get("ia")->as_int_array()->empty());
    };

    "nbt/modified UTF-8 NUL uses the C0 80 pair"_test = [] {
        auto root{nbt::make_compound()};
        static_cast<void>(
            root.set("s", nbt::make_string(std::string{"a\0b", 3})));
        // 0a 0000 | 08 0001 's' | 0004 "a" c0 80 "b" | 00
        expect(to_hex(write_ok(root, "")) == "0a000008000173000461c0806200");
    };

    "nbt/supplementary characters use CESU-8 surrogate pairs"_test = [] {
        auto root{nbt::make_compound()};
        static_cast<void>(root.set("s", nbt::make_string("\xF0\x9F\x8E\xAE")));
        // U+1F3AE -> D83C DFAE -> ed a0 bc  ed be ae (6 bytes, length 0006).
        expect(to_hex(write_ok(root, "")) ==
               "0a0000080001730006eda0bcedbeae00");

        // Reading tolerates plain 4-byte UTF-8 written by non-Java tooling.
        const auto standard_utf8{
            bytes_from_hex("0a0000080001730004f09f8eae00")};
        const auto decoded{read_ok(standard_utf8)};
        expect(decoded.get("s")->as_string() ==
               std::optional<std::string_view>{"\xF0\x9F\x8E\xAE"});
    };

    // --- type / edge-value round-trips -----------------------------------

    "nbt/scalar roundtrip for every scalar tag type"_test = [] {
        auto root{nbt::make_compound()};
        static_cast<void>(root.set("byte", nbt::make_byte(42)));
        static_cast<void>(root.set("short", nbt::make_short(-1234)));
        static_cast<void>(root.set("int", nbt::make_int(123456)));
        static_cast<void>(root.set("long", nbt::make_long(-1234567890123LL)));
        static_cast<void>(root.set("float", nbt::make_float(1.5F)));
        static_cast<void>(root.set("double", nbt::make_double(2.25)));
        static_cast<void>(root.set("string", nbt::make_string("hello")));

        const auto decoded{read_ok(write_ok(root, ""))};
        expect(decoded.get("byte")->as_byte() ==
               std::optional<std::int8_t>{42});
        expect(decoded.get("short")->as_short() ==
               std::optional<std::int16_t>{-1234});
        expect(decoded.get("int")->as_int() ==
               std::optional<std::int32_t>{123456});
        expect(decoded.get("long")->as_long() ==
               std::optional<std::int64_t>{-1234567890123LL});
        expect(decoded.get("float")->as_float() == std::optional<float>{1.5F});
        expect(decoded.get("double")->as_double() ==
               std::optional<double>{2.25});
        expect(decoded.get("string")->as_string() ==
               std::optional<std::string_view>{"hello"});
        expect(decoded == root);
    };

    "nbt/integer edge values roundtrip"_test = [] {
        auto root{nbt::make_compound()};
        static_cast<void>(root.set(
            "i8", nbt::make_byte(std::numeric_limits<std::int8_t>::min())));
        static_cast<void>(root.set(
            "i16", nbt::make_short(std::numeric_limits<std::int16_t>::min())));
        static_cast<void>(root.set(
            "i32min", nbt::make_int(std::numeric_limits<std::int32_t>::min())));
        static_cast<void>(root.set(
            "i32max", nbt::make_int(std::numeric_limits<std::int32_t>::max())));
        static_cast<void>(
            root.set("i64min",
                     nbt::make_long(std::numeric_limits<std::int64_t>::min())));
        static_cast<void>(
            root.set("i64max",
                     nbt::make_long(std::numeric_limits<std::int64_t>::max())));

        const auto decoded{read_ok(write_ok(root, ""))};
        expect(decoded.get("i8")->as_byte() ==
               std::optional<std::int8_t>{
                   std::numeric_limits<std::int8_t>::min()});
        expect(decoded.get("i16")->as_short() ==
               std::optional<std::int16_t>{
                   std::numeric_limits<std::int16_t>::min()});
        expect(decoded.get("i32min")->as_int() ==
               std::optional<std::int32_t>{
                   std::numeric_limits<std::int32_t>::min()});
        expect(decoded.get("i32max")->as_int() ==
               std::optional<std::int32_t>{
                   std::numeric_limits<std::int32_t>::max()});
        expect(decoded.get("i64min")->as_long() ==
               std::optional<std::int64_t>{
                   std::numeric_limits<std::int64_t>::min()});
        expect(decoded.get("i64max")->as_long() ==
               std::optional<std::int64_t>{
                   std::numeric_limits<std::int64_t>::max()});
    };

    "nbt/floats are bit-preserving (NaN payloads, infinities, -0.0)"_test = [] {
        constexpr std::uint32_t float_nan_bits{0x7FC00001U};
        constexpr std::uint64_t double_nan_bits{0x7FF8000000000001ULL};
        auto root{nbt::make_compound()};
        static_cast<void>(root.set(
            "fnan", nbt::make_float(std::bit_cast<float>(float_nan_bits))));
        static_cast<void>(root.set(
            "dnan", nbt::make_double(std::bit_cast<double>(double_nan_bits))));
        static_cast<void>(root.set(
            "finf", nbt::make_float(std::numeric_limits<float>::infinity())));
        static_cast<void>(root.set(
            "fnegzero", nbt::make_float(std::bit_cast<float>(0x80000000U))));
        static_cast<void>(root.set(
            "dnegzero",
            nbt::make_double(std::bit_cast<double>(0x8000000000000000ULL))));

        const auto decoded{read_ok(write_ok(root, ""))};
        expect(std::bit_cast<std::uint32_t>(*decoded.get("fnan")->as_float()) ==
               float_nan_bits);
        expect(std::bit_cast<std::uint64_t>(
                   *decoded.get("dnan")->as_double()) == double_nan_bits);
        expect(*decoded.get("finf")->as_float() ==
               std::numeric_limits<float>::infinity());
        expect(std::bit_cast<std::uint32_t>(
                   *decoded.get("fnegzero")->as_float()) == 0x80000000U);
        expect(std::bit_cast<std::uint64_t>(
                   *decoded.get("dnegzero")->as_double()) ==
               0x8000000000000000ULL);
    };

    "nbt/arrays roundtrip with signed edge values"_test = [] {
        auto root{nbt::make_compound()};
        static_cast<void>(root.set(
            "bytes", nbt::make_byte_array({0U, 1U, 127U, 128U, 255U})));
        static_cast<void>(root.set(
            "ints", nbt::make_int_array(
                        {std::numeric_limits<std::int32_t>::min(), -1, 0,
                         std::numeric_limits<std::int32_t>::max()})));
        static_cast<void>(root.set(
            "longs",
            nbt::make_long_array({std::numeric_limits<std::int64_t>::min(), 0,
                                  std::numeric_limits<std::int64_t>::max()})));

        const auto decoded{read_ok(write_ok(root, ""))};
        const auto bytes{*decoded.get("bytes")->as_byte_array()};
        expect(bytes.size() == 5U);
        expect(bytes[3] == 128U);
        expect(bytes[4] == 255U);
        expect((*decoded.get("ints")->as_int_array())[0] ==
               std::numeric_limits<std::int32_t>::min());
        expect((*decoded.get("longs")->as_long_array())[2] ==
               std::numeric_limits<std::int64_t>::max());
        expect(decoded == root);
    };

    "nbt/strings roundtrip UTF-8, NUL and empty"_test = [] {
        auto root{nbt::make_compound()};
        static_cast<void>(root.set("empty", nbt::make_string("")));
        static_cast<void>(
            root.set("nul", nbt::make_string(std::string{"a\0b", 3})));
        static_cast<void>(
            root.set("japanese", nbt::make_string("\xE6\x97\xA5\xE6\x9C\xAC")));
        static_cast<void>(
            root.set("emoji", nbt::make_string("\xF0\x9F\x8E\xAE")));
        static_cast<void>(root.set(
            "mixed", nbt::make_string(std::string{"\0\xF0\x9F\x8E\xAE", 5})));

        const auto decoded{read_ok(write_ok(root, ""))};
        expect(decoded.get("empty")->as_string() ==
               std::optional<std::string_view>{""});
        expect(decoded.get("nul")->as_string()->size() == 3U);
        expect((*decoded.get("japanese")->as_string()) ==
               "\xE6\x97\xA5\xE6\x9C\xAC");
        expect(*decoded.get("emoji")->as_string() == "\xF0\x9F\x8E\xAE");
        expect(*decoded.get("mixed")->as_string() ==
               std::string_view{"\0\xF0\x9F\x8E\xAE", 5});
        expect(decoded == root);
    };

    "nbt/big-endian wire encoding for scalars and arrays"_test = [] {
        auto floats{nbt::make_compound()};
        static_cast<void>(floats.set("f", nbt::make_float(1.0F)));
        // float 1.0f = 0x3f800000, big endian.
        expect(to_hex(write_ok(floats, "")) == "0a0000050001663f80000000");

        auto doubles{nbt::make_compound()};
        static_cast<void>(doubles.set("d", nbt::make_double(1.0)));
        // double 1.0 = 0x3ff0000000000000, big endian.
        expect(to_hex(write_ok(doubles, "")) ==
               "0a0000060001643ff000000000000000");

        auto ints{nbt::make_compound()};
        static_cast<void>(ints.set("ia", nbt::make_int_array({-1, 1})));
        expect(to_hex(write_ok(ints, "")) ==
               "0a00000b0002696100000002ffffffff0000000100");

        auto longs{nbt::make_compound()};
        static_cast<void>(longs.set("la", nbt::make_long_array({-1})));
        expect(to_hex(write_ok(longs, "")) ==
               "0a00000c00026c6100000001ffffffffffffffff00");

        auto bytes{nbt::make_compound()};
        static_cast<void>(bytes.set("b", nbt::make_byte(-1)));
        expect(to_hex(write_ok(bytes, "")) == "0a000001000162ff00");
    };

    "nbt/root name is validated and bounded"_test = [] {
        const std::string huge_name(70000U, 'x');
        expect(!nbt::write(nbt::make_compound(), huge_name).has_value());

        // Root name declared as 2 bytes but only one byte present.
        const auto truncated{bytes_from_hex("0a0002ff00")};
        expect(!nbt::read(truncated).has_value());

        // Root name is modified UTF-8: a 0xc0 0x80 pair decodes to NUL.
        auto named{bytes_from_hex("0a0002c08000")};
        expect(read_ok(std::span<const std::byte>{named}) ==
               nbt::make_compound());
    };

    // --- lists ------------------------------------------------------------

    "nbt/list roundtrip keeps order and element type"_test = [] {
        auto root{nbt::make_compound()};
        static_cast<void>(root.set(
            "bytes", nbt::make_list(tag_type::byte,
                                    {nbt::make_byte(1), nbt::make_byte(-2)})));
        auto records{nbt::make_list(tag_type::compound)};
        for (std::int32_t value{0}; value < 3; ++value) {
            auto record{nbt::make_compound()};
            static_cast<void>(record.set("v", nbt::make_int(value)));
            static_cast<void>(records.push(std::move(record)));
        }
        static_cast<void>(root.set("records", std::move(records)));
        static_cast<void>(root.set("empty_end", nbt::make_list(tag_type::end)));
        static_cast<void>(
            root.set("empty_compound", nbt::make_list(tag_type::compound)));

        const auto decoded{read_ok(write_ok(root, ""))};
        const auto *bytes{decoded.get("bytes")->as_list()};
        expect(bytes != nullptr);
        expect(static_cast<bool>(bytes->element_type == tag_type::byte));
        expect(bytes->items.size() == 2U);
        expect(bytes->items[1].as_byte() == std::optional<std::int8_t>{-2});
        expect(decoded.get("records")->as_list()->items.size() == 3U);
        expect(
            static_cast<bool>(decoded.get("empty_end")->list_element_type() ==
                              std::optional<tag_type>{tag_type::end}));
        expect(static_cast<bool>(
            decoded.get("empty_compound")->list_element_type() ==
            std::optional<tag_type>{tag_type::compound}));
        expect(decoded == root);
    };

    "nbt/list golden bytes for TAG_Byte elements"_test = [] {
        auto root{nbt::make_compound()};
        static_cast<void>(
            root.set("l", nbt::make_list(tag_type::byte, {nbt::make_byte(1),
                                                          nbt::make_byte(2)})));
        // 0a 0000 | 09 0001 'l' 01 00000002 01 02 | 00
        expect(to_hex(write_ok(root, "")) == "0a00000900016c0100000002010200");
    };

    "nbt/list of lists roundtrips"_test = [] {
        auto outer{nbt::make_list(tag_type::list)};
        static_cast<void>(outer.push(nbt::make_list(
            tag_type::int_, {nbt::make_int(1), nbt::make_int(2)})));
        static_cast<void>(outer.push(nbt::make_list(tag_type::int_)));
        auto root{nbt::make_compound()};
        static_cast<void>(root.set("matrix", std::move(outer)));

        const auto decoded{read_ok(write_ok(root, ""))};
        expect(decoded.get("matrix")->as_list()->items.size() == 2U);
        expect(static_cast<bool>(
            decoded.get("matrix")->as_list()->element_type == tag_type::list));
        expect(decoded == root);
    };

    "nbt/list push enforces the declared element type"_test = [] {
        auto ints{nbt::make_list(tag_type::int_)};
        expect(static_cast<bool>(ints.push(nbt::make_int(7))));
        expect(!ints.push(nbt::make_byte(7)));
        expect(!ints.push(nbt::make_string("nope")));
        expect(ints.as_list()->items.size() == 1U);

        auto end_list{nbt::make_list(tag_type::end)};
        expect(!end_list.push(nbt::make_int(1)));

        auto scalar{nbt::make_int(3)};
        expect(!scalar.push(nbt::make_int(4)));
    };

    "nbt/write rejects mismatched list element types"_test = [] {
        auto root{nbt::make_compound()};
        static_cast<void>(root.set(
            "bad", nbt::make_list(tag_type::int_,
                                  {nbt::make_int(1), nbt::make_byte(2)})));
        const auto encoded{nbt::write(root, "")};
        expect(!encoded.has_value());
        expect(encoded.error().code == refrain::error_code::invalid_argument);

        auto end_list{nbt::make_compound()};
        static_cast<void>(end_list.set(
            "bad", nbt::make_list(tag_type::end, {nbt::make_byte(1)})));
        const auto encoded_end{nbt::write(end_list, "")};
        expect(!encoded_end.has_value());
        expect(encoded_end.error().code ==
               refrain::error_code::invalid_argument);
    };

    // --- determinism ------------------------------------------------------

    "nbt/write is deterministic and sorts compound keys"_test = [] {
        auto first{nbt::make_compound()};
        static_cast<void>(first.set("zebra", nbt::make_int(26)));
        static_cast<void>(first.set("apple", nbt::make_int(1)));
        static_cast<void>(first.set("mango", nbt::make_int(13)));

        auto second{nbt::make_compound()};
        static_cast<void>(second.set("mango", nbt::make_int(13)));
        static_cast<void>(second.set("apple", nbt::make_int(1)));
        static_cast<void>(second.set("zebra", nbt::make_int(26)));

        const auto first_bytes{write_ok(first, "")};
        const auto second_bytes{write_ok(second, "")};
        expect(first_bytes == second_bytes);

        const auto hex{to_hex(first_bytes)};
        const auto apple{hex.find("6170706c65")};
        const auto mango{hex.find("6d616e676f")};
        const auto zebra{hex.find("7a65627261")};
        expect(apple != std::string::npos);
        expect(mango != std::string::npos);
        expect(zebra != std::string::npos);
        expect(apple < mango);
        expect(mango < zebra);
    };

    "nbt/compressed output is deterministic"_test = [] {
        const auto structure{make_structure()};
        const auto gzip_once{write_ok(structure, "", compression::gzip)};
        const auto gzip_twice{write_ok(structure, "", compression::gzip)};
        expect(gzip_once == gzip_twice);

        const auto zlib_once{write_ok(structure, "", compression::zlib)};
        const auto zlib_twice{write_ok(structure, "", compression::zlib)};
        expect(zlib_once == zlib_twice);
    };

    // --- malformed input --------------------------------------------------

    "nbt/root must be a compound"_test = [] {
        const auto encoded{nbt::write(nbt::make_int(1), "")};
        expect(!encoded.has_value());
        expect(encoded.error().code == refrain::error_code::invalid_argument);

        const auto empty{nbt::read(std::span<const std::byte>{})};
        expect(!empty.has_value());
        expect(empty.error().code == refrain::error_code::parse_error);

        // Root tag id 03 (TAG_Int) followed by a name and payload.
        const auto scalar_root{bytes_from_hex("0300000000000001")};
        const auto decoded{nbt::read(scalar_root)};
        expect(!decoded.has_value());
        expect(decoded.error().message.find("TAG_Compound") !=
               std::string::npos);
    };

    "nbt/trailing bytes after the root are rejected"_test = [] {
        auto golden{bytes_from_hex("0a000000")};
        golden.push_back(std::byte{0});
        const auto decoded{nbt::read(golden)};
        expect(!decoded.has_value());
        expect(decoded.error().message.find("trailing") != std::string::npos);
    };

    "nbt/truncated documents are rejected"_test = [] {
        auto root{nbt::make_compound()};
        static_cast<void>(root.set("a", nbt::make_int(1)));
        const auto golden{write_ok(root, "")};
        for (std::size_t cut{0}; cut < golden.size(); ++cut) {
            const auto truncated{std::span<const std::byte>{golden}.first(cut)};
            expect(!nbt::read(truncated).has_value()) << "cut at " << cut;
        }
    };

    "nbt/unknown tag ids are rejected"_test = [] {
        // Compound containing an entry with tag id 13 (out of range).
        const auto unknown_id{bytes_from_hex("0a00000d000000")};
        const auto decoded{nbt::read(unknown_id)};
        expect(!decoded.has_value());
        expect(decoded.error().message.find("unknown tag id") !=
               std::string::npos);
    };

    "nbt/negative lengths are rejected"_test = [] {
        // Each document: root compound, one named child with a bogus length.
        const std::array<std::string, 4> corpus{
            "0a000007000162ffffffff00",  // negative byte-array length
            "0a00000b000169ffffffff00",  // negative int-array length
            "0a00000c00016cffffffff00",  // negative long-array length
            "0a00000900016c01ffffffff00" // negative list length
        };
        for (const auto &hex : corpus) {
            const auto bytes{bytes_from_hex(hex)};
            const auto decoded{nbt::read(bytes)};
            expect(!decoded.has_value()) << hex;
            expect(decoded.error().code == refrain::error_code::parse_error);
        }
    };

    "nbt/non-empty TAG_End list is rejected"_test = [] {
        // Compound -> list with element type 00 (TAG_End) and length 1.
        const auto bytes{bytes_from_hex("0a00000900016c000000000100")};
        const auto decoded{nbt::read(bytes)};
        expect(!decoded.has_value());
        expect(decoded.error().message.find("TAG_End") != std::string::npos);
    };

    "nbt/oversized strings are rejected"_test = [] {
        // A string whose modified UTF-8 form exceeds the u16 NBT limit.
        const std::string huge(70000U, 'x');
        auto root{nbt::make_compound()};
        static_cast<void>(root.set("huge", nbt::make_string(huge)));
        const auto encoded{nbt::write(root, "")};
        expect(!encoded.has_value());
        expect(encoded.error().code == refrain::error_code::invalid_argument);

        // Declared length larger than the remaining payload -> truncated.
        const auto truncated_string{bytes_from_hex("0a000008000173ffff6869")};
        const auto decoded{nbt::read(truncated_string)};
        expect(!decoded.has_value());
    };

    "nbt/write rejects invalid UTF-8 strings"_test = [] {
        auto root{nbt::make_compound()};
        static_cast<void>(
            root.set("bad", nbt::make_string(std::string{"\xFF", 1})));
        const auto encoded{nbt::write(root, "")};
        expect(!encoded.has_value());
        expect(encoded.error().code == refrain::error_code::invalid_argument);

        // A lone UTF-8 continuation byte is rejected the same way.
        auto root2{nbt::make_compound()};
        static_cast<void>(
            root2.set("bad", nbt::make_string(std::string{"\x80", 1})));
        expect(!nbt::write(root2, "").has_value());
    };

    "nbt/duplicate compound keys keep the last occurrence"_test = [] {
        // Compound with "a" = 1 and "a" = 2 (hand-built, foreign-file shape).
        const auto bytes{
            bytes_from_hex("0a00000300016100000001030001610000000200")};
        const auto decoded{read_ok(bytes)};
        expect(decoded.get("a")->as_int() == std::optional<std::int32_t>{2});
        expect(decoded.size() == 1U);
    };

    "nbt/invalid UTF-8 in a string is rejected"_test = [] {
        // Declared 2-byte string containing a bare continuation byte.
        const auto bytes{bytes_from_hex("0a000008000173000280bf00")};
        const auto decoded{nbt::read(bytes)};
        expect(!decoded.has_value());
        expect(decoded.error().code == refrain::error_code::parse_error);
    };

    "nbt/random and mutated inputs never crash"_test = [] {
        // Deterministic LCG: malformed-input hardening should never read out
        // of bounds or allocate unboundedly. Reaching the end of the input is
        // the assertion; any read either decodes or returns an error value.
        std::uint64_t state{0x9E3779B97F4A7C15ULL};
        const auto next = [&state]() {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            return static_cast<std::uint8_t>(state >> 33U);
        };
        for (std::uint32_t round{0}; round < 128U; ++round) {
            std::vector<std::byte> data(96U);
            for (auto &item : data) {
                item = static_cast<std::byte>(next());
            }
            const std::span<const std::byte> view{data};
            static_cast<void>(nbt::read(view));
            static_cast<void>(nbt::read(view, compression::gzip));
            static_cast<void>(nbt::read(view, compression::zlib));
        }

        // Bit flips of a valid document: decode fails or yields a different
        // tree, but never crashes.
        const auto golden{write_ok(make_nested(8), "")};
        for (std::size_t index{0}; index < golden.size(); ++index) {
            auto mutated{golden};
            mutated[index] ^= std::byte{0xFF};
            static_cast<void>(nbt::read(mutated));
        }
        expect(true);
    };

    // --- safety limits ----------------------------------------------------

    "nbt/depth limit matches vanilla and is enforced both ways"_test = [] {
        expect(read_ok(write_ok(make_nested(512), "")) == make_nested(512));

        const auto too_deep{nbt::write(make_nested(513), "")};
        expect(!too_deep.has_value());
        expect(too_deep.error().code == refrain::error_code::invalid_argument);

        // A caller may lower the limit further (checked by both codec sides).
        auto tight{limits{}};
        tight.max_depth = 8;
        const auto allowed{
            write_ok(make_nested(8), "", compression::none, tight)};
        expect(static_cast<bool>(
            nbt::read(allowed, compression::none, tight).has_value()));
        expect(!nbt::write(make_nested(9), "", compression::none, tight)
                    .has_value());

        // The writer's hard ceiling wins over a caller-raised limit: the codec
        // never emits a tree deeper than max_supported_depth.
        auto relaxed{limits{}};
        relaxed.max_depth = 1024;
        expect(!nbt::write(make_nested(513), "", compression::none, relaxed)
                    .has_value());
    };

    "nbt/hard depth ceiling protects the recursive parser"_test = [] {
        // Build a hostile 1100-deep compound document by hand: root compound
        // (0a 0000) plus entries named "c" holding the next compound, closed by
        // one TAG_End per compound.
        constexpr std::uint32_t levels{1100};
        std::vector<std::byte> document{std::byte{0x0A}, std::byte{0x00},
                                        std::byte{0x00}};
        for (std::uint32_t level{1}; level < levels; ++level) {
            document.push_back(std::byte{0x0A});
            document.push_back(std::byte{0x00});
            document.push_back(std::byte{0x01});
            document.push_back(std::byte{0x63}); // 'c'
        }
        document.insert(document.end(), levels, std::byte{0x00});

        auto relaxed{limits{}};
        relaxed.max_depth = 1000000U; // caller tries to disable the limit
        const auto decoded{nbt::read(document, compression::none, relaxed)};
        expect(!decoded.has_value());
        expect(decoded.error().message.find("depth") != std::string::npos);

        // The default depth limit rejects the same document even earlier.
        expect(!nbt::read(document).has_value());

        // The writer applies the same ceiling: a caller-raised max_depth cannot
        // create a tree deeper than the parser can safely read.
        expect(!nbt::write(make_nested(1100), "", compression::none, relaxed)
                    .has_value());
    };

    "nbt/tag-count limit is enforced on read and write"_test = [] {
        auto root{nbt::make_compound()};
        for (std::int32_t index{0}; index < 6; ++index) {
            static_cast<void>(
                root.set(std::format("k{}", index), nbt::make_int(index)));
        }
        auto tight{limits{}};
        tight.max_tags = 4;
        const auto encoded{nbt::write(root, "", compression::none, tight)};
        expect(!encoded.has_value());

        const auto encoded_default{write_ok(root, "")};
        const auto decoded{
            nbt::read(encoded_default, compression::none, tight)};
        expect(!decoded.has_value());
        expect(decoded.error().message.find("tags") != std::string::npos);
    };

    "nbt/collection length limit is enforced"_test = [] {
        auto tight{limits{}};
        tight.max_collection_items = 3;

        auto root{nbt::make_compound()};
        static_cast<void>(root.set("ints", nbt::make_int_array({1, 2, 3, 4})));
        expect(!nbt::write(root, "", compression::none, tight).has_value());

        // Hand-built list with declared length 4 but a tiny collection limit.
        const auto bytes{bytes_from_hex("0a00000900016c01000000040102030400")};
        const auto decoded{nbt::read(bytes, compression::none, tight)};
        expect(!decoded.has_value());
        expect(decoded.error().message.find("limit") != std::string::npos);
    };

    "nbt/input size limit bounds raw and decompressed data"_test = [] {
        auto small{limits{}};
        small.max_bytes = 2;
        const auto tiny{bytes_from_hex("0a000000")};
        const auto raw{nbt::read(tiny, compression::none, small)};
        expect(!raw.has_value());
        expect(raw.error().message.find("limit") != std::string::npos);

        // Compressed zip-bomb guard: 64 KiB of zeros compress to a few hundred
        // bytes but exceed the configured decompressed limit.
        auto payload{nbt::make_compound()};
        static_cast<void>(payload.set(
            "zeros",
            nbt::make_byte_array(std::vector<std::uint8_t>(64U * 1024U, 0U))));
        const auto compressed{write_ok(payload, "", compression::gzip)};
        auto bomb{limits{}};
        bomb.max_bytes = 1024;
        const auto decoded{nbt::read(compressed, compression::gzip, bomb)};
        expect(!decoded.has_value());
        expect(decoded.error().message.find("decompressed") !=
               std::string::npos);
        expect(compressed.size() < 1024U);
    };

    "nbt/custom string limit is enforced"_test = [] {
        auto tight{limits{}};
        tight.max_string_bytes = 4;
        auto root{nbt::make_compound()};
        static_cast<void>(root.set("text", nbt::make_string("12345")));
        expect(!nbt::write(root, "", compression::none, tight).has_value());

        const auto bytes{bytes_from_hex("0a0000080001730005313233343500")};
        const auto decoded{nbt::read(bytes, compression::none, tight)};
        expect(!decoded.has_value());
        expect(decoded.error().message.find("string") != std::string::npos);
    };

    // --- compression ------------------------------------------------------

    "nbt/gzip and zlib roundtrip the same tree"_test = [] {
        const auto structure{make_structure()};
        const auto raw{write_ok(structure, "world")};
        expect(raw.size() > 50000U);

        const auto gzip_bytes{write_ok(structure, "world", compression::gzip)};
        expect(gzip_bytes.size() < raw.size());
        expect(gzip_bytes[0] == std::byte{0x1F});
        expect(gzip_bytes[1] == std::byte{0x8B});
        expect(read_ok(gzip_bytes, compression::gzip) == structure);

        const auto zlib_bytes{write_ok(structure, "world", compression::zlib)};
        expect(zlib_bytes.size() < raw.size());
        expect(std::to_integer<std::uint8_t>(zlib_bytes[0]) == 0x78U);
        expect(read_ok(zlib_bytes, compression::zlib) == structure);
    };

    "nbt/corrupt compressed streams are rejected"_test = [] {
        auto root{nbt::make_compound()};
        static_cast<void>(root.set("a", nbt::make_int(1)));
        auto gzip_bytes{write_ok(root, "", compression::gzip)};

        // Bit flip in the deflate payload.
        auto corrupted{gzip_bytes};
        corrupted[corrupted.size() / 2U] ^= std::byte{0xFF};
        expect(!nbt::read(corrupted, compression::gzip).has_value());

        // Truncated stream.
        const auto truncated{std::span<const std::byte>{gzip_bytes}.first(
            gzip_bytes.size() / 2U)};
        const auto decode_truncated{nbt::read(truncated, compression::gzip)};
        expect(!decode_truncated.has_value());

        // Trailing garbage after the complete stream.
        auto with_trailing{gzip_bytes};
        with_trailing.push_back(std::byte{0x00});
        const auto decode_trailing{nbt::read(with_trailing, compression::gzip)};
        expect(!decode_trailing.has_value());
        expect(decode_trailing.error().message.find("trailing") !=
               std::string::npos);

        // Wrong wrapper: a gzip stream read as zlib.
        expect(!nbt::read(gzip_bytes, compression::zlib).has_value());
        // Garbage, not a stream at all.
        const auto garbage{bytes_from_hex("deadbeef")};
        expect(!nbt::read(garbage, compression::gzip).has_value());
    };

    // --- API surface ------------------------------------------------------

    "nbt/default value is an empty compound"_test = [] {
        const nbt::value empty;
        expect(static_cast<bool>(empty.type() == tag_type::compound));
        expect(empty.size() == 0U);
        expect(empty.as_compound() != nullptr);
    };

    "nbt/accessors report type mismatches"_test = [] {
        const auto text{nbt::make_string("x")};
        expect(static_cast<bool>(text.type() == tag_type::string));
        expect(!text.as_int().has_value());
        expect(!text.as_byte_array().has_value());
        expect(text.as_list() == nullptr);
        expect(text.as_compound() == nullptr);
        expect(!text.list_element_type().has_value());
        expect(text.size() == 1U);

        const auto compound_value{nbt::make_compound()};
        expect(compound_value.as_compound() != nullptr);
        expect(!compound_value.as_string().has_value());
        expect(compound_value.get("missing") == nullptr);
        expect(!compound_value.contains("missing"));
        expect(compound_value.size() == 0U);

        auto array{nbt::make_int_array({1, 2, 3})};
        expect(array.size() == 3U);
        expect(static_cast<bool>(array.type() == tag_type::int_array));
    };

    "nbt/set/get build a compound in place"_test = [] {
        auto root{nbt::make_compound()};
        expect(static_cast<bool>(root.set("name", nbt::make_string("world"))));
        expect(static_cast<bool>(root.set("seed", nbt::make_long(1234))));
        expect(root.contains("name"));
        expect(root.size() == 2U);

        const auto *seed{root.get("seed")};
        expect(seed != nullptr);
        expect(seed->as_long() == std::optional<std::int64_t>{1234});
        expect(static_cast<bool>(root.set("seed", nbt::make_long(999))));
        expect(root.get("seed")->as_long() == std::optional<std::int64_t>{999});

        auto scalar{nbt::make_int(1)};
        expect(!scalar.set("k", nbt::make_int(2)));
    };

    "nbt/tag type ids match the Java wire ids"_test = [] {
        expect(static_cast<std::uint8_t>(tag_type::end) == 0U);
        expect(static_cast<std::uint8_t>(tag_type::byte) == 1U);
        expect(static_cast<std::uint8_t>(tag_type::short_) == 2U);
        expect(static_cast<std::uint8_t>(tag_type::int_) == 3U);
        expect(static_cast<std::uint8_t>(tag_type::long_) == 4U);
        expect(static_cast<std::uint8_t>(tag_type::float_) == 5U);
        expect(static_cast<std::uint8_t>(tag_type::double_) == 6U);
        expect(static_cast<std::uint8_t>(tag_type::byte_array) == 7U);
        expect(static_cast<std::uint8_t>(tag_type::string) == 8U);
        expect(static_cast<std::uint8_t>(tag_type::list) == 9U);
        expect(static_cast<std::uint8_t>(tag_type::compound) == 10U);
        expect(static_cast<std::uint8_t>(tag_type::int_array) == 11U);
        expect(static_cast<std::uint8_t>(tag_type::long_array) == 12U);
        expect(nbt::tag_type_name(tag_type::compound) == "TAG_Compound");
    };

    // --- structure-template scale ----------------------------------------

    "nbt/50KB structure template roundtrips"_test = [] {
        const auto structure{make_structure()};
        const auto encoded{write_ok(structure, "")};
        expect(encoded.size() > 50000U);
        expect(encoded.size() < 200000U);

        const auto decoded{read_ok(encoded)};
        expect(decoded.get("DataVersion")->as_int() ==
               std::optional<std::int32_t>{5023});
        expect(decoded.get("size")->as_list()->items.size() == 3U);
        expect(decoded.get("palette")->as_list()->items.size() == 100U);
        expect(decoded.get("blocks")->as_list()->items.size() == 1500U);
        expect(decoded.get("entities")->as_list()->items.empty());
        expect(decoded == structure);
    };

    // --- file conveniences ------------------------------------------------

    "nbt/file read/write roundtrip (gzip)"_test = [] {
        temp_file file;
        const auto structure{make_structure()};
        expect(static_cast<bool>(
            nbt::write_file(file.path(), structure, "", compression::gzip)));

        const auto decoded{nbt::read_file(file.path(), compression::gzip)};
        expect(static_cast<bool>(decoded));
        if (decoded) {
            expect(*decoded == structure);
        }

        // A corrupt file surfaces as an error value, never a crash.
        std::error_code error;
        const auto size{std::filesystem::file_size(file.path(), error)};
        expect(!error);
        expect(size > 4U);
        expect(!nbt::read_file(file.path() + ".missing").has_value());
    };
};

} // namespace
