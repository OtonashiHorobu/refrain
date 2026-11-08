// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/command/argument.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace refrain::command {

/// String argument parser (`single_word`, `quotable_phrase` or
/// `greedy_phrase`).
class string_parser final : public argument_parser {
  public:
    explicit string_parser(string_mode mode, std::string suggestion_type = {});

    [[nodiscard]] auto descriptor() const -> parser_descriptor override;
    [[nodiscard]] auto parse(reader &in) const
        -> result<argument_value> override;
    void suggest(std::string_view prefix, const source &src,
                 suggestion_builder &out) const override;

  private:
    string_mode mode_;
    std::string suggestion_type_;
};

class bool_parser final : public argument_parser {
  public:
    bool_parser() = default;

    [[nodiscard]] auto descriptor() const -> parser_descriptor override;
    [[nodiscard]] auto parse(reader &in) const
        -> result<argument_value> override;
    void suggest(std::string_view prefix, const source &src,
                 suggestion_builder &out) const override;
};

/// Signed integer parser. `long_form` only changes the tree descriptor; both
/// forms accept the full 64-bit range and the same optional bounds.
class integer_parser final : public argument_parser {
  public:
    integer_parser(std::optional<std::int64_t> minimum,
                   std::optional<std::int64_t> maximum, bool long_form);

    [[nodiscard]] auto descriptor() const -> parser_descriptor override;
    [[nodiscard]] auto parse(reader &in) const
        -> result<argument_value> override;

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
    [[nodiscard]] auto parse(reader &in) const
        -> result<argument_value> override;

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
    [[nodiscard]] auto parse(reader &in) const
        -> result<argument_value> override;
    void suggest(std::string_view prefix, const source &src,
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
    [[nodiscard]] auto parse(reader &in) const
        -> result<argument_value> override;
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

/// Game mode names and the numeric aliases 0..3. The canonical value is the
/// lower-case name (`survival`, `creative`, `adventure`, `spectator`).
[[nodiscard]] auto gamemode_arg() -> enum_parser;

[[nodiscard]] auto resource_location_arg() -> resource_location_parser;

} // namespace refrain::command
