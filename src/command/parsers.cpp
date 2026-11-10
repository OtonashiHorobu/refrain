// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/command/parsers.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace refrain::command {

namespace {

[[nodiscard]] auto invalid_value(std::string_view word, std::string_view kind)
    -> error {
    return make_error(error_code::parse_error,
                      fmt::format("'{}' is not a valid {}", word, kind));
}

[[nodiscard]] auto equals_ignore_case(std::string_view left,
                                      std::string_view right) noexcept -> bool {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        auto left_character = static_cast<unsigned char>(left[index]);
        auto right_character = static_cast<unsigned char>(right[index]);
        if (left_character >= 'A' && left_character <= 'Z') {
            left_character =
                static_cast<unsigned char>(left_character - 'A' + 'a');
        }
        if (right_character >= 'A' && right_character <= 'Z') {
            right_character =
                static_cast<unsigned char>(right_character - 'A' + 'a');
        }
        if (left_character != right_character) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] auto starts_with_ignore_case(std::string_view text,
                                           std::string_view prefix) noexcept
    -> bool {
    if (prefix.size() > text.size()) {
        return false;
    }
    return equals_ignore_case(text.substr(0, prefix.size()), prefix);
}

[[nodiscard]] auto parse_int64(std::string_view word,
                               std::int64_t &out) noexcept -> bool {
    if (word.empty()) {
        return false;
    }
    std::size_t index{0};
    bool negative{false};
    if (word[index] == '+' || word[index] == '-') {
        negative = word[index] == '-';
        ++index;
    }
    if (index == word.size()) {
        return false;
    }
    constexpr auto max_magnitude{
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())};
    const auto limit{max_magnitude + (negative ? 1U : 0U)};
    std::uint64_t magnitude{0};
    for (; index < word.size(); ++index) {
        const auto character{word[index]};
        if (character < '0' || character > '9') {
            return false;
        }
        const auto digit{static_cast<std::uint64_t>(character - '0')};
        if (magnitude > (limit - digit) / 10U) {
            return false;
        }
        magnitude = magnitude * 10U + digit;
    }
    if (negative) {
        if (magnitude == max_magnitude + 1U) {
            out = std::numeric_limits<std::int64_t>::min();
            return true;
        }
        out = -static_cast<std::int64_t>(magnitude);
    } else {
        out = static_cast<std::int64_t>(magnitude);
    }
    return true;
}

[[nodiscard]] auto parse_double(std::string_view word, double &out) -> bool {
    if (word.empty()) {
        return false;
    }
    if (word.front() == '+') {
        word.remove_prefix(1);
    }
    if (word.empty()) {
        return false;
    }
    const auto result = std::from_chars(word.data(), word.data() + word.size(),
                                        out, std::chars_format::general);
    return result.ec == std::errc{} &&
           result.ptr == word.data() + word.size() && std::isfinite(out);
}

} // namespace

string_parser::string_parser(string_mode mode, std::string suggestion_type)
    : mode_{mode}, suggestion_type_{std::move(suggestion_type)} {}

auto string_parser::descriptor() const -> parser_descriptor {
    parser_descriptor descriptor{};
    descriptor.kind = parser_kind::string;
    descriptor.string = mode_;
    descriptor.suggestion_type = suggestion_type_;
    return descriptor;
}

auto string_parser::parse(reader &in) const -> result<argument_value> {
    switch (mode_) {
    case string_mode::greedy_phrase: {
        if (!in.can_read()) {
            return std::unexpected{
                make_error(error_code::parse_error, "expected a string")};
        }
        return argument_value{in.read_remaining()};
    }
    case string_mode::quotable_phrase: {
        if (in.peek() == '"' || in.peek() == '\'') {
            auto quoted = in.read_quoted();
            if (!quoted) {
                return std::unexpected{std::move(quoted.error())};
            }
            return argument_value{std::move(*quoted)};
        }
        const auto word = in.read_word();
        if (word.empty()) {
            return std::unexpected{
                make_error(error_code::parse_error, "expected a string")};
        }
        return argument_value{word};
    }
    case string_mode::single_word:
    default: {
        const auto word = in.read_word();
        if (word.empty()) {
            return std::unexpected{
                make_error(error_code::parse_error, "expected a word")};
        }
        return argument_value{word};
    }
    }
}

void string_parser::suggest(std::string_view, const source &,
                            suggestion_builder &) const {}

auto bool_parser::descriptor() const -> parser_descriptor {
    parser_descriptor descriptor{};
    descriptor.kind = parser_kind::boolean;
    return descriptor;
}

auto bool_parser::parse(reader &in) const -> result<argument_value> {
    const auto word = in.read_word();
    if (word == "true") {
        return argument_value{true};
    }
    if (word == "false") {
        return argument_value{false};
    }
    return std::unexpected{invalid_value(word, "boolean")};
}

void bool_parser::suggest(std::string_view prefix, const source &,
                          suggestion_builder &out) const {
    if (std::string_view{"true"}.starts_with(prefix)) {
        out.add("true");
    }
    if (std::string_view{"false"}.starts_with(prefix)) {
        out.add("false");
    }
}

integer_parser::integer_parser(std::optional<std::int64_t> minimum,
                               std::optional<std::int64_t> maximum,
                               bool long_form)
    : minimum_{minimum}, maximum_{maximum}, long_form_{long_form} {}

auto integer_parser::descriptor() const -> parser_descriptor {
    parser_descriptor descriptor{};
    descriptor.kind =
        long_form_ ? parser_kind::long_integer : parser_kind::integer;
    descriptor.has_min = minimum_.has_value();
    descriptor.has_max = maximum_.has_value();
    descriptor.integer_min = minimum_.value_or(0);
    descriptor.integer_max = maximum_.value_or(0);
    return descriptor;
}

auto integer_parser::parse(reader &in) const -> result<argument_value> {
    const auto word = in.read_word();
    std::int64_t value{0};
    if (!parse_int64(word, value)) {
        return std::unexpected{invalid_value(word, "integer")};
    }
    if (minimum_.has_value() && value < *minimum_) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       fmt::format("integer {} is below the minimum {}", value,
                                   *minimum_))};
    }
    if (maximum_.has_value() && value > *maximum_) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       fmt::format("integer {} is above the maximum {}", value,
                                   *maximum_))};
    }
    return argument_value{value};
}

number_parser::number_parser(bool float_form, std::optional<double> minimum,
                             std::optional<double> maximum)
    : float_form_{float_form}, minimum_{minimum}, maximum_{maximum} {}

auto number_parser::descriptor() const -> parser_descriptor {
    parser_descriptor descriptor{};
    descriptor.kind =
        float_form_ ? parser_kind::float_number : parser_kind::double_number;
    descriptor.has_min = minimum_.has_value();
    descriptor.has_max = maximum_.has_value();
    descriptor.number_min = minimum_.value_or(0.0);
    descriptor.number_max = maximum_.value_or(0.0);
    return descriptor;
}

auto number_parser::parse(reader &in) const -> result<argument_value> {
    const auto word = in.read_word();
    double value{0.0};
    if (!parse_double(word, value)) {
        return std::unexpected{invalid_value(
            word, float_form_ ? "single-precision number" : "number")};
    }
    if (float_form_ &&
        std::fabs(value) >
            static_cast<double>(std::numeric_limits<float>::max())) {
        return std::unexpected{make_error(
            error_code::parse_error,
            fmt::format("number {} is outside the single-precision range",
                        value))};
    }
    if (minimum_.has_value() && value < *minimum_) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       fmt::format("number {} is below the minimum {}", value,
                                   *minimum_))};
    }
    if (maximum_.has_value() && value > *maximum_) {
        return std::unexpected{
            make_error(error_code::parse_error,
                       fmt::format("number {} is above the maximum {}", value,
                                   *maximum_))};
    }
    return argument_value{value};
}

enum_parser::enum_parser(std::vector<enum_value> values,
                         std::string suggestion_type)
    : values_{std::move(values)}, suggestion_type_{std::move(suggestion_type)} {
}

auto enum_parser::descriptor() const -> parser_descriptor {
    parser_descriptor descriptor{};
    descriptor.kind = parser_kind::enum_values;
    descriptor.enum_values.reserve(values_.size());
    for (const auto &value : values_) {
        descriptor.enum_values.push_back(value.name);
    }
    descriptor.suggestion_type = suggestion_type_;
    return descriptor;
}

auto enum_parser::parse(reader &in) const -> result<argument_value> {
    const auto word = in.read_word();
    for (const auto &value : values_) {
        if (equals_ignore_case(word, value.name)) {
            return argument_value{value.name};
        }
        for (const auto &alias : value.aliases) {
            if (equals_ignore_case(word, alias)) {
                return argument_value{value.name};
            }
        }
    }
    return std::unexpected{invalid_value(word, "value")};
}

void enum_parser::suggest(std::string_view prefix, const source &,
                          suggestion_builder &out) const {
    for (const auto &value : values_) {
        const auto matches =
            starts_with_ignore_case(value.name, prefix) ||
            std::ranges::any_of(
                value.aliases, [prefix](const std::string &alias) {
                    return starts_with_ignore_case(alias, prefix);
                });
        if (matches) {
            out.add(value.name, value.tooltip);
        }
    }
}

auto resource_location_parser::descriptor() const -> parser_descriptor {
    parser_descriptor descriptor{};
    descriptor.kind = parser_kind::resource_location;
    return descriptor;
}

auto resource_location_parser::parse(reader &in) const
    -> result<argument_value> {
    const auto word = in.read_word();
    auto location = data::resource_location::parse(word);
    if (!location) {
        return std::unexpected{invalid_value(word, "resource location")};
    }
    return argument_value{std::move(*location)};
}

auto word_arg() -> string_parser {
    return string_parser{string_mode::single_word};
}

auto quoted_arg() -> string_parser {
    return string_parser{string_mode::quotable_phrase};
}

auto greedy_arg() -> string_parser {
    return string_parser{string_mode::greedy_phrase};
}

auto bool_arg() -> bool_parser { return bool_parser{}; }

auto integer_arg(std::optional<std::int64_t> minimum,
                 std::optional<std::int64_t> maximum) -> integer_parser {
    return integer_parser{minimum, maximum, false};
}

auto long_arg(std::optional<std::int64_t> minimum,
              std::optional<std::int64_t> maximum) -> integer_parser {
    return integer_parser{minimum, maximum, true};
}

auto float_arg(std::optional<double> minimum, std::optional<double> maximum)
    -> number_parser {
    return number_parser{true, minimum, maximum};
}

auto double_arg(std::optional<double> minimum, std::optional<double> maximum)
    -> number_parser {
    return number_parser{false, minimum, maximum};
}

auto enum_arg(std::vector<enum_value> values, std::string suggestion_type)
    -> enum_parser {
    return enum_parser{std::move(values), std::move(suggestion_type)};
}

auto gamemode_arg() -> enum_parser {
    return enum_parser{
        {
            enum_value{"survival", "Survival mode", {"0"}},
            enum_value{"creative", "Creative mode", {"1"}},
            enum_value{"adventure", "Adventure mode", {"2"}},
            enum_value{"spectator", "Spectator mode", {"3"}},
        },
    };
}

auto resource_location_arg() -> resource_location_parser {
    return resource_location_parser{};
}

} // namespace refrain::command
