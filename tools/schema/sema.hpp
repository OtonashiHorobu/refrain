// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Semantic analysis for RSC/RIDL: resolves types, computes the deterministic
// C ABI layout of contract structs, validates service declarations and
// produces the model consumed by the emitters. The model is intentionally
// plain data (indices, not pointers) so generation stays deterministic.

#pragma once

#include <schema/ast.hpp>
#include <schema/diagnostic.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::schema {

enum class primitive_kind : std::uint8_t {
    u8,
    u16,
    u32,
    u64,
    i8,
    i16,
    i32,
    i64,
    f32,
    f64,
    boolean,
    string,
    bytes,
    void_type,
};

[[nodiscard]] auto primitive_name(primitive_kind value) noexcept
    -> std::string_view;

[[nodiscard]] auto primitive_from_name(std::string_view name) noexcept
    -> std::optional<primitive_kind>;

/// Resolved type expression.
struct resolved_type {
    enum class category : std::uint8_t {
        primitive,
        named, ///< enum/flags/struct declared in a module
        handle,
        array,
        slice,
        optional,
        repeated,
    };

    category kind{category::primitive};
    primitive_kind primitive{primitive_kind::u32};
    std::uint32_t module_index{0};
    std::uint32_t decl_index{0};
    std::vector<resolved_type> arguments;

    [[nodiscard]] auto is_void() const noexcept -> bool {
        return kind == category::primitive &&
               primitive == primitive_kind::void_type;
    }
};

struct field_model {
    std::string name;
    std::string documentation;
    std::vector<attribute> attributes;
    resolved_type type;
    bool out{false};
    std::optional<std::string> default_value;
    source_location location;

    // Computed for RIDL contract structs only.
    std::uint32_t abi_offset{0};
    std::uint32_t abi_size{0};
    std::uint32_t abi_alignment{0};
};

/// Tick boundary a `#[schedule]` method runs on (mirrors
/// `refrain::core::schedule::phase`; the emitter spells the core enum).
enum class schedule_phase_kind : std::uint8_t {
    pre_tick,
    post_tick,
};

struct method_model {
    std::string name;
    std::string documentation;
    std::vector<attribute> attributes;
    std::vector<field_model> parameters;
    bool can_fail{false};
    resolved_type result_type; ///< void for plain void methods
    resolved_type error_type;  ///< enum used by `result<T, E>`
    /// `#[schedule]` metadata. Only true when the attribute and the
    /// method signature passed validation.
    bool scheduled{false};
    std::int64_t schedule_interval_ticks{1};
    schedule_phase_kind schedule_phase_of{schedule_phase_kind::post_tick};
    source_location location;
};

struct decl_model {
    declaration_kind kind{declaration_kind::structure};
    std::string name;
    std::string documentation;
    /// Repository-relative source file path (used for RSC header placement).
    std::string source_path;
    std::vector<attribute> attributes;
    source_location location;

    // struct/record/packet
    std::vector<field_model> fields;
    std::optional<std::int64_t> packet_id;

    // enum/flags
    std::string underlying;
    std::vector<enum_entry> entries;

    // service
    std::string service_version;
    /// Parsed `service_version` (validated in sema; emitters consume these so
    /// the version parsing/normalization rules live in exactly one place).
    /// SemVer 2.0.0: the release triple is emitted as
    /// `_ABI_MAJOR/_ABI_MINOR/_ABI_PATCH`; prerelease versions are rejected by
    /// sema because the ABI constants have no place to encode them.
    std::uint32_t service_version_major{0};
    std::uint32_t service_version_minor{0};
    std::uint32_t service_version_patch{0};
    service_decl service;
    std::vector<method_model> methods;

    // ABI layout (RIDL structs; empty for RSC data types).
    std::uint32_t abi_size{0};
    std::uint32_t abi_alignment{0};

    // Event metadata (`declaration_kind::event` only).
    bool event_batch{false};
    bool event_cancellable{false};
};

struct module_model {
    std::string name;
    std::string version;
    std::vector<std::string> imports;
    dialect kind{dialect::rsc};
    std::vector<decl_model> declarations;
};

struct model {
    std::vector<module_model> modules; ///< sorted by module name
};

/// Dialect-aware semantic analysis. Returns nullopt and appends diagnostics on
/// any error. `units` are sorted by path before analysis for determinism.
[[nodiscard]] auto analyze(std::vector<source_unit> units, diagnostics &sink)
    -> std::optional<model>;

} // namespace refrain::schema
