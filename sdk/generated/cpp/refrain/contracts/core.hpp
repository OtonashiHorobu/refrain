// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 8a20087cf2517c49

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_CORE_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_CORE_HPP

#include <refrain/contracts/core_abi.h>
#include <refrain/contracts/abi.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <refrain/core/error.hpp>
#include <refrain/core/schedule.hpp>

namespace refrain::core {

/// Error codes shared by all services. Zero is reserved for success
/// (REFRAIN_STATUS_OK), so this enum starts at 1.
enum class core_error : std::uint32_t { // enum 'core_error'
    invalid_argument = 1,
    not_found = 2,
    already_exists = 3,
    permission_denied = 4,
    io_error = 5,
    out_of_memory = 6,
    unsupported = 7,
    version_mismatch = 8,
};

enum class chunk_status : std::uint32_t { // enum 'chunk_status'
    empty = 0,
    structure_starts = 1,
    structure_references = 2,
    biomes = 3,
    noise = 4,
    surface = 5,
    carvers = 6,
    features = 7,
    light = 8,
    spawn = 9,
    full = 10,
};

enum class block_face : std::uint8_t { // enum 'block_face'
    north = 0,
    east = 1,
    south = 2,
    west = 3,
    up = 4,
    down = 5,
};

namespace detail {
struct player_tag;
struct connection_tag;
struct gen_ctx_tag;
struct storage_world_tag;
} // namespace detail

/// Opaque host-side handles. The tag is the C++ handle type.
using player = ::refrain::contracts::handle<detail::player_tag>;
using connection = ::refrain::contracts::handle<detail::connection_tag>;
using gen_ctx = ::refrain::contracts::handle<detail::gen_ctx_tag>;
using storage_world = ::refrain::contracts::handle<detail::storage_world_tag>;

/// Chunk column coordinate on the horizontal plane.
using chunk_pos = ::refrain_core_chunk_pos;
/// Zero-initialized `chunk_pos` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_chunk_pos() noexcept -> chunk_pos {
    chunk_pos value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
/// Block coordinate inside a chunk column.
using block_pos = ::refrain_core_block_pos;
/// Zero-initialized `block_pos` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_block_pos() noexcept -> block_pos {
    block_pos value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
/// One section of the Chunk ABI: the versioned in-memory layout shared by
/// `refrain:world`, `refrain:worldgen` and `refrain:sim`.
using chunk_section_view = ::refrain_core_chunk_section_view;
/// Zero-initialized `chunk_section_view` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_chunk_section_view() noexcept -> chunk_section_view {
    chunk_section_view value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
/// Borrowed view of a loaded chunk. The slice lifetime is the contract call
/// during which it was produced.
using chunk_view = ::refrain_core_chunk_view;
/// Zero-initialized `chunk_view` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_chunk_view() noexcept -> chunk_view {
    chunk_view value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
/// One block-state change, batched by the world/sim contracts.
using block_change = ::refrain_core_block_change;
/// Zero-initialized `block_change` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_block_change() noexcept -> block_change {
    block_change value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
/// Caller-provided chunk serialization buffer (out parameters use this).
using chunk_buffer = ::refrain_core_chunk_buffer;
/// Zero-initialized `chunk_buffer` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_chunk_buffer() noexcept -> chunk_buffer {
    chunk_buffer value{};
    ::refrain::contracts::init_struct(value);
    return value;
}

static_assert(sizeof(chunk_pos) == 12);
static_assert(alignof(chunk_pos) == 4);
static_assert(offsetof(chunk_pos, struct_size) == 0);
static_assert(offsetof(chunk_pos, x) == 4);
static_assert(offsetof(chunk_pos, z) == 8);
static_assert(sizeof(block_pos) == 16);
static_assert(alignof(block_pos) == 4);
static_assert(offsetof(block_pos, struct_size) == 0);
static_assert(offsetof(block_pos, x) == 4);
static_assert(offsetof(block_pos, y) == 8);
static_assert(offsetof(block_pos, z) == 12);
static_assert(sizeof(chunk_section_view) == 32);
static_assert(alignof(chunk_section_view) == 8);
static_assert(offsetof(chunk_section_view, struct_size) == 0);
static_assert(offsetof(chunk_section_view, section_y) == 4);
static_assert(offsetof(chunk_section_view, block_count) == 8);
static_assert(offsetof(chunk_section_view, bits_per_entry) == 10);
static_assert(offsetof(chunk_section_view, flags) == 11);
static_assert(offsetof(chunk_section_view, block_entities) == 16);
static_assert(sizeof(chunk_view) == 40);
static_assert(alignof(chunk_view) == 8);
static_assert(offsetof(chunk_view, struct_size) == 0);
static_assert(offsetof(chunk_view, pos) == 4);
static_assert(offsetof(chunk_view, status) == 16);
static_assert(offsetof(chunk_view, section_count) == 20);
static_assert(offsetof(chunk_view, sections) == 24);
static_assert(sizeof(block_change) == 28);
static_assert(alignof(block_change) == 4);
static_assert(offsetof(block_change, struct_size) == 0);
static_assert(offsetof(block_change, pos) == 4);
static_assert(offsetof(block_change, old_state) == 20);
static_assert(offsetof(block_change, new_state) == 24);
static_assert(sizeof(chunk_buffer) == 40);
static_assert(alignof(chunk_buffer) == 8);
static_assert(offsetof(chunk_buffer, struct_size) == 0);
static_assert(offsetof(chunk_buffer, pos) == 4);
static_assert(offsetof(chunk_buffer, generation) == 16);
static_assert(offsetof(chunk_buffer, word_count) == 20);
static_assert(offsetof(chunk_buffer, data) == 24);

} // namespace refrain::core

#endif // REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_CORE_HPP
