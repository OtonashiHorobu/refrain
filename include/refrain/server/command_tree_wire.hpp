// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/command/tree.hpp>
#include <refrain/net/wire.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace refrain::server {

/// Protocol parser id of one framework parser kind, or `std::nullopt` for a
/// kind that carries no argument type (the root and literal nodes).
///
/// The ids are the 26.3 `minecraft:command_argument_type` registry ids, which
/// are assigned in the reference's registration order. The command module keeps
/// them out of its own taxonomy on purpose; this adapter is the single place
/// that pairs a framework parser with a wire id.
[[nodiscard]] auto wire_parser_id(command::parser_kind kind)
    -> std::optional<std::uint32_t>;

/// Human-readable 26.3 parser id (`brigadier:integer`, `block_pos`, ...).
/// Used by logs and by the tree consistency check; empty for `none`.
[[nodiscard]] auto wire_parser_name(command::parser_kind kind)
    -> std::string_view;

/// Convert a command tree snapshot into the clientbound `commands` payload.
///
/// Node order and indices are those of the snapshot; the snapshot already is a
/// preorder walk of the permitted graph, so the wire tree needs no reindexing.
/// `root_index` is the snapshot root. The payload declares the reference's
/// ask-server suggestion provider by leaving every argument node without a
/// custom provider: the server answers completion requests itself.
[[nodiscard]] auto to_wire_tree(const command::tree &snapshot)
    -> net::command_tree;

} // namespace refrain::server
