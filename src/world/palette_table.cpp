// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/world/palette_table.hpp>

#include "rwf_internal.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace refrain::world {
namespace {

constexpr std::array<std::byte, 4> magic{std::byte{'R'}, std::byte{'F'},
                                         std::byte{'P'}, std::byte{'T'}};
constexpr std::size_t magic_bytes{4};
constexpr std::uint16_t table_version{1};
constexpr std::size_t max_entries{1U << 20U};

[[nodiscard]] auto parse_failure(std::string message)
    -> std::unexpected<error> {
    return std::unexpected{
        make_error(error_code::parse_error, std::move(message))};
}

} // namespace

auto palette_table::intern(std::string_view key) -> std::uint32_t {
    if (const auto found{ids_.find(std::string{key})}; found != ids_.end()) {
        return found->second;
    }
    const auto id{static_cast<std::uint32_t>(entries_.size())};
    entries_.emplace_back(key);
    ids_.emplace(entries_.back(), id);
    return id;
}

auto palette_table::find(std::uint32_t id) const -> const std::string * {
    if (id >= entries_.size()) {
        return nullptr;
    }
    return &entries_[id];
}

auto palette_table::find(std::string_view key) const -> const std::uint32_t * {
    const auto found{ids_.find(std::string{key})};
    return found == ids_.end() ? nullptr : &found->second;
}

auto palette_table::serialize() const -> std::vector<std::byte> {
    std::vector<std::byte> out;
    out.insert(out.end(), magic.begin(), magic.end());
    const auto head{out.size()};
    out.resize(head + 6);
    rwf::detail::put_u16(out, head, table_version);
    rwf::detail::put_u32(out, head + 2,
                         static_cast<std::uint32_t>(entries_.size()));
    for (const auto &entry : entries_) {
        const auto length{out.size()};
        out.resize(length + 2);
        rwf::detail::put_u16(out, length,
                             static_cast<std::uint16_t>(entry.size()));
        out.insert(out.end(), reinterpret_cast<const std::byte *>(entry.data()),
                   reinterpret_cast<const std::byte *>(entry.data()) +
                       entry.size());
    }
    return out;
}

auto palette_table::parse(std::span<const std::byte> bytes)
    -> result<palette_table> {
    if (bytes.size() < 10) {
        return parse_failure("palette table is truncated");
    }
    if (!std::equal(magic.begin(), magic.end(), bytes.begin())) {
        return parse_failure("palette table has a bad RFPT magic");
    }
    if (rwf::detail::get_u16(bytes, 4) != table_version) {
        return parse_failure("palette table has an unsupported version");
    }
    const auto count{rwf::detail::get_u32(bytes, 6)};
    if (count > max_entries) {
        return parse_failure("palette table has too many entries");
    }
    palette_table table;
    std::size_t offset{10};
    table.entries_.reserve(count);
    for (std::uint32_t id{0}; id < count; ++id) {
        if (offset + 2 > bytes.size()) {
            return parse_failure("palette table entry is truncated");
        }
        const auto length{rwf::detail::get_u16(bytes, offset)};
        offset += 2;
        if (length == 0 || offset + length > bytes.size()) {
            return parse_failure("palette table string is invalid");
        }
        std::string entry{reinterpret_cast<const char *>(bytes.data() + offset),
                          length};
        offset += length;
        if (!table.ids_.emplace(entry, id).second) {
            return parse_failure(
                fmt::format("palette table has duplicate '{}'", entry));
        }
        table.entries_.push_back(std::move(entry));
    }
    if (offset != bytes.size()) {
        return parse_failure("palette table has trailing bytes");
    }
    return table;
}

} // namespace refrain::world
