// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The canonical block-state key used by the registry: `name` or
// `name[property=value,...]` with properties sorted by name.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/worldgen/core/java_compat.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace refrain::data::json {
class value;
} // namespace refrain::data::json

namespace refrain::worldgen {

/// Decodes a `BlockState` JSON value: a state key string or an object with
/// `id`/`Name` and optional `properties`.
[[nodiscard]] auto decode_state_key(const data::json::value &value)
    -> result<std::string>;

/// One property value from a canonical key.
[[nodiscard]] auto state_property(std::string_view key,
                                  std::string_view property)
    -> std::optional<std::string_view>;

/// Replaces one property in a canonical key (leaves it unchanged when the
/// property is absent).
[[nodiscard]] auto with_state_property(std::string_view key,
                                       std::string_view property,
                                       std::string_view value) -> std::string;

/// The block part of a canonical state key (before `[`).
[[nodiscard]] auto state_block_name(std::string_view key) -> std::string_view;

/// True for the three air block names.
[[nodiscard]] auto is_air_block_name(std::string_view name) -> bool;

} // namespace refrain::worldgen
