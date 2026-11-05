// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/io.hpp>
#include <refrain/data/json.hpp>

#include <boost/ut.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace {

using namespace boost::ut;

namespace json = refrain::data::json;
namespace io = refrain::core::io;
using json::type;
using json::value;

[[nodiscard]] auto parse_ok(std::string_view text) -> value {
    auto parsed = value::parse(text);
    expect(static_cast<bool>(parsed)) << "expected valid JSON: " << text;
    return parsed ? std::move(*parsed) : value{};
}

[[nodiscard]] auto parse_error_of(std::string_view text) -> refrain::error {
    auto parsed = value::parse(text);
    expect(!parsed) << "expected invalid JSON: " << text;
    return parsed ? refrain::error{} : std::move(parsed.error());
}

[[nodiscard]] auto parse_error_of(std::span<const std::byte> bytes)
    -> refrain::error {
    auto parsed = value::parse(bytes);
    expect(!parsed) << "expected invalid JSON bytes";
    return parsed ? refrain::error{} : std::move(parsed.error());
}

/// Raw JSON bytes (the JSON codec works on byte spans too).
[[nodiscard]] auto as_bytes(std::string_view text) -> std::vector<std::byte> {
    std::vector<std::byte> bytes{};
    bytes.reserve(text.size());
    for (const char character : text) {
        bytes.push_back(
            static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    return bytes;
}

/// Self-cleaning unique scratch directory under the system temp dir.
class temp_directory {
  public:
    temp_directory() {
        std::error_code error{};
        auto base = std::filesystem::temp_directory_path(error);
        if (error) {
            base = std::filesystem::path{"."};
        }
        const auto stamp = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = base / ("refrain-json-test-" + std::to_string(stamp));
        std::filesystem::create_directories(path_, error);
    }

    ~temp_directory() {
        std::error_code error{};
        std::filesystem::remove_all(path_, error);
    }

    temp_directory(const temp_directory &) = delete;
    auto operator=(const temp_directory &) -> temp_directory & = delete;
    temp_directory(temp_directory &&) = delete;
    auto operator=(temp_directory &&) -> temp_directory & = delete;

    [[nodiscard]] auto path() const -> const std::filesystem::path & {
        return path_;
    }

  private:
    std::filesystem::path path_{};
};

/// Deeply nested array document: `[[[...value...]]]`.
[[nodiscard]] auto nested_array(std::size_t depth) -> std::string {
    std::string text{};
    text.reserve(depth * 2U + 1U);
    for (std::size_t index = 0; index < depth; ++index) {
        text.push_back('[');
    }
    text.push_back('1');
    for (std::size_t index = 0; index < depth; ++index) {
        text.push_back(']');
    }
    return text;
}

[[nodiscard]] auto complex_document() -> std::string {
    return std::string{
        R"({"id":42,"name":"stone","ratio":0.5,"enabled":true,)"
        R"("missing":null,"tags":["a","b"],"nested":{"z":1,"a":[1,2,3]},)"
        R"("empty_object":{},"empty_array":[],"unicode":"\u00e9\ud83d\ude00"})"};
}

const suite json_tests = [] {
    "json/parses all value kinds with the right type tag"_test = [] {
        const auto null_value = parse_ok("null");
        expect(null_value.kind() == type::null);
        expect(null_value.is_null());
        expect(!null_value.is_number());

        const auto boolean = parse_ok("true");
        expect(boolean.kind() == type::boolean);
        expect(boolean.is_boolean());
        expect(*boolean.as_bool());

        const auto integer = parse_ok("42");
        expect(integer.kind() == type::integer);
        expect(integer.is_integer());
        expect(integer.is_number());
        expect(*integer.as_int64() == 42);

        const auto real = parse_ok("1.5");
        expect(real.kind() == type::real);
        expect(real.is_real());
        expect(real.is_number());
        expect(*real.as_double() == 1.5);

        const auto text = parse_ok(R"("hello")");
        expect(text.kind() == type::string);
        expect(*text.as_string() == "hello");

        const auto array = parse_ok("[1,2,3]");
        expect(array.kind() == type::array);
        expect(array.size() == std::size_t{3});

        const auto object = parse_ok(R"({"a":1})");
        expect(object.kind() == type::object);
        expect(object.size() == std::size_t{1});
    };

    "json/type names are stable for diagnostics"_test = [] {
        expect(json::to_string(type::null) == "null");
        expect(json::to_string(type::boolean) == "boolean");
        expect(json::to_string(type::integer) == "integer");
        expect(json::to_string(type::real) == "real");
        expect(json::to_string(type::string) == "string");
        expect(json::to_string(type::array) == "array");
        expect(json::to_string(type::object) == "object");
    };

    "json/int64 boundaries stay integers"_test = [] {
        const auto maximum = parse_ok("9223372036854775807");
        expect(maximum.is_integer());
        expect(*maximum.as_int64() == std::int64_t{9223372036854775807});
        expect(parse_ok("9223372036854775807").dump() == "9223372036854775807");

        const auto minimum = parse_ok("-9223372036854775808");
        expect(minimum.is_integer());
        expect(*minimum.as_int64() ==
               (std::numeric_limits<std::int64_t>::min)());

        const auto zero = parse_ok("0");
        expect(*zero.as_int64() == 0);
        // `-0` is an integer literal: JSON has no negative integer zero.
        expect(*parse_ok("-0").as_int64() == 0);
    };

    "json/integer literals above int64 are rejected, not widened"_test = [] {
        const auto too_large = parse_error_of("9223372036854775808");
        expect(too_large.code == refrain::error_code::parse_error);
        expect(too_large.message.find("int64") != std::string::npos);

        const auto too_small = parse_error_of("-9223372036854775809");
        expect(too_small.code == refrain::error_code::parse_error);

        const auto huge = parse_error_of("18446744073709551616");
        expect(huge.code == refrain::error_code::parse_error);
        expect(huge.message.find("64-bit") != std::string::npos);

        const auto enormous = parse_error_of("999999999999999999999999999999");
        expect(enormous.code == refrain::error_code::parse_error);
    };

    "json/real literals keep the real kind and negative zero"_test = [] {
        expect(parse_ok("1.0").is_real());
        expect(parse_ok("1e3").is_real());
        expect(*parse_ok("1e3").as_double() == 1000.0);
        expect(parse_ok("1E+2").is_real());
        expect(parse_ok("1e-3").is_real());
        expect(parse_ok("-2.5").is_real());

        const auto negative_zero = parse_ok("-0.0");
        expect(negative_zero.is_real());
        expect(std::signbit(*negative_zero.as_double()));
        expect(negative_zero.dump() == "-0.0");

        // Integers are not reals (the kind distinction is part of the API).
        expect(!parse_ok("1").is_real());
        expect(!parse_ok("1.0").is_integer());
    };

    "json/non-finite numbers are malformed JSON"_test = [] {
        for (const std::string_view text :
             {"1e999", "-1e999", "1e9999999", "NaN", "Infinity"}) {
            const auto failure = parse_error_of(text);
            expect(failure.code == refrain::error_code::parse_error) << text;
        }
    };

    "json/rejects malformed documents"_test = [] {
        for (const std::string_view text : {"",
                                            "   ",
                                            "\t\r\n",
                                            "// comment\n{}",
                                            "{} // trailing comment",
                                            "/*x*/{}",
                                            "{,}",
                                            "[1,2,]",
                                            "{\"a\":1,}",
                                            "[1 2]",
                                            "{\"a\" 1}",
                                            "{\"a\":}",
                                            "{\"a\"}",
                                            "tru",
                                            "fals",
                                            "nul",
                                            "True",
                                            "NULL",
                                            "01",
                                            "1.",
                                            ".5",
                                            "+1",
                                            "-",
                                            "1e",
                                            "1e+",
                                            "'single'",
                                            "{unquoted:1}",
                                            "{} {}",
                                            "[1] [2]",
                                            "null x",
                                            "{\"a\":1}garbage",
                                            "\"unterminated",
                                            "\"bad escape \\q\"",
                                            "{\"a\":1 \"b\":2}"}) {
            const auto failure = parse_error_of(text);
            expect(failure.code == refrain::error_code::parse_error) << text;
        }
    };

    "json/trailing content reports a byte offset"_test = [] {
        const auto two_objects = parse_error_of("{} {}");
        expect(two_objects.message.find("byte 3") != std::string::npos)
            << two_objects.message;

        const auto two_arrays = parse_error_of("[1] [2]");
        expect(two_arrays.message.find("byte 4") != std::string::npos)
            << two_arrays.message;

        const auto after_scalar = parse_error_of("null x");
        expect(after_scalar.message.find("byte 5") != std::string::npos)
            << after_scalar.message;

        // Empty/whitespace-only input reports the end of the input as offset.
        const auto empty = parse_error_of("   ");
        expect(empty.message.find("byte 3") != std::string::npos)
            << empty.message;
    };

    "json/accepts a UTF-8 BOM only at the start"_test = [] {
        const auto bom_object = parse_ok("\xEF\xBB\xBF{\"a\":1}");
        expect(bom_object.kind() == type::object);
        expect(*bom_object["a"].as_int64() == 1);

        const auto bom_scalar = parse_ok("\xEF\xBB\xBF 42 ");
        expect(*bom_scalar.as_int64() == 42);

        // A BOM in front of trailing content still reports the absolute offset.
        const auto bom_trailing = parse_error_of("\xEF\xBB\xBF{} x");
        expect(bom_trailing.message.find("byte 6") != std::string::npos)
            << bom_trailing.message;

        // UTF-16 input gets a specific encoding diagnosis.
        const auto utf16 = parse_error_of("\xFF\xFE{\x00}\x00");
        expect(utf16.code == refrain::error_code::parse_error);
        expect(utf16.message.find("UTF-16") != std::string::npos)
            << utf16.message;

        // Truncated BOM is diagnosed instead of a generic UTF-8 error.
        const auto truncated_bom = parse_error_of("\xEF\xBB");
        expect(truncated_bom.message.find("BOM") != std::string::npos)
            << truncated_bom.message;
    };

    "json/decodes unicode escapes and surrogate pairs"_test = [] {
        const auto accented = parse_ok(R"("\u00e9")");
        expect(*accented.as_string() == "\xC3\xA9"); // U+00E9 in UTF-8

        const auto emoji = parse_ok(R"("\ud83d\ude00")");
        expect(*emoji.as_string() == "\xF0\x9F\x98\x80"); // U+1F600

        const auto mixed = parse_ok(R"("a\u0041\u00dfz")");
        expect(*mixed.as_string() == "aA\xC3\x9Fz");

        // Raw (unescaped) UTF-8 passes through unchanged.
        const auto raw = parse_ok("\"h\xC3\xA9llo\"");
        expect(*raw.as_string() == "h\xC3\xA9llo");

        // Lone/incorrect surrogates are malformed JSON.
        for (const std::string_view text :
             {R"("\ud800")", R"("\udfff")", R"("\ud800x")",
              R"("\ud800\ud800")"}) {
            const auto failure = parse_error_of(text);
            expect(failure.code == refrain::error_code::parse_error) << text;
        }
    };

    "json/rejects invalid UTF-8 input"_test = [] {
        const auto bad_string = parse_error_of("\"\xFF\"");
        expect(bad_string.code == refrain::error_code::parse_error);
        expect(bad_string.message.find("UTF-8") != std::string::npos)
            << bad_string.message;

        const auto bad_key = parse_error_of("{\"\\xFF\":1}");
        expect(bad_key.code == refrain::error_code::parse_error);
    };

    "json/strings may contain escaped NUL and it round-trips"_test = [] {
        const auto document = parse_ok(R"("a\u0000b")");
        const auto text = document.as_string();
        expect(static_cast<bool>(text));
        if (text) {
            expect(text->size() == std::size_t{3});
            expect((*text)[0] == 'a');
            expect((*text)[1] == '\0');
            expect((*text)[2] == 'b');
        }
        expect(document.dump() == R"("a\u0000b")");
        expect(parse_ok(document.dump()) == document);
    };

    "json/duplicate keys keep the last occurrence"_test = [] {
        const auto document = parse_ok(R"({"a":1,"b":2,"a":3})");
        expect(document.size() == std::size_t{2});
        expect(*document["a"].as_int64() == 3);
        expect(document.dump() == R"({"a":3,"b":2})");

        // A run of duplicates collapses to one member holding the last value.
        const auto repeated = parse_ok(R"({"a":1,"a":2,"a":3})");
        expect(repeated.size() == std::size_t{1});
        const auto last = repeated.at("a");
        expect(static_cast<bool>(last));
        if (last) {
            expect(*last->get().as_int64() == 3);
        }
        expect(repeated.dump() == R"({"a":3})");
    };

    "json/large object conversion keeps every key (performance sanity)"_test =
        [] {
            // Regression guard against the former O(k^2) conversion: all
            // members are collected into one vector, sorted once by key and
            // then moved into the flat_map in ascending order. The input is
            // reverse-ordered so a one-by-one sorted insertion would have to
            // shift every already-inserted member.
            constexpr std::size_t key_count = 4000;
            std::string document{"{"};
            document.reserve(key_count * 12U + 1U);
            for (std::size_t position = 0; position < key_count; ++position) {
                if (position != 0) {
                    document.push_back(',');
                }
                const std::size_t key = key_count - 1U - position;
                document += fmt::format(R"("k{:04}":{})", key, key % 10U);
            }
            document.push_back('}');

            const auto parsed = parse_ok(document);
            expect(parsed.size() == key_count);
            const auto first = parsed.at("k0000");
            const auto last = parsed.at("k3999");
            expect(static_cast<bool>(first));
            expect(static_cast<bool>(last));
            if (first) {
                expect(*first->get().as_int64() == 0);
            }
            if (last) {
                expect(*last->get().as_int64() == 9);
            }
            // dump() walks the members in key order and re-parses equal.
            const std::string dumped{parsed.dump()};
            expect(dumped.starts_with(R"({"k0000":0,"k0001":1,)"));
            expect(parse_ok(dumped) == parsed);
        };

    "json/object keys are kept sorted regardless of input order"_test = [] {
        const auto document =
            parse_ok(R"({"zebra":1,"alpha":2,"Mike":3,"beta":4})");
        expect(document.dump() == R"({"Mike":3,"alpha":2,"beta":4,"zebra":1})");

        // Programmatically built objects use the same sorted order.
        value::object_type members{};
        members.emplace("z", value{std::int64_t{1}});
        members.emplace("a", value{std::int64_t{2}});
        members.emplace("m", value{std::int64_t{3}});
        const value built{std::move(members)};
        expect(built.dump() == R"({"a":2,"m":3,"z":1})");
    };

    "json/arrays preserve element order"_test = [] {
        const auto document = parse_ok("[[2,1],[3],{\"b\":1,\"a\":2}]");
        expect(document.dump() == R"([[2,1],[3],{"a":2,"b":1}])");
        expect(document.size() == std::size_t{3});
        const auto elements = document.as_array();
        expect(static_cast<bool>(elements));
        if (elements) {
            expect((*elements)[0].dump() == "[2,1]");
            expect((*elements)[2].dump() == R"({"a":2,"b":1})");
        }
    };

    "json/compact dump escapes control characters and quotes"_test = [] {
        value::object_type members{};
        members.emplace(
            "text",
            value{std::string{"quote\" backslash\\ newline\n tab\t cr\r ff\f "
                              "bs\b nul\x01 tail"}});
        const value document{std::move(members)};
        const std::string dumped{document.dump()};
        expect(dumped ==
               R"({"text":"quote\" backslash\\ newline\n tab\t cr\r ff\f )"
               R"(bs\b nul\u0001 tail"})")
            << dumped;
        expect(parse_ok(dumped) == document);
    };

    "json/compact dump keeps UTF-8 bytes as-is"_test = [] {
        const auto document =
            parse_ok("\"h\xC3\xA9llo \xE4\xB8\xAD\xE6\x96\x87\"");
        expect(document.dump() == "\"h\xC3\xA9llo \xE4\xB8\xAD\xE6\x96\x87\"");
    };

    "json/number serialization is type-stable"_test = [] {
        expect(value{std::int64_t{42}}.dump() == "42");
        expect(value{std::int64_t{-7}}.dump() == "-7");
        expect(value{1.5}.dump() == "1.5");
        // A real written without a fraction/exponent would reparse as an
        // integer; dump appends ".0" to keep the kind.
        expect(value{1.0}.dump() == "1.0");
        expect(parse_ok(value{1.0}.dump()).is_real());
        expect(value{-0.0}.dump() == "-0.0");
        expect(std::signbit(*parse_ok(value{-0.0}.dump()).as_double()));
        expect(value{1e300}.dump().find('e') != std::string::npos);
    };

    "json/integral construction converts safely"_test = [] {
        expect(value{42}.dump() == "42");
        expect(value{std::int32_t{-1}}.dump() == "-1");
        expect(value{std::uint32_t{7}}.dump() == "7");
        expect(value{std::int64_t{-5}}.dump() == "-5");
        expect(value{true}.dump() == "true");
        expect(value{nullptr}.is_null());

        static_assert(std::is_constructible_v<value, int>);
        static_assert(std::is_constructible_v<value, std::int32_t>);
        static_assert(std::is_constructible_v<value, std::uint32_t>);
        static_assert(!std::is_constructible_v<value, std::uint64_t>);
        if constexpr (sizeof(std::size_t) >= 8) {
            // 64-bit unsigned values cannot be represented as JSON int64 and
            // would silently wrap; that is a compile-time error.
            static_assert(!std::is_constructible_v<value, std::size_t>);
        }
    };

    "json/non-finite doubles serialize as null (documented)"_test = [] {
        expect(value{std::numeric_limits<double>::quiet_NaN()}.dump() ==
               "null");
        expect(value{std::numeric_limits<double>::infinity()}.dump() == "null");
        expect(value{-std::numeric_limits<double>::infinity()}.dump() ==
               "null");
    };

    "json/pretty dump is deterministic and reparsable"_test = [] {
        const auto document = parse_ok(R"({"b":[1,2],"a":{"c":true}})");
        const std::string expected{"{\n"
                                   "  \"a\": {\n"
                                   "    \"c\": true\n"
                                   "  },\n"
                                   "  \"b\": [\n"
                                   "    1,\n"
                                   "    2\n"
                                   "  ]\n"
                                   "}"};
        expect(document.dump_pretty() == expected) << document.dump_pretty();
        expect(document.dump_pretty() == document.dump_pretty());
        expect(document.dump_pretty(4).find("\n    \"a\"") !=
               std::string::npos);
        expect(parse_ok(document.dump_pretty()) == document);

        // Empty containers stay inline.
        expect(parse_ok("{}").dump_pretty() == "{}");
        expect(parse_ok("[]").dump_pretty() == "[]");
        expect(parse_ok(R"({"a":{},"b":[]})").dump_pretty() ==
               "{\n  \"a\": {},\n  \"b\": []\n}");
    };

    "json/round-trip is stable for a mixed document"_test = [] {
        const auto document = parse_ok(complex_document());
        const auto compact = document.dump();
        const auto pretty = document.dump_pretty();
        expect(parse_ok(compact) == document);
        expect(parse_ok(pretty) == document);
        expect(parse_ok(compact).dump() == compact);
        expect(parse_ok(pretty).dump_pretty() == pretty);
        // Sorted keys make the output byte-identical across calls.
        expect(document.dump() == compact);
        expect(document.dump_pretty() == pretty);
    };

    "json/nesting limit is enforced"_test = [] {
        // A scalar inside N containers has depth N+1; max_depth = 128 allows
        // 127 nested containers and rejects the next level.
        expect(static_cast<bool>(
            value::parse(nested_array(value::max_depth - 1))));
        const auto too_deep = parse_error_of(nested_array(value::max_depth));
        expect(too_deep.code == refrain::error_code::parse_error);
        expect(too_deep.message.find("128") != std::string::npos)
            << too_deep.message;
        // Pathologically deep input is rejected without crashing.
        expect(!value::parse(nested_array(2000)));
    };

    "json/typed accessors report kind mismatches"_test = [] {
        const auto object = parse_ok(R"({"a":1})");
        expect(!object.as_bool().has_value());
        expect(object.as_bool().error().code ==
               refrain::error_code::invalid_argument);
        expect(!object.as_int64().has_value());
        expect(!object.as_double().has_value());
        expect(!object.as_string().has_value());
        expect(!object.as_array().has_value());
        expect(static_cast<bool>(object.as_object()));

        const auto text = parse_ok(R"("s")");
        expect(text.as_bool().error().code ==
               refrain::error_code::invalid_argument);
        expect(text.as_array().error().code ==
               refrain::error_code::invalid_argument);

        // int -> double is allowed; real -> int is not.
        expect(*parse_ok("5").as_double() == 5.0);
        expect(!parse_ok("5.0").as_int64().has_value());
        expect(parse_ok("5.0").as_int64().error().code ==
               refrain::error_code::invalid_argument);
    };

    "json/object navigation handles missing keys and wrong receivers"_test =
        [] {
            const auto document =
                parse_ok(R"({"id":7,"nested":{"flag":true}})");

            expect(document.contains("id"));
            expect(document.contains("nested"));
            expect(!document.contains("missing"));
            expect(!document.contains(""));

            const auto *id = document.find("id");
            expect(id != nullptr);
            if (id != nullptr) {
                expect(*id->as_int64() == 7);
            }
            expect(document.find("missing") == nullptr);

            const auto strict = document.at("id");
            expect(static_cast<bool>(strict));
            if (strict) {
                expect(*strict->get().as_int64() == 7);
            }
            const auto missing = document.at("missing");
            expect(!missing.has_value());
            expect(missing.error().code == refrain::error_code::not_found);

            // Lenient access never inserts and returns a null member.
            expect(document["missing"].is_null());
            expect(document.size() == std::size_t{2});
            expect(!parse_ok("[1]").contains("id"));
            expect(parse_ok("[1]")["id"].is_null());
            expect(parse_ok("[1]").at("id").error().code ==
                   refrain::error_code::invalid_argument);
            expect(parse_ok("null")["x"].is_null());
        };

    "json/size reflects the kind"_test = [] {
        expect(parse_ok("[1,2,3]").size() == std::size_t{3});
        expect(parse_ok(R"({"a":1,"b":2})").size() == std::size_t{2});
        expect(parse_ok(R"("hello")").size() == std::size_t{5});
        expect(parse_ok("null").size() == std::size_t{0});
        expect(parse_ok("42").size() == std::size_t{0});
    };

    "json/whitespace forms are accepted between tokens"_test = [] {
        const auto document =
            parse_ok(" \t\r\n { \"a\" : [ 1 , 2 ] , \"b\" :\t{ } } \r\n ");
        expect(document.dump() == R"({"a":[1,2],"b":{}})");
        // Vertical tab / form feed are not JSON whitespace.
        expect(!value::parse("\v{}"));
        expect(!value::parse("\f{}"));
    };

    "json/parses byte spans"_test = [] {
        const auto bytes = as_bytes(R"({"a":[1,2,3],"b":null})");
        auto parsed = value::parse(bytes);
        expect(static_cast<bool>(parsed));
        if (parsed) {
            expect((*parsed)["a"].size() == std::size_t{3});
            expect((*parsed)["b"].is_null());
        }
        const auto bad = as_bytes("{\"a\":}");
        const auto failure = parse_error_of(bad);
        expect(failure.code == refrain::error_code::parse_error);
        // An empty span is an empty document, not a crash.
        expect(!value::parse(std::span<const std::byte>{}));
    };

    "json/parse_file reads valid files and reports io/parse errors"_test = [] {
        const temp_directory directory{};
        const auto valid_path = (directory.path() / "valid.json").string();
        expect(io::write_text(valid_path, R"({"file":true})").has_value());
        auto parsed = value::parse_file(valid_path);
        expect(static_cast<bool>(parsed));
        if (parsed) {
            expect(*((*parsed)["file"].as_bool()));
        }

        const auto missing_path = (directory.path() / "missing.json").string();
        const auto missing = value::parse_file(missing_path);
        expect(!missing);
        if (!missing) {
            expect(missing.error().code == refrain::error_code::io_error);
        }

        const auto invalid_path = (directory.path() / "invalid.json").string();
        expect(io::write_text(invalid_path, "{\"broken\":}").has_value());
        const auto invalid = value::parse_file(invalid_path);
        expect(!invalid);
        if (!invalid) {
            expect(invalid.error().code == refrain::error_code::parse_error);
            expect(invalid.error().message.find("invalid.json") !=
                   std::string::npos);
        }
    };

    "json/conversion errors name the failing JSON path"_test = [] {
        const auto overflow =
            parse_error_of(R"({"items":[1,9223372036854775808]})");
        expect(overflow.code == refrain::error_code::parse_error);
        expect(overflow.message.find("$.items[1]") != std::string::npos)
            << overflow.message;

        const auto huge = parse_error_of(R"({"v":[123456789012345678901234]})");
        expect(huge.message.find("$.v[0]") != std::string::npos)
            << huge.message;
    };

    "json/values are copyable and structurally comparable"_test = [] {
        const auto original = parse_ok(complex_document());
        const value copy{original};
        expect(copy == original);

        value::object_type members{};
        members.emplace("a", value{std::int64_t{1}});
        const value first{std::move(members)};
        value::object_type other_members{};
        other_members.emplace("a", value{std::int64_t{1}});
        const value second{std::move(other_members)};
        expect(first == second);
        expect(!(first == original));

        // A moved-from value stays assignable (containers rely on this).
        value moved{std::move(first)};
        expect(moved.size() == std::size_t{1});
    };

    "json/truncated and mutated input never crashes"_test = [] {
        // Every prefix of a valid document must fail cleanly (never read out of
        // bounds / crash); this is the cheap deterministic robustness check
        // that stands in for a fuzzer in unit tests.
        const auto document = complex_document();
        std::size_t errors{0};
        for (std::size_t length = 0; length < document.size(); ++length) {
            auto parsed =
                value::parse(std::string_view{document}.substr(0, length));
            if (!parsed) {
                expect(parsed.error().code == refrain::error_code::parse_error)
                    << "prefix length " << length;
                ++errors;
            }
        }
        expect(errors > 0);

        // Single-byte mutations across a short document: either parse or a
        // value-based error, never a crash.
        const std::string_view base{R"({"a":[1,2],"b":"text","c":true})"};
        for (std::size_t position = 0; position < base.size(); ++position) {
            for (const char replacement : {'x', '"', '\0', '}', '['}) {
                std::string mutated{base};
                mutated[position] = replacement;
                (void)value::parse(mutated);
            }
        }
        expect(true) << "mutation sweep completed without crashing";
    };

    "json/escaped solidus is accepted and normalized on dump"_test = [] {
        const auto document = parse_ok(R"("a\/b")");
        expect(*document.as_string() == "a/b");
        // The DOM stores the decoded value; dump() is not required to re-escape
        // "/" (JSON does not need it).
        expect(document.dump() == R"("a/b")");
        expect(parse_ok(document.dump()) == document);
    };

    "json/real values round-trip through their shortest representation"_test =
        [] {
            for (const double number :
                 {3.141592653589793, 2.5, 1e300, 1e-300, 5e-324,
                  std::numeric_limits<double>::max(),
                  std::numeric_limits<double>::denorm_min()}) {
                const value original{number};
                const auto reparsed = parse_ok(original.dump());
                expect(reparsed.is_real()) << original.dump();
                expect(*reparsed.as_double() == number) << original.dump();
            }
        };

    "json/non-ascii object keys sort by UTF-8 byte order"_test = [] {
        const auto document = parse_ok("{\"\u00e9\":1,\"a\":2,\"Z\":3}");
        // std::less<std::string> is byte-wise: uppercase ASCII sorts before
        // lowercase, and non-ASCII bytes sort after ASCII.
        expect(document.dump() == "{\"Z\":3,\"a\":2,\"\xC3\xA9\":1}")
            << document.dump();
    };

    "json/parses a ~50 KiB document repeatedly (performance sanity)"_test = [] {
        std::string document{"["};
        for (int index = 0; index < 450; ++index) {
            if (index != 0) {
                document.push_back(',');
            }
            document += R"({"id":)" + std::to_string(index) +
                        R"(,"name":"entity-)" + std::to_string(index) +
                        R"(","position":[1.5,2.5,3.5],"active":true,)";
            document += R"("tags":["alpha","beta","gamma"]})";
        }
        document.push_back(']');
        expect(document.size() > 40U * 1024U);

        const auto started = std::chrono::steady_clock::now();
        for (int iteration = 0; iteration < 20; ++iteration) {
            auto parsed = value::parse(document);
            expect(static_cast<bool>(parsed));
            if (parsed) {
                expect(parsed->size() == std::size_t{450});
            }
        }
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
        // Loose sanity bound only (no timing benchmark). Reference on the
        // development machine (Release, GCC 16): ~0.46 ms per parse of the
        // 45 KiB document (~100 MB/s end-to-end including DOM construction).
        expect(elapsed.count() < 5000) << elapsed.count() << " ms";
    };

    "json/builder round-trip preserves every value kind"_test = [] {
        value::array_type tags{};
        tags.emplace_back(value{std::string{"a\"b\\c\n\t\x01"}});
        tags.emplace_back(value{1.0});
        tags.emplace_back(value{std::int64_t{-9007199254740993}});

        value::object_type nested{};
        nested.emplace("empty_array", value{value::array_type{}});
        nested.emplace("empty_object", value{value::object_type{}});
        nested.emplace("real", value{-0.0});

        value::object_type members{};
        members.emplace("nested", value{std::move(nested)});
        members.emplace("tags", value{std::move(tags)});
        members.emplace("text", value{std::string{"quote\" backslash\\ slash/ "
                                                  "\xC3\xA9"}});
        const value document{std::move(members)};

        const std::string compact{document.dump()};
        const std::string pretty{document.dump_pretty()};

        const auto from_compact = parse_ok(compact);
        const auto from_pretty = parse_ok(pretty);
        expect(from_compact == document) << compact;
        expect(from_pretty == document) << pretty;
        expect(from_compact.dump() == compact);
        expect(from_pretty.dump_pretty() == pretty);

        // Equal DOMs always produce the same bytes (sorted members, stable
        // real formatting), including across copies.
        const value copy{document};
        expect(copy == document);
        expect(copy.dump() == compact);
        expect(copy.dump_pretty() == pretty);
    };

    "json/real kind survives compact and pretty dumps"_test = [] {
        constexpr std::array<double, 8> reals{
            0.0,
            -0.0,
            1.0,
            1.5,
            1e300,
            1e-300,
            std::numeric_limits<double>::max(),
            std::numeric_limits<double>::denorm_min()};
        for (const double number : reals) {
            const value original{number};
            const std::string compact{original.dump()};
            const std::string pretty{original.dump_pretty()};
            // A real must carry a fraction or an exponent; otherwise the
            // text would parse back as an integer.
            expect(compact.find_first_of(".eE") != std::string::npos)
                << compact;
            expect(pretty.find_first_of(".eE") != std::string::npos) << pretty;
            const auto reparsed_compact = parse_ok(compact);
            const auto reparsed_pretty = parse_ok(pretty);
            expect(reparsed_compact.is_real()) << compact;
            expect(reparsed_pretty.is_real()) << pretty;
            expect(*reparsed_compact.as_double() == number) << compact;
            expect(*reparsed_pretty.as_double() == number) << pretty;
            expect(std::signbit(*reparsed_compact.as_double()) ==
                   std::signbit(number))
                << compact;
        }

        // Integers never gain a fraction and stay integers.
        for (const std::int64_t integer :
             {std::int64_t{0}, std::int64_t{42},
              (std::numeric_limits<std::int64_t>::min)(),
              (std::numeric_limits<std::int64_t>::max)()}) {
            const value original{integer};
            const auto reparsed = parse_ok(original.dump());
            expect(reparsed.is_integer()) << original.dump();
            expect(*reparsed.as_int64() == integer);
        }

        // The pretty path uses the same number writer inside containers.
        value::array_type elements{};
        elements.emplace_back(value{1.0});
        const value array{std::move(elements)};
        expect(array.dump_pretty() == "[\n  1.0\n]") << array.dump_pretty();
    };

    "json/escape golden covers every control character"_test = [] {
        std::string raw{};
        for (int code = 0; code < 0x20; ++code) {
            raw.push_back(static_cast<char>(code));
        }
        raw += "\"\\/";
        raw.push_back('\x7F'); // DEL is not a JSON control character.
        raw += "\xC3\xA9";     // U+00E9 passes through as raw UTF-8.
        const value document{raw};
        const std::string expected{
            "\"\\u0000\\u0001\\u0002\\u0003\\u0004\\u0005\\u0006\\u0007"
            "\\b\\t\\n\\u000b\\f\\r\\u000e\\u000f"
            "\\u0010\\u0011\\u0012\\u0013\\u0014\\u0015\\u0016\\u0017"
            "\\u0018\\u0019\\u001a\\u001b\\u001c\\u001d\\u001e\\u001f"
            "\\\"\\\\/\x7F\xC3\xA9\""};
        expect(document.dump() == expected) << document.dump();
        expect(document.dump_pretty() == expected) << document.dump_pretty();
        expect(parse_ok(document.dump()) == document);

        // Object keys use the same escaping path.
        value::object_type members{};
        members.emplace(std::string{"k\"ey\n"}, value{std::int64_t{1}});
        const value object{std::move(members)};
        expect(object.dump() == "{\"k\\\"ey\\n\":1}") << object.dump();
    };

    "json/pretty golden keeps layout and indent clamping"_test = [] {
        const auto document =
            parse_ok(R"({"a":[1,{"b":"x\ny"}],"c":{},"d":[]})");
        const std::string expected{"{\n"
                                   "  \"a\": [\n"
                                   "    1,\n"
                                   "    {\n"
                                   "      \"b\": \"x\\ny\"\n"
                                   "    }\n"
                                   "  ],\n"
                                   "  \"c\": {},\n"
                                   "  \"d\": []\n"
                                   "}"};
        expect(document.dump_pretty() == expected) << document.dump_pretty();
        // Indent is clamped to 1..16 and the output never ends in a newline.
        expect(document.dump_pretty(0) == document.dump_pretty(1));
        const std::string wide{document.dump_pretty(99)};
        expect(wide.find("\n" + std::string(16, ' ') + "\"a\"") !=
               std::string::npos)
            << wide;
        expect(!wide.empty());
        expect(wide.back() != '\n');
        expect(parse_ok(wide) == document);

        // Empty containers stay inline at any indent.
        expect(parse_ok("{}").dump_pretty(16) == "{}");
        expect(parse_ok("[]").dump_pretty(0) == "[]");
    };

    "json/non-finite doubles serialize as null at any position"_test = [] {
        value::array_type elements{};
        elements.emplace_back(value{std::numeric_limits<double>::quiet_NaN()});
        elements.emplace_back(value{std::numeric_limits<double>::infinity()});
        elements.emplace_back(value{-std::numeric_limits<double>::infinity()});
        const value document{std::move(elements)};
        expect(document.dump() == "[null,null,null]");
        expect(document.dump_pretty() == "[\n  null,\n  null,\n  null\n]")
            << document.dump_pretty();
        expect(parse_ok(document.dump()) == parse_ok("[null,null,null]"));

        value::object_type members{};
        members.emplace("nan", value{std::numeric_limits<double>::quiet_NaN()});
        const value object{std::move(members)};
        expect(object.dump() == R"({"nan":null})");
        expect(parse_ok(object.dump_pretty()) == parse_ok(R"({"nan":null})"));
    };
};

} // namespace
