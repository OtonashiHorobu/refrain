// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/permissions/persist.hpp>

#include <refrain/core/config.hpp>
#include <refrain/permissions/policy.hpp>

#include <fmt/format.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::permissions {

namespace {

namespace config = core::config;

constexpr std::size_t no_position{std::string_view::npos};

[[nodiscard]] auto is_horizontal(char character) noexcept -> bool {
    return character == ' ' || character == '\t';
}

[[nodiscard]] auto is_bare_key_character(char character) noexcept -> bool {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '_' ||
           character == '-';
}

void skip_comment(std::string_view text, std::size_t &index,
                  std::size_t end) noexcept {
    if (index >= end || text[index] != '#') {
        return;
    }
    while (index < end && text[index] != '\n') {
        ++index;
    }
}

void skip_line(std::string_view text, std::size_t &index,
               std::size_t end) noexcept {
    while (index < end && text[index] != '\n') {
        ++index;
    }
}

/// Position just after the closing quote of the basic string that starts at
/// `begin`, or `no_position` when an unterminated string reaches `end`.
[[nodiscard]] auto basic_string_end(std::string_view text, std::size_t begin,
                                    std::size_t end) noexcept -> std::size_t {
    std::size_t index{begin + 1};
    while (index < end) {
        if (text[index] == '\\') {
            index += 2;
            continue;
        }
        if (text[index] == '"') {
            return index + 1;
        }
        ++index;
    }
    return no_position;
}

/// Position just after the closing quote of the literal string that starts at
/// `begin`, or `no_position` when it never closes.
[[nodiscard]] auto literal_string_end(std::string_view text, std::size_t begin,
                                      std::size_t end) noexcept -> std::size_t {
    for (std::size_t index{begin + 1}; index < end; ++index) {
        if (text[index] == '\'') {
            return index + 1;
        }
    }
    return no_position;
}

/// Decode a TOML basic string body. The escape set the editor needs is the
/// printable subset: a rule never contains a quote, a backslash or a control
/// character, so an unknown escape is kept verbatim and simply does not match.
[[nodiscard]] auto decode_basic_string(std::string_view body) -> std::string {
    std::string out{};
    out.reserve(body.size());
    for (std::size_t index{0}; index < body.size(); ++index) {
        const auto character{body[index]};
        if (character != '\\' || index + 1 == body.size()) {
            out.push_back(character);
            continue;
        }
        const auto escaped{body[++index]};
        switch (escaped) {
        case 'n':
            out.push_back('\n');
            break;
        case 't':
            out.push_back('\t');
            break;
        case 'r':
            out.push_back('\r');
            break;
        case 'b':
            out.push_back('\b');
            break;
        case 'f':
            out.push_back('\f');
            break;
        default:
            out.push_back(escaped);
            break;
        }
    }
    return out;
}

/// Format one rule or group reference as a TOML basic string. The grammar of a
/// rule and a group name has no quote or backslash, but the editor escapes both
/// so a malformed candidate can never break the document.
[[nodiscard]] auto quote(std::string_view text) -> std::string {
    std::string out{"\""};
    for (const auto character : text) {
        if (character == '"' || character == '\\') {
            out.push_back('\\');
        }
        out.push_back(character);
    }
    out.push_back('"');
    return out;
}

/// One table header of the document with the byte range of its body.
struct section_range {
    std::string name{};
    std::size_t body_begin{0};
    std::size_t body_end{0};
};

/// Decode a table name between the brackets: bare, basic or literal.
[[nodiscard]] auto decode_table_name(std::string_view text) -> std::string {
    while (!text.empty() && is_horizontal(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_horizontal(text.back())) {
        text.remove_suffix(1);
    }
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        return decode_basic_string(text.substr(1, text.size() - 2));
    }
    if (text.size() >= 2 && text.front() == '\'' && text.back() == '\'') {
        return std::string{text.substr(1, text.size() - 2)};
    }
    return std::string{text};
}

/// Every `[table]` header of the document with its body range.
[[nodiscard]] auto scan_sections(std::string_view text)
    -> std::vector<section_range> {
    std::vector<section_range> sections{};
    const auto size{text.size()};
    std::size_t index{0};
    std::size_t line_start{0};
    bool at_line_start{true};
    bool in_basic{false};
    bool in_literal{false};
    bool in_comment{false};
    std::size_t depth{0};
    std::size_t current{no_position};
    while (index < size) {
        const auto character{text[index]};
        if (in_comment) {
            if (character == '\n') {
                in_comment = false;
                at_line_start = true;
                line_start = index + 1;
            }
            ++index;
            continue;
        }
        if (in_basic) {
            if (character == '\\') {
                index += index + 1 < size ? 2 : 1;
                continue;
            }
            if (character == '"') {
                in_basic = false;
            }
            ++index;
            continue;
        }
        if (in_literal) {
            if (character == '\'') {
                in_literal = false;
            }
            ++index;
            continue;
        }
        if (character == '#') {
            in_comment = true;
            ++index;
            continue;
        }
        if (character == '"') {
            in_basic = true;
            ++index;
            continue;
        }
        if (character == '\'') {
            in_literal = true;
            ++index;
            continue;
        }
        if (at_line_start) {
            if (is_horizontal(character)) {
                ++index;
                continue;
            }
            if (character == '\n') {
                ++index;
                line_start = index;
                continue;
            }
            at_line_start = false;
            if (depth == 0 && character == '[') {
                std::size_t cursor{index + 1};
                bool header_basic{false};
                bool header_literal{false};
                while (cursor < size) {
                    const auto inner{text[cursor]};
                    if (header_basic) {
                        if (inner == '\\') {
                            cursor += cursor + 1 < size ? 2 : 1;
                            continue;
                        }
                        if (inner == '"') {
                            header_basic = false;
                        }
                        ++cursor;
                        continue;
                    }
                    if (header_literal) {
                        if (inner == '\'') {
                            header_literal = false;
                        }
                        ++cursor;
                        continue;
                    }
                    if (inner == '"') {
                        header_basic = true;
                        ++cursor;
                        continue;
                    }
                    if (inner == '\'') {
                        header_literal = true;
                        ++cursor;
                        continue;
                    }
                    if (inner == ']') {
                        break;
                    }
                    ++cursor;
                }
                if (cursor < size) {
                    auto name{decode_table_name(
                        text.substr(index + 1, cursor - index - 1))};
                    if (current != no_position) {
                        sections[current].body_end = line_start;
                    }
                    std::size_t body{cursor + 1};
                    while (body < size && text[body] != '\n') {
                        ++body;
                    }
                    if (body < size) {
                        ++body;
                    }
                    sections.push_back(section_range{.name = std::move(name),
                                                     .body_begin = body,
                                                     .body_end = size});
                    current = sections.size() - 1;
                    index = body;
                    line_start = body;
                    at_line_start = true;
                    continue;
                }
            }
        }
        if (character == '[') {
            ++depth;
        } else if (character == ']') {
            if (depth > 0) {
                --depth;
            }
        } else if (character == '\n') {
            at_line_start = true;
            line_start = index + 1;
        }
        ++index;
    }
    return sections;
}

/// One `key = [...]` assignment of the `[grants]` table.
struct grants_entry {
    std::size_t array_begin{0};
    std::size_t array_end{0};
    std::size_t key_begin{0};
    std::size_t key_end{0};
};

/// Position of the `]` that closes the array that starts at `open`, or
/// `no_position` when it never closes.
[[nodiscard]] auto array_end(std::string_view text, std::size_t open,
                             std::size_t end) noexcept -> std::size_t {
    std::size_t depth{0};
    for (std::size_t index{open}; index < end; ++index) {
        const auto character{text[index]};
        if (character == '#') {
            while (index < end && text[index] != '\n') {
                ++index;
            }
            continue;
        }
        if (character == '"') {
            const auto after{basic_string_end(text, index, end)};
            if (after == no_position) {
                return no_position;
            }
            index = after - 1;
            continue;
        }
        if (character == '\'') {
            const auto after{literal_string_end(text, index, end)};
            if (after == no_position) {
                return no_position;
            }
            index = after - 1;
            continue;
        }
        if (character == '[') {
            ++depth;
            continue;
        }
        if (character == ']') {
            --depth;
            if (depth == 0) {
                return index;
            }
        }
    }
    return no_position;
}

/// Read the key that starts at `begin` and return the position after it.
[[nodiscard]] auto read_key(std::string_view text, std::size_t begin,
                            std::size_t end, std::string &key) -> std::size_t {
    if (begin >= end) {
        return no_position;
    }
    if (text[begin] == '"') {
        const auto after{basic_string_end(text, begin, end)};
        if (after == no_position) {
            return no_position;
        }
        key = decode_basic_string(text.substr(begin + 1, after - begin - 2));
        return after;
    }
    if (text[begin] == '\'') {
        const auto after{literal_string_end(text, begin, end)};
        if (after == no_position) {
            return no_position;
        }
        key = std::string{text.substr(begin + 1, after - begin - 2)};
        return after;
    }
    std::size_t cursor{begin};
    while (cursor < end && is_bare_key_character(text[cursor])) {
        ++cursor;
    }
    if (cursor == begin) {
        return no_position;
    }
    key = std::string{text.substr(begin, cursor - begin)};
    return cursor;
}

/// Find the `subject = [...]` assignment of the `[grants]` table body.
[[nodiscard]] auto find_entry(std::string_view text, std::size_t body_begin,
                              std::size_t body_end, std::string_view subject)
    -> grants_entry {
    std::size_t index{body_begin};
    while (index < body_end) {
        while (index < body_end && is_horizontal(text[index])) {
            ++index;
        }
        if (index >= body_end) {
            break;
        }
        if (text[index] == '\n') {
            ++index;
            continue;
        }
        if (text[index] == '\r') {
            ++index;
            continue;
        }
        if (text[index] == '#') {
            skip_comment(text, index, body_end);
            continue;
        }
        const auto key_begin{index};
        std::string key{};
        const auto after_key{read_key(text, index, body_end, key)};
        if (after_key == no_position) {
            skip_line(text, index, body_end);
            continue;
        }
        index = after_key;
        while (index < body_end && is_horizontal(text[index])) {
            ++index;
        }
        if (index >= body_end || text[index] != '=') {
            skip_line(text, index, body_end);
            continue;
        }
        ++index;
        while (index < body_end && is_horizontal(text[index])) {
            ++index;
        }
        if (index >= body_end || text[index] != '[') {
            skip_line(text, index, body_end);
            continue;
        }
        const auto close{array_end(text, index, body_end)};
        if (close == no_position) {
            return grants_entry{};
        }
        if (key == subject) {
            return grants_entry{.array_begin = index,
                                .array_end = close,
                                .key_begin = key_begin,
                                .key_end = after_key};
        }
        index = close + 1;
    }
    return grants_entry{};
}

/// One token of an array: an element string or the comma separating elements.
struct array_token {
    enum class kind : std::uint8_t { element, comma };

    kind type{kind::comma};
    std::size_t begin{0};
    std::size_t end{0};
    std::string value{};
};

[[nodiscard]] auto parse_array_tokens(std::string_view text, std::size_t open,
                                      std::size_t close)
    -> std::vector<array_token> {
    std::vector<array_token> tokens{};
    std::size_t index{open + 1};
    while (index < close) {
        const auto character{text[index]};
        if (is_horizontal(character) || character == '\n' ||
            character == '\r') {
            ++index;
            continue;
        }
        if (character == '#') {
            skip_comment(text, index, close);
            continue;
        }
        if (character == ',') {
            tokens.push_back(array_token{.type = array_token::kind::comma,
                                         .begin = index,
                                         .end = index + 1});
            ++index;
            continue;
        }
        if (character == '"') {
            const auto after{basic_string_end(text, index, close)};
            if (after == no_position) {
                break;
            }
            tokens.push_back(array_token{
                .type = array_token::kind::element,
                .begin = index,
                .end = after,
                .value = decode_basic_string(
                    text.substr(index + 1, after - index - 2)),
            });
            index = after;
            continue;
        }
        if (character == '\'') {
            const auto after{literal_string_end(text, index, close)};
            if (after == no_position) {
                break;
            }
            tokens.push_back(array_token{
                .type = array_token::kind::element,
                .begin = index,
                .end = after,
                .value = std::string{text.substr(index + 1, after - index - 2)},
            });
            index = after;
            continue;
        }
        ++index;
    }
    return tokens;
}

/// Leading indentation of the line that contains `position`, or four spaces
/// when the line starts with something other than whitespace.
[[nodiscard]] auto indentation_of(std::string_view text, std::size_t position)
    -> std::string {
    std::size_t line_begin{position};
    while (line_begin > 0 && text[line_begin - 1] != '\n') {
        --line_begin;
    }
    std::size_t cursor{line_begin};
    while (cursor < position && is_horizontal(text[cursor])) {
        ++cursor;
    }
    if (cursor == position) {
        return std::string{text.substr(line_begin, position - line_begin)};
    }
    return "    ";
}

/// Splice one element into an array, or remove one from it.
[[nodiscard]] auto edit_array(std::string_view text, std::size_t open,
                              std::size_t close, std::string_view rule,
                              bool add) -> std::string {
    auto tokens{parse_array_tokens(text, open, close)};
    std::vector<std::size_t> elements{};
    for (std::size_t index{0}; index < tokens.size(); ++index) {
        if (tokens[index].type == array_token::kind::element) {
            elements.push_back(index);
        }
    }
    const auto has_rule = [&tokens, &elements, rule] {
        for (const auto index : elements) {
            if (tokens[index].value == rule) {
                return true;
            }
        }
        return false;
    };
    if (add && has_rule()) {
        return std::string{text};
    }
    if (add) {
        const auto quoted{quote(rule)};
        const auto multiline{text.substr(open, close - open).find('\n') !=
                             std::string_view::npos};
        if (elements.empty()) {
            return fmt::format("{}{}{}", text.substr(0, open + 1), quoted,
                               text.substr(open + 1));
        }
        const auto &last{tokens[elements.back()]};
        const auto addition{
            multiline ? fmt::format(",\n{}{}", indentation_of(text, last.begin),
                                    quoted)
                      : fmt::format(", {}", quoted)};
        return fmt::format("{}{}{}", text.substr(0, last.end), addition,
                           text.substr(last.end));
    }
    std::size_t target{no_position};
    for (const auto index : elements) {
        if (tokens[index].value == rule) {
            target = index;
            break;
        }
    }
    if (target == no_position) {
        return std::string{text};
    }
    const auto element_begin{tokens[target].begin};
    const auto element_end{tokens[target].end};
    std::size_t erase_begin{element_begin};
    std::size_t erase_end{element_end};
    if (target + 1 < tokens.size() &&
        tokens[target + 1].type == array_token::kind::comma) {
        erase_end = tokens[target + 1].end;
    } else if (target > 0 &&
               tokens[target - 1].type == array_token::kind::comma) {
        erase_begin = tokens[target - 1].begin;
    }
    return fmt::format("{}{}", text.substr(0, erase_begin),
                       text.substr(erase_end));
}

/// True when `subject` holds the rule `rule` in `policy`.
[[nodiscard]] auto holds_rule(const policy &policy_value,
                              std::string_view subject, const node_rule &rule)
    -> bool {
    const auto found{policy_value.subjects.find(subject)};
    if (found == policy_value.subjects.end()) {
        return false;
    }
    for (const auto &entry : found->second.nodes.rules()) {
        if (entry.node == rule.node && entry.deny == rule.deny) {
            return true;
        }
    }
    return false;
}

} // namespace

auto edit_grants(std::string_view text, std::string_view subject,
                 std::string_view rule_text_value, bool add)
    -> result<std::string> {
    auto rule{parse_rule(rule_text_value, "permission rule")};
    if (!rule) {
        return std::unexpected{std::move(rule.error())};
    }
    if (!valid_subject_key(subject)) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            fmt::format("'{}' is not a configurable permission subject",
                        subject))};
    }
    const auto canonical_rule{rule_text(*rule)};
    const auto sections{scan_sections(text)};
    const section_range *grants{nullptr};
    for (const auto &section : sections) {
        if (section.name == "grants") {
            grants = &section;
            break;
        }
    }

    std::string edited{};
    if (grants == nullptr) {
        if (!add) {
            return std::string{text};
        }
        auto base{std::string{text}};
        if (!base.empty() && base.back() != '\n') {
            base.push_back('\n');
        }
        edited = fmt::format("{}\n[grants]\n{} = [{}]\n", base, quote(subject),
                             quote(canonical_rule));
    } else {
        const auto entry{
            find_entry(text, grants->body_begin, grants->body_end, subject)};
        if (entry.array_begin != 0) {
            auto array{edit_array(text, entry.array_begin, entry.array_end,
                                  canonical_rule, add)};
            edited = std::move(array);
        } else if (!add) {
            return std::string{text};
        } else {
            const auto insertion{fmt::format("{} = [{}]\n", quote(subject),
                                             quote(canonical_rule))};
            const auto body_end{grants->body_end};
            const auto needs_newline{body_end > 0 &&
                                     text[body_end - 1] != '\n'};
            edited = fmt::format("{}{}{}{}", text.substr(0, body_end),
                                 needs_newline ? "\n" : "", insertion,
                                 text.substr(body_end));
        }
    }

    if (edited == text) {
        return edited;
    }
    auto document{config::document::parse_text(edited, "permissions.toml")};
    if (!document) {
        return std::unexpected{make_error(
            error_code::parse_error,
            "the edited permission configuration is not valid TOML: " +
                document.error().message)};
    }
    auto parsed{parse_policy(*document)};
    if (!parsed) {
        return std::unexpected{make_error(
            parsed.error().code,
            "the edited permission configuration is not a valid policy: " +
                parsed.error().message)};
    }
    if (holds_rule(*parsed, subject, *rule) != add) {
        return std::unexpected{make_error(
            error_code::unknown,
            "the edited permission configuration does not contain the "
            "requested change")};
    }
    return edited;
}

} // namespace refrain::permissions
