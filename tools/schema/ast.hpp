// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// AST produced by the hand-written parser. RSC and RIDL share one node model;
// the dialect selects which declarations/attributes are legal during semantic
// analysis.

#pragma once

#include <schema/diagnostic.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace refrain::schema {

/// Maximum nesting depth of a type expression (`array<array<...>>`). The
/// parser/sema are recursive descent, so a hostile or fat-fingered schema could
/// otherwise overflow the stack. 64 is far above any real contract type.
inline constexpr std::uint32_t max_type_depth = 64;

enum class dialect : std::uint8_t { rsc, ridl };

[[nodiscard]] auto dialect_name(dialect value) noexcept -> const char *;

enum class attribute_argument_kind : std::uint8_t {
    integer,
    floating,
    string,
    identifier,
};

struct attribute_argument {
    attribute_argument_kind kind{attribute_argument_kind::identifier};
    /// Argument name for the `name = value` spelling used by RIDL attributes
    /// such as `#[schedule(interval=1, phase=pre_tick)]`. Empty for positional
    /// arguments (the only spelling RSC attributes accept).
    std::optional<std::string> name;
    std::string text;
};

struct attribute {
    std::string name;
    std::vector<attribute_argument> arguments;
    source_location location;
};

/// Type expression: `u32`, `string`, `handle<entity>`, `array<chunk_section>`,
/// `result<generate_outcome, worldgen_error>`.
struct type_ref {
    std::string name;
    std::vector<type_ref> arguments;
    source_location location;
};

struct field_decl {
    std::vector<attribute> attributes;
    type_ref type;
    std::string name;
    bool out{false}; ///< RIDL method parameter direction
    std::optional<std::string> default_value;
    std::string documentation;
    source_location location;
};

struct enum_entry {
    std::string name;
    std::optional<std::int64_t> value;
    std::string documentation;
    source_location location;
};

struct method_decl {
    std::vector<attribute> attributes;
    std::string name;
    std::vector<field_decl> parameters;
    bool has_return{false};
    type_ref return_type;
    std::string documentation;
    source_location location;
};

struct service_decl {
    /// `service <name> : <semver>`; a full SemVer 2.0.0 triple (`1.0.0`) is
    /// required by sema. The default is the current ABI baseline.
    std::string version{"1.0.0"};
    std::string side{"both"};
    std::string thread{"any"};
    /// Contract composition mode: exclusive | instanced | chain | registry |
    /// sealed.
    /// Defaults to `exclusive` (one active provider per service).
    std::string composition{"exclusive"};
    std::vector<std::string> provides;
    std::vector<std::string> requires_list;
    std::vector<std::string> capabilities;
    std::vector<std::string> emits;
    std::vector<std::string> consumes;
};

enum class declaration_kind : std::uint8_t {
    structure,
    enumeration,
    flags,
    packet,
    record,
    handle,
    service,
    /// C function pointer type usable as a service method parameter (RIDL
    /// only). Callbacks are not value types: they cannot appear in structs or
    /// as return types.
    callback,
    /// In-process, transient notification (RIDL only). Events are not
    /// C ABI structs: they carry no `struct_size` and never cross the wire.
    event,
};

[[nodiscard]] auto declaration_kind_name(declaration_kind kind) noexcept
    -> const char *;

struct declaration {
    declaration_kind kind{declaration_kind::structure};
    std::string name;
    std::string documentation;
    std::vector<attribute> attributes;
    source_location location;

    // struct/record/packet
    std::vector<field_decl> fields;
    std::optional<std::int64_t> packet_id;

    // enum/flags
    std::string underlying;
    std::vector<enum_entry> entries;

    // service
    service_decl service;
    std::vector<method_decl> methods;

    // callback
    std::vector<field_decl> callback_parameters;
    type_ref callback_return;
    bool callback_has_return{false};
};

/// One parsed `.rsc`/`.ridl` file.
struct source_unit {
    dialect kind{dialect::rsc};
    std::string path; ///< repo-relative, '/'-separated, used for diagnostics
    std::string module;
    std::string version;
    std::vector<std::string> imports;
    std::vector<declaration> declarations;
};

} // namespace refrain::schema
