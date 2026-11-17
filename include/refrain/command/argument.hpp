// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/command/reader.hpp>
#include <refrain/command/source.hpp>
#include <refrain/command/suggestions.hpp>
#include <refrain/command/tree.hpp>
#include <refrain/data/resource_location.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace refrain::command {

/// Game mode value of the `gamemode` argument. The enumerator order matches the
/// protocol's numeric game mode ids, so a network adapter converts without a
/// table.
enum class game_mode : std::uint8_t {
    survival = 0,
    creative = 1,
    adventure = 2,
    spectator = 3,
};

[[nodiscard]] auto to_string(game_mode mode) noexcept -> std::string_view;

/// One block position argument. Absolute coordinates are whole blocks; a `~`
/// coordinate is relative to the command source and may carry a fractional
/// amount. `local` marks the `^` form, whose three components are a left/up/
/// forwards offset from the source anchor instead of world axes.
struct block_position {
    double x{0.0};
    double y{0.0};
    double z{0.0};
    bool relative_x{false};
    bool relative_y{false};
    bool relative_z{false};
    bool local{false};

    friend auto operator==(const block_position &, const block_position &)
        -> bool = default;
};

/// One `vec3` argument: a world position with fractional absolute values.
///
/// Absolute components are used as given, with the reference's rule that an
/// integer absolute X or Z names the block centre and is rounded by half a
/// block; Y is never centred. The parser applies that rule, so the values an
/// executor reads are the final coordinates it should use. A `~` component is
/// relative to the command source and `^` marks the local form, whose three
/// components are a left/up/forwards offset from the source anchor instead of
/// world axes.
struct vec3_position {
    double x{0.0};
    double y{0.0};
    double z{0.0};
    bool relative_x{false};
    bool relative_y{false};
    bool relative_z{false};
    bool local{false};

    friend auto operator==(const vec3_position &, const vec3_position &)
        -> bool = default;
};

/// One rotation argument: yaw followed by pitch, in degrees. A `~` component is
/// relative to the command source's current rotation.
struct rotation {
    double yaw{0.0};
    double pitch{0.0};
    bool relative_yaw{false};
    bool relative_pitch{false};

    friend auto operator==(const rotation &, const rotation &)
        -> bool = default;
};

/// A 128-bit identifier in wire byte order. Kept in the command module so an
/// argument can carry the parsed value without a network dependency.
struct uuid {
    std::array<std::uint8_t, 16> bytes{};

    friend auto operator==(const uuid &, const uuid &) -> bool = default;

    /// Canonical `8-4-4-4-12` lowercase hexadecimal form.
    [[nodiscard]] auto to_string() const -> std::string;

    /// Parse the canonical form; hyphens are required and either case is
    /// accepted.
    [[nodiscard]] static auto parse(std::string_view text) -> result<uuid>;
};

/// Which entity selector family an `entity_selector` names.
enum class selector_kind : std::uint8_t {
    /// A bare player name, which is the reference's name-or-uuid target.
    name = 0,
    /// `@p`: the nearest player, or one of the nearest on a distance tie.
    nearest_player,
    /// `@a`: every online player.
    all_players,
    /// `@r`: a randomly drawn player.
    random_player,
    /// `@s`: the entity that runs the command.
    self,
    /// `@e`: every entity the executor can see.
    all_entities,
    /// `@n`: the nearest entity.
    nearest_entity,
};

/// Ordering applied to the matched entities before the limit is taken.
enum class selector_order : std::uint8_t {
    arbitrary = 0,
    nearest,
    furthest,
    random,
};

/// One inclusive numeric range of the selector grammar: `5`, `5..`, `..5` or
/// `5..10`. An absent bound is unbounded on that side.
struct selector_range {
    std::optional<double> minimum{};
    std::optional<double> maximum{};

    friend auto operator==(const selector_range &, const selector_range &)
        -> bool = default;
};

/// One `name=` filter. An inverted filter matches every entity whose name is
/// not the given one.
struct selector_name_filter {
    std::string name{};
    bool inverted{false};

    friend auto operator==(const selector_name_filter &,
                           const selector_name_filter &) -> bool = default;
};

/// One `gamemode=` filter, including its inverted form.
struct selector_gamemode_filter {
    game_mode mode{game_mode::survival};
    bool inverted{false};

    friend auto operator==(const selector_gamemode_filter &,
                           const selector_gamemode_filter &) -> bool = default;
};

/// One `type=` filter: an entity type id and its inverted form.
struct selector_type_filter {
    std::string id{};
    bool inverted{false};

    friend auto operator==(const selector_type_filter &,
                           const selector_type_filter &) -> bool = default;
};

/// The context-free result of parsing one entity selector argument.
///
/// The value names what to match, never what matched: a selector is parsed
/// while the command line is parsed, and resolving it against the command
/// source and the online players happens only when the executor runs. That
/// separation is what keeps a packet path from reading live entity state.
///
/// `single` and `players_only` are properties of the argument form
/// (`EntityArgument.entity()`, `players()`, ...) rather than of the grammar.
/// They decide the parse-time cardinality checks and which "not found" message
/// the executor reports, exactly like the reference's argument factories.
struct entity_selector {
    selector_kind kind{selector_kind::name};
    /// The bare player name of `kind == name`; empty for `@` selectors.
    std::string name{};
    /// Upper bound on the matched entities (`limit=`), 1 for a bare name and
    /// for the single-result selectors.
    std::int32_t max_results{1};
    /// True for `@e`, `@n` and `@s`: the selector may match entities that are
    /// not players.
    bool includes_entities{false};
    /// True for `@s`: resolve against the command source entity only.
    bool current_entity{false};
    /// True when the input carried a `limit=` filter.
    bool limited{false};
    /// The argument form allows exactly one result.
    bool single{false};
    /// The argument form accepts players only.
    bool players_only{false};
    selector_order order{selector_order::arbitrary};
    /// `distance=`: a range over the distance to the resolution origin.
    std::optional<selector_range> distance{};
    /// `x=`/`y=`/`z=`: absolute origin overrides.
    std::optional<double> x{};
    std::optional<double> y{};
    std::optional<double> z{};
    /// `dx=`/`dy=`/`dz=`: the box filter, relative to the origin.
    std::optional<double> delta_x{};
    std::optional<double> delta_y{};
    std::optional<double> delta_z{};
    std::vector<selector_name_filter> names{};
    std::vector<selector_gamemode_filter> gamemodes{};
    std::optional<selector_type_filter> type{};

    friend auto operator==(const entity_selector &, const entity_selector &)
        -> bool = default;
};

/// One parsed argument value.
///
/// Unquoted words and greedy strings are borrowed views into the command input;
/// quoted strings, resource locations and enum values own their bytes; numbers,
/// booleans and the structured values are stored without allocation. The
/// duplicated string alternatives are what make the borrowed form possible:
/// `std::string_view` means "slice of the input" and `std::string` means
/// "decoded or canonicalized copy".
using argument_value =
    std::variant<std::monostate, bool, std::int64_t, double, std::string_view,
                 std::string, data::resource_location, block_position,
                 vec3_position, rotation, uuid, game_mode, entity_selector>;

/// Why a parser rejected its input, and where.
///
/// `offset` is a byte offset from the start of the argument, so a multi-token
/// parser reports the token that actually failed instead of the argument start.
struct argument_error {
    std::string message{};
    std::size_t offset{0};
};

using parse_result = std::expected<argument_value, argument_error>;

/// Build an `argument_error` at a byte offset from the argument start.
[[nodiscard]] auto make_argument_error(std::string message,
                                       std::size_t offset = 0)
    -> argument_error;

/// Extensible argument parser. A parser reads one argument from `in`, returns
/// its typed value, and offers completions for the partial argument.
///
/// The reader is positioned at the first byte of the argument. A parser may
/// consume several whitespace-separated tokens (a block position consumes
/// three), but must stop immediately after the argument's last byte: the
/// dispatcher enforces the boundary (a token must be followed by whitespace or
/// end of input) for every parser, so a parser must not consume the following
/// separator.
///
/// No method may throw: failures are values.
class argument_parser {
  public:
    argument_parser() = default;
    virtual ~argument_parser() = default;
    argument_parser(const argument_parser &) = delete;
    argument_parser &operator=(const argument_parser &) = delete;
    argument_parser(argument_parser &&) = default;
    argument_parser &operator=(argument_parser &&) = default;

    /// Tree properties for this parser. Called only while a tree snapshot is
    /// built, so returning a fresh descriptor by value is acceptable.
    [[nodiscard]] virtual auto descriptor() const -> parser_descriptor = 0;

    /// Parse one argument. The reader is placed at the argument start.
    [[nodiscard]] virtual auto parse(reader &in) const -> parse_result = 0;

    /// Offer completions for the partial argument.
    ///
    /// `remaining` is the input from the start of the argument up to the
    /// cursor, so a multi-token parser receives all tokens typed so far (the
    /// completions replace that whole range). Entries are added through
    /// `suggestion_builder`, which bounds the count and the text length and
    /// drops duplicates. The default implementation offers nothing.
    virtual void suggest(std::string_view, const source &,
                         suggestion_builder &) const {}
};

} // namespace refrain::command
