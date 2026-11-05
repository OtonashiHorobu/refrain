// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// World-level palette string table (RWF pre-release v1). Block/biome palette
// entries store u32 ids; the table maps ids to canonical strings and is stored
// as the `palette` object in the world object store. The table is append-only
// within one world; import sorts strings for deterministic ids.

#pragma once

#include <refrain/core/error.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace refrain::world {

class palette_table {
  public:
    /// Append `key` when absent and return its id.
    [[nodiscard]] auto intern(std::string_view key) -> std::uint32_t;

    /// Resolve an id; nullptr when out of range.
    [[nodiscard]] auto find(std::uint32_t id) const -> const std::string *;

    /// Resolve a key without mutating the table; nullptr when absent.
    [[nodiscard]] auto find(std::string_view key) const
        -> const std::uint32_t *;

    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return entries_.size();
    }
    [[nodiscard]] auto empty() const noexcept -> bool {
        return entries_.empty();
    }

    /// Deterministic `RFPT` v1 encoding: magic, version, count, then
    /// `u16 len + bytes` in id order.
    [[nodiscard]] auto serialize() const -> std::vector<std::byte>;

    /// Parse an `RFPT` payload. Rejects duplicates/truncation/oversize.
    [[nodiscard]] static auto parse(std::span<const std::byte> bytes)
        -> result<palette_table>;

    [[nodiscard]] auto entries() const noexcept
        -> const std::vector<std::string> & {
        return entries_;
    }

  private:
    std::vector<std::string> entries_;
    std::unordered_map<std::string, std::uint32_t> ids_;
};

} // namespace refrain::world
