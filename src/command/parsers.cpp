// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/command/parsers.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace refrain::command {

auto make_argument_error(std::string message, std::size_t offset)
    -> argument_error {
    return argument_error{std::move(message), offset};
}

auto to_string(game_mode mode) noexcept -> std::string_view {
    switch (mode) {
    case game_mode::survival:
        return "survival";
    case game_mode::creative:
        return "creative";
    case game_mode::adventure:
        return "adventure";
    case game_mode::spectator:
        return "spectator";
    }
    return "survival";
}

namespace {

constexpr auto hex_digit(std::uint8_t value) noexcept -> char {
    return static_cast<char>(value < 10U
                                 ? static_cast<char>('0' + value)
                                 : static_cast<char>('a' + value - 10U));
}

[[nodiscard]] auto hex_value(char character) noexcept -> int {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

[[nodiscard]] auto ascii_lower(char character) noexcept -> char {
    return character >= 'A' && character <= 'Z'
               ? static_cast<char>(character - 'A' + 'a')
               : character;
}

[[nodiscard]] auto equals_ignore_case(std::string_view left,
                                      std::string_view right) noexcept -> bool {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (ascii_lower(left[index]) != ascii_lower(right[index])) {
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

/// Parse the whole token as a signed 64-bit integer, accepting an explicit
/// leading sign like the reference's `readLong`.
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

/// Parse the whole token as a finite double.
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

/// Read the longest numeric prefix from the reader and advance past it. Used by
/// the parsers whose number is glued to a suffix (a time unit) or followed by a
/// coordinate separator.
[[nodiscard]] auto read_number_prefix(reader &in, double &out) -> bool {
    const auto remaining = in.input().substr(in.cursor());
    bool explicit_plus = false;
    auto view = remaining;
    if (!view.empty() && view.front() == '+') {
        explicit_plus = true;
        view.remove_prefix(1);
    }
    if (view.empty()) {
        return false;
    }
    const auto parsed = std::from_chars(view.data(), view.data() + view.size(),
                                        out, std::chars_format::general);
    if (parsed.ec != std::errc{} || parsed.ptr == view.data()) {
        return false;
    }
    if (!std::isfinite(out)) {
        return false;
    }
    const auto digits{static_cast<std::size_t>(parsed.ptr - view.data())};
    in.set_cursor(in.cursor() + digits + (explicit_plus ? 1U : 0U));
    return true;
}

[[nodiscard]] auto read_integer_prefix(reader &in, std::int64_t &out) -> bool {
    const auto remaining = in.input().substr(in.cursor());
    bool explicit_plus = false;
    auto view = remaining;
    if (!view.empty() && view.front() == '+') {
        explicit_plus = true;
        view.remove_prefix(1);
    }
    if (view.empty()) {
        return false;
    }
    const auto parsed =
        std::from_chars(view.data(), view.data() + view.size(), out);
    if (parsed.ec != std::errc{} || parsed.ptr == view.data()) {
        return false;
    }
    const auto digits{static_cast<std::size_t>(parsed.ptr - view.data())};
    in.set_cursor(in.cursor() + digits + (explicit_plus ? 1U : 0U));
    return true;
}

[[nodiscard]] auto word_error(std::string_view word, std::string_view kind)
    -> argument_error {
    return make_argument_error(
        fmt::format("'{}' is not a valid {}", word, kind));
}

/// One world-axis coordinate. `value` is already the signed amount; whether it
/// is relative to the source rotation/position is reported separately.
struct coordinate {
    double value{0.0};
    bool relative{false};
};

/// Parse one coordinate of a block position or rotation.
///
/// `absolute_integer` mirrors the reference's split: absolute block-position
/// coordinates are integers, while relative ones and rotation angles accept a
/// fractional part.
[[nodiscard]] auto parse_world_coordinate(reader &in,
                                          std::size_t argument_start,
                                          bool absolute_integer)
    -> std::expected<coordinate, argument_error> {
    if (in.peek() == '^') {
        return std::unexpected{
            make_argument_error("you cannot mix world and local coordinates",
                                in.cursor() - argument_start)};
    }
    if (!in.can_read()) {
        return std::unexpected{make_argument_error(
            absolute_integer ? "expected an integer" : "expected a number",
            in.cursor() - argument_start)};
    }
    bool relative{false};
    if (in.peek() == '~') {
        relative = true;
        in.skip();
    }
    double value{0.0};
    if (in.can_read() && in.peek() != ' ') {
        const auto number_start = in.cursor() - argument_start;
        bool parsed{false};
        if (absolute_integer && !relative) {
            std::int64_t integer{0};
            parsed = read_integer_prefix(in, integer);
            value = static_cast<double>(integer);
        } else {
            parsed = read_number_prefix(in, value);
        }
        if (!parsed) {
            return std::unexpected{make_argument_error(
                absolute_integer ? "invalid integer" : "invalid number",
                number_start)};
        }
    }
    return coordinate{value, relative};
}

/// Parse one world-axis coordinate of a `vec3` position.
///
/// Absolute values may be fractional, unlike a block position. The reference
/// centres an integer absolute X or Z by half a block, because a block position
/// names the block the entity stands in, and never centres Y; `centre` is false
/// for the Y component. The centring test is the reference's: the typed number
/// contains no decimal point.
[[nodiscard]] auto parse_vec3_coordinate(reader &in, std::size_t argument_start,
                                         bool centre)
    -> std::expected<coordinate, argument_error> {
    if (in.peek() == '^') {
        return std::unexpected{
            make_argument_error("you cannot mix world and local coordinates",
                                in.cursor() - argument_start)};
    }
    if (!in.can_read()) {
        return std::unexpected{make_argument_error(
            "expected a number", in.cursor() - argument_start)};
    }
    bool relative{false};
    if (in.peek() == '~') {
        relative = true;
        in.skip();
    }
    double value{0.0};
    bool has_number{false};
    bool has_decimal_point{false};
    if (in.can_read() && in.peek() != ' ') {
        const auto number_start = in.cursor();
        if (!read_number_prefix(in, value)) {
            return std::unexpected{make_argument_error(
                "invalid number", number_start - argument_start)};
        }
        has_number = true;
        has_decimal_point =
            in.input()
                .substr(number_start, in.cursor() - number_start)
                .find('.') != std::string_view::npos;
    }
    if (!relative && centre && has_number && !has_decimal_point) {
        value += 0.5;
    }
    return coordinate{value, relative};
}

/// Parse one `^` local coordinate component.
[[nodiscard]] auto parse_local_coordinate(reader &in,
                                          std::size_t argument_start)
    -> std::expected<coordinate, argument_error> {
    if (!in.can_read()) {
        return std::unexpected{make_argument_error(
            "expected a number", in.cursor() - argument_start)};
    }
    if (in.peek() != '^') {
        return std::unexpected{
            make_argument_error("you cannot mix world and local coordinates",
                                in.cursor() - argument_start)};
    }
    in.skip();
    double value{0.0};
    if (in.can_read() && in.peek() != ' ') {
        const auto number_start = in.cursor() - argument_start;
        if (!read_number_prefix(in, value)) {
            return std::unexpected{
                make_argument_error("invalid number", number_start)};
        }
    }
    return coordinate{value, true};
}

/// Consume the separator between two components of one multi-token argument.
[[nodiscard]] auto expect_separator(reader &in) -> bool {
    if (!in.can_read() || in.peek() != ' ') {
        return false;
    }
    in.skip();
    return true;
}

/// Split a partial multi-token argument on single spaces. Trailing empty
/// fields are dropped, so a trailing space reads as "the next component is
/// being typed" rather than as an empty current component.
[[nodiscard]] auto split_fields(std::string_view text)
    -> std::vector<std::string_view> {
    std::vector<std::string_view> fields{};
    std::size_t start{0};
    while (true) {
        const auto space = text.find(' ', start);
        if (space == std::string_view::npos) {
            fields.push_back(text.substr(start));
            break;
        }
        fields.push_back(text.substr(start, space - start));
        start = space + 1;
    }
    while (!fields.empty() && fields.back().empty()) {
        fields.pop_back();
    }
    return fields;
}

/// True when `candidate` is a complete argument for `parser`.
template <typename Parser>
[[nodiscard]] auto is_complete_argument(const Parser &parser,
                                        std::string_view candidate) -> bool {
    reader probe{candidate};
    const auto parsed = parser.parse(probe);
    return parsed.has_value() && probe.at_end();
}

/// Join the typed coordinate components with the template unit filling the
/// ones not typed yet, up to `count` components.
[[nodiscard]] auto join_components(const std::vector<std::string_view> &fields,
                                   std::string_view unit, std::size_t count)
    -> std::string {
    std::string text{};
    for (std::size_t component = 0; component < count; ++component) {
        if (component != 0) {
            text += ' ';
        }
        text += component < fields.size() ? fields[component] : unit;
    }
    return text;
}

/// Suggestions for a coordinate-style argument: the source-relative template
/// (`~`) or the anchor-local template (`^`), extended component by component.
/// Completions replace the whole partial argument, so every offered text is a
/// complete argument.
template <typename Parser>
void suggest_coordinates(std::string_view remaining, bool local,
                         std::size_t components, const Parser &parser,
                         suggestion_builder &out) {
    const std::string_view unit = local ? "^" : "~";
    const auto fields = split_fields(remaining);
    if (fields.size() >= components) {
        return;
    }
    if (fields.empty()) {
        for (std::size_t count = 1; count <= components; ++count) {
            const auto candidate = join_components(fields, unit, count);
            if (count == components &&
                !is_complete_argument(parser, candidate)) {
                return;
            }
            out.add(candidate);
        }
        return;
    }
    const auto full = join_components(fields, unit, components);
    if (!is_complete_argument(parser, full)) {
        return;
    }
    for (std::size_t count = fields.size() + 1; count <= components; ++count) {
        out.add(join_components(fields, unit, count));
    }
}

constexpr std::int64_t ticks_per_day = 24'000;
constexpr std::int64_t ticks_per_second = 20;

struct time_unit {
    std::string_view suffix;
    std::int64_t factor;
};

/// Every suffix a completed time value may take, in suggestion order.
constexpr std::array<time_unit, 4> time_units{{
    {"", 1},
    {"t", 1},
    {"s", ticks_per_second},
    {"d", ticks_per_day},
}};

/// One game mode spelling: canonical name, completion tooltip and the numeric
/// alias the server accepted before the typed parser existed.
struct gamemode_entry {
    game_mode mode;
    std::string_view name;
    std::string_view tooltip;
    std::string_view alias;
};

constexpr std::array<gamemode_entry, 4> gamemode_entries{{
    {game_mode::survival, "survival", "Survival mode", "0"},
    {game_mode::creative, "creative", "Creative mode", "1"},
    {game_mode::adventure, "adventure", "Adventure mode", "2"},
    {game_mode::spectator, "spectator", "Spectator mode", "3"},
}};

} // namespace

auto uuid::to_string() const -> std::string {
    std::string text{};
    text.reserve(36U);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4U || index == 6U || index == 8U || index == 10U) {
            text.push_back('-');
        }
        text.push_back(
            hex_digit(static_cast<std::uint8_t>(bytes[index] >> 4U)));
        text.push_back(
            hex_digit(static_cast<std::uint8_t>(bytes[index] & 0x0FU)));
    }
    return text;
}

auto uuid::parse(std::string_view text) -> result<uuid> {
    constexpr std::array<std::size_t, 4> hyphens{8U, 13U, 18U, 23U};
    if (text.size() != 36U) {
        return std::unexpected{
            make_error(error_code::invalid_argument, "uuid must be 36 bytes")};
    }
    uuid value{};
    std::size_t byte_index{0};
    bool upper_nibble{true};
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (std::ranges::find(hyphens, index) != hyphens.end()) {
            if (text[index] != '-') {
                return std::unexpected{make_error(error_code::invalid_argument,
                                                  "uuid hyphen is misplaced")};
            }
            continue;
        }
        const auto nibble = hex_value(text[index]);
        if (nibble < 0) {
            return std::unexpected{make_error(error_code::invalid_argument,
                                              "uuid has a non-hex character")};
        }
        if (byte_index >= value.bytes.size()) {
            return std::unexpected{
                make_error(error_code::invalid_argument, "uuid is too long")};
        }
        if (upper_nibble) {
            value.bytes[byte_index] =
                static_cast<std::uint8_t>(static_cast<unsigned>(nibble) << 4U);
        } else {
            value.bytes[byte_index] = static_cast<std::uint8_t>(
                value.bytes[byte_index] | static_cast<unsigned>(nibble));
            ++byte_index;
        }
        upper_nibble = !upper_nibble;
    }
    if (byte_index != value.bytes.size() || !upper_nibble) {
        return std::unexpected{
            make_error(error_code::invalid_argument, "uuid is truncated")};
    }
    return value;
}

string_parser::string_parser(string_mode mode, std::string suggestion_type)
    : mode_{mode}, suggestion_type_{std::move(suggestion_type)} {}

auto string_parser::descriptor() const -> parser_descriptor {
    parser_descriptor descriptor{};
    descriptor.kind = parser_kind::string;
    descriptor.string = mode_;
    descriptor.suggestion_type = suggestion_type_;
    return descriptor;
}

auto string_parser::parse(reader &in) const -> parse_result {
    switch (mode_) {
    case string_mode::greedy_phrase: {
        if (!in.can_read()) {
            return std::unexpected{make_argument_error("expected a string", 0)};
        }
        return argument_value{in.read_remaining()};
    }
    case string_mode::quotable_phrase: {
        if (in.peek() == '"' || in.peek() == '\'') {
            auto quoted = in.read_quoted();
            if (!quoted) {
                return std::unexpected{
                    make_argument_error("unterminated quoted string", 0)};
            }
            return argument_value{std::move(*quoted)};
        }
        const auto word = in.read_word();
        if (word.empty()) {
            return std::unexpected{make_argument_error("expected a string", 0)};
        }
        return argument_value{word};
    }
    case string_mode::single_word:
    default: {
        const auto word = in.read_word();
        if (word.empty()) {
            return std::unexpected{make_argument_error("expected a word", 0)};
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

auto bool_parser::parse(reader &in) const -> parse_result {
    const auto word = in.read_word();
    if (word == "true") {
        return argument_value{true};
    }
    if (word == "false") {
        return argument_value{false};
    }
    return std::unexpected{word_error(word, "boolean")};
}

void bool_parser::suggest(std::string_view remaining, const source &,
                          suggestion_builder &out) const {
    if (std::string_view{"true"}.starts_with(remaining)) {
        out.add("true");
    }
    if (std::string_view{"false"}.starts_with(remaining)) {
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

auto integer_parser::parse(reader &in) const -> parse_result {
    const auto word = in.read_word();
    std::int64_t value{0};
    if (!parse_int64(word, value)) {
        return std::unexpected{word_error(word, "integer")};
    }
    if (minimum_.has_value() && value < *minimum_) {
        return std::unexpected{make_argument_error(fmt::format(
            "integer {} is below the minimum {}", value, *minimum_))};
    }
    if (maximum_.has_value() && value > *maximum_) {
        return std::unexpected{make_argument_error(fmt::format(
            "integer {} is above the maximum {}", value, *maximum_))};
    }
    return argument_value{value};
}

void integer_parser::suggest(std::string_view remaining, const source &,
                             suggestion_builder &out) const {
    if (minimum_.has_value() && remaining.empty()) {
        out.add(std::to_string(*minimum_));
    }
    if (maximum_.has_value() && remaining.empty()) {
        out.add(std::to_string(*maximum_));
    }
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

auto number_parser::parse(reader &in) const -> parse_result {
    const auto word = in.read_word();
    double value{0.0};
    if (!parse_double(word, value)) {
        return std::unexpected{word_error(
            word, float_form_ ? "single-precision number" : "number")};
    }
    if (float_form_ &&
        std::fabs(value) >
            static_cast<double>(std::numeric_limits<float>::max())) {
        return std::unexpected{make_argument_error(fmt::format(
            "number {} is outside the single-precision range", value))};
    }
    if (minimum_.has_value() && value < *minimum_) {
        return std::unexpected{make_argument_error(fmt::format(
            "number {} is below the minimum {}", value, *minimum_))};
    }
    if (maximum_.has_value() && value > *maximum_) {
        return std::unexpected{make_argument_error(fmt::format(
            "number {} is above the maximum {}", value, *maximum_))};
    }
    return argument_value{value};
}

void number_parser::suggest(std::string_view remaining, const source &,
                            suggestion_builder &out) const {
    if (remaining.empty() && minimum_.has_value()) {
        out.add(fmt::format("{}", *minimum_));
    }
    if (remaining.empty() && maximum_.has_value()) {
        out.add(fmt::format("{}", *maximum_));
    }
}

enum_parser::enum_parser(std::vector<enum_value> values,
                         std::string suggestion_type)
    : values_{std::move(values)}, suggestion_type_{std::move(suggestion_type)} {
}

auto enum_parser::descriptor() const -> parser_descriptor {
    parser_descriptor descriptor{};
    descriptor.kind = parser_kind::enum_values;
    descriptor.enum_values = values_ |
                             std::views::transform([](const enum_value &value) {
                                 return value.name;
                             }) |
                             std::ranges::to<std::vector<std::string>>();
    descriptor.suggestion_type = suggestion_type_;
    return descriptor;
}

auto enum_parser::parse(reader &in) const -> parse_result {
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
    return std::unexpected{word_error(word, "value")};
}

void enum_parser::suggest(std::string_view remaining, const source &,
                          suggestion_builder &out) const {
    for (const auto &value : values_) {
        const auto matches =
            starts_with_ignore_case(value.name, remaining) ||
            std::ranges::any_of(
                value.aliases, [remaining](const std::string &alias) {
                    return starts_with_ignore_case(alias, remaining);
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

auto resource_location_parser::parse(reader &in) const -> parse_result {
    const auto word = in.read_word();
    auto location = data::resource_location::parse(word);
    if (!location) {
        return std::unexpected{word_error(word, "resource location")};
    }
    return argument_value{std::move(*location)};
}

void resource_location_parser::suggest(std::string_view, const source &,
                                       suggestion_builder &) const {}

auto block_position_parser::descriptor() const -> parser_descriptor {
    parser_descriptor descriptor{};
    descriptor.kind = parser_kind::block_position;
    return descriptor;
}

auto block_position_parser::parse(reader &in) const -> parse_result {
    const auto argument_start = in.cursor();
    block_position out{};
    const auto local = in.peek() == '^';
    out.local = local;
    std::array<coordinate, 3> components{};
    for (std::size_t axis = 0; axis < components.size(); ++axis) {
        if (axis != 0 && !expect_separator(in)) {
            return std::unexpected{
                make_argument_error("expected a complete block position", 0)};
        }
        auto component = local
                             ? parse_local_coordinate(in, argument_start)
                             : parse_world_coordinate(in, argument_start, true);
        if (!component) {
            return std::unexpected{std::move(component.error())};
        }
        components[axis] = *component;
    }
    out.x = components[0].value;
    out.y = components[1].value;
    out.z = components[2].value;
    out.relative_x = components[0].relative;
    out.relative_y = components[1].relative;
    out.relative_z = components[2].relative;
    return argument_value{out};
}

void block_position_parser::suggest(std::string_view remaining, const source &,
                                    suggestion_builder &out) const {
    suggest_coordinates(remaining, remaining.starts_with('^'), 3, *this, out);
}

auto vec3_parser::descriptor() const -> parser_descriptor {
    parser_descriptor descriptor{};
    descriptor.kind = parser_kind::vec3;
    return descriptor;
}

auto vec3_parser::parse(reader &in) const -> parse_result {
    const auto argument_start = in.cursor();
    vec3_position out{};
    const auto local = in.peek() == '^';
    out.local = local;
    std::array<coordinate, 3> components{};
    for (std::size_t axis = 0; axis < components.size(); ++axis) {
        if (axis != 0 && !expect_separator(in)) {
            return std::unexpected{
                make_argument_error("expected a complete position", 0)};
        }
        auto component =
            local ? parse_local_coordinate(in, argument_start)
                  : parse_vec3_coordinate(in, argument_start, axis != 1);
        if (!component) {
            return std::unexpected{std::move(component.error())};
        }
        components[axis] = *component;
    }
    out.x = components[0].value;
    out.y = components[1].value;
    out.z = components[2].value;
    out.relative_x = components[0].relative;
    out.relative_y = components[1].relative;
    out.relative_z = components[2].relative;
    return argument_value{out};
}

void vec3_parser::suggest(std::string_view remaining, const source &,
                          suggestion_builder &out) const {
    suggest_coordinates(remaining, remaining.starts_with('^'), 3, *this, out);
}

auto rotation_parser::descriptor() const -> parser_descriptor {
    parser_descriptor descriptor{};
    descriptor.kind = parser_kind::rotation;
    return descriptor;
}

auto rotation_parser::parse(reader &in) const -> parse_result {
    const auto argument_start = in.cursor();
    if (!in.can_read()) {
        return std::unexpected{
            make_argument_error("expected a complete rotation", 0)};
    }
    auto yaw = parse_world_coordinate(in, argument_start, false);
    if (!yaw) {
        return std::unexpected{std::move(yaw.error())};
    }
    if (!expect_separator(in)) {
        return std::unexpected{
            make_argument_error("expected a complete rotation", 0)};
    }
    auto pitch = parse_world_coordinate(in, argument_start, false);
    if (!pitch) {
        return std::unexpected{std::move(pitch.error())};
    }
    rotation out{};
    out.yaw = yaw->value;
    out.pitch = pitch->value;
    out.relative_yaw = yaw->relative;
    out.relative_pitch = pitch->relative;
    return argument_value{out};
}

void rotation_parser::suggest(std::string_view remaining, const source &,
                              suggestion_builder &out) const {
    suggest_coordinates(remaining, false, 2, *this, out);
}

auto player_name_parser::descriptor() const -> parser_descriptor {
    parser_descriptor descriptor{};
    descriptor.kind = parser_kind::player_name;
    return descriptor;
}

auto player_name_parser::parse(reader &in) const -> parse_result {
    const auto word = in.read_word();
    if (word.empty()) {
        return std::unexpected{
            make_argument_error("expected a player name", 0)};
    }
    if (word.front() == '@') {
        return std::unexpected{make_argument_error(
            "entity selectors are not supported by this argument", 0)};
    }
    return argument_value{word};
}

void player_name_parser::suggest(std::string_view remaining, const source &src,
                                 suggestion_builder &out) const {
    for (const auto &name : src.online_players()) {
        if (starts_with_ignore_case(name, remaining)) {
            out.add(name);
        }
    }
}

auto gamemode_parser::descriptor() const -> parser_descriptor {
    parser_descriptor descriptor{};
    descriptor.kind = parser_kind::gamemode;
    return descriptor;
}

auto gamemode_parser::parse(reader &in) const -> parse_result {
    const auto word = in.read_word();
    for (const auto &entry : gamemode_entries) {
        if (equals_ignore_case(word, entry.name) ||
            equals_ignore_case(word, entry.alias)) {
            return argument_value{entry.mode};
        }
    }
    return std::unexpected{
        make_argument_error(fmt::format("unknown game mode '{}'", word))};
}

void gamemode_parser::suggest(std::string_view remaining, const source &,
                              suggestion_builder &out) const {
    for (const auto &entry : gamemode_entries) {
        if (starts_with_ignore_case(entry.name, remaining)) {
            out.add(entry.name, entry.tooltip);
        }
    }
}

time_parser::time_parser(std::int64_t minimum_ticks,
                         std::vector<time_marker> markers)
    : minimum_ticks_{minimum_ticks}, markers_{std::move(markers)} {}

auto time_parser::descriptor() const -> parser_descriptor {
    parser_descriptor descriptor{};
    descriptor.kind = parser_kind::time;
    descriptor.integer_min = minimum_ticks_;
    return descriptor;
}

auto time_parser::parse(reader &in) const -> parse_result {
    const auto argument_start = in.cursor();
    const auto token = in.read_word();
    if (token.empty()) {
        return std::unexpected{make_argument_error("expected a time", 0)};
    }
    for (const auto &marker : markers_) {
        if (equals_ignore_case(token, marker.name)) {
            return argument_value{marker.ticks};
        }
    }
    in.set_cursor(argument_start);
    double value{0.0};
    if (!read_number_prefix(in, value)) {
        return std::unexpected{make_argument_error(
            fmt::format("'{}' is not a valid time", token), 0)};
    }
    const auto unit_start = in.cursor();
    const auto suffix = in.read_word();
    std::optional<std::int64_t> factor{};
    for (const auto &entry : time_units) {
        if (suffix == entry.suffix) {
            factor = entry.factor;
            break;
        }
    }
    if (!factor.has_value()) {
        return std::unexpected{make_argument_error(
            suffix.empty() ? "invalid time unit"
                           : fmt::format("unknown time unit '{}'", suffix),
            unit_start - argument_start)};
    }
    const auto scaled = value * static_cast<double>(*factor);
    if (!std::isfinite(scaled) ||
        std::fabs(scaled) >
            static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return std::unexpected{make_argument_error("time is out of range", 0)};
    }
    // Java's Math.round rounds halves towards positive infinity; std::round
    // rounds away from zero, which differs below zero.
    const auto ticks = static_cast<std::int64_t>(std::floor(scaled + 0.5));
    if (ticks < minimum_ticks_) {
        return std::unexpected{make_argument_error(
            fmt::format("tick count {} is below the minimum {}", ticks,
                        minimum_ticks_),
            0)};
    }
    return argument_value{ticks};
}

void time_parser::suggest(std::string_view remaining, const source &,
                          suggestion_builder &out) const {
    if (remaining.find(' ') != std::string_view::npos) {
        return;
    }
    for (const auto &marker : markers_) {
        if (starts_with_ignore_case(marker.name, remaining)) {
            out.add(marker.name, marker.tooltip);
        }
    }
    if (remaining.empty()) {
        out.add("1d", "One in-game day");
        out.add("1s", "One in-game second");
        out.add("1t", "One tick");
        return;
    }
    // Suggest the unit suffixes after a numeric prefix: `20` completes to
    // `20d`, `20s` and `20t`. A non-numeric prefix only completes markers.
    double number{0.0};
    std::size_t consumed{0};
    const auto parsed =
        std::from_chars(remaining.data(), remaining.data() + remaining.size(),
                        number, std::chars_format::general);
    if (parsed.ec != std::errc{} || parsed.ptr == remaining.data()) {
        return;
    }
    consumed = static_cast<std::size_t>(parsed.ptr - remaining.data());
    const auto typed_number = remaining.substr(0, consumed);
    const auto typed_suffix = remaining.substr(consumed);
    for (const auto &entry : time_units) {
        if (!typed_suffix.empty() && entry.suffix != typed_suffix) {
            continue;
        }
        std::string text{typed_number};
        text += entry.suffix;
        out.add(text);
    }
}

auto dimension_parser::default_ids() -> std::vector<std::string> {
    return {"minecraft:overworld", "minecraft:nether", "minecraft:the_end"};
}

dimension_parser::dimension_parser(std::vector<std::string> ids)
    : ids_{std::move(ids)} {}

auto dimension_parser::descriptor() const -> parser_descriptor {
    parser_descriptor descriptor{};
    descriptor.kind = parser_kind::dimension;
    return descriptor;
}

auto dimension_parser::parse(reader &in) const -> parse_result {
    const auto word = in.read_word();
    auto location = data::resource_location::parse(word);
    if (!location) {
        return std::unexpected{word_error(word, "dimension id")};
    }
    return argument_value{std::move(*location)};
}

void dimension_parser::suggest(std::string_view remaining, const source &,
                               suggestion_builder &out) const {
    for (const auto &id : ids_) {
        if (id.starts_with(remaining)) {
            out.add(id);
        }
    }
}

auto uuid_parser::descriptor() const -> parser_descriptor {
    parser_descriptor descriptor{};
    descriptor.kind = parser_kind::uuid;
    return descriptor;
}

auto uuid_parser::parse(reader &in) const -> parse_result {
    const auto word = in.read_word();
    auto parsed = uuid::parse(word);
    if (!parsed) {
        return std::unexpected{word_error(word, "uuid")};
    }
    return argument_value{*parsed};
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

auto gamemode_arg() -> gamemode_parser { return gamemode_parser{}; }

auto resource_location_arg() -> resource_location_parser {
    return resource_location_parser{};
}

auto block_position_arg() -> block_position_parser {
    return block_position_parser{};
}

auto vec3_arg() -> vec3_parser { return vec3_parser{}; }

auto rotation_arg() -> rotation_parser { return rotation_parser{}; }

auto player_name_arg() -> player_name_parser { return player_name_parser{}; }

auto time_arg(std::int64_t minimum_ticks, std::vector<time_marker> markers)
    -> time_parser {
    return time_parser{minimum_ticks, std::move(markers)};
}

auto dimension_arg() -> dimension_parser {
    return dimension_parser{dimension_parser::default_ids()};
}

auto dimension_arg(std::vector<std::string> ids) -> dimension_parser {
    return dimension_parser{std::move(ids)};
}

auto uuid_arg() -> uuid_parser { return uuid_parser{}; }

auto entity_selector_arg(bool single, bool players_only)
    -> entity_selector_parser {
    return entity_selector_parser{single, players_only};
}

auto world_time_markers() -> std::vector<time_marker> {
    return {
        time_marker{"day", 1'000, "Start of the day"},
        time_marker{"morning", 1'000, "Start of the day"},
        time_marker{"noon", 6'000, "Middle of the day"},
        time_marker{"night", 13'000, "Start of the night"},
        time_marker{"midnight", 18'000, "Middle of the night"},
    };
}

} // namespace refrain::command
