// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/world/anvil.hpp>

#include <refrain/core/io.hpp>
#include <refrain/core/jobs.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/data/path.hpp>
#include <refrain/platform/file.hpp>
#include <refrain/platform/process.hpp>
#include <refrain/platform/random_access_file.hpp>
#include <refrain/world/chunk_codec.hpp>
#include <refrain/world/model.hpp>
#include <refrain/world/rwf.hpp>
#include <refrain/world/store.hpp>

#include "anvil_lz4.hpp"

#include <fmt/format.h>

#include <zdict.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace refrain::world::anvil {
namespace {

using data::join_path;

namespace nbt = data::nbt;

using nbt::tag_type;
using nbt::value;

constexpr std::size_t anvil_header_bytes = 8192;
constexpr std::size_t anvil_sector_bytes = 4096;
constexpr std::size_t anvil_slots = 1024;
constexpr std::size_t max_region_bytes = 1ULL << 30U;
constexpr std::size_t max_anvil_chunk_bytes = 1ULL << 20U;
// An LZ4/zlib payload may expand well beyond the 1 MiB compressed-sector
// limit. 16 MiB is far above vanilla chunk NBT while keeping a malicious
// stream from allocating unbounded memory before the NBT parser sees it.
constexpr std::size_t max_anvil_decompressed_bytes = 16ULL << 20U;
/// 26.3 external chunk payloads (`.mcc`) live outside the 255-sector limit;
/// 64 MiB is far above any vanilla chunk we have observed and keeps a corrupt
/// file from driving an unbounded allocation.
constexpr std::size_t max_anvil_external_bytes = 64ULL << 20U;
constexpr std::uint8_t anvil_external_stream_flag = 0x80;
constexpr std::uint8_t anvil_compression_mask = 0x7F;
constexpr std::uint32_t max_anvil_internal_sectors = 255;
/// Total decoded-NBT budget for one region read. The per-chunk limit alone
/// would still let a crafted region hold 1024 × 16 MiB of DOM at once.
constexpr std::size_t max_anvil_region_document_bytes = 256ULL << 20U;

/// Approximate heap footprint of a decoded NBT tree (containers/keys included),
/// clamped to `limit`. Used as a region-level allocation budget without
/// re-serializing the document.
[[nodiscard]] auto nbt_footprint(const value &node, std::size_t limit)
    -> std::size_t {
    switch (node.type()) {
    case tag_type::byte:
    case tag_type::short_:
    case tag_type::int_:
    case tag_type::long_:
    case tag_type::float_:
    case tag_type::double_:
        return 16;
    case tag_type::string:
    case tag_type::byte_array:
        return 32 + node.size();
    case tag_type::int_array:
        return 32 + node.size() * sizeof(std::int32_t);
    case tag_type::long_array:
        return 32 + node.size() * sizeof(std::int64_t);
    case tag_type::compound: {
        std::size_t total{32};
        for (const auto &[key, child] : *node.as_compound()) {
            if (total > limit) {
                return limit + 1;
            }
            total += 32 + key.size() + nbt_footprint(child, limit - total);
            if (total > limit) {
                return limit + 1;
            }
        }
        return total;
    }
    case tag_type::list: {
        std::size_t total{32};
        for (const auto &child : node.as_list()->items) {
            if (total > limit) {
                return limit + 1;
            }
            total += 16 + nbt_footprint(child, limit - total);
            if (total > limit) {
                return limit + 1;
            }
        }
        return total;
    }
    case tag_type::end:
        break;
    }
    return 32;
}

[[nodiscard]] auto fail(error_code code, std::string message)
    -> std::unexpected<error> {
    return std::unexpected{make_error(code, std::move(message))};
}

[[nodiscard]] auto parse_failure(std::string message)
    -> std::unexpected<error> {
    return fail(error_code::parse_error, std::move(message));
}

[[nodiscard]] auto unsupported(std::string message) -> std::unexpected<error> {
    return fail(error_code::unsupported, std::move(message));
}

[[nodiscard]] auto be32(std::span<const std::byte> data, std::size_t offset)
    -> std::uint32_t {
    return (static_cast<std::uint32_t>(data[offset]) << 24U) |
           (static_cast<std::uint32_t>(data[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(data[offset + 2]) << 8U) |
           static_cast<std::uint32_t>(data[offset + 3]);
}

void put_be32(std::span<std::byte> data, std::size_t offset,
              std::uint32_t value) {
    data[offset] = static_cast<std::byte>((value >> 24U) & 0xFFU);
    data[offset + 1] = static_cast<std::byte>((value >> 16U) & 0xFFU);
    data[offset + 2] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    data[offset + 3] = static_cast<std::byte>(value & 0xFFU);
}

[[nodiscard]] auto zlib_compress(std::span<const std::byte> input)
    -> result<std::vector<std::byte>> {
    uLongf capacity{compressBound(static_cast<uLong>(input.size()))};
    std::vector<std::byte> out(capacity);
    const auto status{compress2(reinterpret_cast<Bytef *>(out.data()),
                                &capacity,
                                reinterpret_cast<const Bytef *>(input.data()),
                                static_cast<uLong>(input.size()), 6)};
    if (status != Z_OK) {
        return fail(error_code::io_error,
                    fmt::format("zlib compression failed with {}", status));
    }
    out.resize(capacity);
    return out;
}

[[nodiscard]] auto parse_region_name(std::string_view name,
                                     std::string_view extension)
    -> std::optional<std::pair<std::int32_t, std::int32_t>> {
    constexpr std::string_view prefix{"r."};
    if (!name.starts_with(prefix) || !name.ends_with(extension) ||
        name.size() <= prefix.size() + extension.size()) {
        return std::nullopt;
    }
    const auto stem{name.substr(prefix.size(), name.size() - prefix.size() -
                                                   extension.size())};
    const auto dot{stem.find('.')};
    if (dot == std::string_view::npos) {
        return std::nullopt;
    }
    const auto parse =
        [](std::string_view text) -> std::optional<std::int32_t> {
        if (text.empty()) {
            return std::nullopt;
        }
        std::int32_t value{0};
        const auto parsed{
            std::from_chars(text.data(), text.data() + text.size(), value)};
        if (parsed.ec != std::errc{} ||
            parsed.ptr != text.data() + text.size()) {
            return std::nullopt;
        }
        return value;
    };
    const auto x{parse(stem.substr(0, dot))};
    const auto z{parse(stem.substr(dot + 1))};
    if (!x || !z) {
        return std::nullopt;
    }
    // Keep `region * 32 + local` inside int32 (chunk coordinates are int32).
    constexpr std::int32_t max_region_coordinate{67108863};
    if (*x < -max_region_coordinate - 1 || *x > max_region_coordinate ||
        *z < -max_region_coordinate - 1 || *z > max_region_coordinate) {
        return std::nullopt;
    }
    return std::pair{*x, *z};
}

void set_child(value &parent, std::string key, value child) {
    static_cast<void>(parent.set(std::move(key), std::move(child)));
}

[[nodiscard]] auto child_int(const value &parent, std::string_view key)
    -> std::optional<std::int32_t> {
    if (const auto *item{parent.get(key)}; item != nullptr) {
        return item->as_int();
    }
    return std::nullopt;
}

[[nodiscard]] auto child_long(const value &parent, std::string_view key)
    -> std::optional<std::int64_t> {
    if (const auto *item{parent.get(key)}; item != nullptr) {
        return item->as_long();
    }
    return std::nullopt;
}

[[nodiscard]] auto child_string(const value &parent, std::string_view key)
    -> std::optional<std::string> {
    if (const auto *item{parent.get(key)}; item != nullptr) {
        if (const auto text{item->as_string()}) {
            return std::string{*text};
        }
    }
    return std::nullopt;
}

[[nodiscard]] auto list_of(const value &parent, std::string_view key)
    -> const nbt::list_value * {
    if (const auto *item{parent.get(key)}; item != nullptr) {
        return item->as_list();
    }
    return nullptr;
}

/// Palette entry to the canonical block state key.
///
/// 26.3 palette entries: a default state is `{"": "minecraft:stone"}` and a
/// state with properties is `{"id": "minecraft:oak_log", "properties":
/// {"axis": "y"}}`. Property values are strings.
[[nodiscard]] auto palette_key(const value &entry) -> result<std::string> {
    // A default state is the bare block id string; a state with properties is
    // `{id, properties}`. Both forms occur in 26.3 saves.
    if (const auto text{entry.as_string()}; text) {
        return canonical_block_state_key(*text, {});
    }
    const auto *compound{entry.as_compound()};
    if (compound == nullptr) {
        return parse_failure("block palette entry is not a string or compound");
    }
    std::string name;
    const value *properties_value{nullptr};
    if (const auto *id{entry.get("id")}; id != nullptr) {
        const auto text{id->as_string()};
        if (!text || text->empty()) {
            return parse_failure("block palette 'id' is not a string");
        }
        name = std::string{*text};
        properties_value = entry.get("properties");
    } else if (compound->size() == 1 && compound->contains("")) {
        const auto text{compound->at("").as_string()};
        if (!text || text->empty()) {
            return parse_failure("block palette constant is not a string");
        }
        name = std::string{*text};
    } else {
        return parse_failure("block palette entry has neither '' nor 'id'");
    }
    std::vector<std::pair<std::string, std::string>> properties;
    if (properties_value != nullptr &&
        properties_value->as_compound() != nullptr) {
        for (const auto &[key, item] : *properties_value->as_compound()) {
            const auto text{item.as_string()};
            if (!text) {
                return parse_failure(fmt::format(
                    "block palette property '{}' is not a string", key));
            }
            properties.emplace_back(key, std::string{*text});
        }
    }
    return canonical_block_state_key(name, properties);
}

[[nodiscard]] auto section_bits(std::size_t palette_size, bool blocks)
    -> std::uint8_t {
    const auto minimum{blocks ? std::uint8_t{4} : std::uint8_t{1}};
    const auto natural{bits_for_palette(palette_size)};
    return std::max(minimum, natural);
}

[[nodiscard]] auto words_for(const value *data, std::size_t palette_size,
                             bool blocks, std::uint8_t &bits,
                             std::vector<std::uint64_t> &words) -> status {
    if (data == nullptr || !data->as_long_array()) {
        return status_ok();
    }
    const auto array{*data->as_long_array()};
    const auto entries{blocks ? section_volume : biome_volume};
    const auto natural{section_bits(palette_size, blocks)};
    const auto expected{packed_word_count(natural, entries)};
    if (array.size() == expected) {
        bits = natural;
    } else {
        std::uint8_t found{0};
        for (std::uint8_t candidate{1}; candidate <= 32; ++candidate) {
            if (packed_word_count(candidate, entries) == array.size() &&
                candidate >= bits_for_palette(palette_size)) {
                found = candidate;
                break;
            }
        }
        if (found == 0) {
            return parse_failure(fmt::format(
                "packed palette has {} words for a {} entry palette ({} bits "
                "expected)",
                array.size(), palette_size, natural));
        }
        bits = found;
    }
    words.reserve(array.size());
    for (const auto word : array) {
        words.push_back(std::bit_cast<std::uint64_t>(word));
    }
    return status_ok();
}

[[nodiscard]] constexpr auto is_air_key(std::string_view key) noexcept -> bool {
    return key == "minecraft:air" || key == "minecraft:cave_air" ||
           key == "minecraft:void_air";
}

[[nodiscard]] auto read_block_states(const value &section, section_record &out)
    -> status {
    const auto *states{section.get("block_states")};
    if (states == nullptr || states->as_compound() == nullptr) {
        out.kind = section_kind::single_value;
        out.bits_per_entry = 0;
        out.palette.push_back(
            palette_entry{.key = "minecraft:air", .count = 0, .state = 0});
        out.block_count = 0;
        return status_ok();
    }
    const auto *palette{states->get("palette")};
    if (palette == nullptr || palette->as_list() == nullptr) {
        return parse_failure("section block_states has no palette list");
    }
    for (const auto &entry : palette->as_list()->items) {
        auto key{palette_key(entry)};
        if (!key) {
            return std::unexpected{key.error()};
        }
        out.palette.push_back(
            palette_entry{.key = std::move(*key), .count = 0, .state = 0});
    }
    if (out.palette.empty()) {
        out.kind = section_kind::single_value;
        out.palette.push_back(
            palette_entry{.key = "minecraft:air", .count = 0, .state = 0});
        out.block_count = 0;
        return status_ok();
    }
    const auto *data{states->get("data")};
    if (out.palette.size() == 1 && data == nullptr) {
        out.kind = section_kind::single_value;
        out.block_count =
            is_air_key(out.palette.front().key) ? 0 : section_volume;
        out.palette.front().count = out.block_count;
        return status_ok();
    }
    if (out.palette.size() > 256) {
        return unsupported(fmt::format(
            "section has {} block states (vanilla direct palette is not "
            "supported)",
            out.palette.size()));
    }
    std::uint8_t bits{0};
    if (auto status{words_for(data, out.palette.size(), true, bits, out.data)};
        !status) {
        return status;
    }
    if (out.data.empty()) {
        return parse_failure("section palette has multiple entries but no "
                             "packed data");
    }
    out.kind = section_kind::palette;
    out.bits_per_entry = bits;
    // Anvil does not persist RWF `block_count`; recompute it from the packed
    // palette so a maintenance/Anvil round-trip preserves `semantic_hash`.
    std::vector<std::uint32_t> indices(section_volume);
    if (auto unpacked{unpack_palette_indices(out.data, bits, section_volume,
                                             out.palette.size(), indices)};
        !unpacked) {
        return unpacked;
    }
    std::vector<std::uint16_t> counts(out.palette.size(), 0);
    std::uint16_t non_air{0};
    for (const auto index : indices) {
        if (index >= counts.size()) {
            return parse_failure("block palette index is out of range");
        }
        ++counts[index];
        if (!is_air_key(out.palette[index].key)) {
            ++non_air;
        }
    }
    for (std::size_t index{0}; index < out.palette.size(); ++index) {
        out.palette[index].count = counts[index];
    }
    out.block_count = non_air;
    return status_ok();
}

[[nodiscard]] auto read_biomes(const value &section, section_record &out)
    -> status {
    const auto *biomes{section.get("biomes")};
    if (biomes == nullptr || biomes->as_compound() == nullptr) {
        out.biome_kind = section_kind::single_value;
        out.biome_palette.push_back("minecraft:plains");
        return status_ok();
    }
    const auto *palette{biomes->get("palette")};
    if (palette == nullptr || palette->as_list() == nullptr) {
        return parse_failure("section biomes has no palette list");
    }
    for (const auto &entry : palette->as_list()->items) {
        const auto text{entry.as_string()};
        if (!text) {
            return parse_failure("biome palette entry is not a string");
        }
        out.biome_palette.emplace_back(*text);
    }
    if (out.biome_palette.empty()) {
        out.biome_palette.push_back("minecraft:plains");
        out.biome_kind = section_kind::single_value;
        return status_ok();
    }
    const auto *data{biomes->get("data")};
    if (out.biome_palette.size() == 1 && data == nullptr) {
        out.biome_kind = section_kind::single_value;
        return status_ok();
    }
    std::uint8_t bits{0};
    if (auto status{words_for(data, out.biome_palette.size(), false, bits,
                              out.biome_data)};
        !status) {
        return status;
    }
    if (out.biome_data.empty()) {
        return parse_failure("biome palette has multiple entries but no packed "
                             "data");
    }
    out.biome_kind = section_kind::palette;
    out.biome_bits_per_entry = bits;
    return status_ok();
}

[[nodiscard]] auto read_light(const value &section, std::string_view key,
                              std::vector<std::byte> &out) -> status {
    const value *item{section.get(key)};
    if (item == nullptr) {
        return status_ok();
    }
    const auto array{item->as_byte_array()};
    if (!array) {
        return parse_failure(
            fmt::format("light '{}' is not a byte array", key));
    }
    if (!array->empty() && array->size() != light_nibble_bytes) {
        return parse_failure(
            fmt::format("light '{}' has {} bytes (expected {})", key,
                        array->size(), light_nibble_bytes));
    }
    out.reserve(array->size());
    for (const auto byte : *array) {
        out.push_back(static_cast<std::byte>(byte));
    }
    return status_ok();
}

[[nodiscard]] auto heightmap_bits(std::size_t words) -> std::uint8_t {
    for (std::uint8_t bits{1}; bits <= 32; ++bits) {
        if (packed_word_count(bits, heightmap_columns) == words) {
            return bits;
        }
    }
    return 0;
}

[[nodiscard]] auto read_heightmaps(const value &root, chunk &out) -> status {
    const auto *heightmaps{root.get("Heightmaps")};
    if (heightmaps == nullptr) {
        return status_ok();
    }
    if (heightmaps->as_compound() == nullptr) {
        return parse_failure("Heightmaps is not a compound");
    }
    for (const auto &[name, item] : *heightmaps->as_compound()) {
        const auto array{item.as_long_array()};
        if (!array) {
            return parse_failure(fmt::format("heightmap '{}' is not a long "
                                             "array",
                                             name));
        }
        const auto bits{heightmap_bits(array->size())};
        if (bits == 0) {
            return parse_failure(fmt::format("heightmap '{}' has {} words",
                                             name, array->size()));
        }
        heightmap_record record{};
        record.name = name;
        record.bits_per_entry = bits;
        record.data.reserve(array->size());
        for (const auto word : *array) {
            record.data.push_back(std::bit_cast<std::uint64_t>(word));
        }
        out.heightmaps.push_back(std::move(record));
    }
    return status_ok();
}

[[nodiscard]] auto read_ticks(const value &root, std::string_view key,
                              std::uint8_t kind, std::vector<saved_tick> &out)
    -> status {
    const auto *list{list_of(root, key)};
    if (list == nullptr) {
        return status_ok();
    }
    for (const auto &item : list->items) {
        if (item.as_compound() == nullptr) {
            return parse_failure(
                fmt::format("{} entry is not a compound", key));
        }
        saved_tick tick{};
        tick.id = child_string(item, "i").value_or("");
        if (tick.id.empty()) {
            return parse_failure(fmt::format("{} entry has no block id", key));
        }
        tick.pos.x = child_int(item, "x").value_or(0);
        tick.pos.y =
            static_cast<std::int16_t>(child_int(item, "y").value_or(0));
        tick.pos.z = child_int(item, "z").value_or(0);
        const auto priority{child_int(item, "p").value_or(0)};
        // Vanilla TickPriority is a signed value: -3 (extremely high) ..
        // 3 (extremely low). Accepting 0..255 here used to reject valid
        // negative priorities and silently wrap out-of-range positive ones.
        if (priority < -3 || priority > 3) {
            return parse_failure(
                fmt::format("{} entry has priority {}", key, priority));
        }
        tick.priority = static_cast<std::int8_t>(priority);
        tick.kind = kind;
        const auto remaining{child_long(item, "t").value_or(0)};
        if (remaining < 0 || remaining > 0xFFFFFFFFLL) {
            return parse_failure(
                fmt::format("{} entry has {} remaining ticks", key, remaining));
        }
        tick.remaining_ticks = static_cast<std::uint32_t>(remaining);
        out.push_back(std::move(tick));
    }
    return status_ok();
}

[[nodiscard]] auto encode_blob(const value &item)
    -> result<std::vector<std::byte>> {
    return nbt::write(item, "", nbt::compression::none);
}

[[nodiscard]] auto read_block_entities(const value &root, chunk &out)
    -> status {
    const auto *list{list_of(root, "block_entities")};
    if (list == nullptr) {
        return status_ok();
    }
    for (const auto &item : list->items) {
        if (item.as_compound() == nullptr) {
            return parse_failure("block entity is not a compound");
        }
        auto bytes{encode_blob(item)};
        if (!bytes) {
            return std::unexpected{bytes.error()};
        }
        out.block_entities.push_back(std::move(*bytes));
    }
    return status_ok();
}

[[nodiscard]] auto read_post_processing(const value &root, chunk &out)
    -> status {
    const auto *list{list_of(root, "PostProcessing")};
    if (list == nullptr) {
        return status_ok();
    }
    std::vector<std::vector<std::int16_t>> post;
    post.reserve(list->items.size());
    for (const auto &item : list->items) {
        const auto *inner{item.as_list()};
        if (inner == nullptr) {
            return parse_failure("PostProcessing entry is not a list");
        }
        std::vector<std::int16_t> values;
        values.reserve(inner->items.size());
        for (const auto &entry : inner->items) {
            const auto number{entry.as_short()};
            if (!number) {
                return parse_failure("PostProcessing value is not a short");
            }
            values.push_back(*number);
        }
        post.push_back(std::move(values));
    }
    out.post_processing = std::move(post);
    return status_ok();
}

[[nodiscard]] auto read_chunk_extras(const value &root, chunk &out) -> status {
    if (const auto *structures{root.get("structures")}; structures != nullptr) {
        auto bytes{encode_blob(*structures)};
        if (!bytes) {
            return std::unexpected{bytes.error()};
        }
        out.structures = std::move(*bytes);
    }
    value blending{nbt::make_compound()};
    for (const auto *name : {"blending_data", "below_zero_retrogen"}) {
        if (const auto *item{root.get(std::string_view{name})};
            item != nullptr) {
            set_child(blending, name, *item);
        }
    }
    if (!blending.as_compound()->empty()) {
        auto bytes{encode_blob(blending)};
        if (!bytes) {
            return std::unexpected{bytes.error()};
        }
        out.blending = std::move(*bytes);
    }
    static constexpr std::array<std::string_view, 18> known{
        "xPos",           "zPos",           "yPos",
        "Status",         "LastUpdate",     "InhabitedTime",
        "sections",       "Heightmaps",     "block_ticks",
        "fluid_ticks",    "block_entities", "structures",
        "PostProcessing", "blending_data",  "below_zero_retrogen",
        "isLightOn",      "DataVersion",    "UpgradeData"};
    value extra{nbt::make_compound()};
    for (const auto &[name, item] : *root.as_compound()) {
        const auto known_key{std::find(known.begin(), known.end(), name) !=
                             known.end()};
        if (!known_key) {
            set_child(extra, name, item);
        } else if (name == "UpgradeData") {
            set_child(extra, name, item);
        }
    }
    if (!extra.as_compound()->empty()) {
        auto bytes{encode_blob(extra)};
        if (!bytes) {
            return std::unexpected{bytes.error()};
        }
        out.extra = std::move(*bytes);
    }
    return status_ok();
}

struct staged_section {
    section_record record;
    light_section light;
    bool has_blocks{false};
    bool has_biomes{false};
};

[[nodiscard]] auto chunk_from_nbt(const value &root, chunk_pos expected)
    -> result<chunk> {
    if (root.as_compound() == nullptr) {
        return parse_failure("chunk NBT root is not a compound");
    }
    const auto x{child_int(root, "xPos")};
    const auto z{child_int(root, "zPos")};
    if (!x || !z) {
        return parse_failure("chunk NBT has no xPos/zPos");
    }
    if (*x != expected.x || *z != expected.z) {
        return parse_failure(
            fmt::format("chunk NBT is at ({}, {}), expected {}", *x, *z,
                        format_chunk_pos(expected)));
    }
    const auto status_text{child_string(root, "Status")};
    if (!status_text) {
        return parse_failure("chunk NBT has no Status");
    }
    const auto parsed_status{parse_chunk_status(*status_text)};
    if (!parsed_status) {
        return std::unexpected{parsed_status.error()};
    }
    chunk out{};
    out.format_version = chunk_format_version;
    out.status = *parsed_status;
    out.last_update = child_long(root, "LastUpdate").value_or(0);
    out.inhabited_time = child_long(root, "InhabitedTime").value_or(0);
    const auto y_pos{child_int(root, "yPos")};
    if (!y_pos) {
        return parse_failure("chunk NBT has no yPos");
    }
    out.y_pos = *y_pos;

    const auto *sections{list_of(root, "sections")};
    std::vector<staged_section> staged;
    if (sections != nullptr) {
        for (const auto &item : sections->items) {
            if (item.as_compound() == nullptr) {
                return parse_failure("section entry is not a compound");
            }
            section_record section{};
            const auto *y{item.get("Y")};
            const std::optional<std::int8_t> number{
                y != nullptr ? y->as_byte() : std::nullopt};
            if (!number) {
                return parse_failure("section Y is missing or not a byte");
            }
            section.y = *number;
            const bool has_blocks{item.get("block_states") != nullptr};
            const bool has_biomes{item.get("biomes") != nullptr};
            if (auto status{read_block_states(item, section)}; !status) {
                return std::unexpected{status.error()};
            }
            if (auto status{read_biomes(item, section)}; !status) {
                return std::unexpected{status.error()};
            }
            light_section light{};
            light.y = *number;
            if (auto status{read_light(item, "BlockLight", light.block_light)};
                !status) {
                return std::unexpected{status.error()};
            }
            if (auto status{read_light(item, "SkyLight", light.sky_light)};
                !status) {
                return std::unexpected{status.error()};
            }
            staged.push_back(staged_section{.record = std::move(section),
                                            .light = std::move(light),
                                            .has_blocks = has_blocks,
                                            .has_biomes = has_biomes});
        }
    }
    // Canonical order: sections ascending by Y (post-processing follows).
    std::stable_sort(staged.begin(), staged.end(),
                     [](const auto &left, const auto &right) {
                         return left.record.y < right.record.y;
                     });
    // A vanilla section record can have a null block-state container with
    // light only: the light-engine padding section at `minSectionY - 1` /
    // `maxSectionY + 1`. Such sections have neither `block_states` nor
    // `biomes`; every other section is a normal air/block column section. Light
    // layers are stored separately from block sections, including for normal
    // sections.
    std::optional<std::int32_t> last_block_y;
    for (const auto &entry : staged) {
        if (entry.has_blocks || entry.has_biomes) {
            last_block_y = entry.record.y;
        }
    }
    for (auto &entry : staged) {
        const bool padding{
            !entry.has_blocks && !entry.has_biomes &&
            (entry.record.y < out.y_pos ||
             (last_block_y && entry.record.y == *last_block_y + 1))};
        if (!padding) {
            out.sections.push_back(std::move(entry.record));
        }
        if (!entry.light.block_light.empty() ||
            !entry.light.sky_light.empty()) {
            out.light_sections.push_back(std::move(entry.light));
        }
    }
    if (auto status{read_heightmaps(root, out)}; !status) {
        return std::unexpected{status.error()};
    }
    if (auto status{read_ticks(root, "block_ticks", 0, out.block_ticks)};
        !status) {
        return std::unexpected{status.error()};
    }
    if (auto status{read_ticks(root, "fluid_ticks", 1, out.fluid_ticks)};
        !status) {
        return std::unexpected{status.error()};
    }
    if (auto status{read_block_entities(root, out)}; !status) {
        return std::unexpected{status.error()};
    }
    if (auto status{read_post_processing(root, out)}; !status) {
        return std::unexpected{status.error()};
    }
    if (auto status{read_chunk_extras(root, out)}; !status) {
        return std::unexpected{status.error()};
    }
    if (auto valid{validate(out)}; !valid) {
        return std::unexpected{valid.error()};
    }
    return out;
}

// --- export ---------------------------------------------------------------

[[nodiscard]] auto palette_entry_value(std::string_view key, bool compound_form)
    -> result<value> {
    const auto parsed{parse_block_state_key(key)};
    if (!parsed) {
        return std::unexpected{parsed.error()};
    }
    if (!compound_form) {
        return nbt::make_string(parsed->name);
    }
    value entry{nbt::make_compound()};
    if (parsed->properties.empty()) {
        set_child(entry, "", nbt::make_string(parsed->name));
        return entry;
    }
    set_child(entry, "id", nbt::make_string(parsed->name));
    value properties{nbt::make_compound()};
    for (const auto &[name, text] : parsed->properties) {
        set_child(properties, name, nbt::make_string(text));
    }
    set_child(entry, "properties", std::move(properties));
    return entry;
}

[[nodiscard]] auto repack_words(std::span<const std::uint64_t> words,
                                std::uint8_t from_bits, std::uint8_t to_bits,
                                std::size_t palette_size, std::size_t entries,
                                std::vector<std::uint64_t> &out) -> status {
    if (from_bits == to_bits) {
        out.assign(words.begin(), words.end());
        return status_ok();
    }
    std::vector<std::uint32_t> indices(entries);
    if (auto status{unpack_palette_indices(words, from_bits, entries,
                                           palette_size, indices)};
        !status) {
        return status;
    }
    return pack_palette_indices(indices, to_bits, palette_size, out);
}

[[nodiscard]] auto light_byte_array(std::span<const std::byte> bytes)
    -> nbt::byte_array {
    nbt::byte_array out;
    out.reserve(bytes.size());
    for (const auto byte : bytes) {
        out.push_back(static_cast<std::uint8_t>(byte));
    }
    return out;
}

void append_light(value &target, const light_section &light) {
    // 26.3 uses the capitalised keys (SerializableChunkData.BLOCK_LIGHT_TAG /
    // SKY_LIGHT_TAG); lowercase is accepted on import for older fixtures.
    if (!light.block_light.empty()) {
        set_child(target, "BlockLight",
                  nbt::make_byte_array(light_byte_array(light.block_light)));
    }
    if (!light.sky_light.empty()) {
        set_child(target, "SkyLight",
                  nbt::make_byte_array(light_byte_array(light.sky_light)));
    }
}

[[nodiscard]] auto section_to_nbt(const section_record &section,
                                  const light_section *light) -> result<value> {
    value result{nbt::make_compound()};
    set_child(result, "Y", nbt::make_byte(section.y));

    value states{nbt::make_compound()};
    const bool compound_form{
        std::any_of(section.palette.begin(), section.palette.end(),
                    [](const palette_entry &entry) {
                        return entry.key.find('[') != std::string::npos;
                    })};
    value palette{
        nbt::make_list(compound_form ? tag_type::compound : tag_type::string)};
    for (const auto &entry : section.palette) {
        auto item{palette_entry_value(entry.key, compound_form)};
        if (!item) {
            return std::unexpected{item.error()};
        }
        if (auto pushed{palette.push(std::move(*item))}; !pushed) {
            return std::unexpected{pushed.error()};
        }
    }
    set_child(states, "palette", std::move(palette));
    if (section.kind == section_kind::palette) {
        const auto target{section_bits(section.palette.size(), true)};
        std::vector<std::uint64_t> words;
        if (auto status{repack_words(section.data, section.bits_per_entry,
                                     target, section.palette.size(),
                                     section_volume, words)};
            !status) {
            return std::unexpected{status.error()};
        }
        std::vector<std::int64_t> raw;
        raw.reserve(words.size());
        for (const auto word : words) {
            raw.push_back(std::bit_cast<std::int64_t>(word));
        }
        set_child(states, "data", nbt::make_long_array(std::move(raw)));
    }
    set_child(result, "block_states", std::move(states));

    value biomes{nbt::make_compound()};
    value biome_palette{nbt::make_list(tag_type::string)};
    for (const auto &key : section.biome_palette) {
        if (auto pushed{biome_palette.push(nbt::make_string(key))}; !pushed) {
            return std::unexpected{pushed.error()};
        }
    }
    set_child(biomes, "palette", std::move(biome_palette));
    if (section.biome_kind == section_kind::palette) {
        const auto target{section_bits(section.biome_palette.size(), false)};
        std::vector<std::uint64_t> words;
        if (auto status{repack_words(
                section.biome_data, section.biome_bits_per_entry, target,
                section.biome_palette.size(), false, words)};
            !status) {
            return std::unexpected{status.error()};
        }
        std::vector<std::int64_t> raw;
        raw.reserve(words.size());
        for (const auto word : words) {
            raw.push_back(std::bit_cast<std::int64_t>(word));
        }
        set_child(biomes, "data", nbt::make_long_array(std::move(raw)));
    }
    set_child(result, "biomes", std::move(biomes));

    if (light != nullptr) {
        append_light(result, *light);
    }
    return result;
}

[[nodiscard]] auto light_section_to_nbt(const light_section &light) -> value {
    value result{nbt::make_compound()};
    set_child(result, "Y", nbt::make_byte(light.y));
    append_light(result, light);
    return result;
}

[[nodiscard]] auto ticks_to_nbt(const std::vector<saved_tick> &ticks)
    -> result<value> {
    value list{nbt::make_list(tag_type::compound)};
    for (const auto &tick : ticks) {
        value item{nbt::make_compound()};
        set_child(item, "i", nbt::make_string(tick.id));
        set_child(item, "p",
                  nbt::make_int(static_cast<std::int32_t>(tick.priority)));
        set_child(item, "x", nbt::make_int(tick.pos.x));
        set_child(item, "y", nbt::make_int(tick.pos.y));
        set_child(item, "z", nbt::make_int(tick.pos.z));
        set_child(
            item, "t",
            nbt::make_int(static_cast<std::int32_t>(tick.remaining_ticks)));
        if (auto pushed{list.push(std::move(item))}; !pushed) {
            return std::unexpected{pushed.error()};
        }
    }
    return list;
}

[[nodiscard]] auto chunk_to_nbt(const chunk &state, chunk_pos pos)
    -> result<value> {
    if (auto valid{validate(state)}; !valid) {
        return std::unexpected{valid.error()};
    }
    value root{nbt::make_compound()};
    set_child(root, "DataVersion", nbt::make_int(supported_data_version));
    set_child(root, "xPos", nbt::make_int(pos.x));
    set_child(root, "zPos", nbt::make_int(pos.z));
    set_child(root, "yPos", nbt::make_int(state.y_pos));
    set_child(root, "Status",
              nbt::make_string(fmt::format("minecraft:{}",
                                           chunk_status_name(state.status))));
    set_child(root, "LastUpdate", nbt::make_long(state.last_update));
    set_child(root, "InhabitedTime", nbt::make_long(state.inhabited_time));
    set_child(root, "isLightOn",
              nbt::make_byte(state.light_sections.empty() ? 0 : 1));

    value sections{nbt::make_list(tag_type::compound)};
    std::size_t light_index{0};
    const auto push_section = [&sections](value item) {
        if (auto pushed{sections.push(std::move(item))}; !pushed) {
            return false;
        }
        return true;
    };
    for (const auto &section : state.sections) {
        while (light_index < state.light_sections.size() &&
               state.light_sections[light_index].y < section.y) {
            if (!push_section(
                    light_section_to_nbt(state.light_sections[light_index]))) {
                return std::unexpected{
                    make_error(error_code::invalid_argument,
                               "chunk sections exceed the NBT list limit")};
            }
            ++light_index;
        }
        const light_section *light{nullptr};
        if (light_index < state.light_sections.size() &&
            state.light_sections[light_index].y == section.y) {
            light = &state.light_sections[light_index];
            ++light_index;
        }
        auto item{section_to_nbt(section, light)};
        if (!item) {
            return std::unexpected{item.error()};
        }
        if (!push_section(std::move(*item))) {
            return std::unexpected{
                make_error(error_code::invalid_argument,
                           "chunk sections exceed the NBT list limit")};
        }
    }
    while (light_index < state.light_sections.size()) {
        if (!push_section(
                light_section_to_nbt(state.light_sections[light_index]))) {
            return std::unexpected{
                make_error(error_code::invalid_argument,
                           "chunk sections exceed the NBT list limit")};
        }
        ++light_index;
    }
    set_child(root, "sections", std::move(sections));

    if (!state.heightmaps.empty()) {
        value heightmaps{nbt::make_compound()};
        for (const auto &heightmap : state.heightmaps) {
            std::vector<std::int64_t> raw;
            raw.reserve(heightmap.data.size());
            for (const auto word : heightmap.data) {
                raw.push_back(std::bit_cast<std::int64_t>(word));
            }
            set_child(heightmaps, heightmap.name,
                      nbt::make_long_array(std::move(raw)));
        }
        set_child(root, "Heightmaps", std::move(heightmaps));
    }
    if (auto ticks{ticks_to_nbt(state.block_ticks)}; ticks) {
        set_child(root, "block_ticks", std::move(*ticks));
    } else {
        return std::unexpected{ticks.error()};
    }
    if (auto ticks{ticks_to_nbt(state.fluid_ticks)}; ticks) {
        set_child(root, "fluid_ticks", std::move(*ticks));
    } else {
        return std::unexpected{ticks.error()};
    }

    value entities{nbt::make_list(tag_type::compound)};
    for (const auto &bytes : state.block_entities) {
        const auto document{nbt::read(bytes)};
        if (!document || document->as_compound() == nullptr) {
            return parse_failure("stored block entity is not valid NBT");
        }
        if (auto pushed{entities.push(*document)}; !pushed) {
            return std::unexpected{pushed.error()};
        }
    }
    set_child(root, "block_entities", std::move(entities));

    if (!state.structures.empty()) {
        const auto document{nbt::read(state.structures)};
        if (!document || document->as_compound() == nullptr) {
            return parse_failure("stored structures payload is not valid NBT");
        }
        set_child(root, "structures", *document);
    }
    if (!state.post_processing.empty()) {
        value post{nbt::make_list(tag_type::list)};
        for (const auto &entry : state.post_processing) {
            value inner{nbt::make_list(tag_type::short_)};
            for (const auto number : entry) {
                if (auto pushed{inner.push(nbt::make_short(number))}; !pushed) {
                    return std::unexpected{pushed.error()};
                }
            }
            if (auto pushed{post.push(std::move(inner))}; !pushed) {
                return std::unexpected{pushed.error()};
            }
        }
        set_child(root, "PostProcessing", std::move(post));
    }
    if (!state.blending.empty()) {
        const auto document{nbt::read(state.blending)};
        if (!document || document->as_compound() == nullptr) {
            return parse_failure("stored blending payload is not valid NBT");
        }
        for (const auto &[name, item] : *document->as_compound()) {
            set_child(root, name, item);
        }
    }
    if (!state.extra.empty()) {
        const auto document{nbt::read(state.extra)};
        if (!document || document->as_compound() == nullptr) {
            return parse_failure("stored extra payload is not valid NBT");
        }
        for (const auto &[name, item] : *document->as_compound()) {
            if (!root.contains(name)) {
                set_child(root, name, item);
            }
        }
    }
    return root;
}

// --- Anvil region files ----------------------------------------------------

[[nodiscard]] auto external_chunk_path(std::string_view region_path,
                                       const chunk_pos &pos) -> std::string {
    return fmt::format("{}/c.{}.{}.mcc",
                       core::io::parent_directory(region_path), pos.x, pos.z);
}

[[nodiscard]] auto parse_external_chunk_name(std::string_view name)
    -> std::optional<chunk_pos> {
    constexpr std::string_view prefix{"c."};
    constexpr std::string_view suffix{".mcc"};
    if (!name.starts_with(prefix) || !name.ends_with(suffix) ||
        name.size() <= prefix.size() + suffix.size()) {
        return std::nullopt;
    }
    const auto stem{name.substr(prefix.size(),
                                name.size() - prefix.size() - suffix.size())};
    auto parsed{parse_chunk_key(stem)};
    if (!parsed) {
        return std::nullopt;
    }
    return *parsed;
}

/// Remove `.mcc` files that belong to this region but are no longer referenced
/// by the exported region. Called only after the new `.mca` is committed, so a
/// failed export never deletes a still-valid external chunk.
[[nodiscard]] auto cleanup_stale_external_chunks(
    std::string_view region_path, std::int32_t region_x, std::int32_t region_z,
    const std::set<std::pair<std::int32_t, std::int32_t>> &expected) -> status {
    const auto directory{core::io::parent_directory(region_path)};
    auto names{core::io::list_directory(directory)};
    if (!names) {
        return std::unexpected{names.error()};
    }
    for (const auto &name : *names) {
        const auto coords{parse_external_chunk_name(name)};
        if (!coords || (coords->x >> 5) != region_x ||
            (coords->z >> 5) != region_z) {
            continue;
        }
        if (expected.contains({coords->x, coords->z})) {
            continue;
        }
        const auto path{join_path(directory, name)};
        if (core::io::is_directory(path) && !core::io::is_symlink(path)) {
            continue;
        }
        std::error_code error;
        static_cast<void>(std::filesystem::remove(path, error));
        if (error) {
            return fail(error_code::io_error,
                        fmt::format("cannot remove stale external chunk '{}': "
                                    "{}",
                                    path, error.message()));
        }
    }
    return status_ok();
}

[[nodiscard]] auto read_anvil_region(std::string_view path,
                                     std::int32_t region_x,
                                     std::int32_t region_z)
    -> result<std::vector<std::pair<chunk_pos, value>>> {
    auto bytes{core::io::read_file(
        path, {.max_bytes = max_region_bytes, .allow_symlink = false})};
    if (!bytes) {
        return std::unexpected{bytes.error()};
    }
    if (bytes->size() < anvil_header_bytes) {
        return parse_failure(
            fmt::format("region '{}' is {} bytes, shorter than the header",
                        path, bytes->size()));
    }
    // Not every implementation pads the final sector to 4096 bytes on close.
    // Round the sector count up for bounds and overlap tracking, then require
    // the actual payload bytes to fit in the file below.
    const auto sectors{(bytes->size() + anvil_sector_bytes - 1) /
                       anvil_sector_bytes};
    std::vector<bool> used(sectors, false);
    if (sectors < 2) {
        return parse_failure(
            fmt::format("region '{}' has no data sectors", path));
    }
    used[0] = true;
    used[1] = true;
    std::vector<std::pair<chunk_pos, value>> chunks;
    std::size_t document_bytes{0};
    nbt::limits chunk_limits{};
    chunk_limits.max_bytes = max_anvil_decompressed_bytes;
    for (std::size_t slot{0}; slot < anvil_slots; ++slot) {
        const auto location{be32(*bytes, slot * 4)};
        const auto offset{location >> 8U};
        const auto count{location & 0xFFU};
        if (offset == 0 && count == 0) {
            continue;
        }
        if (offset < 2 || count == 0 || offset + count > sectors) {
            return parse_failure(fmt::format(
                "region '{}' slot {} has an out-of-bounds location ({}, {})",
                path, slot, offset, count));
        }
        for (auto sector{offset}; sector < offset + count; ++sector) {
            if (used[sector]) {
                return parse_failure(
                    fmt::format("region '{}' slot {} overlaps sector {}", path,
                                slot, sector));
            }
            used[sector] = true;
        }
        const auto base{static_cast<std::size_t>(offset) * anvil_sector_bytes};
        if (base + 5 > bytes->size()) {
            return parse_failure(
                fmt::format("region '{}' slot {} has a truncated chunk header",
                            path, slot));
        }
        const auto length{be32(*bytes, base)};
        if (length < 1 || 5 + (length - 1) > count * anvil_sector_bytes ||
            base + 5 + (length - 1) > bytes->size()) {
            return parse_failure(fmt::format(
                "region '{}' slot {} has length {}", path, slot, length));
        }
        const auto pos{position_for_slot(region_x, region_z,
                                         static_cast<std::uint32_t>(slot),
                                         rwf::region_edge)};
        const auto region_base_x{static_cast<std::int64_t>(region_x) *
                                 rwf::region_edge};
        const auto region_base_z{static_cast<std::int64_t>(region_z) *
                                 rwf::region_edge};
        if (pos.x < region_base_x ||
            pos.x >= region_base_x + rwf::region_edge ||
            pos.z < region_base_z ||
            pos.z >= region_base_z + rwf::region_edge) {
            return parse_failure(fmt::format(
                "region '{}' slot {} maps to chunk {} outside the region", path,
                slot, format_chunk_pos(pos)));
        }
        const auto compression_byte{
            static_cast<std::uint8_t>((*bytes)[base + 4])};
        const bool external{(compression_byte & anvil_external_stream_flag) !=
                            0};
        const auto version{static_cast<std::uint8_t>(compression_byte &
                                                     anvil_compression_mask)};
        std::vector<std::byte> external_payload;
        std::span<const std::byte> payload;
        if (external) {
            if (length != 1 || count != 1) {
                return parse_failure(fmt::format(
                    "region '{}' slot {} has a malformed external chunk stub "
                    "(length {}, sectors {})",
                    path, slot, length, count));
            }
            const auto external_path{external_chunk_path(path, pos)};
            auto loaded{core::io::read_file(
                external_path, {.max_bytes = max_anvil_external_bytes,
                                .allow_symlink = false})};
            if (!loaded) {
                return parse_failure(fmt::format(
                    "region '{}' slot {} external chunk '{}': {}", path, slot,
                    external_path, loaded.error().message));
            }
            external_payload = std::move(*loaded);
            payload = external_payload;
        } else {
            if (length - 1 > max_anvil_chunk_bytes) {
                return parse_failure(fmt::format(
                    "region '{}' slot {} chunk is too large", path, slot));
            }
            payload = std::span<const std::byte>{*bytes}.subspan(base + 5,
                                                                 length - 1);
        }
        auto document = [&]() -> result<value> {
            if (version == 4) {
                auto decompressed{detail::decode_lz4_block_stream(
                    payload, max_anvil_decompressed_bytes)};
                if (!decompressed) {
                    return std::unexpected{decompressed.error()};
                }
                return nbt::read(*decompressed, nbt::compression::none,
                                 chunk_limits);
            }
            const auto mode = [&]() -> std::optional<nbt::compression> {
                switch (version) {
                case 1:
                    return nbt::compression::gzip;
                case 2:
                    return nbt::compression::zlib;
                case 3:
                    return nbt::compression::none;
                default:
                    return std::nullopt;
                }
            }();
            if (!mode) {
                return unsupported(fmt::format(
                    "region '{}' slot {} uses compression {} (custom "
                    "compression is not supported)",
                    path, slot, version));
            }
            return nbt::read(payload, *mode, chunk_limits);
        }();
        if (!document) {
            return parse_failure(fmt::format("region '{}' slot {}: {}", path,
                                             slot, document.error().message));
        }
        if (document->as_compound() == nullptr) {
            return parse_failure(fmt::format(
                "region '{}' slot {} is not a compound", path, slot));
        }
        const auto remaining{max_anvil_region_document_bytes - document_bytes};
        const auto footprint{nbt_footprint(*document, remaining)};
        if (footprint > remaining) {
            return parse_failure(
                fmt::format("region '{}' exceeds the {} MiB decoded-NBT budget",
                            path, max_anvil_region_document_bytes >> 20U));
        }
        document_bytes += footprint;
        chunks.emplace_back(pos, std::move(*document));
    }
    std::sort(chunks.begin(), chunks.end(),
              [](const auto &left, const auto &right) {
                  return left.first < right.first;
              });
    return chunks;
}

template <typename NextDocument>
[[nodiscard]] auto write_anvil_region_impl(std::string_view path,
                                           NextDocument next) -> status {
    if (auto created{
            core::io::create_directories(core::io::parent_directory(path))};
        !created) {
        return created;
    }
    const std::string target{path};
    const std::string temporary{fmt::format("{}.tmp", target)};
    std::error_code ignored;
    std::filesystem::remove(std::filesystem::path{temporary}, ignored);
    auto file{platform::file::random_access_file::open(
        temporary, platform::file::open_mode::read_write,
        {.create = true, .truncate = true, .no_follow = true})};
    if (!file) {
        return std::unexpected{file.error()};
    }
    const auto region_name{std::filesystem::path{target}.filename().u8string()};
    const auto region_coordinates{parse_region_name(
        std::string_view{reinterpret_cast<const char *>(region_name.data()),
                         region_name.size()},
        ".mca")};
    if (!region_coordinates) {
        return fail(error_code::parse_error,
                    fmt::format("region file '{}' has an invalid name", path));
    }
    std::set<std::pair<std::int32_t, std::int32_t>> expected_external;
    std::vector<std::byte> header(anvil_header_bytes, std::byte{0});
    std::uint32_t next_sector{2};
    while (true) {
        auto next_document{next()};
        if (!next_document) {
            return std::unexpected{next_document.error()};
        }
        if (!next_document->has_value()) {
            break;
        }
        const auto &[pos, document] = **next_document;
        auto compressed{zlib_compress(document)};
        if (!compressed) {
            return std::unexpected{compressed.error()};
        }
        const auto payload_size{1 + compressed->size()};
        const auto total{4 + payload_size};
        const auto sectors{static_cast<std::uint32_t>(
            (total + anvil_sector_bytes - 1) / anvil_sector_bytes)};
        const bool external{sectors > max_anvil_internal_sectors};
        if (external) {
            if (compressed->size() > max_anvil_external_bytes) {
                return unsupported(fmt::format(
                    "chunk {} compresses to {} bytes (external chunk limit is "
                    "{} bytes)",
                    format_chunk_pos(pos), compressed->size(),
                    max_anvil_external_bytes));
            }
        } else if (payload_size > max_anvil_chunk_bytes) {
            return unsupported(fmt::format(
                "chunk {} compresses to {} bytes (Anvil internal limit is 1 "
                "MiB)",
                format_chunk_pos(pos), payload_size));
        }
        const auto stored_sectors{external ? 1U : sectors};
        if (next_sector + stored_sectors > 0xFFFFFFU) {
            return unsupported(
                fmt::format("region {} would exceed the Anvil 1 GiB limit",
                            std::string{path}));
        }
        if (external) {
            const auto external_path{external_chunk_path(target, pos)};
            if (auto written{
                    core::io::atomic_write_file(external_path, *compressed)};
                !written) {
                return written;
            }
            expected_external.emplace(pos.x, pos.z);
        }
        std::vector<std::byte> block(static_cast<std::size_t>(stored_sectors) *
                                         anvil_sector_bytes,
                                     std::byte{0});
        put_be32(block, 0,
                 external ? 1U : static_cast<std::uint32_t>(payload_size));
        block[4] = static_cast<std::byte>(
            external ? (2U | anvil_external_stream_flag) : 2U);
        if (!external) {
            std::copy(compressed->begin(), compressed->end(),
                      block.begin() + 5);
        }
        if (auto written{file->write_at(
                static_cast<std::uint64_t>(next_sector) * anvil_sector_bytes,
                block)};
            !written) {
            return written;
        }
        const auto slot{rwf::region_file::slot_for(pos)};
        put_be32(header, slot * 4, (next_sector << 8U) | stored_sectors);
        next_sector += stored_sectors;
    }
    if (auto written{file->write_at(0, header)}; !written) {
        return written;
    }
    if (auto synced{file->sync()}; !synced) {
        return synced;
    }
    *file = platform::file::random_access_file{};
    if (auto renamed{platform::file::rename_replace(temporary, target)};
        !renamed) {
        return renamed;
    }
    if (auto synced{
            platform::file::sync_directory(core::io::parent_directory(target))};
        !synced) {
        return synced;
    }
    if (auto cleaned{cleanup_stale_external_chunks(
            target, region_coordinates->first, region_coordinates->second,
            expected_external)};
        !cleaned) {
        return cleaned;
    }
    return platform::file::sync_directory(core::io::parent_directory(target));
}

[[nodiscard]] auto write_anvil_region(
    std::string_view path,
    std::vector<std::pair<chunk_pos, std::vector<std::byte>>> documents)
    -> status {
    std::sort(documents.begin(), documents.end(),
              [](const auto &left, const auto &right) {
                  return left.first < right.first;
              });
    std::size_t index{0};
    return write_anvil_region_impl(
        path,
        [&]() -> result<
                  std::optional<std::pair<chunk_pos, std::vector<std::byte>>>> {
            if (index >= documents.size()) {
                return std::optional<
                    std::pair<chunk_pos, std::vector<std::byte>>>{};
            }
            auto document{std::move(documents[index++])};
            return std::optional<std::pair<chunk_pos, std::vector<std::byte>>>{
                std::move(document)};
        });
}

/// Streaming variant: `make_document` is called for one chunk at a time, so
/// the exporter never holds a whole region's NBT documents in memory.
template <typename MakeDocument>
[[nodiscard]] auto
write_anvil_region_stream(std::string_view path,
                          std::span<const chunk_pos> positions,
                          MakeDocument make_document) -> status {
    std::vector<chunk_pos> sorted{positions.begin(), positions.end()};
    std::sort(sorted.begin(), sorted.end());
    std::size_t index{0};
    return write_anvil_region_impl(
        path,
        [&]() -> result<
                  std::optional<std::pair<chunk_pos, std::vector<std::byte>>>> {
            if (index >= sorted.size()) {
                return std::optional<
                    std::pair<chunk_pos, std::vector<std::byte>>>{};
            }
            const auto pos{sorted[index++]};
            auto document{make_document(pos)};
            if (!document) {
                return std::unexpected{std::move(document.error())};
            }
            return std::optional<std::pair<chunk_pos, std::vector<std::byte>>>{
                std::pair<chunk_pos, std::vector<std::byte>>{
                    pos, std::move(*document)}};
        });
}

// --- level.dat / meta ------------------------------------------------------

[[nodiscard]] auto read_level_data(std::string_view source) -> result<value> {
    const auto path{join_path(source, "level.dat")};
    auto document{nbt::read_file(path, nbt::compression::gzip)};
    if (!document) {
        return std::unexpected{document.error()};
    }
    const auto *data_value{document->get("Data")};
    if (data_value == nullptr || data_value->as_compound() == nullptr) {
        return parse_failure("level.dat has no Data compound");
    }
    value data{*data_value};
    const auto version{child_int(data, "DataVersion")};
    if (!version) {
        return parse_failure("level.dat has no DataVersion");
    }
    if (*version != supported_data_version) {
        return unsupported(fmt::format(
            "level.dat DataVersion is {} (supported: {}); open the world in "
            "vanilla 26.3 and save it once to upgrade before importing",
            *version, supported_data_version));
    }
    return data;
}

[[nodiscard]] auto meta_from_level(const value &level) -> value {
    value meta{nbt::make_compound()};
    set_child(meta, "format", nbt::make_string(std::string{meta_format}));
    set_child(meta, "data_version", nbt::make_int(supported_data_version));
    set_child(meta, "imported_from", nbt::make_string("anvil"));
    set_child(meta, "gamepack_id", nbt::make_string(""));
    set_child(meta, "mods_lock_fingerprint", nbt::make_string(""));
    set_child(meta, "level", level);
    return meta;
}

// --- directory helpers -----------------------------------------------------

[[nodiscard]] auto remove_tree(std::string_view path) -> status {
    std::error_code error;
    std::filesystem::remove_all(std::filesystem::path{std::string{path}},
                                error);
    if (error) {
        return fail(error_code::io_error, fmt::format("cannot remove '{}': {}",
                                                      path, error.message()));
    }
    return status_ok();
}

/// Absolute, symlink-resolved form used only for the source/target overlap
/// check. Both paths may be missing: `weakly_canonical` resolves the existing
/// prefix and appends the rest, so a not-yet-created target still compares
/// correctly against an existing source.
[[nodiscard]] auto normalized_path(std::string_view path)
    -> std::filesystem::path {
    const std::filesystem::path input{std::string{path}};
    std::error_code error;
    auto normalized{std::filesystem::weakly_canonical(input, error)};
    if (error) {
        error.clear();
        normalized = std::filesystem::absolute(input, error);
    }
    if (error) {
        normalized = input.lexically_normal();
    }
    return normalized;
}

/// True when the paths are equal or one is an ancestor of the other. A source
/// nested inside the target (or vice versa) would make `--force` delete the
/// source before it is read.
[[nodiscard]] auto paths_overlap(const std::filesystem::path &left,
                                 const std::filesystem::path &right) -> bool {
    if (left == right) {
        return true;
    }
    const auto nested = [](const std::filesystem::path &ancestor,
                           const std::filesystem::path &path) {
        auto ancestor_part{ancestor.begin()};
        auto path_part{path.begin()};
        for (; ancestor_part != ancestor.end(); ++ancestor_part, ++path_part) {
            if (path_part == path.end() || *ancestor_part != *path_part) {
                return false;
            }
        }
        return path_part != path.end();
    };
    return nested(left, right) || nested(right, left);
}

/// Guard `world import`/`world export` against source == target and
/// source/target nesting. `--force` removes the target tree, so overlap must
/// be rejected before any filesystem mutation.
[[nodiscard]] auto check_disjoint_paths(std::string_view source,
                                        std::string_view target) -> status {
    if (paths_overlap(normalized_path(source), normalized_path(target))) {
        return fail(error_code::invalid_argument,
                    fmt::format("'--to' ('{}') must not be the source, an "
                                "ancestor or a subdirectory of '--from' ('{}')",
                                target, source));
    }
    return status_ok();
}

[[nodiscard]] auto prepare_target(std::string_view target, bool overwrite)
    -> status {
    if (!core::io::exists(target)) {
        return status_ok();
    }
    if (!overwrite) {
        return fail(error_code::already_exists,
                    fmt::format("'{}' already exists (use --force to "
                                "replace it)",
                                target));
    }
    return remove_tree(target);
}

[[nodiscard]] auto
region_documents(const std::vector<std::pair<chunk_pos, value>> &chunks)
    -> result<std::vector<std::pair<chunk_pos, std::vector<std::byte>>>> {
    std::vector<std::pair<chunk_pos, std::vector<std::byte>>> documents;
    documents.reserve(chunks.size());
    for (const auto &[pos, document] : chunks) {
        auto bytes{nbt::write(document, "", nbt::compression::none)};
        if (!bytes) {
            return std::unexpected{bytes.error()};
        }
        documents.emplace_back(pos, std::move(*bytes));
    }
    return documents;
}

} // namespace

auto detect_world(std::string_view root) -> result<world_format> {
    if (!core::io::is_directory(root)) {
        return fail(error_code::not_found,
                    fmt::format("'{}' is not a directory", root));
    }
    if (core::io::exists(join_path(root, "meta.rfw")) ||
        core::io::is_directory(join_path(root, "regions"))) {
        return world_format::rwf;
    }
    if (core::io::is_directory(
            join_path(root, "dimensions/minecraft/overworld"))) {
        return world_format::anvil;
    }
    return fail(error_code::parse_error,
                fmt::format("'{}' is neither an Anvil nor an RWF world", root));
}

struct dimension_source {
    std::string namespace_name;
    std::string path;
};

[[nodiscard]] auto list_dimensions(std::string_view root)
    -> result<std::vector<dimension_source>>;

[[nodiscard]] auto dimension_name(const dimension_source &dimension)
    -> std::string {
    return fmt::format("{}:{}", dimension.namespace_name, dimension.path);
}

[[nodiscard]] auto scan_rwf_dimension(std::string_view dim_root,
                                      std::string_view dimension,
                                      std::vector<chunk_digest> &out,
                                      const rwf::zstd_dictionary *dictionary,
                                      const palette_table *palette) -> status {
    const auto regions{join_path(dim_root, "regions")};
    if (!core::io::is_directory(regions)) {
        return status_ok();
    }
    auto names{core::io::list_directory(regions)};
    if (!names) {
        return std::unexpected{names.error()};
    }
    for (const auto &name : *names) {
        const auto coordinates{
            parse_region_name(fmt::format("r.{}", name), ".rfw")};
        if (!coordinates) {
            continue;
        }
        auto region{rwf::region_file::load(join_path(regions, name),
                                           {.x = coordinates->first,
                                            .z = coordinates->second,
                                            .dictionary = dictionary})};
        if (!region) {
            return std::unexpected{region.error()};
        }
        for (std::uint32_t slot{0}; slot < rwf::index_slots; ++slot) {
            const auto entries{region->entries()};
            if (entries[slot].empty()) {
                continue;
            }
            const auto pos{position_for_slot(coordinates->first,
                                             coordinates->second, slot,
                                             rwf::region_edge)};
            auto payload{region->read(pos)};
            if (!payload) {
                return std::unexpected{payload.error()};
            }
            auto chunk{decode_chunk(*payload, palette)};
            if (!chunk) {
                return std::unexpected{chunk.error()};
            }
            auto hash{semantic_hash(*chunk)};
            if (!hash) {
                return std::unexpected{hash.error()};
            }
            out.push_back(chunk_digest{
                .dimension = std::string{dimension},
                .pos = pos,
                .status = std::string{chunk_status_name(chunk->status)},
                .hash = std::move(*hash)});
        }
    }
    return status_ok();
}

[[nodiscard]] auto scan_anvil_dimension(std::string_view dim_root,
                                        std::string_view dimension,
                                        std::vector<chunk_digest> &out)
    -> status {
    const auto regions{join_path(dim_root, "region")};
    if (!core::io::is_directory(regions)) {
        return status_ok();
    }
    auto names{core::io::list_directory(regions)};
    if (!names) {
        return std::unexpected{names.error()};
    }
    for (const auto &name : *names) {
        const auto coordinates{parse_region_name(name, ".mca")};
        if (!coordinates) {
            continue;
        }
        auto chunks{read_anvil_region(join_path(regions, name),
                                      coordinates->first, coordinates->second)};
        if (!chunks) {
            return std::unexpected{chunks.error()};
        }
        for (const auto &[pos, document] : *chunks) {
            auto chunk{chunk_from_nbt(document, pos)};
            if (!chunk) {
                return std::unexpected{chunk.error()};
            }
            auto hash{semantic_hash(*chunk)};
            if (!hash) {
                return std::unexpected{hash.error()};
            }
            out.push_back(chunk_digest{
                .dimension = std::string{dimension},
                .pos = pos,
                .status = std::string{chunk_status_name(chunk->status)},
                .hash = std::move(*hash)});
        }
    }
    return status_ok();
}

auto scan_world(std::string_view root) -> result<std::vector<chunk_digest>> {
    auto format{detect_world(root)};
    if (!format) {
        return std::unexpected{format.error()};
    }
    auto dimensions{list_dimensions(root)};
    if (!dimensions) {
        return std::unexpected{dimensions.error()};
    }
    std::vector<chunk_digest> digests;
    std::unique_ptr<world_store> rwf_store;
    const palette_table *palette{nullptr};
    if (*format == world_format::rwf) {
        auto opened{world_store::open(root)};
        if (!opened) {
            return std::unexpected{opened.error()};
        }
        rwf_store = std::make_unique<world_store>(std::move(*opened));
        palette = &rwf_store->palette();
    }
    for (const auto &dimension : *dimensions) {
        const auto dim_root{join_path(
            root, fmt::format("dimensions/{}/{}", dimension.namespace_name,
                              dimension.path))};
        const auto name{dimension_name(dimension)};
        const auto *dictionary =
            rwf_store != nullptr ? rwf_store->dictionary_for(
                                       dimension.namespace_name, dimension.path)
                                 : nullptr;
        auto status{*format == world_format::rwf
                        ? scan_rwf_dimension(dim_root, name, digests,
                                             dictionary, palette)
                        : scan_anvil_dimension(dim_root, name, digests)};
        if (!status) {
            return std::unexpected{status.error()};
        }
    }
    std::sort(digests.begin(), digests.end(),
              [](const auto &left, const auto &right) {
                  if (left.dimension != right.dimension) {
                      return left.dimension < right.dimension;
                  }
                  return left.pos < right.pos;
              });
    return digests;
}

[[nodiscard]] auto media_for_name(std::string_view name) -> object_media {
    if (name.ends_with(".json") || name.ends_with(".mcmeta")) {
        return object_media::json;
    }
    if (name.ends_with(".png")) {
        return object_media::png;
    }
    if (name.ends_with(".zip")) {
        return object_media::zip;
    }
    if (name.ends_with(".txt") || name.ends_with(".md")) {
        return object_media::text;
    }
    return object_media::binary;
}

/// Percent-encode one directory-entry name so spaces/non-ASCII (legal on
/// every supported filesystem) fit the object-store path alphabet.
[[nodiscard]] auto encode_object_component(std::string_view name)
    -> std::string {
    std::string out;
    out.reserve(name.size());
    for (const char raw : name) {
        const auto byte_value{static_cast<unsigned char>(raw)};
        const bool safe{(byte_value >= 'a' && byte_value <= 'z') ||
                        (byte_value >= 'A' && byte_value <= 'Z') ||
                        (byte_value >= '0' && byte_value <= '9') ||
                        byte_value == '_' || byte_value == '-' ||
                        byte_value == '.'};
        if (safe) {
            out.push_back(raw);
        } else {
            out += fmt::format("%{:02X}", byte_value);
        }
    }
    return out;
}

[[nodiscard]] auto hex_digit(char value) -> int {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

[[nodiscard]] auto decode_object_component(std::string_view encoded)
    -> result<std::string> {
    std::string out;
    out.reserve(encoded.size());
    for (std::size_t index{0}; index < encoded.size(); ++index) {
        const char raw{encoded[index]};
        if (raw != '%') {
            out.push_back(raw);
            continue;
        }
        if (index + 2 >= encoded.size()) {
            return fail(error_code::parse_error,
                        fmt::format("object path has a truncated escape '{}'",
                                    encoded));
        }
        const auto high{hex_digit(encoded[index + 1])};
        const auto low{hex_digit(encoded[index + 2])};
        if (high < 0 || low < 0) {
            return fail(
                error_code::parse_error,
                fmt::format("object path has an invalid escape '{}'", encoded));
        }
        const auto byte_value{static_cast<unsigned char>((high << 4) | low)};
        if (byte_value == 0 || byte_value == '/' || byte_value == '\\' ||
            byte_value == '%') {
            return fail(error_code::parse_error,
                        fmt::format("object path escape decodes to an unsafe "
                                    "byte in '{}'",
                                    encoded));
        }
        out.push_back(static_cast<char>(byte_value));
        index += 2;
    }
    if (out.empty() || out == "." || out == "..") {
        return fail(
            error_code::parse_error,
            fmt::format("object path component '{}' is invalid", encoded));
    }
    return out;
}

[[nodiscard]] auto decode_relative_path(std::string_view encoded)
    -> result<std::string> {
    std::string out;
    std::size_t start{0};
    while (start <= encoded.size()) {
        const auto slash{encoded.find('/', start)};
        const auto component{encoded.substr(
            start, slash == std::string_view::npos ? std::string_view::npos
                                                   : slash - start)};
        auto decoded{decode_object_component(component)};
        if (!decoded) {
            return std::unexpected{decoded.error()};
        }
        if (!out.empty()) {
            out.push_back('/');
        }
        out += *decoded;
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return out;
}

/// Store a directory tree of aux files under `prefix` in the object store.
/// `nbt_gzip` is true for `data/**` (`.dat` files are gzip NBT, stored as
/// uncompressed NBT); other trees are stored byte-for-byte.
[[nodiscard]] auto store_object_tree(object_store &objects,
                                     const std::string &from,
                                     const std::string &prefix, bool nbt_gzip,
                                     std::size_t &files, unsigned depth = 0)
    -> status {
    if (!core::io::is_directory(from)) {
        return status_ok();
    }
    if (depth > 16) {
        return fail(error_code::invalid_argument,
                    fmt::format("aux directory '{}' is too deep", from));
    }
    auto names{core::io::list_directory(from)};
    if (!names) {
        return std::unexpected{names.error()};
    }
    for (const auto &name : *names) {
        const auto source{join_path(from, name)};
        if (core::io::is_symlink(source)) {
            continue;
        }
        const auto encoded_name{encode_object_component(name)};
        const auto path{prefix.empty()
                            ? encoded_name
                            : fmt::format("{}/{}", prefix, encoded_name)};
        if (core::io::is_directory(source)) {
            if (auto status{store_object_tree(objects, source, path, nbt_gzip,
                                              files, depth + 1)};
                !status) {
                return status;
            }
            continue;
        }
        if (!core::io::is_regular_file(source)) {
            continue;
        }
        std::vector<std::byte> bytes;
        auto media{object_media::binary};
        if (nbt_gzip && name.ends_with(".dat")) {
            auto document{nbt::read_file(source, nbt::compression::gzip)};
            if (!document) {
                return std::unexpected{document.error()};
            }
            auto encoded{nbt::write(*document, "", nbt::compression::none)};
            if (!encoded) {
                return std::unexpected{encoded.error()};
            }
            bytes = std::move(*encoded);
            media = object_media::nbt;
        } else {
            auto read{core::io::read_file(
                source, {.max_bytes = 64ULL << 20U, .allow_symlink = false})};
            if (!read) {
                return std::unexpected{read.error()};
            }
            bytes = std::move(*read);
            media = media_for_name(name);
        }
        if (auto written{objects.write(path, bytes, media, false)}; !written) {
            return written;
        }
        ++files;
    }
    return status_ok();
}

/// Restore a stored tree under `prefix` to `to` (inverse of store_object_tree).
[[nodiscard]] auto restore_object_tree(object_store &objects,
                                       std::string_view prefix,
                                       const std::string &to, bool nbt_gzip,
                                       std::size_t &files) -> status {
    auto paths{objects.list(prefix)};
    if (!paths) {
        return std::unexpected{paths.error()};
    }
    const std::string base{prefix};
    for (const auto &path : *paths) {
        if (base.empty() || path.size() <= base.size() + 1 ||
            path.compare(0, base.size(), base) != 0 ||
            path[base.size()] != '/') {
            continue;
        }
        const auto encoded_relative{path.substr(base.size() + 1)};
        auto decoded{decode_relative_path(encoded_relative)};
        if (!decoded) {
            return std::unexpected{decoded.error()};
        }
        const auto &relative{*decoded};
        const auto target{join_path(to, relative)};
        auto bytes{objects.read(path)};
        if (!bytes) {
            return std::unexpected{bytes.error()};
        }
        if (nbt_gzip && relative.ends_with(".dat")) {
            if (auto created{core::io::create_directories(
                    core::io::parent_directory(target))};
                !created) {
                return created;
            }
            auto document{nbt::read(*bytes)};
            if (!document) {
                return std::unexpected{document.error()};
            }
            if (auto written{nbt::write_file(target, *document, "",
                                             nbt::compression::gzip)};
                !written) {
                return written;
            }
        } else {
            if (auto created{core::io::create_directories(
                    core::io::parent_directory(target))};
                !created) {
                return created;
            }
            if (auto written{core::io::write_file(target, *bytes)}; !written) {
                return written;
            }
        }
        ++files;
    }
    return status_ok();
}

/// Copy the parts of a 26.3 world the RWF model does not convert yet: global
/// `data/`, non-overworld `dimensions/<ns>/<dim>/` (the overworld's own `data/`
/// included) and `players/{advancements,stats}`. Everything else at the root
/// (`icon.png`, ...) is preserved verbatim as well. `session.lock` and
/// `level.dat_old` are never copied.
[[nodiscard]] auto list_dimensions(std::string_view root)
    -> result<std::vector<dimension_source>> {
    const auto dimensions{join_path(root, "dimensions")};
    if (!core::io::is_directory(dimensions)) {
        return std::vector<dimension_source>{};
    }
    auto namespaces{core::io::list_directory(dimensions)};
    if (!namespaces) {
        return std::unexpected{namespaces.error()};
    }
    std::vector<dimension_source> out;
    for (const auto &namespace_name : *namespaces) {
        const auto namespace_root{join_path(dimensions, namespace_name)};
        if (core::io::is_symlink(namespace_root) ||
            !core::io::is_directory(namespace_root)) {
            continue;
        }
        auto names{core::io::list_directory(namespace_root)};
        if (!names) {
            return std::unexpected{names.error()};
        }
        for (const auto &name : *names) {
            const auto dimension_root{join_path(namespace_root, name)};
            if (core::io::is_symlink(dimension_root) ||
                !core::io::is_directory(dimension_root)) {
                continue;
            }
            out.push_back({namespace_name, name});
        }
    }
    std::sort(out.begin(), out.end(), [](const auto &left, const auto &right) {
        return std::pair{left.namespace_name, left.path} <
               std::pair{right.namespace_name, right.path};
    });
    return out;
}

[[nodiscard]] auto build_palette_table(std::string_view source)
    -> palette_table {
    std::set<std::string> keys;
    auto dimensions{list_dimensions(source)};
    if (!dimensions) {
        return {};
    }
    for (const auto &dimension : *dimensions) {
        const auto regions{join_path(
            source, fmt::format("dimensions/{}/{}/region",
                                dimension.namespace_name, dimension.path))};
        if (!core::io::is_directory(regions)) {
            continue;
        }
        auto names{core::io::list_directory(regions)};
        if (!names) {
            continue;
        }
        for (const auto &name : *names) {
            const auto coordinates{parse_region_name(name, ".mca")};
            if (!coordinates) {
                continue;
            }
            auto chunks{read_anvil_region(join_path(regions, name),
                                          coordinates->first,
                                          coordinates->second)};
            if (!chunks) {
                continue;
            }
            for (const auto &[pos, document] : *chunks) {
                auto chunk{chunk_from_nbt(document, pos)};
                if (!chunk) {
                    continue;
                }
                for (const auto &section : chunk->sections) {
                    for (const auto &entry : section.palette) {
                        keys.insert(entry.key);
                    }
                    for (const auto &key : section.biome_palette) {
                        keys.insert(key);
                    }
                }
            }
        }
    }
    palette_table table;
    for (const auto &key : keys) {
        static_cast<void>(table.intern(key));
    }
    return table;
}

[[nodiscard]] auto import_dimension(std::string_view source_dim,
                                    std::string_view target_dim,
                                    import_result &report, int zstd_level,
                                    const rwf::zstd_dictionary *dictionary,
                                    palette_table *palette) -> status {
    auto created{
        core::io::create_directories(fmt::format("{}/regions", target_dim))};
    if (!created) {
        return created;
    }
    if (auto blobs_created{
            core::io::create_directories(fmt::format("{}/blobs", target_dim))};
        !blobs_created) {
        return blobs_created;
    }
    const auto regions{join_path(source_dim, "region")};
    if (core::io::is_directory(regions)) {
        auto names{core::io::list_directory(regions)};
        if (!names) {
            return std::unexpected{names.error()};
        }
        for (const auto &name : *names) {
            const auto coordinates{parse_region_name(name, ".mca")};
            if (!coordinates) {
                continue;
            }
            auto chunks{read_anvil_region(join_path(regions, name),
                                          coordinates->first,
                                          coordinates->second)};
            if (!chunks) {
                return std::unexpected{chunks.error()};
            }
            if (chunks->empty()) {
                continue;
            }
            auto file{rwf::region_file::load_or_create(
                fmt::format("{}/regions/{}.{}.rfw", target_dim,
                            coordinates->first, coordinates->second),
                {.x = coordinates->first,
                 .z = coordinates->second,
                 .zstd_level = zstd_level,
                 .dictionary = dictionary})};
            if (!file) {
                return std::unexpected{file.error()};
            }
            for (const auto &[pos, document] : *chunks) {
                auto chunk{chunk_from_nbt(document, pos)};
                if (!chunk) {
                    return std::unexpected{chunk.error()};
                }
                auto payload{encode_chunk(*chunk, palette, false)};
                if (!payload) {
                    return std::unexpected{payload.error()};
                }
                if (auto written{file->write(pos, chunk->status, *payload)};
                    !written) {
                    return written;
                }
                ++report.chunks;
            }
            ++report.regions;
            if (auto committed{file->commit()}; !committed) {
                return committed;
            }
        }
    }
    auto blobs{rwf::blob_store{fmt::format("{}/blobs", target_dim)}};
    for (const auto *kind : {"entities", "poi"}) {
        const auto directory{join_path(source_dim, kind)};
        if (!core::io::is_directory(directory)) {
            continue;
        }
        auto names{core::io::list_directory(directory)};
        if (!names) {
            return std::unexpected{names.error()};
        }
        for (const auto &name : *names) {
            const auto coordinates{parse_region_name(name, ".mca")};
            if (!coordinates) {
                continue;
            }
            auto chunks{read_anvil_region(join_path(directory, name),
                                          coordinates->first,
                                          coordinates->second)};
            if (!chunks) {
                return std::unexpected{chunks.error()};
            }
            for (const auto &[pos, document] : *chunks) {
                auto bytes{nbt::write(document, "", nbt::compression::none)};
                if (!bytes) {
                    return std::unexpected{bytes.error()};
                }
                if (auto written{blobs.write(kind, pos.key(), *bytes, false)};
                    !written) {
                    return written;
                }
            }
        }
    }
    if (auto committed{blobs.commit()}; !committed) {
        return committed;
    }
    const auto data{join_path(source_dim, "data")};
    if (core::io::is_directory(data)) {
        object_store objects{fmt::format("{}/objects", target_dim)};
        std::size_t files{0};
        if (auto stored{store_object_tree(objects, data, "data", true, files)};
            !stored) {
            return stored;
        }
        if (auto committed{objects.commit()}; !committed) {
            return committed;
        }
        report.aux_files += files;
    }
    return status_ok();
}

[[nodiscard]] auto
export_dimension(std::string_view rwf_dim, std::string_view target_dim,
                 export_result &report, const palette_table *palette,
                 const rwf::zstd_dictionary *dictionary) -> status {
    const auto regions_dir{fmt::format("{}/regions", rwf_dim)};
    if (core::io::is_directory(regions_dir)) {
        auto names{core::io::list_directory(regions_dir)};
        if (!names) {
            return std::unexpected{names.error()};
        }
        for (const auto &name : *names) {
            const auto coordinates{
                parse_region_name(fmt::format("r.{}", name), ".rfw")};
            if (!coordinates) {
                continue;
            }
            auto region{rwf::region_file::load(join_path(regions_dir, name),
                                               {.x = coordinates->first,
                                                .z = coordinates->second,
                                                .dictionary = dictionary})};
            if (!region) {
                return std::unexpected{region.error()};
            }
            std::vector<std::pair<chunk_pos, value>> chunks;
            for (std::uint32_t slot{0}; slot < rwf::index_slots; ++slot) {
                const auto entries{region->entries()};
                if (entries[slot].empty()) {
                    continue;
                }
                const auto pos{position_for_slot(coordinates->first,
                                                 coordinates->second, slot,
                                                 rwf::region_edge)};
                auto payload{region->read(pos)};
                if (!payload) {
                    return std::unexpected{payload.error()};
                }
                auto chunk{decode_chunk(*payload, palette)};
                if (!chunk) {
                    return std::unexpected{chunk.error()};
                }
                auto nbt_chunk{chunk_to_nbt(*chunk, pos)};
                if (!nbt_chunk) {
                    return std::unexpected{nbt_chunk.error()};
                }
                chunks.emplace_back(pos, std::move(*nbt_chunk));
            }
            if (chunks.empty()) {
                continue;
            }
            auto documents{region_documents(chunks)};
            if (!documents) {
                return std::unexpected{documents.error()};
            }
            const auto path{fmt::format("{}/region/r.{}.{}.mca", target_dim,
                                        coordinates->first,
                                        coordinates->second)};
            if (auto written{write_anvil_region(path, std::move(*documents))};
                !written) {
                return written;
            }
            ++report.regions;
            report.chunks += chunks.size();
        }
    }
    auto blobs{rwf::blob_store{fmt::format("{}/blobs", rwf_dim)}};
    for (const auto *kind : {"entities", "poi"}) {
        auto keys{blobs.list(kind)};
        if (!keys) {
            return std::unexpected{keys.error()};
        }
        std::map<std::pair<std::int32_t, std::int32_t>,
                 std::vector<std::pair<chunk_pos, std::vector<std::byte>>>>
            documents;
        for (const auto &key : *keys) {
            const auto pos{parse_chunk_key(key)};
            if (!pos) {
                return std::unexpected{pos.error()};
            }
            auto payload{blobs.read(kind, key)};
            if (!payload) {
                return std::unexpected{payload.error()};
            }
            documents[{pos->x >> 5, pos->z >> 5}].emplace_back(*pos, *payload);
        }
        for (auto &[key, chunks] : documents) {
            const auto path{fmt::format("{}/{}/r.{}.{}.mca", target_dim, kind,
                                        key.first, key.second)};
            if (auto written{write_anvil_region(path, std::move(chunks))};
                !written) {
                return written;
            }
        }
    }
    object_store objects{fmt::format("{}/objects", rwf_dim)};
    std::size_t files{0};
    if (auto restored{restore_object_tree(
            objects, "data", join_path(target_dim, "data"), true, files)};
        !restored) {
        return restored;
    }
    report.aux_files += files;
    return status_ok();
}

[[nodiscard]] auto
train_chunk_dictionary(std::span<const std::string> dimension_roots,
                       palette_table &palette, std::size_t max_samples = 4096)
    -> std::optional<rwf::zstd_dictionary> {
    constexpr std::size_t max_bytes{32ULL << 20U};
    std::vector<std::byte> samples;
    std::vector<std::size_t> sizes;
    for (const auto &source_dim : dimension_roots) {
        const auto regions{join_path(source_dim, "region")};
        if (!core::io::is_directory(regions)) {
            continue;
        }
        auto names{core::io::list_directory(regions)};
        if (!names) {
            return std::nullopt;
        }
        for (const auto &name : *names) {
            const auto coordinates{parse_region_name(name, ".mca")};
            if (!coordinates) {
                continue;
            }
            auto chunks{read_anvil_region(join_path(regions, name),
                                          coordinates->first,
                                          coordinates->second)};
            if (!chunks) {
                return std::nullopt;
            }
            for (const auto &[pos, document] : *chunks) {
                if (sizes.size() >= max_samples ||
                    samples.size() >= max_bytes) {
                    break;
                }
                auto chunk{chunk_from_nbt(document, pos)};
                if (!chunk) {
                    return std::nullopt;
                }
                auto payload{encode_chunk(*chunk, &palette, false)};
                if (!payload) {
                    return std::nullopt;
                }
                sizes.push_back(payload->size());
                samples.insert(samples.end(), payload->begin(), payload->end());
            }
            if (sizes.size() >= max_samples || samples.size() >= max_bytes) {
                break;
            }
        }
        if (sizes.size() >= max_samples || samples.size() >= max_bytes) {
            break;
        }
    }
    if (sizes.size() < 64 || samples.size() < (256ULL << 10U)) {
        return std::nullopt;
    }
    std::vector<std::byte> dictionary(64U << 10U);
    const auto written{ZDICT_trainFromBuffer(
        dictionary.data(), dictionary.size(), samples.data(), sizes.data(),
        static_cast<unsigned>(sizes.size()))};
    if (ZDICT_isError(written) != 0U) {
        return std::nullopt;
    }
    dictionary.resize(written);
    return rwf::make_zstd_dictionary(dictionary);
}

auto import_world(std::string_view source, std::string_view target,
                  bool overwrite, unsigned threads, int zstd_level,
                  const rwf::zstd_dictionary *dictionary,
                  dictionary_scope scope) -> result<import_result> {
    if (!core::io::is_directory(source)) {
        return fail(error_code::not_found,
                    fmt::format("'{}' is not a world directory", source));
    }
    if (auto disjoint{check_disjoint_paths(source, target)}; !disjoint) {
        return std::unexpected{disjoint.error()};
    }
    if (auto prepared{prepare_target(target, overwrite)}; !prepared) {
        return std::unexpected{prepared.error()};
    }
    auto level{read_level_data(source)};
    if (!level) {
        return std::unexpected{level.error()};
    }
    auto store{world_store::create(target)};
    if (!store) {
        return std::unexpected{store.error()};
    }
    import_result report{};
    palette_table palette{build_palette_table(source)};
    if (auto installed{store->install_palette(palette)}; !installed) {
        return std::unexpected{installed.error()};
    }
    const auto overworld{join_path(source, "dimensions/minecraft/overworld")};
    if (!core::io::is_directory(overworld)) {
        return fail(error_code::parse_error,
                    fmt::format("'{}' is not a 26.3 world (missing "
                                "dimensions/minecraft/overworld)",
                                source));
    }
    auto dimensions{list_dimensions(source)};
    if (!dimensions) {
        return std::unexpected{dimensions.error()};
    }
    std::vector<std::string> dimension_roots;
    dimension_roots.reserve(dimensions->size());
    for (const auto &dimension : *dimensions) {
        const auto root{join_path(source, fmt::format("dimensions/{}/{}",
                                                      dimension.namespace_name,
                                                      dimension.path))};
        if (!core::io::is_directory(join_path(root, "region"))) {
            continue;
        }
        dimension_roots.push_back(root);
    }
    const bool world_scope =
        dictionary == nullptr && scope == dictionary_scope::world;
    const rwf::zstd_dictionary *overworld_dictionary{dictionary};
    std::optional<rwf::zstd_dictionary> trained_world;
    std::optional<rwf::zstd_dictionary> trained_overworld;
    if (dictionary != nullptr) {
        // An explicit --dictionary is a world-level fallback shared by every
        // dimension unless a dimension has its own dictionary.
        if (auto installed{store->install_dictionary(dictionary->bytes)};
            !installed) {
            return std::unexpected{installed.error()};
        }
    } else if (world_scope) {
        const auto per_dimension_samples{std::max<std::size_t>(
            64, 4096 / std::max<std::size_t>(1, dimension_roots.size()))};
        trained_world = train_chunk_dictionary(dimension_roots, palette,
                                               per_dimension_samples);
        if (trained_world.has_value()) {
            overworld_dictionary = &*trained_world;
            if (auto installed{store->install_dictionary(trained_world->bytes)};
                !installed) {
                return std::unexpected{installed.error()};
            }
        }
    } else {
        const std::array<std::string, 1> roots{overworld};
        trained_overworld = train_chunk_dictionary(roots, palette);
        if (trained_overworld.has_value()) {
            overworld_dictionary = &*trained_overworld;
            if (auto installed{store->install_dimension_dictionary(
                    "minecraft", "overworld", trained_overworld->bytes)};
                !installed) {
                return std::unexpected{installed.error()};
            }
        }
    }
    const auto regions{join_path(overworld, "region")};
    if (core::io::is_directory(regions)) {
        auto names{core::io::list_directory(regions)};
        if (!names) {
            return std::unexpected{names.error()};
        }
        struct region_source {
            std::string name;
            std::int32_t x{0};
            std::int32_t z{0};
        };
        std::vector<region_source> sources;
        for (const auto &name : *names) {
            const auto coordinates{parse_region_name(name, ".mca")};
            if (!coordinates) {
                continue;
            }
            sources.push_back({name, coordinates->first, coordinates->second});
        }
        // A worker that throws (bad_alloc, formatting, ...) is caught inside
        // the jobs executor; a default-constructed `status` would then look
        // like success and silently drop the region. Seed every slot with a
        // failure that the worker must overwrite.
        std::vector<status> outcomes(
            sources.size(),
            fail(error_code::io_error, "import worker did not complete"));
        std::vector<std::size_t> counts(sources.size(), 0);
        const auto import_one = [&](std::size_t index) {
            const auto &entry{sources[index]};
            auto chunks{read_anvil_region(join_path(regions, entry.name),
                                          entry.x, entry.z)};
            if (!chunks) {
                outcomes[index] = std::unexpected{chunks.error()};
                return;
            }
            if (chunks->empty()) {
                outcomes[index] = status_ok();
                return;
            }
            const auto path{fmt::format(
                "{}/dimensions/minecraft/overworld/regions/{}.{}.rfw", target,
                entry.x, entry.z)};
            auto file{rwf::region_file::load_or_create(
                path, {.x = entry.x,
                       .z = entry.z,
                       .zstd_level = zstd_level,
                       .dictionary = overworld_dictionary})};
            if (!file) {
                outcomes[index] = std::unexpected{file.error()};
                return;
            }
            for (const auto &[pos, document] : *chunks) {
                auto chunk{chunk_from_nbt(document, pos)};
                if (!chunk) {
                    outcomes[index] = std::unexpected{chunk.error()};
                    return;
                }
                auto payload{encode_chunk(*chunk, &palette, false)};
                if (!payload) {
                    outcomes[index] = std::unexpected{payload.error()};
                    return;
                }
                if (auto written{file->write(pos, chunk->status, *payload)};
                    !written) {
                    outcomes[index] = written;
                    return;
                }
                ++counts[index];
            }
            outcomes[index] = file->commit();
        };
        if (threads == 1 || sources.size() < 2) {
            for (std::size_t index{0}; index < sources.size(); ++index) {
                import_one(index);
            }
        } else {
            try {
                core::jobs::executor pool{threads};
                pool.parallel_for(sources.size(), 1, import_one);
            } catch (const std::system_error &error) {
                return fail(error_code::io_error,
                            fmt::format("cannot start import worker pool: {}",
                                        error.what()));
            }
        }
        for (std::size_t index{0}; index < sources.size(); ++index) {
            if (!outcomes[index]) {
                return std::unexpected{outcomes[index].error()};
            }
            if (counts[index] != 0) {
                ++report.regions;
                report.chunks += counts[index];
            }
        }
    }
    auto &objects{store->objects()};
    std::size_t object_files{0};
    if (auto stored{store_object_tree(objects, join_path(source, "data"),
                                      "global/data", true, object_files)};
        !stored) {
        return std::unexpected{stored.error()};
    }
    const auto overworld_data{join_path(overworld, "data")};
    if (core::io::is_directory(overworld_data)) {
        std::size_t files{0};
        if (auto stored{store_object_tree(objects, overworld_data,
                                          "overworld/data", true, files)};
            !stored) {
            return std::unexpected{stored.error()};
        }
        report.aux_files += files;
    }
    for (const auto *kind : {"advancements", "stats"}) {
        const auto directory{
            join_path(source, fmt::format("players/{}", kind))};
        if (!core::io::is_directory(directory)) {
            continue;
        }
        auto names{core::io::list_directory(directory)};
        if (!names) {
            return std::unexpected{names.error()};
        }
        for (const auto &name : *names) {
            if (!name.ends_with(".json")) {
                continue;
            }
            const auto uuid{name.substr(0, name.size() - 5)};
            if (!object_store::valid_path(uuid)) {
                return fail(error_code::parse_error,
                            fmt::format("player {} name '{}' is not a valid "
                                        "object key",
                                        kind, name));
            }
            auto bytes{core::io::read_file(
                join_path(directory, name),
                {.max_bytes = 16ULL << 20U, .allow_symlink = false})};
            if (!bytes) {
                return std::unexpected{bytes.error()};
            }
            if (auto written{
                    objects.write(fmt::format("players/{}/{}", uuid, kind),
                                  *bytes, object_media::json, false)};
                !written) {
                return std::unexpected{written.error()};
            }
        }
    }
    const auto playerdata{join_path(source, "players/data")};
    if (core::io::is_directory(playerdata)) {
        auto names{core::io::list_directory(playerdata)};
        if (!names) {
            return std::unexpected{names.error()};
        }
        for (const auto &name : *names) {
            if (name.ends_with(".dat")) {
                continue;
            }
            const auto from{join_path(playerdata, name)};
            if (core::io::is_symlink(from) ||
                !core::io::is_regular_file(from)) {
                continue;
            }
            auto bytes{core::io::read_file(
                from, {.max_bytes = 16ULL << 20U, .allow_symlink = false})};
            if (!bytes) {
                return std::unexpected{bytes.error()};
            }
            if (auto written{
                    objects.write(fmt::format("players/extra/{}",
                                              encode_object_component(name)),
                                  *bytes, object_media::binary, false)};
                !written) {
                return std::unexpected{written.error()};
            }
        }
    }
    const auto icon{join_path(source, "icon.png")};
    if (core::io::is_regular_file(icon) && !core::io::is_symlink(icon)) {
        auto bytes{core::io::read_file(
            icon, {.max_bytes = 16ULL << 20U, .allow_symlink = false})};
        if (!bytes) {
            return std::unexpected{bytes.error()};
        }
        if (auto written{
                objects.write("assets/icon", *bytes, object_media::png, false)};
            !written) {
            return std::unexpected{written.error()};
        }
    }
    if (auto stored{store_object_tree(objects, join_path(source, "datapacks"),
                                      "datapacks", false,
                                      report.datapack_files)};
        !stored) {
        return std::unexpected{stored.error()};
    }

    for (const auto *kind : {"entities", "poi"}) {
        const auto directory{join_path(overworld, kind)};
        if (!core::io::is_directory(directory)) {
            continue;
        }
        auto names{core::io::list_directory(directory)};
        if (!names) {
            return std::unexpected{names.error()};
        }
        auto &blobs{store->blobs()};
        for (const auto &name : *names) {
            const auto coordinates{parse_region_name(name, ".mca")};
            if (!coordinates) {
                continue;
            }
            auto chunks{read_anvil_region(join_path(directory, name),
                                          coordinates->first,
                                          coordinates->second)};
            if (!chunks) {
                return std::unexpected{chunks.error()};
            }
            for (const auto &[pos, document] : *chunks) {
                auto bytes{nbt::write(document, "", nbt::compression::none)};
                if (!bytes) {
                    return std::unexpected{bytes.error()};
                }
                if (auto written{blobs.write(kind, pos.key(), *bytes, false)};
                    !written) {
                    return std::unexpected{written.error()};
                }
                if (std::string_view{kind} == "entities") {
                    ++report.entity_chunks;
                } else {
                    ++report.poi_chunks;
                }
            }
        }
    }
    const auto playerdata_blobs{join_path(source, "players/data")};
    if (core::io::is_directory(playerdata_blobs)) {
        auto names{core::io::list_directory(playerdata_blobs)};
        if (!names) {
            return std::unexpected{names.error()};
        }
        auto &blobs{store->blobs()};
        for (const auto &name : *names) {
            if (!name.ends_with(".dat")) {
                continue;
            }
            const auto key{name.substr(0, name.size() - 4)};
            if (!rwf::blob_store::valid_key(key)) {
                return fail(error_code::parse_error,
                            fmt::format("playerdata name '{}' is not a valid "
                                        "key",
                                        name));
            }
            auto document{nbt::read_file(join_path(playerdata_blobs, name),
                                         nbt::compression::gzip)};
            if (!document) {
                return std::unexpected{document.error()};
            }
            auto bytes{nbt::write(*document, "", nbt::compression::none)};
            if (!bytes) {
                return std::unexpected{bytes.error()};
            }
            if (auto written{blobs.write("playerdata", key, *bytes, false)};
                !written) {
                return std::unexpected{written.error()};
            }
            ++report.playerdata;
        }
    }
    for (const auto &dimension : *dimensions) {
        if (dimension.namespace_name == "minecraft" &&
            dimension.path == "overworld") {
            continue;
        }
        const auto source_dim{join_path(
            source, fmt::format("dimensions/{}/{}", dimension.namespace_name,
                                dimension.path))};
        const auto target_dim{join_path(
            target, fmt::format("dimensions/{}/{}", dimension.namespace_name,
                                dimension.path))};
        const rwf::zstd_dictionary *dimension_dictionary{
            world_scope || dictionary != nullptr ? overworld_dictionary
                                                 : nullptr};
        std::optional<rwf::zstd_dictionary> trained_dimension;
        if (dictionary == nullptr && !world_scope) {
            const std::array<std::string, 1> roots{source_dim};
            trained_dimension = train_chunk_dictionary(roots, palette);
            if (trained_dimension.has_value()) {
                dimension_dictionary = &*trained_dimension;
                if (auto installed{store->install_dimension_dictionary(
                        dimension.namespace_name, dimension.path,
                        trained_dimension->bytes)};
                    !installed) {
                    return std::unexpected{installed.error()};
                }
            }
        }
        if (auto imported{import_dimension(source_dim, target_dim, report,
                                           zstd_level, dimension_dictionary,
                                           &palette)};
            !imported) {
            return std::unexpected{imported.error()};
        }
    }
    if (auto committed{store->commit()}; !committed) {
        return std::unexpected{committed.error()};
    }
    // Meta last: an interrupted import leaves no `meta.rfw`, so a partial
    // target is visibly incomplete instead of silently opening.
    const auto meta{meta_from_level(*level)};
    auto meta_bytes{nbt::write(meta, "", nbt::compression::none)};
    if (!meta_bytes) {
        return std::unexpected{meta_bytes.error()};
    }
    if (auto written{store->write_meta(*meta_bytes)}; !written) {
        return std::unexpected{written.error()};
    }
    for (const auto *directory :
         {"", "regions", "blobs/entities", "blobs/poi", "blobs/playerdata"}) {
        const auto path{directory[0] == '\0' ? std::string{target}
                                             : join_path(target, directory)};
        if (core::io::is_directory(path)) {
            if (auto synced{platform::file::sync_directory(path)}; !synced) {
                return std::unexpected{synced.error()};
            }
        }
    }
    return report;
}

auto export_world(std::string_view source, std::string_view target,
                  bool overwrite, unsigned threads) -> result<export_result> {
    if (auto disjoint{check_disjoint_paths(source, target)}; !disjoint) {
        return std::unexpected{disjoint.error()};
    }
    auto store{world_store::open(source)};
    if (!store) {
        return std::unexpected{store.error()};
    }
    auto meta{store->read_meta()};
    if (!meta) {
        return std::unexpected{meta.error()};
    }
    const auto document{nbt::read(*meta)};
    if (!document) {
        return std::unexpected{document.error()};
    }
    const auto *level{document->get("level")};
    if (level == nullptr || level->as_compound() == nullptr) {
        return parse_failure("meta.rfw has no 'level' compound");
    }
    if (auto prepared{prepare_target(target, overwrite)}; !prepared) {
        return std::unexpected{prepared.error()};
    }
    const std::string overworld{
        join_path(target, "dimensions/minecraft/overworld")};
    const std::string players{join_path(target, "players/data")};
    for (const auto &directory :
         {std::string{target}, join_path(overworld, "region"),
          join_path(overworld, "entities"), join_path(overworld, "poi"),
          players, join_path(target, "datapacks")}) {
        if (auto created{core::io::create_directories(directory)}; !created) {
            return std::unexpected{created.error()};
        }
    }
    value level_root{nbt::make_compound()};
    set_child(level_root, "Data", *level);
    if (auto written{nbt::write_file(join_path(target, "level.dat"), level_root,
                                     "", nbt::compression::gzip)};
        !written) {
        return std::unexpected{written.error()};
    }

    export_result report{};
    auto positions{store->chunk_positions()};
    if (!positions) {
        return std::unexpected{positions.error()};
    }
    struct export_region {
        std::int32_t x{0};
        std::int32_t z{0};
        std::vector<chunk_pos> positions;
    };
    std::map<std::pair<std::int32_t, std::int32_t>, std::vector<chunk_pos>>
        grouped;
    for (const auto &pos : *positions) {
        grouped[{pos.x >> 5, pos.z >> 5}].push_back(pos);
    }
    std::vector<export_region> regions;
    regions.reserve(grouped.size());
    for (auto &[key, items] : grouped) {
        regions.push_back({key.first, key.second, std::move(items)});
    }
    std::vector<status> outcomes(
        regions.size(),
        fail(error_code::io_error, "export worker did not complete"));
    std::vector<std::size_t> counts(regions.size(), 0);
    const auto export_one = [&](std::size_t index) {
        auto local{world_store::open(source)};
        if (!local) {
            outcomes[index] = std::unexpected{local.error()};
            return;
        }
        const auto &region{regions[index]};
        const auto path{fmt::format("{}/r.{}.{}.mca",
                                    join_path(overworld, "region"), region.x,
                                    region.z)};
        // One chunk at a time: a full region's NBT documents are never held in
        // memory together.
        const auto make_document =
            [&](chunk_pos pos) -> result<std::vector<std::byte>> {
            auto chunk{local->read_chunk(pos)};
            if (!chunk) {
                return std::unexpected{chunk.error()};
            }
            auto nbt_chunk{chunk_to_nbt(*chunk, pos)};
            if (!nbt_chunk) {
                return std::unexpected{nbt_chunk.error()};
            }
            return nbt::write(*nbt_chunk, "", nbt::compression::none);
        };
        outcomes[index] =
            write_anvil_region_stream(path, region.positions, make_document);
        counts[index] = region.positions.size();
    };
    if (threads == 1 || regions.size() < 2) {
        for (std::size_t index{0}; index < regions.size(); ++index) {
            export_one(index);
        }
    } else {
        try {
            core::jobs::executor pool{threads};
            pool.parallel_for(regions.size(), 1, export_one);
        } catch (const std::system_error &error) {
            return fail(error_code::io_error,
                        fmt::format("cannot start export worker pool: {}",
                                    error.what()));
        }
    }
    for (std::size_t index{0}; index < regions.size(); ++index) {
        if (!outcomes[index]) {
            return std::unexpected{outcomes[index].error()};
        }
        if (counts[index] != 0) {
            ++report.regions;
            report.chunks += counts[index];
        }
    }

    auto &blobs{store->blobs()};
    for (const auto *kind : {"entities", "poi"}) {
        auto keys{blobs.list(kind)};
        if (!keys) {
            return std::unexpected{keys.error()};
        }
        std::map<std::pair<std::int32_t, std::int32_t>,
                 std::vector<std::pair<chunk_pos, std::vector<std::byte>>>>
            documents;
        for (const auto &key : *keys) {
            const auto pos{parse_chunk_key(key)};
            if (!pos) {
                return std::unexpected{pos.error()};
            }
            auto payload{blobs.read(kind, key)};
            if (!payload) {
                return std::unexpected{payload.error()};
            }
            const auto check{nbt::read(*payload)};
            if (!check) {
                return std::unexpected{check.error()};
            }
            documents[{pos->x >> 5, pos->z >> 5}].emplace_back(*pos, *payload);
            if (std::string_view{kind} == "entities") {
                ++report.entity_chunks;
            } else {
                ++report.poi_chunks;
            }
        }
        for (auto &[key, chunks] : documents) {
            const auto path{fmt::format("{}/{}/r.{}.{}.mca", overworld, kind,
                                        key.first, key.second)};
            if (auto written{write_anvil_region(path, std::move(chunks))};
                !written) {
                return std::unexpected{written.error()};
            }
        }
    }
    auto player_keys{blobs.list("playerdata")};
    if (!player_keys) {
        return std::unexpected{player_keys.error()};
    }
    for (const auto &key : *player_keys) {
        auto payload{blobs.read("playerdata", key)};
        if (!payload) {
            return std::unexpected{payload.error()};
        }
        const auto player{nbt::read(*payload)};
        if (!player) {
            return std::unexpected{player.error()};
        }
        if (auto written{nbt::write_file(fmt::format("{}/{}.dat", players, key),
                                         *player, "", nbt::compression::gzip)};
            !written) {
            return std::unexpected{written.error()};
        }
        ++report.playerdata;
    }
    auto &objects{store->objects()};
    std::size_t restored{0};
    if (auto status{restore_object_tree(
            objects, "global/data", join_path(target, "data"), true, restored)};
        !status) {
        return std::unexpected{status.error()};
    }
    std::size_t overworld_files{0};
    if (auto status{restore_object_tree(objects, "overworld/data",
                                        join_path(overworld, "data"), true,
                                        overworld_files)};
        !status) {
        return std::unexpected{status.error()};
    }
    report.aux_files += overworld_files;
    auto object_paths{objects.list("players/")};
    if (!object_paths) {
        return std::unexpected{object_paths.error()};
    }
    for (const auto &path : *object_paths) {
        const auto relative{path.substr(std::string{"players/"}.size())};
        auto bytes{objects.read(path)};
        if (!bytes) {
            return std::unexpected{bytes.error()};
        }
        if (relative.starts_with("extra/")) {
            const auto encoded_name{relative.substr(6)};
            auto name{decode_object_component(encoded_name)};
            if (!name) {
                return std::unexpected{name.error()};
            }
            const auto target_path{join_path(players, *name)};
            if (auto written{core::io::write_file(target_path, *bytes)};
                !written) {
                return std::unexpected{written.error()};
            }
            continue;
        }
        const auto slash{relative.find('/')};
        if (slash == std::string::npos) {
            continue;
        }
        const auto uuid{relative.substr(0, slash)};
        const auto kind{relative.substr(slash + 1)};
        if (kind != "advancements" && kind != "stats") {
            continue;
        }
        const auto directory{
            join_path(target, fmt::format("players/{}", kind))};
        if (auto created{core::io::create_directories(directory)}; !created) {
            return std::unexpected{created.error()};
        }
        if (auto written{core::io::write_file(
                fmt::format("{}/{}.json", directory, uuid), *bytes)};
            !written) {
            return std::unexpected{written.error()};
        }
    }
    if (objects.exists("assets/icon")) {
        auto bytes{objects.read("assets/icon")};
        if (!bytes) {
            return std::unexpected{bytes.error()};
        }
        if (auto written{
                core::io::write_file(join_path(target, "icon.png"), *bytes)};
            !written) {
            return std::unexpected{written.error()};
        }
    }
    if (auto status{restore_object_tree(objects, "datapacks",
                                        join_path(target, "datapacks"), false,
                                        report.datapack_files)};
        !status) {
        return std::unexpected{status.error()};
    }
    auto dimensions{list_dimensions(source)};
    if (!dimensions) {
        return std::unexpected{dimensions.error()};
    }
    for (const auto &dimension : *dimensions) {
        if (dimension.namespace_name == "minecraft" &&
            dimension.path == "overworld") {
            continue;
        }
        const auto rwf_dim{join_path(
            source, fmt::format("dimensions/{}/{}", dimension.namespace_name,
                                dimension.path))};
        const auto target_dim{join_path(
            target, fmt::format("dimensions/{}/{}", dimension.namespace_name,
                                dimension.path))};
        if (auto exported{
                export_dimension(rwf_dim, target_dim, report, &store->palette(),
                                 store->dictionary_for(dimension.namespace_name,
                                                       dimension.path))};
            !exported) {
            return std::unexpected{exported.error()};
        }
    }
    return report;
}

namespace {

struct temp_world_guard {
    std::string path;
    ~temp_world_guard() {
        if (!path.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(std::filesystem::path{path}, ignored);
        }
    }
};

} // namespace

auto rebuild_palette(std::string_view source, std::string_view target,
                     bool overwrite, unsigned threads, int zstd_level)
    -> result<palette_gc_result> {
    if (!core::io::is_directory(source)) {
        return fail(error_code::not_found,
                    fmt::format("'{}' is not a world directory", source));
    }
    if (auto disjoint{check_disjoint_paths(source, target)}; !disjoint) {
        return std::unexpected{disjoint.error()};
    }
    const auto target_path{std::filesystem::path{std::string{target}}};
    const auto parent{target_path.parent_path().empty()
                          ? std::filesystem::path{"."}
                          : target_path.parent_path()};
    // Never delete a pre-existing path to make room: pick the first free
    // `.refrain-palette-gc-<pid>-<n>` sibling instead.
    std::string temporary_path;
    for (unsigned attempt{0}; attempt < 16U; ++attempt) {
        const auto candidate{
            (parent / fmt::format(".refrain-palette-gc-{}-{}",
                                  platform::process_id(), attempt))
                .string()};
        if (!core::io::exists(candidate)) {
            temporary_path = candidate;
            break;
        }
    }
    if (temporary_path.empty()) {
        return fail(error_code::already_exists,
                    fmt::format("cannot allocate a temporary world next to "
                                "'{}'",
                                target));
    }
    temp_world_guard temporary{temporary_path};
    if (auto disjoint{check_disjoint_paths(source, temporary.path)};
        !disjoint) {
        return std::unexpected{disjoint.error()};
    }

    palette_gc_result report{};
    {
        auto store{world_store::open(source)};
        if (!store) {
            return std::unexpected{store.error()};
        }
        report.entries_before = store->palette().size();
    }

    auto exported{export_world(source, temporary.path, false, threads)};
    if (!exported) {
        return std::unexpected{exported.error()};
    }
    auto imported{import_world(temporary.path, target, overwrite, threads,
                               zstd_level, nullptr,
                               dictionary_scope::dimension)};
    if (!imported) {
        return std::unexpected{imported.error()};
    }
    report.chunks = imported->chunks;
    report.regions = imported->regions;

    auto store{world_store::open(target)};
    if (!store) {
        return std::unexpected{store.error()};
    }
    report.entries_after = store->palette().size();
    auto before{world_store::open(source)};
    if (before) {
        if (auto stats{before->stats()}; stats) {
            report.bytes_before = stats->bytes;
        }
    }
    if (auto stats{store->stats()}; stats) {
        report.bytes_after = stats->bytes;
    }
    return report;
}

} // namespace refrain::world::anvil
