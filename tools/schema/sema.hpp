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
    /// Protocol resource location (`namespace:path`); RSC wire schemas only.
    identifier,
    /// Protocol 128-bit identifier in wire order; RSC wire schemas only.
    uuid,
    /// Network NBT text component; RSC wire schemas only.
    text_component,
    /// Item stack; RSC wire schemas only. The current codec encodes empty
    /// stacks and rejects non-empty payloads.
    item_stack,
    /// Quantized low-precision vector; RSC wire schemas only.
    lp_vec3,
    /// Network NBT tag; RSC wire schemas only.
    nbt,
    /// Registry/identifier number encoded as a VarInt; RSC wire schemas only.
    varint_id,
    /// Chat signature reference or full 256-byte signature; RSC wire only.
    message_signature_packed,
    /// Chat filter mask: a type id plus an optional byte mask; RSC wire only.
    filter_mask,
    /// Statistic identity: type id and value id; RSC wire only.
    stat_entry,
    /// Entity position path: linear vector or stepped list; RSC wire only.
    position_path,
    /// Map colour patch: optional width/height/origin and colours.
    map_patch,
    /// Advancement screen selection: action id plus an optional tab.
    advancement_seen,
    /// Entity movement delta: packed flags/step count plus deltas.
    vec_delta,
    /// Sound source/name selector used by stop-sound.
    sound_selector,
    /// Scoreboard objective add/change/remove payload.
    objective_update,
    /// Scoreboard team add/change/join/leave payload.
    team_update,
    /// Player list entries with per-action optional fields.
    player_info_update,
    /// Tracked waypoint add/update/remove payload.
    tracked_waypoint,
    /// Empty exact component predicate.
    component_predicate,
    /// Merchant trade offer.
    merchant_offer,
    /// Entity metadata list terminated by the 255 marker.
    entity_metadata,
    void_type,
};

/// Wire encoding of an integer field. RSC storage defaults preserve the
/// original compact encoding; `#[varint]`, `#[varlong]` and `#[fixed]` select
/// the protocol encoding explicitly.
enum class integer_wire : std::uint8_t {
    natural,
    varint,
    varlong,
    fixed,
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
        map,
        either,
        holder,
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

    /// Wire encoding selected by `#[varint]`/`#[varlong]`/`#[fixed]`.
    integer_wire wire{integer_wire::natural};
    /// `#[max(N)]`: maximum character count for strings, byte count for
    /// `bytes`, element count for arrays. Unset means the codec default.
    std::optional<std::uint64_t> wire_max;
    /// `#[fixed_bytes(N)]`: a `bytes` field encoded without a length prefix.
    std::optional<std::uint64_t> wire_fixed_bytes;
    /// `#[rest]`: a `bytes` field that consumes the remaining packet body.
    bool wire_rest{false};
    /// `#[fixed_count(N)]`: an array field with a fixed element count and no
    /// count prefix.
    std::optional<std::uint64_t> wire_fixed_count;
    /// `#[optional_varint]`: an optional integer encoded as one VarInt where
    /// zero means absent and a stored value `v` is encoded as `v + 1`.
    bool wire_optional_varint{false};

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
