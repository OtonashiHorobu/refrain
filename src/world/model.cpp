// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/world/model.hpp>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::world {
namespace {

[[nodiscard]] auto
fail(error_code code, std::string message,
     std::source_location location = std::source_location::current())
    -> std::unexpected<error> {
    return std::unexpected{make_error(code, std::move(message), location)};
}

[[nodiscard]] auto is_identifier_char(char value) noexcept -> bool {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_' || value == '-' ||
           value == '.' || value == ':' || value == '/';
}

[[nodiscard]] auto is_property_char(char value) noexcept -> bool {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_' || value == '-' ||
           value == '.' || value == '+';
}

[[nodiscard]] auto trim(std::string_view text) noexcept -> std::string_view {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] auto validate_identifier(std::string_view text,
                                       std::string_view what,
                                       std::string_view source) -> status {
    if (text.empty()) {
        return fail(
            error_code::parse_error,
            fmt::format("block state {} is empty in '{}'", what, source));
    }
    for (const char value : text) {
        if (!is_identifier_char(value)) {
            return fail(
                error_code::parse_error,
                fmt::format("block state {} '{}' has an invalid character in "
                            "'{}'",
                            what, text, source));
        }
    }
    return status_ok();
}

[[nodiscard]] auto validate_property(std::string_view text,
                                     std::string_view what,
                                     std::string_view source) -> status {
    if (text.empty()) {
        return fail(
            error_code::parse_error,
            fmt::format("block state {} is empty in '{}'", what, source));
    }
    for (const char value : text) {
        if (!is_property_char(value)) {
            return fail(
                error_code::parse_error,
                fmt::format("block state {} '{}' has an invalid character in "
                            "'{}'",
                            what, text, source));
        }
    }
    return status_ok();
}

[[nodiscard]] auto validate_section(const section_record &section) -> status {
    const auto where = fmt::format("section y={}", section.y);
    if (section.kind == section_kind::single_value) {
        if (section.bits_per_entry != 0 || !section.data.empty()) {
            return fail(error_code::parse_error,
                        fmt::format("{}: single-value blocks must have no "
                                    "bit width or packed data",
                                    where));
        }
        if (section.palette.size() != 1) {
            return fail(error_code::parse_error,
                        fmt::format("{}: single-value blocks need exactly "
                                    "one palette entry (got {})",
                                    where, section.palette.size()));
        }
    } else if (section.kind == section_kind::palette) {
        if (section.bits_per_entry == 0 || section.bits_per_entry > 32) {
            return fail(error_code::parse_error,
                        fmt::format("{}: invalid block bit width {}", where,
                                    section.bits_per_entry));
        }
        if (section.palette.empty() ||
            section.palette.size() > max_palette_entries) {
            return fail(error_code::parse_error,
                        fmt::format("{}: block palette size {} out of "
                                    "range",
                                    where, section.palette.size()));
        }
        const auto capacity = section.bits_per_entry >= 32
                                  ? max_palette_entries
                                  : (std::size_t{1} << static_cast<unsigned>(
                                         section.bits_per_entry));
        if (section.palette.size() > capacity) {
            return fail(error_code::parse_error,
                        fmt::format("{}: block palette {} exceeds {} bits",
                                    where, section.palette.size(),
                                    section.bits_per_entry));
        }
        const auto expected{
            packed_word_count(section.bits_per_entry, section_volume)};
        if (section.data.size() != expected) {
            return fail(error_code::parse_error,
                        fmt::format("{}: block data has {} words, "
                                    "expected {}",
                                    where, section.data.size(), expected));
        }
    } else {
        return fail(error_code::parse_error,
                    fmt::format("{}: unknown block section kind {}", where,
                                static_cast<unsigned>(section.kind)));
    }

    if (section.block_count > section_volume) {
        return fail(error_code::parse_error,
                    fmt::format("{}: block count {} exceeds {}", where,
                                section.block_count, section_volume));
    }
    for (const auto &entry : section.palette) {
        if (entry.key.size() > 65535) {
            return fail(error_code::parse_error,
                        fmt::format("{}: block state key is {} bytes (limit "
                                    "65535)",
                                    where, entry.key.size()));
        }
        const auto parsed{parse_block_state_key(entry.key)};
        if (!parsed) {
            return fail(error_code::parse_error,
                        fmt::format("{}: {}", where, parsed.error().message));
        }
        if (format_block_state_key(*parsed) != entry.key) {
            return fail(error_code::parse_error,
                        fmt::format("{}: block state '{}' is not in "
                                    "canonical form",
                                    where, entry.key));
        }
    }

    if (section.biome_kind == section_kind::single_value) {
        if (section.biome_bits_per_entry != 0 || !section.biome_data.empty()) {
            return fail(error_code::parse_error,
                        fmt::format("{}: single-value biomes must have no "
                                    "bit width or packed data",
                                    where));
        }
        if (section.biome_palette.size() != 1) {
            return fail(error_code::parse_error,
                        fmt::format("{}: single-value biomes need exactly "
                                    "one palette entry (got {})",
                                    where, section.biome_palette.size()));
        }
    } else if (section.biome_kind == section_kind::palette) {
        if (section.biome_bits_per_entry == 0 ||
            section.biome_bits_per_entry > 32) {
            return fail(error_code::parse_error,
                        fmt::format("{}: invalid biome bit width {}", where,
                                    section.biome_bits_per_entry));
        }
        if (section.biome_palette.empty() ||
            section.biome_palette.size() > max_palette_entries) {
            return fail(error_code::parse_error,
                        fmt::format("{}: biome palette size {} out of "
                                    "range",
                                    where, section.biome_palette.size()));
        }
        const auto capacity = section.biome_bits_per_entry >= 32
                                  ? max_palette_entries
                                  : (std::size_t{1} << static_cast<unsigned>(
                                         section.biome_bits_per_entry));
        if (section.biome_palette.size() > capacity) {
            return fail(error_code::parse_error,
                        fmt::format("{}: biome palette {} exceeds {} bits",
                                    where, section.biome_palette.size(),
                                    section.biome_bits_per_entry));
        }
        const auto expected{
            packed_word_count(section.biome_bits_per_entry, biome_volume)};
        if (section.biome_data.size() != expected) {
            return fail(error_code::parse_error,
                        fmt::format("{}: biome data has {} words, "
                                    "expected {}",
                                    where, section.biome_data.size(),
                                    expected));
        }
    } else {
        return fail(error_code::parse_error,
                    fmt::format("{}: unknown biome section kind {}", where,
                                static_cast<unsigned>(section.biome_kind)));
    }
    for (const auto &key : section.biome_palette) {
        if (key.size() > 65535) {
            return fail(error_code::parse_error,
                        fmt::format("{}: biome key is {} bytes (limit 65535)",
                                    where, key.size()));
        }
        if (key.empty() || key.find_first_of(" \t[]") != std::string::npos) {
            return fail(error_code::parse_error,
                        fmt::format("{}: invalid biome key '{}'", where, key));
        }
    }

    return status_ok();
}

[[nodiscard]] auto validate_light_section(const light_section &light)
    -> status {
    const auto where = fmt::format("light section y={}", light.y);
    if (light.block_light.empty() && light.sky_light.empty()) {
        return fail(error_code::parse_error,
                    fmt::format("{}: light section has no light data", where));
    }
    for (const auto *array : {&light.block_light, &light.sky_light}) {
        if (!array->empty() && array->size() != light_nibble_bytes) {
            return fail(error_code::parse_error,
                        fmt::format("{}: light array has {} bytes, "
                                    "expected 0 or {}",
                                    where, array->size(), light_nibble_bytes));
        }
    }
    return status_ok();
}

} // namespace

auto chunk_pos::key() const -> std::string {
    return fmt::format("{}.{}", x, z);
}

auto format_chunk_pos(const chunk_pos &pos) -> std::string {
    return fmt::format("({}, {})", pos.x, pos.z);
}

auto parse_chunk_key(std::string_view key) -> result<chunk_pos> {
    const auto dot{key.find('.')};
    if (dot == std::string_view::npos ||
        key.find('.', dot + 1) != std::string_view::npos) {
        return fail(error_code::parse_error,
                    fmt::format("invalid chunk key '{}': expected 'x.z'", key));
    }
    const auto parse_component = [&](std::string_view text,
                                     std::int32_t &out) -> status {
        if (text.empty() || text.front() == '+') {
            return fail(
                error_code::parse_error,
                fmt::format("invalid chunk key '{}': bad integer", key));
        }
        const auto *first = text.data();
        const auto *last = text.data() + text.size();
        const auto parsed{std::from_chars(first, last, out)};
        if (parsed.ec != std::errc{} || parsed.ptr != last) {
            return fail(
                error_code::parse_error,
                fmt::format("invalid chunk key '{}': bad integer", key));
        }
        return status_ok();
    };
    chunk_pos pos{};
    if (auto status{parse_component(key.substr(0, dot), pos.x)}; !status) {
        return std::unexpected{std::move(status.error())};
    }
    if (auto status{parse_component(key.substr(dot + 1), pos.z)}; !status) {
        return std::unexpected{std::move(status.error())};
    }
    return pos;
}

auto parse_block_state_key(std::string_view text) -> result<block_state_key> {
    if (text.empty()) {
        return fail(error_code::parse_error, "block state key is empty");
    }
    const auto open{text.find('[')};
    const auto name{text.substr(0, open)};
    if (auto status{validate_identifier(name, "name", text)}; !status) {
        return std::unexpected{std::move(status.error())};
    }
    block_state_key key;
    key.name = std::string{name};
    if (open == std::string_view::npos) {
        return key;
    }
    if (text.back() != ']' || text.find(']') != text.size() - 1) {
        return fail(error_code::parse_error,
                    fmt::format("block state '{}' has an unterminated or "
                                "trailing property list",
                                text));
    }
    auto inside{text.substr(open + 1, text.size() - open - 2)};
    if (trim(inside).empty()) {
        return fail(error_code::parse_error,
                    fmt::format("block state '{}' has an empty property "
                                "list",
                                text));
    }
    std::set<std::string, std::less<>> seen;
    while (true) {
        const auto comma{inside.find(',')};
        const auto item{trim(inside.substr(0, comma))};
        const auto equals{item.find('=')};
        if (equals == std::string_view::npos) {
            return fail(error_code::parse_error,
                        fmt::format("block state '{}': property '{}' has "
                                    "no '='",
                                    text, item));
        }
        const auto property{trim(item.substr(0, equals))};
        const auto value{trim(item.substr(equals + 1))};
        if (auto status{validate_property(property, "property name", text)};
            !status) {
            return std::unexpected{std::move(status.error())};
        }
        if (auto status{validate_property(value, "property value", text)};
            !status) {
            return std::unexpected{std::move(status.error())};
        }
        if (!seen.emplace(property).second) {
            return fail(error_code::parse_error,
                        fmt::format("block state '{}': duplicate property "
                                    "'{}'",
                                    text, property));
        }
        key.properties.emplace_back(std::string{property}, std::string{value});
        if (comma == std::string_view::npos) {
            break;
        }
        inside.remove_prefix(comma + 1);
    }
    std::sort(key.properties.begin(), key.properties.end(),
              [](const auto &left, const auto &right) {
                  return left.first < right.first;
              });
    return key;
}

auto format_block_state_key(const block_state_key &key) -> std::string {
    if (key.properties.empty()) {
        return key.name;
    }
    std::vector<std::string> parts;
    parts.reserve(key.properties.size());
    for (const auto &[name, value] : key.properties) {
        parts.push_back(fmt::format("{}={}", name, value));
    }
    return fmt::format("{}[{}]", key.name, fmt::join(parts, ","));
}

auto canonical_block_state_key(
    std::string_view name,
    std::span<const std::pair<std::string, std::string>> properties)
    -> result<std::string> {
    block_state_key key;
    key.name = std::string{name};
    if (auto status{validate_identifier(name, "name", name)}; !status) {
        return std::unexpected{std::move(status.error())};
    }
    std::set<std::string, std::less<>> seen;
    key.properties.reserve(properties.size());
    for (const auto &[property_name, value] : properties) {
        if (auto status{
                validate_property(property_name, "property name", name)};
            !status) {
            return std::unexpected{std::move(status.error())};
        }
        if (auto status{validate_property(value, "property value", name)};
            !status) {
            return std::unexpected{std::move(status.error())};
        }
        if (!seen.emplace(property_name).second) {
            return fail(error_code::parse_error,
                        fmt::format("duplicate block state property '{}' "
                                    "for '{}'",
                                    property_name, name));
        }
        key.properties.emplace_back(property_name, value);
    }
    std::sort(key.properties.begin(), key.properties.end(),
              [](const auto &left, const auto &right) {
                  return left.first < right.first;
              });
    return format_block_state_key(key);
}

auto bits_for_palette(std::size_t palette_size) -> std::uint8_t {
    if (palette_size <= 1) {
        return 0;
    }
    std::uint8_t bits{1};
    std::size_t capacity{2};
    while (capacity < palette_size && bits < 32) {
        capacity <<= 1;
        ++bits;
    }
    return bits;
}

auto packed_word_count(std::uint8_t bits_per_entry, std::size_t entries)
    -> std::size_t {
    if (bits_per_entry == 0 || bits_per_entry > 32) {
        return 0;
    }
    const auto per_long{std::size_t{64} / bits_per_entry};
    return (entries + per_long - 1) / per_long;
}

auto unpack_palette_indices(std::span<const std::uint64_t> words,
                            std::uint8_t bits_per_entry, std::size_t entries,
                            std::size_t palette_size,
                            std::span<std::uint32_t> out) -> status {
    if (bits_per_entry == 0 || bits_per_entry > 32) {
        return fail(
            error_code::invalid_argument,
            fmt::format("invalid palette bit width {}", bits_per_entry));
    }
    if (out.size() != entries) {
        return fail(error_code::invalid_argument,
                    fmt::format("palette output has {} entries, expected "
                                "{}",
                                out.size(), entries));
    }
    const auto expected{packed_word_count(bits_per_entry, entries)};
    if (words.size() != expected) {
        return fail(error_code::parse_error,
                    fmt::format("packed palette has {} words, expected {}",
                                words.size(), expected));
    }
    const auto per_long{std::size_t{64} / bits_per_entry};
    const auto mask{bits_per_entry >= 32
                        ? std::uint64_t{0xFFFF'FFFFULL}
                        : ((std::uint64_t{1} << bits_per_entry) - 1)};
    for (std::size_t index{0}; index < entries; ++index) {
        const auto word{words[index / per_long]};
        const auto shift{(index % per_long) * bits_per_entry};
        const auto value{static_cast<std::uint32_t>((word >> shift) & mask)};
        if (value >= palette_size) {
            return fail(error_code::parse_error,
                        fmt::format("palette index {} at entry {} is "
                                    "outside a {} entry palette",
                                    value, index, palette_size));
        }
        out[index] = value;
    }
    return status_ok();
}

auto pack_palette_indices(std::span<const std::uint32_t> indices,
                          std::uint8_t bits_per_entry, std::size_t palette_size,
                          std::vector<std::uint64_t> &out) -> status {
    if (bits_per_entry == 0 || bits_per_entry > 32) {
        return fail(
            error_code::invalid_argument,
            fmt::format("invalid palette bit width {}", bits_per_entry));
    }
    const auto mask{bits_per_entry >= 32
                        ? std::uint64_t{0xFFFF'FFFFULL}
                        : ((std::uint64_t{1} << bits_per_entry) - 1)};
    out.assign(packed_word_count(bits_per_entry, indices.size()), 0);
    const auto per_long{std::size_t{64} / bits_per_entry};
    for (std::size_t index{0}; index < indices.size(); ++index) {
        const auto value{indices[index]};
        if (value >= palette_size) {
            return fail(error_code::parse_error,
                        fmt::format("palette index {} at entry {} is "
                                    "outside a {} entry palette",
                                    value, index, palette_size));
        }
        const auto shift{(index % per_long) * bits_per_entry};
        out[index / per_long] |= (static_cast<std::uint64_t>(value) & mask)
                                 << shift;
    }
    return status_ok();
}

auto validate(const chunk &value) -> status {
    if (value.format_version != chunk_format_version) {
        return fail(error_code::version_mismatch,
                    fmt::format("chunk format version {} is not supported "
                                "(expected {})",
                                value.format_version, chunk_format_version));
    }
    if (static_cast<std::uint8_t>(value.status) >
        static_cast<std::uint8_t>(chunk_status::full)) {
        return fail(error_code::parse_error,
                    fmt::format("unknown chunk status {}",
                                static_cast<unsigned>(value.status)));
    }
    if (value.y_pos < min_section_y || value.y_pos > max_section_y) {
        return fail(error_code::parse_error,
                    fmt::format("chunk yPos={} is outside [{}, {}]",
                                value.y_pos, min_section_y, max_section_y));
    }
    if (value.sections.size() > max_sections_per_chunk) {
        return fail(error_code::parse_error,
                    fmt::format("chunk has {} sections (limit {})",
                                value.sections.size(), max_sections_per_chunk));
    }
    std::int32_t previous{min_section_y - 1};
    for (const auto &section : value.sections) {
        if (section.y < value.y_pos) {
            return fail(error_code::parse_error,
                        fmt::format("section y={} is below the chunk yPos={}",
                                    section.y, value.y_pos));
        }
        if (section.y < min_section_y || section.y > max_section_y) {
            return fail(error_code::parse_error,
                        fmt::format("section y={} is outside [{}, {}]",
                                    section.y, min_section_y, max_section_y));
        }
        if (section.y <= previous) {
            return fail(error_code::parse_error,
                        fmt::format("section y={} is duplicated or out of "
                                    "order",
                                    section.y));
        }
        previous = section.y;
        if (auto status{validate_section(section)}; !status) {
            return std::unexpected{std::move(status.error())};
        }
    }

    // Light layers are independent of block sections: the lowest light
    // section is one below the lowest block section and the highest is one
    // above the top block section. A light layer without a block section is
    // the vanilla padding section.
    if (value.light_sections.size() > max_light_sections) {
        return fail(error_code::parse_error,
                    fmt::format("chunk has {} light sections (limit {})",
                                value.light_sections.size(),
                                max_light_sections));
    }
    if (!value.light_sections.empty() && value.sections.empty()) {
        return fail(error_code::parse_error,
                    "light sections require at least one block section");
    }
    const auto min_light_y{value.y_pos - 1};
    const auto max_light_y{
        value.sections.empty() ? value.y_pos : value.sections.back().y + 1};
    std::int32_t previous_light{0};
    bool first_light{true};
    for (const auto &light : value.light_sections) {
        if (!first_light && light.y <= previous_light) {
            return fail(error_code::parse_error,
                        fmt::format("light section y={} is duplicated or out "
                                    "of order",
                                    light.y));
        }
        first_light = false;
        previous_light = light.y;
        if (light.y < min_light_y || light.y > max_light_y) {
            return fail(error_code::parse_error,
                        fmt::format("light section y={} is outside [{}, {}]",
                                    light.y, min_light_y, max_light_y));
        }
        const bool padding{light.y == min_light_y || light.y == max_light_y};
        if (!padding) {
            const bool has_section{
                std::any_of(value.sections.begin(), value.sections.end(),
                            [&light](const section_record &section) {
                                return section.y == light.y;
                            })};
            if (!has_section) {
                return fail(error_code::parse_error,
                            fmt::format("light section y={} has no block "
                                        "section",
                                        light.y));
            }
        }
        if (auto status{validate_light_section(light)}; !status) {
            return std::unexpected{std::move(status.error())};
        }
    }

    if (value.heightmaps.size() > max_heightmaps) {
        return fail(error_code::parse_error,
                    fmt::format("chunk has {} heightmaps (limit {})",
                                value.heightmaps.size(), max_heightmaps));
    }
    std::set<std::string, std::less<>> heightmap_names;
    for (const auto &heightmap : value.heightmaps) {
        if (heightmap.name.empty()) {
            return fail(error_code::parse_error, "heightmap has an empty name");
        }
        if (!heightmap_names.emplace(heightmap.name).second) {
            return fail(
                error_code::parse_error,
                fmt::format("duplicate heightmap '{}'", heightmap.name));
        }
        if (heightmap.bits_per_entry == 0 || heightmap.bits_per_entry > 32) {
            return fail(error_code::parse_error,
                        fmt::format("heightmap '{}' has invalid bit width "
                                    "{}",
                                    heightmap.name, heightmap.bits_per_entry));
        }
        const auto expected{
            packed_word_count(heightmap.bits_per_entry, heightmap_columns)};
        if (heightmap.data.size() != expected) {
            return fail(error_code::parse_error,
                        fmt::format("heightmap '{}' has {} words, "
                                    "expected {}",
                                    heightmap.name, heightmap.data.size(),
                                    expected));
        }
    }

    if (value.block_ticks.size() > max_ticks ||
        value.fluid_ticks.size() > max_ticks) {
        return fail(error_code::parse_error,
                    fmt::format("chunk tick lists exceed {}", max_ticks));
    }
    for (const auto *ticks : {&value.block_ticks, &value.fluid_ticks}) {
        for (const auto &tick : *ticks) {
            if (tick.id.empty()) {
                return fail(error_code::parse_error,
                            "saved tick has an empty block id");
            }
            if (tick.id.size() > 65535) {
                return fail(error_code::parse_error,
                            fmt::format("saved tick id is {} bytes (limit "
                                        "65535)",
                                        tick.id.size()));
            }
        }
    }
    if (value.block_entities.size() > max_block_entities) {
        return fail(error_code::parse_error,
                    fmt::format("chunk has {} block entities (limit {})",
                                value.block_entities.size(),
                                max_block_entities));
    }
    for (const auto &entity : value.block_entities) {
        if (entity.size() > max_payload_bytes) {
            return fail(error_code::parse_error,
                        fmt::format("block entity payload is {} bytes "
                                    "(limit {})",
                                    entity.size(), max_payload_bytes));
        }
    }
    if (value.post_processing.size() > max_sections_per_chunk) {
        return fail(error_code::parse_error,
                    fmt::format("post-processing has {} lists (limit {})",
                                value.post_processing.size(),
                                max_sections_per_chunk));
    }
    for (const auto &list : value.post_processing) {
        if (list.size() > max_post_processing_entries) {
            return fail(error_code::parse_error,
                        fmt::format("post-processing list has {} entries "
                                    "(limit {})",
                                    list.size(), max_post_processing_entries));
        }
    }
    for (const auto &blob :
         {&value.structures, &value.blending, &value.extra}) {
        if (blob->size() > max_payload_bytes) {
            return fail(error_code::parse_error,
                        fmt::format("chunk payload is {} bytes (limit {})",
                                    blob->size(), max_payload_bytes));
        }
    }
    if (value.extensions.size() > max_extension_count) {
        return fail(error_code::parse_error,
                    fmt::format("chunk has {} extensions (limit {})",
                                value.extensions.size(), max_extension_count));
    }
    for (const auto &extension : value.extensions) {
        if (extension.size() > max_payload_bytes) {
            return fail(error_code::parse_error,
                        fmt::format("extension payload is {} bytes "
                                    "(limit {})",
                                    extension.size(), max_payload_bytes));
        }
    }
    return status_ok();
}

auto section_at(const chunk &value, std::int32_t section_y)
    -> const section_record * {
    for (const auto &section : value.sections) {
        if (section.y == section_y) {
            return &section;
        }
        if (section.y > section_y) {
            break;
        }
    }
    return nullptr;
}

auto light_at(const chunk &value, std::int32_t section_y)
    -> const light_section * {
    for (const auto &light : value.light_sections) {
        if (light.y == section_y) {
            return &light;
        }
        if (light.y > section_y) {
            break;
        }
    }
    return nullptr;
}

auto chunk_status_name(chunk_status status) -> std::string_view {
    switch (status) {
    case chunk_status::empty:
        return "empty";
    case chunk_status::structure_starts:
        return "structure_starts";
    case chunk_status::structure_references:
        return "structure_references";
    case chunk_status::biomes:
        return "biomes";
    case chunk_status::terrain:
        return "terrain";
    case chunk_status::features:
        return "features";
    case chunk_status::initialize_light:
        return "initialize_light";
    case chunk_status::light:
        return "light";
    case chunk_status::spawn:
        return "spawn";
    case chunk_status::full:
        return "full";
    }
    return "unknown";
}

auto parse_chunk_status(std::string_view name) -> result<chunk_status> {
    constexpr std::string_view prefix{"minecraft:"};
    if (name.starts_with(prefix)) {
        name.remove_prefix(prefix.size());
    }
    static constexpr std::array<std::pair<std::string_view, chunk_status>, 10>
        names{{{"empty", chunk_status::empty},
               {"structure_starts", chunk_status::structure_starts},
               {"structure_references", chunk_status::structure_references},
               {"biomes", chunk_status::biomes},
               {"terrain", chunk_status::terrain},
               {"features", chunk_status::features},
               {"initialize_light", chunk_status::initialize_light},
               {"light", chunk_status::light},
               {"spawn", chunk_status::spawn},
               {"full", chunk_status::full}}};
    for (const auto &[candidate, value] : names) {
        if (candidate == name) {
            return value;
        }
    }
    return fail(error_code::parse_error,
                fmt::format("unknown chunk status '{}'", name));
}

} // namespace refrain::world
