// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Helpers shared by the RSC and RIDL emitters: deterministic names, include
// paths, header guards/banners and C/C++ primitive spellings.

#pragma once

#include <schema/ast.hpp>
#include <schema/sema.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::schema::emit {

/// Schema/emitter format version. It is mixed into the schema hash and printed
/// in the generated banner, so the banner is self-describing: an emitter change
/// that keeps the schema text identical still changes the hash/format line
/// (`schema gen --check` catches the byte difference).
inline constexpr std::string_view schema_format_version{"v1"};

/// Escape text so it can be embedded in a `//` comment or a quoted C/C++ string
/// without terminating the comment/string: newlines, carriage returns,
/// tabs, backslashes and quotes are escaped and remaining control characters
/// become three-digit octal escapes (`\001`). Octal is used instead of `\xNN`
/// because hexadecimal escapes consume as many digits as possible: `\x01f`
/// denotes 0x1F, while `\001f` is 0x01 followed by 'f'.
[[nodiscard]] inline auto escape_text(std::string_view text) -> std::string {
    std::string result;
    result.reserve(text.size());
    for (const char character : text) {
        switch (character) {
        case '\\':
            result += "\\\\";
            break;
        case '"':
            result += "\\\"";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default: {
            const auto byte{static_cast<unsigned char>(character)};
            if (byte < 0x20U || byte == 0x7FU) {
                result += std::format("\\{:03o}", byte);
            } else {
                result += character;
            }
            break;
        }
        }
    }
    return result;
}

struct generated_file {
    std::string path; ///< relative to the generated output root, '/'-separated
    std::string content;
};

/// C++ spelling for a declaration name. Declarations whose schema name is not
/// a C++ identifier (quoted packet names such as `debug/block_value`) map to a
/// sanitized identifier while the schema/name string keeps its original form.
[[nodiscard]] inline auto cpp_identifier(std::string_view name) -> std::string {
    std::string result;
    result.reserve(name.size() + 1);
    for (const char character : name) {
        const auto byte{static_cast<unsigned char>(character)};
        result.push_back(std::isalnum(byte) != 0 || character == '_' ? character
                                                                     : '_');
    }
    if (result.empty() ||
        std::isdigit(static_cast<unsigned char>(result.front())) != 0) {
        result.insert(result.begin(), '_');
    }
    return result;
}

[[nodiscard]] inline auto split_module(std::string_view module)
    -> std::vector<std::string> {
    std::vector<std::string> segments;
    std::size_t begin{0};
    while (begin <= module.size()) {
        const auto end{module.find("::", begin)};
        if (end == std::string_view::npos) {
            segments.emplace_back(module.substr(begin));
            break;
        }
        segments.emplace_back(module.substr(begin, end - begin));
        begin = end + 2;
    }
    return segments;
}

/// C++ namespace for a module, always rooted at `refrain`.
[[nodiscard]] inline auto cpp_namespace(std::string_view module)
    -> std::string {
    const auto segments{split_module(module)};
    std::string result{"refrain"};
    std::size_t index{0};
    if (!segments.empty() && segments.front() == "refrain") {
        index = 1;
    }
    for (; index < segments.size(); ++index) {
        result += "::";
        result += segments[index];
    }
    return result;
}

/// C ABI symbol prefix: all module segments joined with '_'.
[[nodiscard]] inline auto abi_prefix(std::string_view module) -> std::string {
    const auto segments{split_module(module)};
    std::string result;
    for (const auto &segment : segments) {
        if (!result.empty()) {
            result += '_';
        }
        result += segment;
    }
    return result;
}

/// Module path relative to the `refrain` root, used for generated file names.
[[nodiscard]] inline auto module_rel_path(std::string_view module)
    -> std::string {
    const auto segments{split_module(module)};
    std::string result;
    std::size_t index{0};
    if (!segments.empty() && segments.front() == "refrain") {
        index = 1;
    }
    for (; index < segments.size(); ++index) {
        if (!result.empty()) {
            result += '/';
        }
        result += segments[index];
    }
    return result;
}

[[nodiscard]] inline auto c_abi_header_path(std::string_view module)
    -> std::string {
    return "c/refrain/contracts/" + module_rel_path(module) + "_abi.h";
}

[[nodiscard]] inline auto cpp_header_path(std::string_view module)
    -> std::string {
    return "cpp/refrain/contracts/" + module_rel_path(module) + ".hpp";
}

[[nodiscard]] inline auto c_abi_include(std::string_view module)
    -> std::string {
    return "<refrain/contracts/" + module_rel_path(module) + "_abi.h>";
}

[[nodiscard]] inline auto cpp_include(std::string_view module) -> std::string {
    return "<refrain/contracts/" + module_rel_path(module) + ".hpp>";
}

/// `schema/<path>.rsc` -> `cpp/refrain/schema/<path>.hpp` (RSC data model).
[[nodiscard]] inline auto rsc_cpp_header_path(std::string_view source_path)
    -> std::string {
    auto path{std::string{source_path}};
    constexpr std::string_view prefix{"schema/"};
    if (path.starts_with(prefix)) {
        path.erase(0, prefix.size());
    }
    constexpr std::string_view extension{".rsc"};
    if (path.ends_with(extension)) {
        path.erase(path.size() - extension.size());
    }
    return "cpp/refrain/schema/" + path + ".hpp";
}

[[nodiscard]] inline auto rsc_cpp_include(std::string_view source_path)
    -> std::string {
    const auto path{rsc_cpp_header_path(source_path)};
    return "<" + path.substr(std::string_view{"cpp/"}.size()) + ">";
}

/// Aggregate C++ header over every RIDL module (`contracts.hpp`).
[[nodiscard]] inline auto cpp_aggregate_header_path() -> std::string {
    return "cpp/refrain/contracts.hpp";
}

[[nodiscard]] inline auto cpp_aggregate_include() -> std::string {
    return "<refrain/contracts.hpp>";
}

/// Generated `custom_traits` index for the whole C++ output set (RSC + RIDL).
/// Only this one file may include `<refrain/reflect.hpp>`; the per-module
/// headers stay free of reflection macros/includes.
[[nodiscard]] inline auto cpp_reflection_header_path() -> std::string {
    return "cpp/refrain/reflection.hpp";
}

[[nodiscard]] inline auto upper(std::string_view text) -> std::string {
    std::string result;
    result.reserve(text.size());
    for (const auto character : text) {
        const auto is_lower{character >= 'a' && character <= 'z'};
        result +=
            is_lower ? static_cast<char>(character - 'a' + 'A') : character;
    }
    return result;
}

/// Include guard derived from the generated file path.
[[nodiscard]] inline auto guard_for(std::string_view path) -> std::string {
    std::string result{"REFRAIN_GENERATED_"};
    for (const auto character : path) {
        const auto is_alnum{(character >= 'a' && character <= 'z') ||
                            (character >= 'A' && character <= 'Z') ||
                            (character >= '0' && character <= '9')};
        result += is_alnum ? upper(std::string_view{&character, 1}) : "_";
    }
    return result;
}

[[nodiscard]] inline auto banner(std::string_view schema_hash) -> std::string {
    return std::format(
        "// SPDX-License-Identifier: MIT\n"
        "//\n"
        "// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu\n"
        "//\n"
        "// GENERATED FILE - DO NOT EDIT.\n"
        "// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).\n"
        "// Regenerate with: refrain-tools schema gen\n"
        "// Schema format: {}\n"
        "// Schema hash: {}\n\n",
        schema_format_version, schema_hash);
}

/// `///` doc block for C++ output; empty input yields no lines. A line ending
/// in a backslash gets one trailing space: translation phase 2 splices a
/// backslash-newline pair even inside a `//` comment, so without the space the
/// comment would swallow the declaration on the next line.
[[nodiscard]] inline auto doc_block(std::string_view documentation,
                                    std::string_view prefix) -> std::string {
    if (documentation.empty()) {
        return {};
    }
    std::string result;
    std::size_t begin{0};
    while (begin <= documentation.size()) {
        const auto end{documentation.find('\n', begin)};
        const auto line{end == std::string_view::npos
                            ? documentation.substr(begin)
                            : documentation.substr(begin, end - begin)};
        result += prefix;
        result += line;
        if (!line.empty() && line.back() == '\\') {
            result += ' ';
        }
        result += '\n';
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return result;
}

[[nodiscard]] inline auto attribute_text(const attribute &value)
    -> std::string {
    std::string text{"#["};
    text += value.name;
    if (!value.arguments.empty()) {
        text += '(';
        for (std::size_t i = 0; i < value.arguments.size(); ++i) {
            if (i != 0) {
                text += ", ";
            }
            const auto &argument{value.arguments[i]};
            if (argument.name.has_value()) {
                text += *argument.name;
                text += '=';
            }
            if (argument.kind == attribute_argument_kind::string) {
                text += '"';
                text += escape_text(argument.text);
                text += '"';
            } else {
                text += escape_text(argument.text);
            }
        }
        text += ')';
    }
    text += ']';
    return text;
}

[[nodiscard]] inline auto
attribute_comment(const std::vector<attribute> &attributes,
                  std::string_view indent = "") -> std::string {
    if (attributes.empty()) {
        return {};
    }
    std::string text{indent};
    text += "// attributes:";
    for (const auto &value : attributes) {
        text += ' ';
        text += attribute_text(value);
    }
    text += '\n';
    return text;
}

[[nodiscard]] inline auto c_primitive_name(primitive_kind kind)
    -> std::string_view {
    switch (kind) {
    case primitive_kind::u8:
        return "uint8_t";
    case primitive_kind::u16:
        return "uint16_t";
    case primitive_kind::u32:
        return "uint32_t";
    case primitive_kind::u64:
        return "uint64_t";
    case primitive_kind::i8:
        return "int8_t";
    case primitive_kind::i16:
        return "int16_t";
    case primitive_kind::i32:
        return "int32_t";
    case primitive_kind::i64:
        return "int64_t";
    case primitive_kind::f32:
        return "float";
    case primitive_kind::f64:
        return "double";
    case primitive_kind::boolean:
        return "uint8_t";
    case primitive_kind::string:
        return "refrain_str";
    case primitive_kind::bytes:
        return "refrain_bytes";
    case primitive_kind::identifier:
    case primitive_kind::uuid:
    case primitive_kind::text_component:
    case primitive_kind::item_stack:
    case primitive_kind::lp_vec3:
    case primitive_kind::nbt:
    case primitive_kind::varint_id:
    case primitive_kind::message_signature_packed:
    case primitive_kind::filter_mask:
    case primitive_kind::stat_entry:
    case primitive_kind::position_path:
    case primitive_kind::map_patch:
    case primitive_kind::advancement_seen:
    case primitive_kind::vec_delta:
    case primitive_kind::sound_selector:
    case primitive_kind::objective_update:
    case primitive_kind::team_update:
    case primitive_kind::player_info_update:
    case primitive_kind::tracked_waypoint:
    case primitive_kind::component_predicate:
    case primitive_kind::merchant_offer:
    case primitive_kind::entity_metadata:
    case primitive_kind::item_stack_template:
    case primitive_kind::advancement_display:
    case primitive_kind::advancement_requirements:
    case primitive_kind::advancement:
    case primitive_kind::advancement_progress:
    case primitive_kind::holder_set:
    case primitive_kind::slot_display:
    case primitive_kind::recipe_display:
    case primitive_kind::recipe_display_entry:
    case primitive_kind::single_input_set:
    case primitive_kind::particle_options:
    case primitive_kind::debug_subscription_update:
    case primitive_kind::debug_subscription_event:
    case primitive_kind::command_tree:
    case primitive_kind::container_click_data:
        std::unreachable(); // wire-only primitives are rejected in RIDL
    case primitive_kind::void_type:
        return "void";
    }
    std::unreachable();
}

[[nodiscard]] inline auto cpp_primitive_name(primitive_kind kind)
    -> std::string_view {
    switch (kind) {
    case primitive_kind::u8:
        return "std::uint8_t";
    case primitive_kind::u16:
        return "std::uint16_t";
    case primitive_kind::u32:
        return "std::uint32_t";
    case primitive_kind::u64:
        return "std::uint64_t";
    case primitive_kind::i8:
        return "std::int8_t";
    case primitive_kind::i16:
        return "std::int16_t";
    case primitive_kind::i32:
        return "std::int32_t";
    case primitive_kind::i64:
        return "std::int64_t";
    case primitive_kind::f32:
        return "float";
    case primitive_kind::f64:
        return "double";
    case primitive_kind::boolean:
        return "bool";
    case primitive_kind::string:
        return "std::string_view";
    case primitive_kind::bytes:
        return "std::span<const std::uint8_t>";
    case primitive_kind::identifier:
    case primitive_kind::uuid:
    case primitive_kind::text_component:
    case primitive_kind::item_stack:
    case primitive_kind::lp_vec3:
    case primitive_kind::nbt:
    case primitive_kind::varint_id:
    case primitive_kind::message_signature_packed:
    case primitive_kind::filter_mask:
    case primitive_kind::stat_entry:
    case primitive_kind::position_path:
    case primitive_kind::map_patch:
    case primitive_kind::advancement_seen:
    case primitive_kind::vec_delta:
    case primitive_kind::sound_selector:
    case primitive_kind::objective_update:
    case primitive_kind::team_update:
    case primitive_kind::player_info_update:
    case primitive_kind::tracked_waypoint:
    case primitive_kind::component_predicate:
    case primitive_kind::merchant_offer:
    case primitive_kind::entity_metadata:
    case primitive_kind::item_stack_template:
    case primitive_kind::advancement_display:
    case primitive_kind::advancement_requirements:
    case primitive_kind::advancement:
    case primitive_kind::advancement_progress:
    case primitive_kind::holder_set:
    case primitive_kind::slot_display:
    case primitive_kind::recipe_display:
    case primitive_kind::recipe_display_entry:
    case primitive_kind::single_input_set:
    case primitive_kind::particle_options:
    case primitive_kind::debug_subscription_update:
    case primitive_kind::debug_subscription_event:
    case primitive_kind::command_tree:
    case primitive_kind::container_click_data:
        std::unreachable(); // wire-only primitives are rejected in RIDL
    case primitive_kind::void_type:
        return "void";
    }
    std::unreachable();
}

/// Underlying C++ integer type of an enum/flags declaration.
[[nodiscard]] inline auto cpp_underlying_name(const decl_model &decl)
    -> std::string_view {
    const auto primitive{primitive_from_name(decl.underlying)};
    if (!primitive.has_value()) {
        std::unreachable(); // sema rejects non-integer enum underlying types
    }
    return cpp_primitive_name(*primitive);
}

[[nodiscard]] inline auto c_underlying_name(const decl_model &decl)
    -> std::string_view {
    const auto primitive{primitive_from_name(decl.underlying)};
    if (!primitive.has_value()) {
        std::unreachable(); // sema rejects non-integer enum underlying types
    }
    return c_primitive_name(*primitive);
}

} // namespace refrain::schema::emit
