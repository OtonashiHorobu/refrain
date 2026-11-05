// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/world/chunk_codec.hpp>

#include <refrain/data/checksum.hpp>

#include "rwf_internal.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace refrain::world {
namespace {

constexpr std::size_t field_header_bytes = 16;
constexpr std::uint16_t highest_field_id =
    static_cast<std::uint16_t>(chunk_field::sections_inline);

/// 4096 entries at 32 bits per entry (the maximum supported width).
constexpr std::size_t max_packed_words = 2048;

[[nodiscard]] auto parse_failure(std::string message)
    -> std::unexpected<error> {
    return std::unexpected{
        make_error(error_code::parse_error, std::move(message))};
}

class cursor {
  public:
    explicit cursor(std::span<const std::byte> data) : data_{data} {}

    [[nodiscard]] auto take(std::size_t count) -> std::span<const std::byte> {
        if (failed_ || count > data_.size() - offset_) {
            failed_ = true;
            return {};
        }
        const auto out{data_.subspan(offset_, count)};
        offset_ += count;
        return out;
    }

    [[nodiscard]] auto u8() -> std::uint8_t {
        const auto bytes{take(1)};
        return bytes.empty() ? 0 : static_cast<std::uint8_t>(bytes[0]);
    }
    [[nodiscard]] auto u16() -> std::uint16_t {
        const auto bytes{take(2)};
        return bytes.empty() ? 0 : rwf::detail::get_u16(bytes, 0);
    }
    [[nodiscard]] auto u32() -> std::uint32_t {
        const auto bytes{take(4)};
        return bytes.empty() ? 0 : rwf::detail::get_u32(bytes, 0);
    }
    [[nodiscard]] auto u64() -> std::uint64_t {
        const auto bytes{take(8)};
        return bytes.empty() ? 0 : rwf::detail::get_u64(bytes, 0);
    }
    [[nodiscard]] auto i16() -> std::int16_t {
        return static_cast<std::int16_t>(u16());
    }
    [[nodiscard]] auto i32() -> std::int32_t {
        return static_cast<std::int32_t>(u32());
    }
    [[nodiscard]] auto string() -> std::string {
        const auto length{u16()};
        const auto bytes{take(length)};
        if (bytes.empty() && length != 0) {
            return {};
        }
        return std::string{reinterpret_cast<const char *>(bytes.data()),
                           bytes.size()};
    }
    [[nodiscard]] auto bytes(std::size_t count) -> std::span<const std::byte> {
        return take(count);
    }

    [[nodiscard]] auto failed() const noexcept -> bool { return failed_; }
    [[nodiscard]] auto remaining() const noexcept -> std::size_t {
        return data_.size() - offset_;
    }

  private:
    std::span<const std::byte> data_;
    std::size_t offset_{0};
    bool failed_{false};
};

[[nodiscard]] auto append_field(std::vector<std::byte> &out, chunk_field id,
                                std::uint8_t type,
                                std::span<const std::byte> payload) -> bool {
    const auto padded{
        rwf::detail::align_up(static_cast<std::uint64_t>(payload.size()), 8)};
    if (out.size() + field_header_bytes + padded > rwf::max_record_bytes) {
        return false;
    }
    std::array<std::byte, field_header_bytes> header{};
    rwf::detail::put_u16(header, 0, static_cast<std::uint16_t>(id));
    rwf::detail::put_u8(header, 2, type);
    rwf::detail::put_u32(header, 4, static_cast<std::uint32_t>(payload.size()));
    rwf::detail::put_u32(header, 8, data::crc32c(payload));
    out.insert(out.end(), header.begin(), header.end());
    out.insert(out.end(), payload.begin(), payload.end());
    out.resize(static_cast<std::size_t>(out.size() + padded - payload.size()),
               std::byte{0});
    return true;
}

[[nodiscard]] auto encode_status(const chunk &value) -> std::vector<std::byte> {
    std::vector<std::byte> payload(17, std::byte{0});
    rwf::detail::put_u8(payload, 0, static_cast<std::uint8_t>(value.status));
    rwf::detail::put_u64(payload, 1,
                         static_cast<std::uint64_t>(value.last_update));
    rwf::detail::put_u64(payload, 9,
                         static_cast<std::uint64_t>(value.inhabited_time));
    return payload;
}

[[nodiscard]] auto encode_y_pos(const chunk &value) -> std::vector<std::byte> {
    std::vector<std::byte> payload(4, std::byte{0});
    rwf::detail::put_i32(payload, 0, value.y_pos);
    return payload;
}

[[nodiscard]] auto encode_sections(const chunk &value, palette_table &palette,
                                   bool allow_growth)
    -> result<std::vector<std::byte>> {
    // When a palette is shared by parallel import workers, growth must be
    // disabled: a miss is a pre-scan bug, not a licence to mutate the table
    // concurrently.
    const auto id_for = [&palette, allow_growth](
                            std::string_view key) -> result<std::uint32_t> {
        if (allow_growth) {
            return palette.intern(key);
        }
        const auto *found{palette.find(key)};
        if (found == nullptr) {
            return std::unexpected{make_error(
                error_code::invalid_argument,
                fmt::format("palette table is missing '{}' (pre-scan must "
                            "include every state)",
                            key))};
        }
        return *found;
    };
    std::vector<std::byte> payload(2, std::byte{0});
    rwf::detail::put_u16(payload, 0,
                         static_cast<std::uint16_t>(value.sections.size()));
    for (const auto &section : value.sections) {
        const auto head{payload.size()};
        payload.resize(head + 7, std::byte{0});
        rwf::detail::put_u8(payload, head,
                            static_cast<std::uint8_t>(section.y));
        rwf::detail::put_u16(payload, head + 1, section.block_count);
        rwf::detail::put_u8(payload, head + 3,
                            static_cast<std::uint8_t>(section.kind));
        rwf::detail::put_u8(payload, head + 4, section.bits_per_entry);
        rwf::detail::put_u16(
            payload, head + 5,
            static_cast<std::uint16_t>(section.palette.size()));
        for (const auto &entry : section.palette) {
            auto key_id{id_for(entry.key)};
            if (!key_id) {
                return std::unexpected{key_id.error()};
            }
            const auto key_base{payload.size()};
            payload.resize(key_base + 4, std::byte{0});
            rwf::detail::put_u32(payload, key_base, *key_id);
            const auto numbers{payload.size()};
            payload.resize(numbers + 6, std::byte{0});
            rwf::detail::put_u16(payload, numbers, entry.count);
            rwf::detail::put_u32(payload, numbers + 2, entry.state);
        }
        const auto words_base{payload.size()};
        payload.resize(words_base + 4, std::byte{0});
        rwf::detail::put_u32(payload, words_base,
                             static_cast<std::uint32_t>(section.data.size()));
        for (const auto word : section.data) {
            const auto word_base{payload.size()};
            payload.resize(word_base + 8, std::byte{0});
            rwf::detail::put_u64(payload, word_base, word);
        }

        const auto biome_head{payload.size()};
        payload.resize(biome_head + 2, std::byte{0});
        rwf::detail::put_u8(payload, biome_head,
                            static_cast<std::uint8_t>(section.biome_kind));
        rwf::detail::put_u8(payload, biome_head + 1,
                            section.biome_bits_per_entry);
        const auto biome_count_base{payload.size()};
        payload.resize(biome_count_base + 2, std::byte{0});
        rwf::detail::put_u16(
            payload, biome_count_base,
            static_cast<std::uint16_t>(section.biome_palette.size()));
        for (const auto &key : section.biome_palette) {
            auto key_id{id_for(key)};
            if (!key_id) {
                return std::unexpected{key_id.error()};
            }
            const auto key_base{payload.size()};
            payload.resize(key_base + 4, std::byte{0});
            rwf::detail::put_u32(payload, key_base, *key_id);
        }
        const auto biome_words_base{payload.size()};
        payload.resize(biome_words_base + 4, std::byte{0});
        rwf::detail::put_u32(
            payload, biome_words_base,
            static_cast<std::uint32_t>(section.biome_data.size()));
        for (const auto word : section.biome_data) {
            const auto word_base{payload.size()};
            payload.resize(word_base + 8, std::byte{0});
            rwf::detail::put_u64(payload, word_base, word);
        }
    }
    return payload;
}

[[nodiscard]] auto encode_sections_inline(const chunk &value)
    -> result<std::vector<std::byte>> {
    std::vector<std::byte> payload(2, std::byte{0});
    rwf::detail::put_u16(payload, 0,
                         static_cast<std::uint16_t>(value.sections.size()));
    const auto append_string =
        [&payload](std::string_view text) -> result<void> {
        if (text.size() > std::numeric_limits<std::uint16_t>::max()) {
            return std::unexpected{
                make_error(error_code::invalid_argument,
                           "inline palette key exceeds the u16 length field")};
        }
        const auto base{payload.size()};
        payload.resize(base + 2, std::byte{0});
        rwf::detail::put_u16(payload, base,
                             static_cast<std::uint16_t>(text.size()));
        payload.insert(
            payload.end(), reinterpret_cast<const std::byte *>(text.data()),
            reinterpret_cast<const std::byte *>(text.data()) + text.size());
        return status_ok();
    };
    for (const auto &section : value.sections) {
        const auto head{payload.size()};
        payload.resize(head + 7, std::byte{0});
        rwf::detail::put_u8(payload, head,
                            static_cast<std::uint8_t>(section.y));
        rwf::detail::put_u16(payload, head + 1, section.block_count);
        rwf::detail::put_u8(payload, head + 3,
                            static_cast<std::uint8_t>(section.kind));
        rwf::detail::put_u8(payload, head + 4, section.bits_per_entry);
        rwf::detail::put_u16(
            payload, head + 5,
            static_cast<std::uint16_t>(section.palette.size()));
        for (const auto &entry : section.palette) {
            auto appended{append_string(entry.key)};
            if (!appended) {
                return std::unexpected{std::move(appended.error())};
            }
            const auto numbers{payload.size()};
            payload.resize(numbers + 6, std::byte{0});
            rwf::detail::put_u16(payload, numbers, entry.count);
            rwf::detail::put_u32(payload, numbers + 2, entry.state);
        }
        const auto words_base{payload.size()};
        payload.resize(words_base + 4, std::byte{0});
        rwf::detail::put_u32(payload, words_base,
                             static_cast<std::uint32_t>(section.data.size()));
        for (const auto word : section.data) {
            const auto word_base{payload.size()};
            payload.resize(word_base + 8, std::byte{0});
            rwf::detail::put_u64(payload, word_base, word);
        }

        const auto biome_head{payload.size()};
        payload.resize(biome_head + 2, std::byte{0});
        rwf::detail::put_u8(payload, biome_head,
                            static_cast<std::uint8_t>(section.biome_kind));
        rwf::detail::put_u8(payload, biome_head + 1,
                            section.biome_bits_per_entry);
        const auto biome_count_base{payload.size()};
        payload.resize(biome_count_base + 2, std::byte{0});
        rwf::detail::put_u16(
            payload, biome_count_base,
            static_cast<std::uint16_t>(section.biome_palette.size()));
        for (const auto &key : section.biome_palette) {
            auto appended{append_string(key)};
            if (!appended) {
                return std::unexpected{std::move(appended.error())};
            }
        }
        const auto biome_words_base{payload.size()};
        payload.resize(biome_words_base + 4, std::byte{0});
        rwf::detail::put_u32(
            payload, biome_words_base,
            static_cast<std::uint32_t>(section.biome_data.size()));
        for (const auto word : section.biome_data) {
            const auto word_base{payload.size()};
            payload.resize(word_base + 8, std::byte{0});
            rwf::detail::put_u64(payload, word_base, word);
        }
    }
    return payload;
}

[[nodiscard]] auto encode_light_sections(const chunk &value)
    -> std::vector<std::byte> {
    std::vector<std::byte> payload(2, std::byte{0});
    rwf::detail::put_u16(
        payload, 0, static_cast<std::uint16_t>(value.light_sections.size()));
    const auto append_array = [&payload](const std::vector<std::byte> &array) {
        const auto base{payload.size()};
        payload.resize(base + 2, std::byte{0});
        rwf::detail::put_u16(payload, base,
                             static_cast<std::uint16_t>(array.size()));
        payload.insert(payload.end(), array.begin(), array.end());
    };
    for (const auto &light : value.light_sections) {
        payload.push_back(static_cast<std::byte>(light.y));
        append_array(light.block_light);
        append_array(light.sky_light);
    }
    return payload;
}

[[nodiscard]] auto encode_heightmaps(const chunk &value)
    -> std::vector<std::byte> {
    std::vector<std::byte> payload(2, std::byte{0});
    rwf::detail::put_u16(payload, 0,
                         static_cast<std::uint16_t>(value.heightmaps.size()));
    for (const auto &heightmap : value.heightmaps) {
        const auto name_base{payload.size()};
        payload.resize(name_base + 2, std::byte{0});
        rwf::detail::put_u16(payload, name_base,
                             static_cast<std::uint16_t>(heightmap.name.size()));
        payload.insert(
            payload.end(),
            reinterpret_cast<const std::byte *>(heightmap.name.data()),
            reinterpret_cast<const std::byte *>(heightmap.name.data()) +
                heightmap.name.size());
        const auto head{payload.size()};
        payload.resize(head + 3, std::byte{0});
        rwf::detail::put_u8(payload, head, heightmap.bits_per_entry);
        rwf::detail::put_u16(payload, head + 1,
                             static_cast<std::uint16_t>(heightmap.data.size()));
        for (const auto word : heightmap.data) {
            const auto word_base{payload.size()};
            payload.resize(word_base + 8, std::byte{0});
            rwf::detail::put_u64(payload, word_base, word);
        }
    }
    return payload;
}

[[nodiscard]] auto encode_ticks(std::span<const saved_tick> block_ticks,
                                std::span<const saved_tick> fluid_ticks)
    -> std::vector<std::byte> {
    std::vector<std::byte> payload(4, std::byte{0});
    rwf::detail::put_u16(payload, 0,
                         static_cast<std::uint16_t>(block_ticks.size()));
    rwf::detail::put_u16(payload, 2,
                         static_cast<std::uint16_t>(fluid_ticks.size()));
    for (const auto *list : {&block_ticks, &fluid_ticks}) {
        for (const auto &tick : *list) {
            const auto id_base{payload.size()};
            payload.resize(id_base + 2, std::byte{0});
            rwf::detail::put_u16(payload, id_base,
                                 static_cast<std::uint16_t>(tick.id.size()));
            payload.insert(payload.end(),
                           reinterpret_cast<const std::byte *>(tick.id.data()),
                           reinterpret_cast<const std::byte *>(tick.id.data()) +
                               tick.id.size());
            const auto base{payload.size()};
            payload.resize(base + 16, std::byte{0});
            rwf::detail::put_i32(payload, base, tick.pos.x);
            rwf::detail::put_u16(payload, base + 4,
                                 static_cast<std::uint16_t>(tick.pos.y));
            rwf::detail::put_i32(payload, base + 6, tick.pos.z);
            rwf::detail::put_u8(payload, base + 10,
                                static_cast<std::uint8_t>(tick.priority));
            rwf::detail::put_u8(payload, base + 11, tick.kind);
            rwf::detail::put_u32(payload, base + 12, tick.remaining_ticks);
        }
    }
    return payload;
}

[[nodiscard]] auto encode_block_entities(const chunk &value)
    -> std::vector<std::byte> {
    std::vector<std::byte> payload(4, std::byte{0});
    rwf::detail::put_u32(
        payload, 0, static_cast<std::uint32_t>(value.block_entities.size()));
    for (const auto &entity : value.block_entities) {
        const auto base{payload.size()};
        payload.resize(base + 4, std::byte{0});
        rwf::detail::put_u32(payload, base,
                             static_cast<std::uint32_t>(entity.size()));
        payload.insert(payload.end(), entity.begin(), entity.end());
    }
    return payload;
}

[[nodiscard]] auto
encode_post_processing(const std::vector<std::vector<std::int16_t>> &lists)
    -> std::vector<std::byte> {
    std::vector<std::byte> payload(2, std::byte{0});
    rwf::detail::put_u16(payload, 0, static_cast<std::uint16_t>(lists.size()));
    for (const auto &list : lists) {
        const auto base{payload.size()};
        payload.resize(base + 2, std::byte{0});
        rwf::detail::put_u16(payload, base,
                             static_cast<std::uint16_t>(list.size()));
        for (const auto value : list) {
            const auto value_base{payload.size()};
            payload.resize(value_base + 2, std::byte{0});
            rwf::detail::put_u16(
                payload, value_base,
                static_cast<std::uint16_t>(static_cast<std::uint16_t>(value)));
        }
    }
    return payload;
}

auto decode_sections(cursor &in, chunk &out, bool &ok,
                     const palette_table *palette) -> void {
    const auto count{in.u16()};
    if (in.failed() || count > max_sections_per_chunk) {
        ok = false;
        return;
    }
    out.sections.reserve(count);
    out.post_processing.clear();
    for (std::uint16_t index{0}; index < count; ++index) {
        section_record section{};
        section.y = static_cast<std::int8_t>(in.u8());
        section.block_count = in.u16();
        section.kind = static_cast<section_kind>(in.u8());
        section.bits_per_entry = in.u8();
        const auto palette_count{in.u16()};
        if (in.failed() || palette_count > max_palette_entries) {
            ok = false;
            return;
        }
        section.palette.reserve(palette_count);
        for (std::uint16_t entry_index{0}; entry_index < palette_count;
             ++entry_index) {
            const auto id{in.u32()};
            const auto entry_count{in.u16()};
            const auto state{in.u32()};
            if (in.failed() || palette == nullptr ||
                palette->find(id) == nullptr) {
                ok = false;
                return;
            }
            section.palette.push_back(palette_entry{.key = *palette->find(id),
                                                    .count = entry_count,
                                                    .state = state});
        }
        const auto data_count{in.u32()};
        if (in.failed() || data_count > max_packed_words) {
            ok = false;
            return;
        }
        section.data.resize(data_count);
        for (auto &word : section.data) {
            word = in.u64();
        }
        section.biome_kind = static_cast<section_kind>(in.u8());
        section.biome_bits_per_entry = in.u8();
        const auto biome_count{in.u16()};
        if (in.failed() || biome_count > max_palette_entries) {
            ok = false;
            return;
        }
        section.biome_palette.reserve(biome_count);
        for (std::uint16_t entry_index{0}; entry_index < biome_count;
             ++entry_index) {
            const auto id{in.u32()};
            if (in.failed() || palette == nullptr ||
                palette->find(id) == nullptr) {
                ok = false;
                return;
            }
            section.biome_palette.push_back(*palette->find(id));
        }
        const auto biome_words{in.u32()};
        if (in.failed() || biome_words > max_packed_words) {
            ok = false;
            return;
        }
        section.biome_data.resize(biome_words);
        for (auto &word : section.biome_data) {
            word = in.u64();
        }
        out.sections.push_back(std::move(section));
    }
}

auto decode_sections_inline(cursor &in, chunk &out, bool &ok) -> void {
    const auto count{in.u16()};
    if (in.failed() || count > max_sections_per_chunk) {
        ok = false;
        return;
    }
    out.sections.reserve(count);
    for (std::uint16_t index{0}; index < count; ++index) {
        section_record section{};
        section.y = static_cast<std::int8_t>(in.u8());
        section.block_count = in.u16();
        section.kind = static_cast<section_kind>(in.u8());
        section.bits_per_entry = in.u8();
        const auto palette_count{in.u16()};
        if (in.failed() || palette_count > max_palette_entries) {
            ok = false;
            return;
        }
        section.palette.reserve(palette_count);
        for (std::uint16_t entry_index{0}; entry_index < palette_count;
             ++entry_index) {
            palette_entry entry{};
            entry.key = in.string();
            entry.count = in.u16();
            entry.state = in.u32();
            if (in.failed() || entry.key.empty()) {
                ok = false;
                return;
            }
            section.palette.push_back(std::move(entry));
        }
        const auto data_count{in.u32()};
        if (in.failed() || data_count > max_packed_words) {
            ok = false;
            return;
        }
        section.data.resize(data_count);
        for (auto &word : section.data) {
            word = in.u64();
        }
        section.biome_kind = static_cast<section_kind>(in.u8());
        section.biome_bits_per_entry = in.u8();
        const auto biome_count{in.u16()};
        if (in.failed() || biome_count > max_palette_entries) {
            ok = false;
            return;
        }
        section.biome_palette.reserve(biome_count);
        for (std::uint16_t entry_index{0}; entry_index < biome_count;
             ++entry_index) {
            auto key{in.string()};
            if (in.failed() || key.empty()) {
                ok = false;
                return;
            }
            section.biome_palette.push_back(std::move(key));
        }
        const auto biome_words{in.u32()};
        if (in.failed() || biome_words > max_packed_words) {
            ok = false;
            return;
        }
        section.biome_data.resize(biome_words);
        for (auto &word : section.biome_data) {
            word = in.u64();
        }
        out.sections.push_back(std::move(section));
    }
}

auto decode_light_sections(cursor &in, chunk &out, bool &ok) -> void {
    const auto count{in.u16()};
    if (in.failed() || count > max_light_sections) {
        ok = false;
        return;
    }
    out.light_sections.reserve(count);
    for (std::uint16_t index{0}; index < count; ++index) {
        light_section light{};
        light.y = static_cast<std::int8_t>(in.u8());
        const auto block_light{in.u16()};
        if (in.failed() || block_light > light_nibble_bytes) {
            ok = false;
            return;
        }
        auto block_bytes{in.bytes(block_light)};
        if (in.failed()) {
            ok = false;
            return;
        }
        light.block_light.assign(block_bytes.begin(), block_bytes.end());
        const auto sky_light{in.u16()};
        if (in.failed() || sky_light > light_nibble_bytes) {
            ok = false;
            return;
        }
        auto sky_bytes{in.bytes(sky_light)};
        if (in.failed()) {
            ok = false;
            return;
        }
        light.sky_light.assign(sky_bytes.begin(), sky_bytes.end());
        out.light_sections.push_back(std::move(light));
    }
}

auto decode_heightmaps(cursor &in, chunk &out, bool &ok) -> void {
    const auto count{in.u16()};
    if (in.failed() || count > max_heightmaps) {
        ok = false;
        return;
    }
    out.heightmaps.reserve(count);
    for (std::uint16_t index{0}; index < count; ++index) {
        heightmap_record heightmap{};
        heightmap.name = in.string();
        heightmap.bits_per_entry = in.u8();
        const auto words{in.u16()};
        if (in.failed() || heightmap.name.empty()) {
            ok = false;
            return;
        }
        heightmap.data.resize(words);
        for (auto &word : heightmap.data) {
            word = in.u64();
        }
        out.heightmaps.push_back(std::move(heightmap));
    }
}

auto decode_ticks(cursor &in, chunk &out, bool &ok) -> void {
    const auto block_count{in.u16()};
    const auto fluid_count{in.u16()};
    if (in.failed() || block_count > max_ticks || fluid_count > max_ticks) {
        ok = false;
        return;
    }
    const auto read_list = [&in, &ok](std::uint16_t count,
                                      std::vector<saved_tick> &out_list) {
        if (!ok) {
            return;
        }
        out_list.reserve(count);
        for (std::uint16_t index{0}; index < count; ++index) {
            saved_tick tick{};
            tick.id = in.string();
            if (in.failed() || tick.id.empty()) {
                ok = false;
                return;
            }
            tick.pos.x = in.i32();
            tick.pos.y = in.i16();
            tick.pos.z = in.i32();
            tick.priority = static_cast<std::int8_t>(in.u8());
            tick.kind = in.u8();
            tick.remaining_ticks = in.u32();
            if (in.failed()) {
                ok = false;
                return;
            }
            out_list.push_back(tick);
        }
    };
    read_list(block_count, out.block_ticks);
    read_list(fluid_count, out.fluid_ticks);
}

auto decode_block_entities(cursor &in, chunk &out, bool &ok) -> void {
    const auto count{in.u32()};
    if (in.failed() || count > max_block_entities) {
        ok = false;
        return;
    }
    out.block_entities.reserve(count);
    for (std::uint32_t index{0}; index < count; ++index) {
        const auto length{in.u32()};
        if (in.failed() || length > max_payload_bytes) {
            ok = false;
            return;
        }
        const auto bytes{in.bytes(length)};
        if (in.failed()) {
            ok = false;
            return;
        }
        out.block_entities.emplace_back(bytes.begin(), bytes.end());
    }
}

auto decode_post_processing(cursor &in, chunk &out, bool &ok) -> void {
    const auto count{in.u16()};
    if (in.failed() || count > max_sections_per_chunk) {
        ok = false;
        return;
    }
    out.post_processing.reserve(count);
    for (std::uint16_t index{0}; index < count; ++index) {
        const auto length{in.u16()};
        if (in.failed() || length > max_post_processing_entries) {
            ok = false;
            return;
        }
        std::vector<std::int16_t> list(length);
        for (auto &value : list) {
            value = in.i16();
        }
        out.post_processing.push_back(std::move(list));
    }
}

[[nodiscard]] auto copy_bytes(std::span<const std::byte> source)
    -> std::vector<std::byte> {
    return std::vector<std::byte>{source.begin(), source.end()};
}

} // namespace

auto encode_chunk(const chunk &value, palette_table *palette,
                  bool allow_palette_growth, chunk_palette_mode mode)
    -> result<std::vector<std::byte>> {
    chunk canonical = value;
    canonical.format_version = chunk_format_version;
    std::vector<std::size_t> section_order(canonical.sections.size());
    std::iota(section_order.begin(), section_order.end(), std::size_t{0});
    std::stable_sort(section_order.begin(), section_order.end(),
                     [&canonical](std::size_t left, std::size_t right) {
                         return canonical.sections[left].y <
                                canonical.sections[right].y;
                     });
    std::vector<std::size_t> light_order(canonical.light_sections.size());
    std::iota(light_order.begin(), light_order.end(), std::size_t{0});
    std::stable_sort(light_order.begin(), light_order.end(),
                     [&canonical](std::size_t left, std::size_t right) {
                         return canonical.light_sections[left].y <
                                canonical.light_sections[right].y;
                     });
    std::vector<std::size_t> heightmap_order(canonical.heightmaps.size());
    std::iota(heightmap_order.begin(), heightmap_order.end(), std::size_t{0});
    std::stable_sort(heightmap_order.begin(), heightmap_order.end(),
                     [&canonical](std::size_t left, std::size_t right) {
                         return canonical.heightmaps[left].name <
                                canonical.heightmaps[right].name;
                     });
    // Validate the canonical form (sorted sections are what decode produces).
    std::vector<section_record> sorted_sections;
    sorted_sections.reserve(canonical.sections.size());
    for (const auto position : section_order) {
        sorted_sections.push_back(canonical.sections[position]);
    }
    canonical.sections = std::move(sorted_sections);
    std::vector<light_section> sorted_light;
    sorted_light.reserve(canonical.light_sections.size());
    for (const auto position : light_order) {
        sorted_light.push_back(canonical.light_sections[position]);
    }
    canonical.light_sections = std::move(sorted_light);
    std::vector<heightmap_record> sorted_heightmaps;
    sorted_heightmaps.reserve(canonical.heightmaps.size());
    for (const auto position : heightmap_order) {
        sorted_heightmaps.push_back(canonical.heightmaps[position]);
    }
    canonical.heightmaps = std::move(sorted_heightmaps);
    if (auto valid{validate(canonical)}; !valid) {
        return std::unexpected{valid.error()};
    }
    if (!canonical.extensions.empty()) {
        // The schema reserves #[sidecar] fields, but the v1 TLV field set has
        // no sidecar field yet. Reject instead of silently dropping caller
        // data.
        return std::unexpected{make_error(
            error_code::unsupported,
            "chunk mod sidecar extensions are not supported in RWF v1")};
    }

    palette_table local;
    auto &table{palette != nullptr ? *palette : local};
    auto sections{
        mode == chunk_palette_mode::inline_keys
            ? encode_sections_inline(canonical)
            : encode_sections(canonical, table, allow_palette_growth)};
    if (!sections) {
        return std::unexpected{sections.error()};
    }
    const auto heightmaps{encode_heightmaps(canonical)};
    const auto ticks{
        encode_ticks(canonical.block_ticks, canonical.fluid_ticks)};
    const auto entities{encode_block_entities(canonical)};
    const auto post{encode_post_processing(canonical.post_processing)};

    std::vector<std::byte> out;
    if (!append_field(out, chunk_field::status, 1, encode_status(canonical)) ||
        !append_field(out,
                      mode == chunk_palette_mode::inline_keys
                          ? chunk_field::sections_inline
                          : chunk_field::sections,
                      7, *sections) ||
        !append_field(out, chunk_field::heightmaps, 7, heightmaps) ||
        !append_field(out, chunk_field::ticks, 7, ticks) ||
        !append_field(out, chunk_field::block_entities, 7, entities) ||
        !append_field(out, chunk_field::structures, 0, canonical.structures) ||
        !append_field(out, chunk_field::post_processing, 7, post) ||
        !append_field(out, chunk_field::blending, 0, canonical.blending) ||
        !append_field(out, chunk_field::extra, 0, canonical.extra) ||
        !append_field(out, chunk_field::y_pos, 0, encode_y_pos(canonical)) ||
        !append_field(out, chunk_field::light_sections, 7,
                      encode_light_sections(canonical))) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            fmt::format("chunk payload exceeds the {} byte record limit",
                        rwf::max_record_bytes))};
    }
    return out;
}

auto decode_chunk(std::span<const std::byte> payload,
                  const palette_table *palette) -> result<chunk> {
    std::array<std::span<const std::byte>, highest_field_id + 1> fields{};
    std::array<bool, highest_field_id + 1> present{};
    cursor in{payload};
    std::size_t field_count{0};
    while (in.remaining() > 0) {
        if (in.remaining() < field_header_bytes) {
            return parse_failure("chunk payload ends inside a field header");
        }
        const auto id{in.u16()};
        const auto type{in.u8()};
        static_cast<void>(in.u8());
        const auto size{in.u32()};
        const auto crc{in.u32()};
        static_cast<void>(in.u32());
        if (in.failed()) {
            return parse_failure("chunk payload ends inside a field header");
        }
        if (size > rwf::max_record_bytes || type > 8) {
            return parse_failure(fmt::format(
                "field {} declares {} bytes (type {})", id, size, type));
        }
        const auto data{in.bytes(size)};
        if (in.failed()) {
            return parse_failure(fmt::format("field {} is truncated", id));
        }
        if (data::crc32c(data) != crc) {
            return parse_failure(fmt::format("field {} checksum mismatch", id));
        }
        const auto padding{(8 - (size % 8)) % 8};
        if (padding != 0) {
            static_cast<void>(in.bytes(padding));
            if (in.failed()) {
                return parse_failure(
                    fmt::format("field {} padding is truncated", id));
            }
        }
        if (id == 0 || id > highest_field_id) {
            ++field_count;
            if (field_count > 64) {
                return parse_failure("chunk payload has too many fields");
            }
            continue;
        }
        if (present[id]) {
            return parse_failure(fmt::format("field {} appears twice", id));
        }
        present[id] = true;
        fields[id] = data;
        ++field_count;
        if (field_count > 64) {
            return parse_failure("chunk payload has too many fields");
        }
    }
    const auto inline_sections{
        present[static_cast<std::size_t>(chunk_field::sections_inline)]};
    const auto id_sections{
        present[static_cast<std::size_t>(chunk_field::sections)]};
    if (inline_sections && id_sections) {
        return parse_failure(
            "chunk payload carries both sections and sections_inline");
    }
    if (!present[static_cast<std::size_t>(chunk_field::status)] ||
        (!inline_sections && !id_sections)) {
        return parse_failure("chunk payload is missing status or sections");
    }

    chunk out{};
    out.format_version = chunk_format_version;
    // Older (pre-y_pos) v1 payloads did not carry it; the overworld value was
    // implicit. New writers always emit the field.
    out.y_pos = min_section_y;
    bool ok{true};

    {
        cursor field{fields[static_cast<std::size_t>(chunk_field::status)]};
        out.status = static_cast<chunk_status>(field.u8());
        out.last_update = static_cast<std::int64_t>(field.u64());
        out.inhabited_time = static_cast<std::int64_t>(field.u64());
        if (field.failed() || field.remaining() != 0) {
            return parse_failure("status field has the wrong size");
        }
    }
    {
        if (inline_sections) {
            cursor field{
                fields[static_cast<std::size_t>(chunk_field::sections_inline)]};
            decode_sections_inline(field, out, ok);
            if (!ok || field.failed() || field.remaining() != 0) {
                return parse_failure("inline section field is malformed");
            }
        } else {
            cursor field{
                fields[static_cast<std::size_t>(chunk_field::sections)]};
            decode_sections(field, out, ok, palette);
            if (!ok || field.failed() || field.remaining() != 0) {
                return parse_failure("section field is malformed");
            }
        }
    }
    if (present[static_cast<std::size_t>(chunk_field::heightmaps)]) {
        cursor field{fields[static_cast<std::size_t>(chunk_field::heightmaps)]};
        decode_heightmaps(field, out, ok);
        if (!ok || field.failed() || field.remaining() != 0) {
            return parse_failure("heightmap field is malformed");
        }
    }
    if (present[static_cast<std::size_t>(chunk_field::ticks)]) {
        cursor field{fields[static_cast<std::size_t>(chunk_field::ticks)]};
        decode_ticks(field, out, ok);
        if (!ok || field.failed() || field.remaining() != 0) {
            return parse_failure("tick field is malformed");
        }
    }
    if (present[static_cast<std::size_t>(chunk_field::block_entities)]) {
        cursor field{
            fields[static_cast<std::size_t>(chunk_field::block_entities)]};
        decode_block_entities(field, out, ok);
        if (!ok || field.failed() || field.remaining() != 0) {
            return parse_failure("block entity field is malformed");
        }
    }
    if (present[static_cast<std::size_t>(chunk_field::structures)]) {
        out.structures = copy_bytes(
            fields[static_cast<std::size_t>(chunk_field::structures)]);
    }
    if (present[static_cast<std::size_t>(chunk_field::post_processing)]) {
        cursor field{
            fields[static_cast<std::size_t>(chunk_field::post_processing)]};
        decode_post_processing(field, out, ok);
        if (!ok || field.failed() || field.remaining() != 0) {
            return parse_failure("post-processing field is malformed");
        }
    }
    if (present[static_cast<std::size_t>(chunk_field::blending)]) {
        out.blending =
            copy_bytes(fields[static_cast<std::size_t>(chunk_field::blending)]);
    }
    if (present[static_cast<std::size_t>(chunk_field::extra)]) {
        out.extra =
            copy_bytes(fields[static_cast<std::size_t>(chunk_field::extra)]);
    }
    if (present[static_cast<std::size_t>(chunk_field::y_pos)]) {
        cursor field{fields[static_cast<std::size_t>(chunk_field::y_pos)]};
        out.y_pos = field.i32();
        if (field.failed() || field.remaining() != 0) {
            return parse_failure("y_pos field has the wrong size");
        }
    }
    if (present[static_cast<std::size_t>(chunk_field::light_sections)]) {
        cursor field{
            fields[static_cast<std::size_t>(chunk_field::light_sections)]};
        decode_light_sections(field, out, ok);
        if (!ok || field.failed() || field.remaining() != 0) {
            return parse_failure("light section field is malformed");
        }
    }
    if (auto valid{validate(out)}; !valid) {
        return std::unexpected{valid.error()};
    }
    return out;
}

namespace {

/// Storage-level representation is not semantic: vanilla uses a minimum block
/// palette width of 4 bits, while RWF accepts any width the palette needs.
/// Hashing normalises every palette section to `bits_for_palette(size)` so an
/// imported vanilla chunk and its re-exported copy compare equal.
[[nodiscard]] auto normalize_packing(chunk &value) -> status {
    const auto normalize = [](section_kind &kind, std::uint8_t &bits,
                              std::vector<std::uint64_t> &data,
                              std::size_t palette_size,
                              std::size_t entries) -> status {
        if (palette_size == 1) {
            kind = section_kind::single_value;
            bits = 0;
            data.clear();
            return status_ok();
        }
        if (palette_size == 0) {
            return status_ok();
        }
        const auto target{bits_for_palette(palette_size)};
        if (target == bits) {
            return status_ok();
        }
        std::vector<std::uint32_t> indices(entries);
        if (auto decoded{unpack_palette_indices(data, bits, entries,
                                                palette_size, indices)};
            !decoded) {
            return decoded;
        }
        if (auto packed{
                pack_palette_indices(indices, target, palette_size, data)};
            !packed) {
            return packed;
        }
        bits = target;
        return status_ok();
    };
    for (auto &section : value.sections) {
        if (section.kind == section_kind::palette) {
            if (auto status{normalize(section.kind, section.bits_per_entry,
                                      section.data, section.palette.size(),
                                      section_volume)};
                !status) {
                return status;
            }
        }
        if (section.biome_kind == section_kind::palette) {
            if (auto status{
                    normalize(section.biome_kind, section.biome_bits_per_entry,
                              section.biome_data, section.biome_palette.size(),
                              biome_volume)};
                !status) {
                return status;
            }
        }
    }
    return status_ok();
}

} // namespace

auto semantic_hash(const chunk &value) -> result<std::string> {
    chunk normalized{value};
    if (auto status{normalize_packing(normalized)}; !status) {
        return std::unexpected{status.error()};
    }
    auto payload{encode_chunk(normalized)};
    if (!payload) {
        return std::unexpected{payload.error()};
    }
    return data::to_hex(data::blake3(*payload));
}

} // namespace refrain::world
