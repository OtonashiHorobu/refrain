// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Default block-state properties. A datapack names a plain block when a
// feature places that block's default state; the report records the property
// values of that state so state-dependent behaviour can be reproduced without
// guessing from the key spelling.

#pragma once

#include <refrain/core/error.hpp>

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::worldgen {

struct block_default_property {
    std::string name{};
    std::string value{};
};

/// The default properties of one block, sorted by property name.
using block_default_properties = std::vector<block_default_property>;

/// Default properties keyed by block name.
using block_default_map =
    std::map<std::string, block_default_properties, std::less<>>;

/// Reads `<data_root>/../reports/block_defaults/defaults.json`. Returns
/// `not_found` when the report is not installed; a caller that can still work
/// without default-state knowledge treats that as an empty map.
[[nodiscard]] auto load_block_defaults(std::string_view data_root)
    -> result<block_default_map>;

} // namespace refrain::worldgen
