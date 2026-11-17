// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/command/argument.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::command {

/// String argument parser (`single_word`, `quotable_phrase` or
/// `greedy_phrase`).
class string_parser final : public argument_parser {
  public:
    explicit string_parser(string_mode mode, std::string suggestion_type = {});

    [[nodiscard]] auto descriptor() const -> parser_descriptor override;
    [[nodiscard]] auto parse(reader &in) const -> parse_result override;
    void suggest(std::string_view remaining, const source &src,
                 suggestion_builder &out) const override;

  private:
    string_mode mode_;
    std::string suggestion_type_;
};

class bool_parser final : public argument_parser {
  public:
    bool_parser() = default;

    [[nodiscard]] auto descriptor() const -> parser_descriptor override;
    [[nodiscard]] auto parse(reader &in) const -> parse_result override;
    void suggest(std::string_view remaining, const source &src,
                 suggestion_builder &out) const override;
};

/// Signed integer parser. `long_form` only changes the tree descriptor; both
/// forms accept the full 64-bit range and the same optional bounds.
class integer_parser final : public argument_parser {
  public:
    integer_parser(std::optional<std::int64_t> minimum,
                   std::optional<std::int64_t> maximum, bool long_form);

    [[nodiscard]] auto descriptor() const -> parser_descriptor override;
    [[nodiscard]] auto parse(reader &in) const -> parse_result override;
    void suggest(std::string_view remaining, const source &src,
                 suggestion_builder &out) const override;

  private:
    std::optional<std::int64_t> minimum_;
    std::optional<std::int64_t> maximum_;
    bool long_form_;
};

/// Floating-point parser. `float_form` selects the `brigadier:float` descriptor
/// and adds the 32-bit range check; both forms store the value as double.
class number_parser final : public argument_parser {
  public:
    number_parser(bool float_form, std::optional<double> minimum,
                  std::optional<double> maximum);

    [[nodiscard]] auto descriptor() const -> parser_descriptor override;
    [[nodiscard]] auto parse(reader &in) const -> parse_result override;
    void suggest(std::string_view remaining, const source &src,
                 suggestion_builder &out) const override;

  private:
    bool float_form_;
    std::optional<double> minimum_;
    std::optional<double> maximum_;
};

/// One allowed enum value. `name` is the canonical form stored in the parsed
/// value and shown in suggestions; `aliases` are accepted spellings that are
/// not suggested (for example numeric game mode ids).
struct enum_value {
    std::string name{};
    std::string tooltip{};
    std::vector<std::string> aliases{};
};

/// Fixed-vocabulary parser. Matching is ASCII case-insensitive, unlike literal
/// node names, and the parsed value is always the canonical `name`.
class enum_parser final : public argument_parser {
  public:
    explicit enum_parser(std::vector<enum_value> values,
                         std::string suggestion_type = {});

    [[nodiscard]] auto descriptor() const -> parser_descriptor override;
    [[nodiscard]] auto parse(reader &in) const -> parse_result override;
    void suggest(std::string_view remaining, const source &src,
                 suggestion_builder &out) const override;

    [[nodiscard]] auto values() const noexcept
        -> const std::vector<enum_value> & {
        return values_;
    }

  private:
    std::vector<enum_value> values_;
    std::string suggestion_type_;
};

class resource_location_parser final : public argument_parser {
  public:
    resource_location_parser() = default;

    [[nodiscard]] auto descriptor() const -> parser_descriptor override;
    [[nodiscard]] auto parse(reader &in) const -> parse_result override;
    void suggest(std::string_view remaining, const source &src,
                 suggestion_builder &out) const override;
};

/// Block position argument: three coordinates read as one argument.
///
/// `0 1 2` is absolute, `~ ~1 ~-1` is relative to the command source and
/// `^ ^ ^1` is a local offset from the source anchor. The three forms cannot be
/// mixed, which is the reference's rule. Completion offers the reference's
/// `~`/`^` coordinate templates and extends a partially typed position.
class block_position_parser final : public argument_parser {
  public:
    block_position_parser() = default;

    [[nodiscard]] auto descriptor() const -> parser_descriptor override;
    [[nodiscard]] auto parse(reader &in) const -> parse_result override;
    void suggest(std::string_view remaining, const source &src,
                 suggestion_builder &out) const override;
};

/// Position argument for entities: three coordinates read as one argument.
///
/// Unlike the block position form, an absolute component is a floating-point
/// number (the reference's `vec3`), so `0.5 64 -0.5` is accepted. The reference
/// centres an integer absolute X and Z by half a block because a block
/// position names the block the entity stands in, and leaves Y alone; this
/// parser applies the same rule. `~` is relative to the source and `^` is a
/// local left/up/forwards offset from the source anchor. Completion offers the
/// same `~`/`^` templates as the block position form.
class vec3_parser final : public argument_parser {
  public:
    vec3_parser() = default;

    [[nodiscard]] auto descriptor() const -> parser_descriptor override;
    [[nodiscard]] auto parse(reader &in) const -> parse_result override;
    void suggest(std::string_view remaining, const source &src,
                 suggestion_builder &out) const override;
};

/// Rotation argument: yaw and pitch, each absolute degrees or `~` relative.
class rotation_parser final : public argument_parser {
  public:
    rotation_parser() = default;

    [[nodiscard]] auto descriptor() const -> parser_descriptor override;
    [[nodiscard]] auto parse(reader &in) const -> parse_result override;
    void suggest(std::string_view remaining, const source &src,
                 suggestion_builder &out) const override;
};

/// Player-name argument. The value is the bare name token; resolving the name
/// against the player table happens in the executor, which owns that table.
/// Completion offers the source's online player names.
class player_name_parser final : public argument_parser {
  public:
    player_name_parser() = default;

    [[nodiscard]] auto descriptor() const -> parser_descriptor override;
    [[nodiscard]] auto parse(reader &in) const -> parse_result override;
    void suggest(std::string_view remaining, const source &src,
                 suggestion_builder &out) const override;
};

/// Game mode argument. Accepts the canonical names in any case and, for
/// compatibility with the existing server commands, the numeric ids `0`-`3`.
/// The parsed value is the typed `game_mode`.
class gamemode_parser final : public argument_parser {
  public:
    gamemode_parser() = default;

    [[nodiscard]] auto descriptor() const -> parser_descriptor override;
    [[nodiscard]] auto parse(reader &in) const -> parse_result override;
    void suggest(std::string_view remaining, const source &src,
                 suggestion_builder &out) const override;
};

/// A named time value accepted alongside a tick count. The server clock exposes
/// its day markers through this list so `/time set day` keeps working while the
/// wire argument stays the reference's `time` argument.
struct time_marker {
    std::string name{};
    std::int64_t ticks{0};
    std::string tooltip{};
};

/// Time argument: a tick count with the reference's unit suffixes (`d`, `s`,
/// `t`, or none), bounded below by `minimum_ticks`. Named markers are matched
/// before the numeric form.
class time_parser final : public argument_parser {
  public:
    explicit time_parser(std::int64_t minimum_ticks = 0,
                         std::vector<time_marker> markers = {});

    [[nodiscard]] auto descriptor() const -> parser_descriptor override;
    [[nodiscard]] auto parse(reader &in) const -> parse_result override;
    void suggest(std::string_view remaining, const source &src,
                 suggestion_builder &out) const override;

  private:
    std::int64_t minimum_ticks_;
    std::vector<time_marker> markers_;
};

/// Dimension argument: a dimension id. Completion offers the installed
/// dimension ids; resolving the id to a level happens in the executor.
class dimension_parser final : public argument_parser {
  public:
    explicit dimension_parser(std::vector<std::string> ids = default_ids());

    [[nodiscard]] static auto default_ids() -> std::vector<std::string>;

    [[nodiscard]] auto descriptor() const -> parser_descriptor override;
    [[nodiscard]] auto parse(reader &in) const -> parse_result override;
    void suggest(std::string_view remaining, const source &src,
                 suggestion_builder &out) const override;

  private:
    std::vector<std::string> ids_;
};

/// UUID argument: the canonical `8-4-4-4-12` hexadecimal form.
class uuid_parser final : public argument_parser {
  public:
    uuid_parser() = default;

    [[nodiscard]] auto descriptor() const -> parser_descriptor override;
    [[nodiscard]] auto parse(reader &in) const -> parse_result override;
};

[[nodiscard]] auto word_arg() -> string_parser;
[[nodiscard]] auto quoted_arg() -> string_parser;
[[nodiscard]] auto greedy_arg() -> string_parser;
[[nodiscard]] auto bool_arg() -> bool_parser;
[[nodiscard]] auto
integer_arg(std::optional<std::int64_t> minimum = std::nullopt,
            std::optional<std::int64_t> maximum = std::nullopt)
    -> integer_parser;
[[nodiscard]] auto long_arg(std::optional<std::int64_t> minimum = std::nullopt,
                            std::optional<std::int64_t> maximum = std::nullopt)
    -> integer_parser;
[[nodiscard]] auto float_arg(std::optional<double> minimum = std::nullopt,
                             std::optional<double> maximum = std::nullopt)
    -> number_parser;
[[nodiscard]] auto double_arg(std::optional<double> minimum = std::nullopt,
                              std::optional<double> maximum = std::nullopt)
    -> number_parser;
[[nodiscard]] auto enum_arg(std::vector<enum_value> values,
                            std::string suggestion_type = {}) -> enum_parser;

/// Game mode names and the numeric aliases `0`-`3`. The canonical value is the
/// typed `game_mode` (`survival`, `creative`, `adventure`, `spectator`).
[[nodiscard]] auto gamemode_arg() -> gamemode_parser;

[[nodiscard]] auto resource_location_arg() -> resource_location_parser;
[[nodiscard]] auto block_position_arg() -> block_position_parser;
[[nodiscard]] auto vec3_arg() -> vec3_parser;
[[nodiscard]] auto rotation_arg() -> rotation_parser;
[[nodiscard]] auto player_name_arg() -> player_name_parser;
[[nodiscard]] auto time_arg(std::int64_t minimum_ticks = 0,
                            std::vector<time_marker> markers = {})
    -> time_parser;
[[nodiscard]] auto dimension_arg() -> dimension_parser;
[[nodiscard]] auto dimension_arg(std::vector<std::string> ids)
    -> dimension_parser;
[[nodiscard]] auto uuid_arg() -> uuid_parser;

/// The world clock markers `/time set` accepts, in ticks since the clock start.
[[nodiscard]] auto world_time_markers() -> std::vector<time_marker>;

} // namespace refrain::command
