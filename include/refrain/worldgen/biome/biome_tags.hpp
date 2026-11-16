// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The biome tag tree of a worldgen data pack. Biome selector documents and
// block predicates name a tag as `#namespace:path`; the data pack's
// `tags/worldgen/biome` tree maps that id to biome ids and to other tags.

#pragma once

#include <refrain/core/error.hpp>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace refrain::worldgen {

/// One loaded biome tag tree. Entries preserve the order the tag file lists,
/// and a value that starts with `#` is a reference to another tag.
class biome_tags {
  public:
    biome_tags() = default;
    explicit biome_tags(
        std::unordered_map<std::string, std::vector<std::string>> entries)
        : entries_{std::move(entries)} {}

    /// True when the data pack defines `tag`.
    [[nodiscard]] auto contains(std::string_view tag) const -> bool;

    /// Every biome id `tag` resolves to, in depth-first tag order. A tag that
    /// is missing from the data pack expands to nothing.
    [[nodiscard]] auto expand(std::string_view tag) const
        -> std::vector<std::string>;

    /// True when `biome` is a member of `tag`, directly or through a nested
    /// tag reference.
    [[nodiscard]] auto matches(std::string_view tag,
                               std::string_view biome) const -> bool;

    [[nodiscard]] auto empty() const noexcept -> bool {
        return entries_.empty();
    }

  private:
    std::unordered_map<std::string, std::vector<std::string>> entries_{};
};

/// Loads `<namespace>/tags/worldgen/biome/*.json` under every namespace of
/// `data_root`. A missing tags tree yields an empty table.
[[nodiscard]] auto load_biome_tags(std::string_view data_root)
    -> result<biome_tags>;

} // namespace refrain::worldgen
