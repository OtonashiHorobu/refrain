// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Entity selector argument and its resolution.
//
// The grammar follows the reference's EntitySelectorParser: `@` followed by one
// family character (`p`, `a`, `r`, `s`, `e`, `n`) and an optional
// `[key=value,...]` option list, or a bare player name. The parse result is the
// context-free `entity_selector`; `resolve_selector` applies it to a candidate
// list the executor builds when the command runs.
//
// Option handling follows the reference's EntitySelectorOptions: which options
// exist, which may be given once, which can be inverted, and the value ranges.
// The reference models every option against a live server; this build has no
// entity type registry, scoreboard, team, NBT or advancement state, so those
// options are recognized and rejected instead of being parsed and ignored.
//
// Determinism: `@r` and `sort=random` draw from the request seed through a
// SplitMix64 stream, never from a global generator, so the same server state
// and the same command text reproduce the same choice.

#include <refrain/command/parsers.hpp>

#include <refrain/command/selector.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace refrain::command {

namespace {

constexpr char selector_start = '@';
constexpr char options_start = '[';
constexpr char options_end = ']';
constexpr char option_separator = ',';
constexpr char option_assignment = '=';
constexpr char negation = '!';
constexpr char tag_marker = '#';

/// Player entity type id: the only entity type this build models.
constexpr std::string_view player_type = "minecraft:player";

/// One selectable family of the grammar, in the reference's suggestion order.
struct family_entry {
    std::string_view text{};
    std::string_view tooltip{};
};

constexpr std::array<family_entry, 6> family_entries{{
    {"@p", "the nearest player"},
    {"@a", "every online player"},
    {"@r", "a randomly chosen player"},
    {"@s", "the entity that runs the command"},
    {"@e", "every entity in the world"},
    {"@n", "the nearest entity"},
}};

/// One modelled option, in the reference's registration order.
struct option_entry {
    std::string_view key{};
    std::string_view tooltip{};
};

constexpr std::array<option_entry, 12> option_entries{{
    {"name", "player name"},
    {"distance", "distance range from the origin"},
    {"x", "origin x coordinate"},
    {"y", "origin y coordinate"},
    {"z", "origin z coordinate"},
    {"dx", "box width relative to the origin"},
    {"dy", "box height relative to the origin"},
    {"dz", "box depth relative to the origin"},
    {"limit", "maximum number of results"},
    {"sort", "result order"},
    {"gamemode", "game mode"},
    {"type", "entity type"},
}};

/// Options the reference defines but this build cannot evaluate. They are
/// rejected by name instead of being ignored, so a filter never silently
/// matches everything.
constexpr std::array<std::string_view, 9> unsupported_options{{
    "level",
    "x_rotation",
    "y_rotation",
    "team",
    "tag",
    "nbt",
    "scores",
    "advancements",
    "predicate",
}};

/// The sort order names the reference's `sort=` filter accepts.
constexpr std::array<std::pair<std::string_view, selector_order>, 4>
    sort_orders{{
        {"nearest", selector_order::nearest},
        {"furthest", selector_order::furthest},
        {"random", selector_order::random},
        {"arbitrary", selector_order::arbitrary},
    }};

constexpr std::array<std::pair<std::string_view, game_mode>, 4> gamemode_names{{
    {"survival", game_mode::survival},
    {"creative", game_mode::creative},
    {"adventure", game_mode::adventure},
    {"spectator", game_mode::spectator},
}};

/// The largest result count `limit=` accepts, which is also the result count of
/// the unbounded selectors.
constexpr std::int32_t infinite_results =
    std::numeric_limits<std::int32_t>::max();

[[nodiscard]] auto to_ascii_lower(char character) noexcept -> char {
    return character >= 'A' && character <= 'Z'
               ? static_cast<char>(character - 'A' + 'a')
               : character;
}

[[nodiscard]] auto equals_ignore_case(std::string_view left,
                                      std::string_view right) noexcept -> bool {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index{0}; index < left.size(); ++index) {
        if (to_ascii_lower(left[index]) != to_ascii_lower(right[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] auto starts_with_ignore_case(std::string_view text,
                                           std::string_view prefix) noexcept
    -> bool {
    return text.size() >= prefix.size() &&
           equals_ignore_case(text.substr(0, prefix.size()), prefix);
}

/// Characters the reference's `StringReader.readUnquotedString` accepts.
[[nodiscard]] auto is_unquoted_character(char character) noexcept -> bool {
    return (character >= '0' && character <= '9') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z') || character == '_' ||
           character == '-' || character == '.' || character == '+';
}

/// Characters the reference's `Identifier.read` accepts.
[[nodiscard]] auto is_identifier_character(char character) noexcept -> bool {
    return (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '_' ||
           character == ':' || character == '/' || character == '.' ||
           character == '-';
}

/// Read a run of characters the predicate accepts and return it as a view.
template <typename Allowed>
[[nodiscard]] auto read_run(reader &in, Allowed allowed) -> std::string_view {
    const auto start{in.cursor()};
    while (in.can_read() && allowed(in.peek())) {
        in.skip();
    }
    return in.input().substr(start, in.cursor() - start);
}

/// Read a number token with the reference's character set: digits, `-` and a
/// `.` that does not start a `..` range separator.
[[nodiscard]] auto read_number_token(reader &in) -> std::string_view {
    const auto start{in.cursor()};
    while (in.can_read()) {
        const auto character{in.peek()};
        if ((character >= '0' && character <= '9') || character == '-') {
            in.skip();
            continue;
        }
        if (character == '.') {
            const auto next{in.cursor() + 1};
            if (next < in.input().size() && in.input()[next] == '.') {
                break;
            }
            in.skip();
            continue;
        }
        break;
    }
    return in.input().substr(start, in.cursor() - start);
}

[[nodiscard]] auto parse_double_token(std::string_view token) noexcept
    -> std::optional<double> {
    if (token.empty() || token.front() == '+') {
        return std::nullopt;
    }
    double value{0.0};
    const auto result{
        std::from_chars(token.data(), token.data() + token.size(), value)};
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] auto parse_int_token(std::string_view token) noexcept
    -> std::optional<std::int64_t> {
    if (token.empty() || token.front() == '+') {
        return std::nullopt;
    }
    std::int64_t value{0};
    const auto result{
        std::from_chars(token.data(), token.data() + token.size(), value)};
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] auto read_string_token(reader &in, std::size_t argument_start)
    -> std::expected<std::string, argument_error> {
    if (in.can_read() && (in.peek() == '"' || in.peek() == '\'')) {
        const auto quote_start{in.cursor()};
        auto text{in.read_quoted()};
        if (!text) {
            return std::unexpected{make_argument_error(
                text.error().message, quote_start - argument_start)};
        }
        return std::move(*text);
    }
    return std::string{read_run(in, is_unquoted_character)};
}

/// One `min..max` bound pair, read the way `MinMaxBounds.Bounds.fromReader`
/// reads it: an optional minimum, an optional `..` and an optional maximum. A
/// single number is a point range.
[[nodiscard]] auto read_range(reader &in, std::size_t argument_start)
    -> std::expected<selector_range, argument_error> {
    const auto range_start{in.cursor()};
    const auto read_bound = [&]() -> std::optional<double> {
        const auto token{read_number_token(in)};
        if (token.empty()) {
            return std::nullopt;
        }
        return parse_double_token(token);
    };
    const auto minimum{read_bound()};
    std::optional<double> maximum{minimum};
    if (in.can_read() && in.peek() == '.' &&
        in.cursor() + 1 < in.input().size() &&
        in.input()[in.cursor() + 1] == '.') {
        in.skip();
        in.skip();
        maximum = read_bound();
    }
    if (!minimum && !maximum) {
        return std::unexpected{make_argument_error(
            "expected a selector range", range_start - argument_start)};
    }
    if (minimum && maximum && *minimum > *maximum) {
        return std::unexpected{make_argument_error(
            "the selector range minimum is larger than its maximum",
            range_start - argument_start)};
    }
    return selector_range{.minimum = minimum, .maximum = maximum};
}

/// Parser state of one selector, mirroring the reference's EntitySelectorParser
/// fields plus the "given once" bookkeeping of its option states.
class selector_parser {
  public:
    selector_parser(reader &in, std::size_t start, bool single,
                    bool players_only)
        : in_{in}, start_{start}, single_{single}, players_only_{players_only} {
    }

    [[nodiscard]] auto parse() -> parse_result {
        if (in_.can_read() && in_.peek() == selector_start) {
            in_.skip();
            if (auto problem{parse_selector()}; problem.has_value()) {
                return std::unexpected{std::move(*problem)};
            }
        } else if (auto problem{parse_name()}; problem.has_value()) {
            return std::unexpected{std::move(*problem)};
        }
        return finish();
    }

  private:
    [[nodiscard]] auto offset(std::size_t position) const noexcept
        -> std::size_t {
        return position >= start_ ? position - start_ : 0;
    }

    [[nodiscard]] auto error_at(std::string message, std::size_t position) const
        -> argument_error {
        return make_argument_error(std::move(message), offset(position));
    }

    [[nodiscard]] auto error(std::string message) const -> argument_error {
        return error_at(std::move(message), in_.cursor());
    }

    /// Reference `parseNameOrUUID`: a bare player name. This build has no UUID
    /// entity lookup, so a canonical UUID is rejected by name instead of
    /// silently matching nothing.
    [[nodiscard]] auto parse_name() -> std::optional<argument_error> {
        const auto name_start{in_.cursor()};
        auto name{read_string_token(in_, start_)};
        if (!name) {
            return std::move(name.error());
        }
        if (uuid::parse(*name).has_value()) {
            return error_at("entity targets by uuid are not supported by this "
                            "build; use the player name",
                            name_start);
        }
        if (name->empty() || name->size() > 16) {
            return error_at("expected a player name or an entity selector",
                            name_start);
        }
        kind_ = selector_kind::name;
        name_ = std::move(*name);
        max_results_ = 1;
        return std::nullopt;
    }

    [[nodiscard]] auto parse_selector() -> std::optional<argument_error> {
        if (!in_.can_read()) {
            return error("expected an entity selector type");
        }
        const auto type_start{in_.cursor()};
        const auto type{in_.read()};
        switch (type) {
        case 'a':
            kind_ = selector_kind::all_players;
            max_results_ = infinite_results;
            order_ = selector_order::arbitrary;
            break;
        case 'e':
            kind_ = selector_kind::all_entities;
            max_results_ = infinite_results;
            includes_entities_ = true;
            order_ = selector_order::arbitrary;
            break;
        case 'n':
            kind_ = selector_kind::nearest_entity;
            max_results_ = 1;
            includes_entities_ = true;
            order_ = selector_order::nearest;
            break;
        case 'p':
            kind_ = selector_kind::nearest_player;
            max_results_ = 1;
            order_ = selector_order::nearest;
            break;
        case 'r':
            kind_ = selector_kind::random_player;
            max_results_ = 1;
            order_ = selector_order::random;
            break;
        case 's':
            kind_ = selector_kind::self;
            max_results_ = 1;
            includes_entities_ = true;
            current_entity_ = true;
            break;
        default:
            return error_at(fmt::format("unknown entity selector '@{}'", type),
                            type_start);
        }
        if (!in_.can_read() || in_.peek() != options_start) {
            return std::nullopt;
        }
        in_.skip();
        return parse_options();
    }

    /// Reference `parseOptions`: `[key=value (,key=value)*]`, with whitespace
    /// allowed around the tokens and a trailing separator tolerated.
    [[nodiscard]] auto parse_options() -> std::optional<argument_error> {
        in_.skip_whitespace();
        while (in_.can_read() && in_.peek() != options_end) {
            in_.skip_whitespace();
            const auto key_start{in_.cursor()};
            auto key{read_string_token(in_, start_)};
            if (!key) {
                return std::move(key.error());
            }
            in_.skip_whitespace();
            if (!in_.can_read() || in_.peek() != option_assignment) {
                return error_at(
                    fmt::format("expected a value for the selector option '{}'",
                                *key),
                    key_start);
            }
            in_.skip();
            in_.skip_whitespace();
            if (auto problem{parse_option(*key, key_start)};
                problem.has_value()) {
                return problem;
            }
            in_.skip_whitespace();
            if (in_.can_read()) {
                if (in_.peek() == option_separator) {
                    in_.skip();
                    continue;
                }
                if (in_.peek() != options_end) {
                    return error("expected the end of the selector options");
                }
            }
            break;
        }
        if (!in_.can_read()) {
            return error("expected the end of the selector options");
        }
        in_.skip();
        return std::nullopt;
    }

    [[nodiscard]] auto parse_option(std::string_view key, std::size_t key_start)
        -> std::optional<argument_error> {
        const auto inapplicable = [this, key_start, key]() -> argument_error {
            return error_at(
                fmt::format("the entity selector option '{}' isn't applicable "
                            "here",
                            key),
                key_start);
        };
        const auto once = [&](bool &given) -> std::optional<argument_error> {
            if (given) {
                return inapplicable();
            }
            given = true;
            return std::nullopt;
        };
        if (key == "name") {
            const auto inverted{read_negation()};
            // The reference's invertible-set rule: one positive entry, any
            // number of inverted ones, and a positive entry may follow the
            // inverted ones.
            if (name_filter_positive_ && !inverted) {
                return inapplicable();
            }
            if (!inverted) {
                name_filter_positive_ = true;
            }
            auto value{read_string_token(in_, start_)};
            if (!value) {
                return std::move(value.error());
            }
            names_.push_back(selector_name_filter{.name = std::move(*value),
                                                  .inverted = inverted});
            return std::nullopt;
        }
        if (key == "gamemode") {
            const auto inverted{read_negation()};
            if (gamemode_filter_positive_ && !inverted) {
                return inapplicable();
            }
            const auto value_start{in_.cursor()};
            const auto text{read_run(in_, is_unquoted_character)};
            const auto mode{find_gamemode(text)};
            if (!mode.has_value()) {
                return error_at(fmt::format("unknown game mode '{}'", text),
                                value_start);
            }
            if (!inverted) {
                gamemode_filter_positive_ = true;
            }
            includes_entities_ = false;
            gamemodes_.push_back(
                selector_gamemode_filter{.mode = *mode, .inverted = inverted});
            return std::nullopt;
        }
        if (key == "type") {
            return parse_type_option(key_start);
        }
        if (key == "sort") {
            if (current_entity_) {
                return inapplicable();
            }
            if (auto problem{once(sort_given_)}; problem.has_value()) {
                return problem;
            }
            const auto value_start{in_.cursor()};
            const auto text{read_run(in_, is_unquoted_character)};
            const auto found{
                std::ranges::find_if(sort_orders, [text](const auto &entry) {
                    return entry.first == text;
                })};
            if (found == sort_orders.end()) {
                return error_at(fmt::format("unknown sort mode '{}'", text),
                                value_start);
            }
            order_ = found->second;
            return std::nullopt;
        }
        if (key == "limit") {
            if (current_entity_) {
                return inapplicable();
            }
            if (auto problem{once(limit_given_)}; problem.has_value()) {
                return problem;
            }
            const auto value_start{in_.cursor()};
            const auto token{read_run(in_, [](char character) {
                return (character >= '0' && character <= '9') ||
                       character == '-';
            })};
            const auto count{parse_int_token(token)};
            if (!count.has_value()) {
                return error_at(
                    "expected an integer for the selector option 'limit'",
                    value_start);
            }
            if (*count < 1) {
                return error_at("the selector limit must be at least 1",
                                value_start);
            }
            if (*count > static_cast<std::int64_t>(infinite_results)) {
                return error_at("the selector limit is too large", value_start);
            }
            max_results_ = static_cast<std::int32_t>(*count);
            limited_ = true;
            return std::nullopt;
        }
        if (key == "distance") {
            if (auto problem{once(distance_given_)}; problem.has_value()) {
                return problem;
            }
            const auto value_start{in_.cursor()};
            auto range{read_range(in_, start_)};
            if (!range) {
                return std::move(range.error());
            }
            if ((range->minimum && *range->minimum < 0.0) ||
                (range->maximum && *range->maximum < 0.0)) {
                return error_at("the distance cannot be negative", value_start);
            }
            distance_ = *range;
            return std::nullopt;
        }
        struct coordinate_option {
            std::string_view key{};
            std::optional<double> *slot{};
            bool *given{};
        };
        const std::array<coordinate_option, 6> coordinates{{
            {"x", &x_, &x_given_},
            {"y", &y_, &y_given_},
            {"z", &z_, &z_given_},
            {"dx", &delta_x_, &delta_x_given_},
            {"dy", &delta_y_, &delta_y_given_},
            {"dz", &delta_z_, &delta_z_given_},
        }};
        for (const auto &coordinate : coordinates) {
            if (key != coordinate.key) {
                continue;
            }
            if (*coordinate.given) {
                return inapplicable();
            }
            *coordinate.given = true;
            const auto value_start{in_.cursor()};
            const auto token{read_number_token(in_)};
            const auto value{parse_double_token(token)};
            if (!value.has_value()) {
                return error_at(
                    fmt::format(
                        "expected a number for the selector option '{}'", key),
                    value_start);
            }
            *coordinate.slot = *value;
            return std::nullopt;
        }
        if (std::ranges::find(unsupported_options, key) !=
            unsupported_options.end()) {
            return error_at(
                fmt::format("the entity selector option '{}' is not supported "
                            "by this build: the state it filters on is not "
                            "modelled",
                            key),
                key_start);
        }
        return error_at(fmt::format("unknown entity selector option '{}'", key),
                        key_start);
    }

    [[nodiscard]] auto parse_type_option(std::size_t key_start)
        -> std::optional<argument_error> {
        const auto value_start{in_.cursor()};
        const auto inverted{read_negation()};
        if (type_filter_positive_ && !inverted) {
            return error_at(
                "the entity selector option 'type' isn't applicable here",
                key_start);
        }
        if (in_.can_read() && in_.peek() == tag_marker) {
            return error("the 'type' option accepts an entity type id, not an "
                         "entity type tag");
        }
        const auto text{read_run(in_, is_identifier_character)};
        const auto id{data::resource_location::parse(text)};
        if (!id.has_value()) {
            return error_at(fmt::format("invalid entity type '{}'", text),
                            value_start);
        }
        if (!inverted) {
            type_filter_positive_ = true;
        }
        const auto canonical{id->to_string()};
        if (!inverted && canonical == player_type) {
            includes_entities_ = false;
        }
        type_ = selector_type_filter{.id = canonical, .inverted = inverted};
        return std::nullopt;
    }

    /// Read the optional `!` in front of an option value. The reference skips
    /// whitespace before and after it, so `name = ! Alice` is accepted.
    [[nodiscard]] auto read_negation() -> bool {
        in_.skip_whitespace();
        if (in_.can_read() && in_.peek() == negation) {
            in_.skip();
            in_.skip_whitespace();
            return true;
        }
        return false;
    }

    [[nodiscard]] static auto find_gamemode(std::string_view text) noexcept
        -> std::optional<game_mode> {
        for (const auto &[name, mode] : gamemode_names) {
            if (equals_ignore_case(name, text)) {
                return mode;
            }
        }
        return std::nullopt;
    }

    /// Build the parsed value and apply the argument-form cardinality checks
    /// the reference performs in `EntityArgument.parse`.
    [[nodiscard]] auto finish() -> parse_result {
        entity_selector selector{};
        selector.kind = kind_;
        selector.name = std::move(name_);
        selector.max_results = max_results_;
        selector.includes_entities = includes_entities_;
        selector.current_entity = current_entity_;
        selector.limited = limited_;
        selector.single = single_;
        selector.players_only = players_only_;
        selector.order = order_;
        selector.distance = distance_;
        selector.x = x_;
        selector.y = y_;
        selector.z = z_;
        selector.delta_x = delta_x_;
        selector.delta_y = delta_y_;
        selector.delta_z = delta_z_;
        selector.names = std::move(names_);
        selector.gamemodes = std::move(gamemodes_);
        selector.type = std::move(type_);
        if (selector.max_results > 1 && single_) {
            return std::unexpected{make_argument_error(
                players_only_
                    ? "Only one player is allowed, but the provided selector "
                      "allows more than one"
                    : "Only one entity is allowed, but the provided selector "
                      "allows more than one",
                0)};
        }
        if (selector.includes_entities && players_only_ &&
            !selector.current_entity) {
            return std::unexpected{make_argument_error(
                "Only players may be affected by this command, but the "
                "provided selector includes entities",
                0)};
        }
        return argument_value{std::move(selector)};
    }

    reader &in_;
    std::size_t start_{0};
    bool single_{false};
    bool players_only_{false};

    selector_kind kind_{selector_kind::name};
    std::string name_{};
    std::int32_t max_results_{1};
    bool includes_entities_{false};
    bool current_entity_{false};
    bool limited_{false};
    selector_order order_{selector_order::arbitrary};
    std::optional<selector_range> distance_{};
    std::optional<double> x_{};
    std::optional<double> y_{};
    std::optional<double> z_{};
    std::optional<double> delta_x_{};
    std::optional<double> delta_y_{};
    std::optional<double> delta_z_{};
    std::vector<selector_name_filter> names_{};
    std::vector<selector_gamemode_filter> gamemodes_{};
    std::optional<selector_type_filter> type_{};

    bool limit_given_{false};
    bool sort_given_{false};
    bool distance_given_{false};
    bool x_given_{false};
    bool y_given_{false};
    bool z_given_{false};
    bool delta_x_given_{false};
    bool delta_y_given_{false};
    bool delta_z_given_{false};
    bool name_filter_positive_{false};
    bool gamemode_filter_positive_{false};
    bool type_filter_positive_{false};
};

/// SplitMix64: the deterministic stream behind `@r` and `sort=random`.
class splitmix64 {
  public:
    explicit splitmix64(std::uint64_t seed) noexcept : state_{seed} {}

    [[nodiscard]] auto next() noexcept -> std::uint64_t {
        state_ += 0x9E3779B97F4A7C15ULL;
        auto value{state_};
        value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
        return value ^ (value >> 31U);
    }

    [[nodiscard]] auto bounded(std::size_t bound) noexcept -> std::size_t {
        return bound == 0 ? 0 : static_cast<std::size_t>(next() % bound);
    }

  private:
    std::uint64_t state_{0};
};

/// One axis-aligned box of the `dx`/`dy`/`dz` filter, in world coordinates.
struct selector_box {
    double min_x{0.0};
    double min_y{0.0};
    double min_z{0.0};
    double max_x{0.0};
    double max_y{0.0};
    double max_z{0.0};
};

[[nodiscard]] auto make_box(const entity_selector &selector,
                            const selector_position &origin) noexcept
    -> selector_box {
    const auto span = [](double delta) -> std::pair<double, double> {
        // The reference widens a component by one block and treats a negative
        // extent as pointing the other way from the origin.
        return delta < 0.0 ? std::pair<double, double>{delta, 1.0}
                           : std::pair<double, double>{0.0, delta + 1.0};
    };
    const auto x{span(selector.delta_x.value_or(0.0))};
    const auto y{span(selector.delta_y.value_or(0.0))};
    const auto z{span(selector.delta_z.value_or(0.0))};
    return selector_box{
        .min_x = origin.x + x.first,
        .min_y = origin.y + y.first,
        .min_z = origin.z + z.first,
        .max_x = origin.x + x.second,
        .max_y = origin.y + y.second,
        .max_z = origin.z + z.second,
    };
}

/// Java `AABB.intersects(AABB)`: an open-interval overlap on every axis, tested
/// against the candidate's hitbox.
[[nodiscard]] auto intersects(const selector_box &box,
                              const selector_candidate &candidate) noexcept
    -> bool {
    const auto half_width{candidate.width / 2.0};
    return box.min_x < candidate.x + half_width &&
           box.max_x > candidate.x - half_width &&
           box.min_y < candidate.y + candidate.height &&
           box.max_y > candidate.y && box.min_z < candidate.z + half_width &&
           box.max_z > candidate.z - half_width;
}

/// Squared distance in the reference's `Entity.distanceToSqr(Vec3)` form: from
/// the candidate's feet to the origin.
[[nodiscard]] auto distance_squared(const selector_candidate &candidate,
                                    const selector_position &origin) noexcept
    -> double {
    const auto x{candidate.x - origin.x};
    const auto y{candidate.y - origin.y};
    const auto z{candidate.z - origin.z};
    return x * x + y * y + z * z;
}

[[nodiscard]] auto make_unsupported_type_error(const std::string &type)
    -> selector_error {
    return selector_error{
        .failure = selector_error::kind::unsupported_entity_type,
        .message = fmt::format(
            "The entity type '{}' is not available: only players are modelled",
            type),
    };
}

[[nodiscard]] auto make_missing_position_error(const selector_candidate &entity)
    -> selector_error {
    return selector_error{
        .failure = selector_error::kind::missing_candidate_position,
        .message = fmt::format("No position is available for '{}', so the "
                               "selector cannot apply a distance or box filter",
                               entity.name),
    };
}

/// Offer the online player names that continue `prefix`.
void suggest_players(std::string_view prefix, const source &from,
                     suggestion_builder &out) {
    for (const auto &name : from.online_players()) {
        if (starts_with_ignore_case(name, prefix)) {
            out.add(name);
        }
    }
}

} // namespace

auto to_string(selector_kind kind) noexcept -> std::string_view {
    switch (kind) {
    case selector_kind::name:
        return "name";
    case selector_kind::nearest_player:
        return "nearest_player";
    case selector_kind::all_players:
        return "all_players";
    case selector_kind::random_player:
        return "random_player";
    case selector_kind::self:
        return "self";
    case selector_kind::all_entities:
        return "all_entities";
    case selector_kind::nearest_entity:
        return "nearest_entity";
    }
    return "unknown";
}

auto selector_range_matches_squared(const selector_range &range,
                                    double value_squared) noexcept -> bool {
    if (range.minimum && *range.minimum * *range.minimum > value_squared) {
        return false;
    }
    if (range.maximum && *range.maximum * *range.maximum < value_squared) {
        return false;
    }
    return true;
}

auto entity_selector_parser::descriptor() const -> parser_descriptor {
    parser_descriptor descriptor{};
    descriptor.kind = parser_kind::entity_selector;
    descriptor.entity_single = single_;
    descriptor.entity_players_only = players_only_;
    return descriptor;
}

entity_selector_parser::entity_selector_parser(bool single, bool players_only)
    : single_{single}, players_only_{players_only} {}

auto entity_selector_parser::parse(reader &in) const -> parse_result {
    selector_parser parser{in, in.cursor(), single_, players_only_};
    return parser.parse();
}

void entity_selector_parser::suggest(std::string_view remaining,
                                     const source &src,
                                     suggestion_builder &out) const {
    if (remaining.find(options_start) == std::string_view::npos) {
        // A bare player name remains a valid target before any `@` is typed.
        suggest_players(remaining, src, out);
        for (const auto &entry : family_entries) {
            if (starts_with_ignore_case(entry.text, remaining)) {
                out.add(entry.text, entry.tooltip);
            }
            if (equals_ignore_case(entry.text, remaining)) {
                out.add(fmt::format("{}[", entry.text), "add selector options");
            }
        }
        return;
    }

    // Inside the option list: complete the current key or the current value.
    // The response replaces the whole argument, so every suggestion carries the
    // text typed before the token being completed.
    const auto separator{remaining.find_last_of(", [") == std::string_view::npos
                             ? std::string_view::npos
                             : remaining.find_last_of(",[")};
    const auto token_start{separator == std::string_view::npos ? 0U
                                                               : separator + 1};
    const auto head{remaining.substr(0, token_start)};
    const auto token{remaining.substr(token_start)};
    const auto assignment{token.find(option_assignment)};
    if (assignment == std::string_view::npos) {
        for (const auto &entry : option_entries) {
            if (starts_with_ignore_case(entry.key, token)) {
                out.add(fmt::format("{}{}=", head, entry.key), entry.tooltip);
            }
        }
        return;
    }
    const auto key{token.substr(0, assignment)};
    auto value{token.substr(assignment + 1U)};
    auto negated{false};
    if (!value.empty() && value.front() == negation) {
        negated = true;
        value.remove_prefix(1);
    }
    const auto value_prefix{
        fmt::format("{}{}={}", head, key, negated ? "!" : "")};
    if (key == "sort") {
        for (const auto &[name, order] : sort_orders) {
            static_cast<void>(order);
            if (starts_with_ignore_case(name, value)) {
                out.add(fmt::format("{}{}", value_prefix, name));
            }
        }
        return;
    }
    if (key == "gamemode") {
        for (const auto &[name, mode] : gamemode_names) {
            static_cast<void>(mode);
            if (!starts_with_ignore_case(name, value)) {
                continue;
            }
            out.add(fmt::format("{}{}", value_prefix, name));
        }
    } else if (key == "type") {
        if (starts_with_ignore_case(player_type, value)) {
            out.add(fmt::format("{}{}", value_prefix, player_type));
        }
    } else if (key == "name") {
        for (const auto &name : src.online_players()) {
            if (starts_with_ignore_case(name, value)) {
                out.add(fmt::format("{}{}", value_prefix, name));
            }
        }
    }
}

auto resolve_selector(const entity_selector &selector,
                      const selector_request &request)
    -> std::expected<std::vector<std::size_t>, selector_error> {
    const auto &candidates{request.candidates};
    std::vector<std::size_t> matches{};

    // A bare name names one player and ignores every filter, like the
    // reference's early `playerName` branch. Player names are matched without
    // regard to case, which is how the reference resolves a name target.
    if (selector.kind == selector_kind::name) {
        for (std::size_t index{0}; index < candidates.size(); ++index) {
            if (candidates[index].player &&
                equals_ignore_case(candidates[index].name, selector.name)) {
                matches.push_back(index);
                break;
            }
        }
        return matches;
    }

    // `type=` names the entity type a match must have. Only players exist in
    // this build, so any other type fails closed instead of selecting nothing.
    if (selector.type.has_value() && selector.type->id != player_type) {
        return std::unexpected{make_unsupported_type_error(selector.type->id)};
    }

    // The origin is only resolved when a filter or an ordering needs it. A
    // source without a position can therefore still run `@a`, an unfiltered
    // `@e`, or a selector that supplies all three absolute coordinates.
    const auto needs_origin{
        selector.distance.has_value() || selector.delta_x.has_value() ||
        selector.delta_y.has_value() || selector.delta_z.has_value() ||
        selector.order == selector_order::nearest ||
        selector.order == selector_order::furthest};
    std::optional<selector_position> origin{};
    if (needs_origin) {
        if (selector.x.has_value() && selector.y.has_value() &&
            selector.z.has_value()) {
            origin = selector_position{*selector.x, *selector.y, *selector.z};
        } else if (request.source_position.has_value()) {
            origin = selector_position{
                selector.x.value_or(request.source_position->x),
                selector.y.value_or(request.source_position->y),
                selector.z.value_or(request.source_position->z),
            };
        } else {
            return std::unexpected{selector_error{
                .failure = selector_error::kind::no_source_position,
                .message = "The command source has no position; the selector "
                           "needs one for its distance, box or sort filter",
            }};
        }
    }
    const auto box{
        origin.has_value() &&
                (selector.delta_x.has_value() || selector.delta_y.has_value() ||
                 selector.delta_z.has_value())
            ? std::optional<selector_box>{make_box(selector, *origin)}
            : std::nullopt};

    const auto matches_filters = [&selector, &origin,
                                  &box](const selector_candidate &candidate)
        -> std::expected<bool, selector_error> {
        if (selector.type.has_value() &&
            candidate.player == selector.type->inverted) {
            return false;
        }
        for (const auto &filter : selector.names) {
            if ((candidate.name == filter.name) == filter.inverted) {
                return false;
            }
        }
        for (const auto &filter : selector.gamemodes) {
            // A non-player entity never matches a game-mode filter, in either
            // polarity, which is the reference's behaviour.
            if (!candidate.player || !candidate.mode.has_value()) {
                return false;
            }
            if ((*candidate.mode == filter.mode) == filter.inverted) {
                return false;
            }
        }
        if (box.has_value()) {
            if (!candidate.has_position) {
                return std::unexpected{make_missing_position_error(candidate)};
            }
            if (!intersects(*box, candidate)) {
                return false;
            }
        }
        if (selector.distance.has_value()) {
            if (!candidate.has_position) {
                return std::unexpected{make_missing_position_error(candidate)};
            }
            if (!selector_range_matches_squared(
                    *selector.distance, distance_squared(candidate, *origin))) {
                return false;
            }
        }
        return true;
    };

    if (selector.current_entity) {
        // `@s` resolves against the source entity only; a source without one
        // (the console) matches nothing.
        if (!request.self_identity.has_value()) {
            return matches;
        }
        for (std::size_t index{0}; index < candidates.size(); ++index) {
            if (candidates[index].identity != *request.self_identity) {
                continue;
            }
            auto matched{matches_filters(candidates[index])};
            if (!matched) {
                return std::unexpected{std::move(matched.error())};
            }
            if (*matched) {
                matches.push_back(index);
            }
            break;
        }
        return matches;
    }

    for (std::size_t index{0}; index < candidates.size(); ++index) {
        const auto &candidate{candidates[index]};
        if (!selector.includes_entities && !candidate.player) {
            continue;
        }
        auto matched{matches_filters(candidate)};
        if (!matched) {
            return std::unexpected{std::move(matched.error())};
        }
        if (*matched) {
            matches.push_back(index);
        }
    }

    // Sorting happens before the limit, so a limited selector takes the first
    // entries of the requested order rather than the first matches.
    switch (selector.order) {
    case selector_order::arbitrary:
        break;
    case selector_order::nearest:
    case selector_order::furthest: {
        const auto point{*origin};
        for (const auto index : matches) {
            if (!candidates[index].has_position) {
                return std::unexpected{
                    make_missing_position_error(candidates[index])};
            }
        }
        const auto nearest_first{selector.order == selector_order::nearest};
        std::stable_sort(matches.begin(), matches.end(),
                         [nearest_first, &point,
                          &candidates](std::size_t left, std::size_t right) {
                             const auto left_distance{
                                 distance_squared(candidates[left], point)};
                             const auto right_distance{
                                 distance_squared(candidates[right], point)};
                             return nearest_first
                                        ? left_distance < right_distance
                                        : right_distance < left_distance;
                         });
        break;
    }
    case selector_order::random: {
        splitmix64 random{request.random_seed};
        for (std::size_t remaining{matches.size()}; remaining > 1;
             --remaining) {
            const auto other{random.bounded(remaining)};
            std::swap(matches[remaining - 1U], matches[other]);
        }
        break;
    }
    }

    const auto limit{static_cast<std::size_t>(selector.max_results)};
    if (matches.size() > limit) {
        matches.resize(limit);
    }
    return matches;
}

} // namespace refrain::command
