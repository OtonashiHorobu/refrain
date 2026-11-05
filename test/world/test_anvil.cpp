// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/io.hpp>
#include <refrain/data/nbt.hpp>
#include <refrain/world/anvil.hpp>
#include <refrain/world/chunk_codec.hpp>
#include <refrain/world/model.hpp>
#include <refrain/world/store.hpp>

#include <boost/ut.hpp>

#include <fmt/format.h>

#include <lz4.h>
#include <xxhash.h>

#include <cstdio>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;
namespace nbt = refrain::data::nbt;
namespace world = refrain::world;
namespace anvil = refrain::world::anvil;
namespace io = refrain::core::io;
namespace fs = std::filesystem;

using nbt::tag_type;
using nbt::value;

class temp_directory {
  public:
    temp_directory() {
        static std::atomic<std::uint64_t> sequence{0};
        std::error_code error;
        const auto base{fs::temp_directory_path(error)};
        if (error) {
            return;
        }
        for (int attempt{0}; attempt < 16; ++attempt) {
            const auto candidate{
                base / ("refrain-anvil-" + std::to_string(sequence.fetch_add(
                                               1, std::memory_order_relaxed)))};
            if (fs::create_directories(candidate, error)) {
                path_ = candidate;
                return;
            }
            if (error && error != std::errc::file_exists) {
                return;
            }
            error.clear();
        }
    }
    temp_directory(const temp_directory &) = delete;
    auto operator=(const temp_directory &) -> temp_directory & = delete;
    ~temp_directory() {
        if (!path_.empty()) {
            std::error_code error;
            fs::remove_all(path_, error);
        }
    }

    [[nodiscard]] auto path() const -> const fs::path & { return path_; }
    [[nodiscard]] auto sub(std::string_view name) const -> std::string {
        return (path_ / std::string{name}).string();
    }

  private:
    fs::path path_;
};

void set(value &parent, std::string key, value child) {
    expect(static_cast<bool>(parent.set(std::move(key), std::move(child))));
}

[[nodiscard]] auto make_int(std::int32_t number) -> value {
    return nbt::make_int(number);
}

[[nodiscard]] auto make_long(std::int64_t number) -> value {
    return nbt::make_long(number);
}

[[nodiscard]] auto make_string(std::string text) -> value {
    return nbt::make_string(std::move(text));
}

[[nodiscard]] auto make_byte_array(const std::vector<std::byte> &bytes)
    -> value {
    nbt::byte_array out;
    out.reserve(bytes.size());
    for (const auto byte : bytes) {
        out.push_back(static_cast<std::uint8_t>(byte));
    }
    return nbt::make_byte_array(std::move(out));
}

[[nodiscard]] auto make_compound() -> value { return nbt::make_compound(); }

[[nodiscard]] auto new_list(tag_type type) -> value {
    return nbt::make_list(type);
}

void push(value &list, value item) {
    expect(static_cast<bool>(list.push(std::move(item))));
}

[[nodiscard]] auto palette_entry(std::string name) -> value {
    return make_string(std::move(name));
}

[[nodiscard]] auto palette_entry_compound(std::string name) -> value {
    value entry{make_compound()};
    set(entry, "", make_string(std::move(name)));
    return entry;
}

[[nodiscard]] auto
palette_entry(std::string name,
              std::vector<std::pair<std::string, std::string>> properties)
    -> value {
    value entry{make_compound()};
    set(entry, "id", make_string(std::move(name)));
    value props{make_compound()};
    for (auto &[key, text] : properties) {
        set(props, std::move(key), make_string(std::move(text)));
    }
    set(entry, "properties", std::move(props));
    return entry;
}

[[nodiscard]] auto packed_words(std::size_t entries, std::uint8_t bits,
                                std::size_t palette_size)
    -> std::vector<std::int64_t> {
    const auto per_long{std::size_t{64} / bits};
    std::vector<std::int64_t> words(world::packed_word_count(bits, entries), 0);
    for (std::size_t index{0}; index < entries; ++index) {
        const auto palette_index{index % palette_size};
        const auto shift{(index % per_long) * bits};
        words[index / per_long] |= static_cast<std::int64_t>(
            static_cast<std::uint64_t>(palette_index << shift));
    }
    return words;
}

[[nodiscard]] auto section_single(std::int8_t y, std::string_view block)
    -> value {
    value section{make_compound()};
    set(section, "Y", nbt::make_byte(y));
    value states{make_compound()};
    value palette{new_list(tag_type::string)};
    push(palette, palette_entry(std::string{block}));
    set(states, "palette", std::move(palette));
    set(section, "block_states", std::move(states));
    value biomes{make_compound()};
    value biome_palette{new_list(tag_type::string)};
    push(biome_palette, make_string("minecraft:plains"));
    set(biomes, "palette", std::move(biome_palette));
    set(section, "biomes", std::move(biomes));
    return section;
}

[[nodiscard]] auto section_palette(std::int8_t y) -> value {
    value section{make_compound()};
    set(section, "Y", nbt::make_byte(y));
    value states{make_compound()};
    value palette{new_list(tag_type::compound)};
    push(palette, palette_entry_compound("minecraft:air"));
    push(palette, palette_entry_compound("minecraft:stone"));
    push(palette, palette_entry("minecraft:oak_log", {{"axis", "y"}}));
    set(states, "palette", std::move(palette));
    // Vanilla uses 4 bits for a 3-entry block palette.
    set(states, "data",
        nbt::make_long_array(packed_words(world::section_volume, 4, 3)));
    set(section, "block_states", std::move(states));

    value biomes{make_compound()};
    value biome_palette{new_list(tag_type::string)};
    push(biome_palette, make_string("minecraft:plains"));
    push(biome_palette, make_string("minecraft:desert"));
    set(biomes, "palette", std::move(biome_palette));
    set(biomes, "data",
        nbt::make_long_array(packed_words(world::biome_volume, 1, 2)));
    set(section, "biomes", std::move(biomes));
    std::vector<std::byte> light(world::light_nibble_bytes, std::byte{0xFF});
    set(section, "SkyLight", make_byte_array(light));
    return section;
}

/// Vanilla light-engine padding section: `Y` plus one light array, no
/// `block_states`/`biomes`.
[[nodiscard]] auto section_light_padding(std::int8_t y, bool sky) -> value {
    value section{make_compound()};
    set(section, "Y", nbt::make_byte(y));
    std::vector<std::byte> light(world::light_nibble_bytes, std::byte{0});
    light[0] = sky ? std::byte{0x22} : std::byte{0x11};
    set(section, sky ? "SkyLight" : "BlockLight", make_byte_array(light));
    return section;
}

[[nodiscard]] auto make_chunk_nbt(world::chunk_pos pos, int tick_priority = -1,
                                  int y_pos = -4, int section_base = -4)
    -> value {
    value root{make_compound()};
    set(root, "DataVersion", make_int(5023));
    set(root, "xPos", make_int(pos.x));
    set(root, "zPos", make_int(pos.z));
    set(root, "yPos", make_int(y_pos));
    set(root, "Status", make_string("minecraft:full"));
    set(root, "LastUpdate", make_long(1000));
    set(root, "InhabitedTime", make_long(2000));
    set(root, "isLightOn", nbt::make_byte(1));
    value sections{new_list(tag_type::compound)};
    push(sections, section_single(static_cast<std::int8_t>(section_base),
                                  "minecraft:bedrock"));
    push(sections, section_palette(static_cast<std::int8_t>(section_base + 4)));
    set(root, "sections", std::move(sections));

    value heightmaps{make_compound()};
    set(heightmaps, "MOTION_BLOCKING",
        nbt::make_long_array(
            std::vector<std::int64_t>(37, static_cast<std::int64_t>(0x0102))));
    set(root, "Heightmaps", std::move(heightmaps));

    value ticks{new_list(tag_type::compound)};
    value tick{make_compound()};
    set(tick, "i", make_string("minecraft:water"));
    set(tick, "p", make_int(tick_priority));
    set(tick, "x", make_int(0));
    set(tick, "y", make_int(70));
    set(tick, "z", make_int(0));
    set(tick, "t", make_int(20));
    push(ticks, std::move(tick));
    set(root, "block_ticks", std::move(ticks));

    value entities{new_list(tag_type::compound)};
    value chest{make_compound()};
    set(chest, "id", make_string("minecraft:chest"));
    set(chest, "x", make_int(0));
    set(chest, "y", make_int(70));
    set(chest, "z", make_int(0));
    set(chest, "KeepPacked", nbt::make_byte(0));
    push(entities, std::move(chest));
    set(root, "block_entities", std::move(entities));

    value structures{make_compound()};
    set(structures, "starts", make_compound());
    set(structures, "References", make_compound());
    set(root, "structures", std::move(structures));

    value post{new_list(tag_type::list)};
    for (std::size_t index{0}; index < 2; ++index) {
        value inner{new_list(tag_type::short_)};
        if (index == 1) {
            push(inner, nbt::make_short(15));
        }
        push(post, std::move(inner));
    }
    set(root, "PostProcessing", std::move(post));
    return root;
}

[[nodiscard]] auto get_be32(std::span<const std::byte> data, std::size_t offset)
    -> std::uint32_t {
    return (static_cast<std::uint32_t>(data[offset]) << 24U) |
           (static_cast<std::uint32_t>(data[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(data[offset + 2]) << 8U) |
           static_cast<std::uint32_t>(data[offset + 3]);
}

void put_be32(std::vector<std::byte> &data, std::size_t offset,
              std::uint32_t value) {
    data[offset] = static_cast<std::byte>((value >> 24U) & 0xFFU);
    data[offset + 1] = static_cast<std::byte>((value >> 16U) & 0xFFU);
    data[offset + 2] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    data[offset + 3] = static_cast<std::byte>(value & 0xFFU);
}

void put_le32(std::vector<std::byte> &data, std::size_t offset,
              std::uint32_t value) {
    data[offset] = static_cast<std::byte>(value & 0xFFU);
    data[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    data[offset + 2] = static_cast<std::byte>((value >> 16U) & 0xFFU);
    data[offset + 3] = static_cast<std::byte>((value >> 24U) & 0xFFU);
}

/// Minimal Anvil region writer used only by the tests (the production writer
/// lives in src/world/anvil.cpp and is exercised through `world export`).
void write_region_file(
    const std::string &path,
    const std::vector<std::pair<std::uint32_t, std::vector<std::byte>>>
        &documents,
    std::uint8_t compression_type = 2) {
    constexpr std::size_t sector_bytes = 4096;
    std::vector<std::byte> header(8192, std::byte{0});
    std::uint32_t next_sector{2};
    std::vector<std::byte> body;
    for (const auto &[slot, document] : documents) {
        const auto payload_size{1 + document.size()};
        const auto total{4 + payload_size};
        const auto sectors{static_cast<std::uint32_t>(
            (total + sector_bytes - 1) / sector_bytes)};
        std::vector<std::byte> block(sectors * sector_bytes, std::byte{0});
        put_be32(block, 0, static_cast<std::uint32_t>(payload_size));
        block[4] = static_cast<std::byte>(compression_type);
        std::copy(document.begin(), document.end(), block.begin() + 5);
        put_be32(header, slot * 4, (next_sector << 8U) | sectors);
        body.insert(body.end(), block.begin(), block.end());
        next_sector += sectors;
    }
    std::vector<std::byte> file{header.begin(), header.end()};
    file.insert(file.end(), body.begin(), body.end());
    expect(static_cast<bool>(io::write_file(path, file)));
}

[[nodiscard]] auto raw_document_bytes(const value &document)
    -> std::vector<std::byte> {
    auto bytes{nbt::write(document, "", nbt::compression::none)};
    expect(static_cast<bool>(bytes));
    return bytes ? std::move(*bytes) : std::vector<std::byte>{};
}

/// Synthetic lz4-java `LZ4BlockOutputStream` fixture: one compressed 64 KiB
/// block followed by the terminal empty block. The decoder under test is the
/// production path; this writer exists only in the test. Checksums are masked
/// with `0x0FFFFFFF` because lz4-java's `StreamingXXHash32.asChecksum()` drops
/// the high nibble; real 26.3 `region-file-compression=lz4` files carry the
/// masked value.
[[nodiscard]] auto lz4_block_stream(const std::vector<std::byte> &raw,
                                    bool force_raw = false)
    -> std::vector<std::byte> {
    constexpr std::array<std::byte, 8> magic{
        std::byte{'L'}, std::byte{'Z'}, std::byte{'4'}, std::byte{'B'},
        std::byte{'l'}, std::byte{'o'}, std::byte{'c'}, std::byte{'k'}};
    expect(raw.size() <= 65536U);
    std::vector<std::byte> payload;
    std::uint8_t token{0x26}; // method 32 (LZ4), 64 KiB block
    if (force_raw) {
        payload = raw;
        token = 0x16; // method 16 (raw), 64 KiB block
    } else {
        const auto bound{LZ4_compressBound(static_cast<int>(raw.size()))};
        expect(bound > 0);
        payload.resize(static_cast<std::size_t>(bound));
        const auto written{
            LZ4_compress_default(reinterpret_cast<const char *>(raw.data()),
                                 reinterpret_cast<char *>(payload.data()),
                                 static_cast<int>(raw.size()), bound)};
        expect(written > 0);
        payload.resize(static_cast<std::size_t>(written));
    }

    std::vector<std::byte> stream;
    stream.insert(stream.end(), magic.begin(), magic.end());
    stream.push_back(static_cast<std::byte>(token));
    const auto base{stream.size()};
    stream.resize(base + 12);
    put_le32(stream, base, static_cast<std::uint32_t>(payload.size()));
    put_le32(stream, base + 4, static_cast<std::uint32_t>(raw.size()));
    // lz4-java stores XXH32 & 0x0FFFFFFF (see anvil_lz4.cpp).
    put_le32(stream, base + 8,
             XXH32(raw.data(), raw.size(), 0x9747B28CU) & 0x0FFFFFFFU);
    stream.insert(stream.end(), payload.begin(), payload.end());

    stream.insert(stream.end(), magic.begin(), magic.end());
    stream.push_back(std::byte{0x20}); // terminal empty block
    stream.resize(stream.size() + 12, std::byte{0});
    return stream;
}

/// Multi-block lz4-java stream: 64 KiB blocks plus the terminal marker. Real
/// 1 MiB chunks use ~16 blocks, so the decoder loop cannot be tested with the
/// single-block fixture alone.
[[nodiscard]] auto lz4_block_stream_multi(const std::vector<std::byte> &raw)
    -> std::vector<std::byte> {
    constexpr std::array<std::byte, 8> magic{
        std::byte{'L'}, std::byte{'Z'}, std::byte{'4'}, std::byte{'B'},
        std::byte{'l'}, std::byte{'o'}, std::byte{'c'}, std::byte{'k'}};
    std::vector<std::byte> stream;
    for (std::size_t offset{0}; offset < raw.size(); offset += 65536U) {
        const auto count{std::min<std::size_t>(65536U, raw.size() - offset)};
        const std::span<const std::byte> block{raw.data() + offset, count};
        std::vector<std::byte> payload;
        std::uint8_t token{0x26};
        const auto bound{LZ4_compressBound(static_cast<int>(count))};
        expect(bound > 0);
        payload.resize(static_cast<std::size_t>(bound));
        const auto written{
            LZ4_compress_default(reinterpret_cast<const char *>(block.data()),
                                 reinterpret_cast<char *>(payload.data()),
                                 static_cast<int>(count), bound)};
        expect(written > 0);
        payload.resize(static_cast<std::size_t>(written));
        if (payload.size() >= count) {
            payload.assign(block.begin(), block.end());
            token = 0x16;
        }
        stream.insert(stream.end(), magic.begin(), magic.end());
        stream.push_back(static_cast<std::byte>(token));
        const auto base{stream.size()};
        stream.resize(base + 12);
        put_le32(stream, base, static_cast<std::uint32_t>(payload.size()));
        put_le32(stream, base + 4, static_cast<std::uint32_t>(count));
        put_le32(stream, base + 8,
                 XXH32(block.data(), block.size(), 0x9747B28CU) & 0x0FFFFFFFU);
        stream.insert(stream.end(), payload.begin(), payload.end());
    }
    stream.insert(stream.end(), magic.begin(), magic.end());
    stream.push_back(std::byte{0x20});
    stream.resize(stream.size() + 12, std::byte{0});
    return stream;
}

[[nodiscard]] auto external_path_for(std::string_view region_path,
                                     world::chunk_pos pos) -> std::string {
    const auto parent{
        std::filesystem::path{std::string{region_path}}.parent_path()};
    return (parent / ("c." + std::to_string(pos.x) + "." +
                      std::to_string(pos.z) + ".mcc"))
        .string();
}

/// Write one external-chunk stub into a region file. `external_name` lets tests
/// create missing/wrong-coordinate files; `write_external` false leaves the
/// `.mcc` absent.
void write_external_region_file(const std::string &path, std::uint32_t slot,
                                const std::string &external_name,
                                const std::vector<std::byte> &payload,
                                std::uint8_t compression_type = 2,
                                bool write_external = true) {
    constexpr std::size_t sector_bytes = 4096;
    std::vector<std::byte> header(8192, std::byte{0});
    put_be32(header, slot * 4, (2U << 8U) | 1U);
    std::vector<std::byte> block(sector_bytes, std::byte{0});
    put_be32(block, 0, 1);
    block[4] = static_cast<std::byte>(compression_type | 0x80U);
    std::vector<std::byte> file{header.begin(), header.end()};
    file.insert(file.end(), block.begin(), block.end());
    expect(static_cast<bool>(io::write_file(path, file)));
    if (write_external) {
        const auto external{std::filesystem::path{path}.parent_path() /
                            external_name};
        expect(static_cast<bool>(io::write_file(external.string(), payload)));
    }
}

[[nodiscard]] auto make_large_chunk_nbt(world::chunk_pos pos) -> value {
    value root{make_chunk_nbt(pos)};
    // Keep below the NBT collection limit (1,048,576) while still producing
    // a zlib payload above the 1 MiB internal-sector threshold.
    nbt::byte_array bytes(1'048'576);
    std::uint32_t state{0x9E3779B9U};
    for (auto &byte : bytes) {
        state = state * 1664525U + 1013904223U;
        byte = static_cast<std::uint8_t>(state >> 24U);
    }
    set(root, "refrain_large_test", nbt::make_byte_array(std::move(bytes)));
    return root;
}

[[nodiscard]] auto document_bytes(const value &document)
    -> std::vector<std::byte> {
    auto bytes{nbt::write(document, "", nbt::compression::zlib)};
    expect(static_cast<bool>(bytes));
    return bytes ? std::move(*bytes) : std::vector<std::byte>{};
}

void write_level_dat(const std::string &path) {
    value data{make_compound()};
    set(data, "DataVersion", make_int(5023));
    set(data, "LevelName", make_string("Refrain Fixture \xF0\x9F\x98\x80"));
    set(data, "LastPlayed", make_long(0));
    set(data, "Time", make_long(24000));
    value spawn{make_compound()};
    set(spawn, "pos", nbt::make_int_array({8, 72, 8}));
    set(spawn, "dimension", make_string("minecraft:overworld"));
    set(data, "spawn", std::move(spawn));
    value root{make_compound()};
    set(root, "Data", std::move(data));
    expect(static_cast<bool>(
        nbt::write_file(path, root, "", nbt::compression::gzip)));
}

void write_entities_region(const std::string &path, std::int32_t x,
                           std::int32_t z) {
    value document{make_compound()};
    set(document, "DataVersion", make_int(5023));
    value position{make_compound()};
    set(position, "x", make_int(x));
    set(position, "z", make_int(z));
    set(document, "Position", std::move(position));
    value entities{new_list(tag_type::compound)};
    value pig{make_compound()};
    set(pig, "id", make_string("minecraft:pig"));
    push(entities, std::move(pig));
    set(document, "Entities", std::move(entities));
    write_region_file(path, {{0, document_bytes(document)}});
}

[[nodiscard]] auto build_world(const temp_directory &directory) -> std::string {
    const auto root{directory.sub("source")};
    const auto overworld{root + "/dimensions/minecraft/overworld"};
    expect(static_cast<bool>(io::create_directories(overworld + "/region")));
    expect(static_cast<bool>(io::create_directories(overworld + "/entities")));
    expect(static_cast<bool>(io::create_directories(overworld + "/poi")));
    expect(static_cast<bool>(io::create_directories(root + "/players/data")));
    expect(static_cast<bool>(io::create_directories(root + "/datapacks/test")));
    expect(static_cast<bool>(io::create_directories(root + "/data/minecraft")));
    value settings{make_compound()};
    set(settings, "DataVersion", make_int(5023));
    value settings_data{make_compound()};
    set(settings_data, "seed", make_long(12345));
    set(settings_data, "generate_structures", nbt::make_byte(1));
    set(settings, "data", std::move(settings_data));
    expect(static_cast<bool>(
        nbt::write_file(root + "/data/minecraft/world_gen_settings.dat",
                        settings, "", nbt::compression::gzip)));
    write_level_dat(root + "/level.dat");
    write_entities_region(overworld + "/entities/r.0.0.mca", 0, 0);
    write_region_file(overworld + "/poi/r.0.0.mca",
                      {{0, document_bytes([] {
                            value document{make_compound()};
                            set(document, "DataVersion", make_int(5023));
                            set(document, "Sections",
                                make_list(tag_type::compound));
                            return document;
                        }())}});
    value player{make_compound()};
    set(player, "DataVersion", make_int(5023));
    set(player, "XpLevel", make_int(5));
    expect(static_cast<bool>(nbt::write_file(
        root + "/players/data/00000000-0000-0000-0000-000000000001.dat", player,
        "", nbt::compression::gzip)));
    expect(static_cast<bool>(
        io::create_directories(root + "/players/advancements")));
    expect(static_cast<bool>(io::create_directories(root + "/players/stats")));
    expect(static_cast<bool>(io::write_text(
        root +
            "/players/advancements/00000000-0000-0000-0000-000000000001.json",
        "{\"minecraft:story/root\":{}}")));
    expect(static_cast<bool>(io::write_text(
        root + "/players/stats/00000000-0000-0000-0000-000000000001.json",
        "{\"minecraft:custom\":{\"minecraft:walk_one_cm\":1}}")));
    expect(static_cast<bool>(
        io::write_text(root + "/players/data/notes.txt", "keep me")));
    expect(static_cast<bool>(io::write_text(root + "/icon.png", "PNGDATA")));
    expect(static_cast<bool>(io::write_text(
        root + "/datapacks/test/pack.mcmeta",
        "{\"pack\":{\"pack_format\":71,\"description\":\"fixture\"}}")));

    const auto chunk_a{document_bytes(make_chunk_nbt({.x = 0, .z = 0}))};
    const auto chunk_b{document_bytes(make_chunk_nbt({.x = 1, .z = 0}))};
    write_region_file(overworld + "/region/r.0.0.mca",
                      {{0, chunk_a}, {1, chunk_b}});
    return root;
}

} // namespace

suite world_storage = [] {
    "anvil_import_export_roundtrip"_test = [] {
        const temp_directory directory;
        const auto source{build_world(directory)};
        const auto rwf{directory.sub("rwf")};
        auto imported{anvil::import_world(source, rwf, false)};
        expect(static_cast<bool>(imported));
        if (!imported) {
            return;
        }
        expect(imported->chunks == 2U);
        expect(imported->entity_chunks == 1U);
        expect(imported->poi_chunks == 1U);
        expect(imported->playerdata == 1U);
        expect(imported->datapack_files == 1U);
        expect(imported->aux_files == 0U);

        auto store{world::world_store::open(rwf)};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        const auto level{store->level()};
        expect(static_cast<bool>(level));
        if (level) {
            expect(level->data_version == 5023);
            expect(level->seed == 12345);
            expect(level->spawn_x == 8);
            expect(level->level_name == "Refrain Fixture \xF0\x9F\x98\x80");
        }
        const auto positions{store->chunk_positions()};
        expect(static_cast<bool>(positions));
        if (positions) {
            expect(positions->size() == 2U);
        }
        const auto chunk{store->read_chunk({.x = 1, .z = 0})};
        expect(static_cast<bool>(chunk));
        if (!chunk) {
            return;
        }
        expect(chunk->sections.size() == 2U);
        expect(chunk->sections[1].kind == world::section_kind::palette);
        expect(chunk->sections[1].bits_per_entry == 4);
        expect(chunk->sections[1].palette.size() == 3U);
        expect(chunk->sections[1].palette[2].key ==
               "minecraft:oak_log[axis=y]");
        expect(chunk->block_ticks.size() == 1U);
        expect(chunk->block_ticks[0].id == "minecraft:water");
        expect(chunk->block_ticks[0].priority == -1);
        expect(chunk->block_entities.size() == 1U);
        expect(chunk->post_processing.size() == 2U);
        const auto *light{world::light_at(*chunk, 0)};
        expect(light != nullptr);
        if (light != nullptr) {
            expect(light->sky_light.size() == world::light_nibble_bytes);
        }
        const auto baseline{world::semantic_hash(*chunk)};
        expect(static_cast<bool>(baseline));

        const auto exported{directory.sub("exported")};
        auto result{anvil::export_world(rwf, exported, false)};
        expect(static_cast<bool>(result));
        if (!result) {
            return;
        }
        expect(!io::exists(external_path_for(
            exported + "/dimensions/minecraft/overworld/region/r.0.0.mca",
            {.x = 0, .z = 0})));
        expect(result->chunks == 2U);
        expect(result->entity_chunks == 1U);
        expect(result->poi_chunks == 1U);
        expect(result->playerdata == 1U);
        expect(result->datapack_files == 1U);
        expect(result->aux_files == 0U);
        const auto settings_copy{
            io::exists(exported + "/data/minecraft/world_gen_settings.dat")};
        expect(settings_copy);
        expect(io::exists(exported +
                          "/players/advancements/"
                          "00000000-0000-0000-0000-000000000001.json"));
        expect(io::exists(exported +
                          "/players/stats/"
                          "00000000-0000-0000-0000-000000000001.json"));
        expect(io::exists(exported + "/players/data/notes.txt"));
        expect(io::exists(exported + "/icon.png"));
        const auto notes{io::read_text(exported + "/players/data/notes.txt")};
        expect(static_cast<bool>(notes));
        if (notes) {
            expect(*notes == "keep me");
        }
        const auto icon{io::read_text(exported + "/icon.png")};
        expect(static_cast<bool>(icon));
        if (icon) {
            expect(*icon == "PNGDATA");
        }

        const auto reimported{directory.sub("rwf2")};
        auto again{anvil::import_world(exported, reimported, false)};
        expect(static_cast<bool>(again));
        if (!again) {
            return;
        }
        auto second{world::world_store::open(reimported)};
        expect(static_cast<bool>(second));
        if (!second) {
            return;
        }
        const auto copy{second->read_chunk({.x = 1, .z = 0})};
        expect(static_cast<bool>(copy));
        if (!copy) {
            return;
        }
        expect(world::semantic_hash(*copy).value_or("") == *baseline);
        const auto second_level{second->level()};
        expect(static_cast<bool>(second_level));
        if (second_level) {
            expect(second_level->seed == 12345);
            expect(second_level->level_name == level->level_name);
        }
        const auto stats{second->stats()};
        expect(static_cast<bool>(stats));
        if (stats) {
            expect(stats->aux_files == 2U);
            expect(stats->objects == 7U);
            expect(stats->entity_chunks == 1U);
            expect(stats->poi_chunks == 1U);
            expect(stats->playerdata == 1U);
        }
    };

    "world_import_export_reject_source_target_overlap"_test = [] {
        const temp_directory directory;
        const auto source{build_world(directory)};
        const auto parent{directory.path().string()};

        // import: target == source / target nested in source / target is an
        // ancestor of source must be rejected before `--force` deletes it.
        for (const auto &target : {source, source + "/nested-rwf", parent}) {
            const auto result{anvil::import_world(source, target, true)};
            expect(!result) << target;
            if (!result) {
                expect(result.error().code ==
                       refrain::error_code::invalid_argument)
                    << target;
            }
            expect(io::exists(source + "/level.dat")) << target;
        }

        // export: same checks against an actual RWF world.
        const auto rwf{directory.sub("rwf-overlap")};
        auto imported{anvil::import_world(source, rwf, false)};
        expect(static_cast<bool>(imported));
        if (!imported) {
            return;
        }
        for (const auto &target : {rwf, rwf + "/nested-anvil", parent}) {
            const auto result{anvil::export_world(rwf, target, true)};
            expect(!result) << target;
            if (!result) {
                expect(result.error().code ==
                       refrain::error_code::invalid_argument)
                    << target;
            }
            expect(io::exists(rwf + "/meta.rfw")) << target;
        }
    };

    "fsck_repair_rejects_symlinked_regions"_test = [] {
        const temp_directory directory;
        const auto source{build_world(directory)};
        const auto rwf{directory.sub("rwf-symlink")};
        auto imported{anvil::import_world(source, rwf, false)};
        expect(static_cast<bool>(imported));
        if (!imported) {
            return;
        }
        const auto region{rwf +
                          "/dimensions/minecraft/overworld/regions/0.0.rfw"};
        const auto victim{directory.sub("victim.bin")};
        expect(static_cast<bool>(
            io::write_text(victim, "ORIGINAL-VICTIM-CONTENT")));
        std::error_code error;
        fs::remove(region, error);
        error.clear();
        fs::create_symlink(victim, region, error);
        if (error) {
            // Windows without developer mode cannot create symlinks; the
            // POSIX path is covered by CI.
            return;
        }
        auto store{world::world_store::open(rwf)};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        const auto checked{store->check(true)};
        expect(static_cast<bool>(checked));
        if (checked) {
            expect(checked->corrupt >= 1U);
            expect(!checked->problems.empty());
        }
        const auto text{io::read_text(victim)};
        expect(static_cast<bool>(text));
        if (text) {
            expect(*text == "ORIGINAL-VICTIM-CONTENT");
        }
        expect(io::is_symlink(region));
    };

    "anvil_rejects_out_of_range_tick_priority"_test = [] {
        const temp_directory directory;
        const auto source{build_world(directory)};
        const auto region{source +
                          "/dimensions/minecraft/overworld/region/r.0.0.mca"};
        write_region_file(
            region, {{0, document_bytes(make_chunk_nbt({.x = 0, .z = 0}, 4))}});
        const auto result{
            anvil::import_world(source, directory.sub("rwf-priority"), false)};
        expect(!result);
        if (result) {
            return;
        }
        expect(result.error().code == refrain::error_code::parse_error);
        expect(result.error().message.find("priority") != std::string::npos);
    };

    "fsck_repair_does_not_relocate_header_corruption"_test = [] {
        const temp_directory directory;
        const auto source{build_world(directory)};
        const auto rwf{directory.sub("rwf-header-flip")};
        auto imported{anvil::import_world(source, rwf, false)};
        expect(static_cast<bool>(imported));
        if (!imported) {
            return;
        }
        const auto region{rwf +
                          "/dimensions/minecraft/overworld/regions/0.0.rfw"};
        auto bytes{io::read_file(region)};
        expect(static_cast<bool>(bytes));
        if (!bytes) {
            return;
        }
        // Flip chunk_x of the first record from 0 to 2. The frame header has no
        // per-record CRC, so the repair path must detect the mismatch from the
        // record key instead of trusting the payload and relocating the chunk
        // to slot (2, 0).
        const auto record{world::rwf::data_offset};
        expect(bytes->size() > record + 12U);
        (*bytes)[record + 8] = std::byte{2};
        expect(static_cast<bool>(io::write_file(region, *bytes)));

        const auto plain{
            world::rwf::check_region(region, {.x = 0, .z = 0}, false)};
        expect(static_cast<bool>(plain));
        if (plain) {
            expect(!plain->problems.empty());
            expect(!plain->repaired);
        }
        const auto repaired{
            world::rwf::check_region(region, {.x = 0, .z = 0}, true)};
        expect(static_cast<bool>(repaired));
        if (repaired) {
            expect(repaired->repaired);
        }
        auto store{world::world_store::open(rwf)};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        // The damaged record is dropped, never moved to the flipped slot.
        expect(!store->contains({.x = 0, .z = 0}));
        expect(!store->contains({.x = 2, .z = 0}));
        expect(store->contains({.x = 1, .z = 0}));
        const auto checked{store->check(false)};
        expect(static_cast<bool>(checked));
        if (checked) {
            expect(checked->corrupt == 0U);
        }
    };

    "world_palette_gc_drops_unused_entries"_test = [] {
        const temp_directory directory;
        const auto source{build_world(directory)};
        const auto rwf{directory.sub("rwf-palette-gc")};
        auto imported{anvil::import_world(source, rwf, false, 1)};
        expect(static_cast<bool>(imported));
        if (!imported) {
            return;
        }
        std::size_t before{0};
        {
            auto store{world::world_store::open(rwf)};
            expect(static_cast<bool>(store));
            if (!store) {
                return;
            }
            before = store->palette().size();
            auto grown{store->palette()};
            static_cast<void>(
                grown.intern("minecraft:refrain_unused_palette_entry"));
            expect(static_cast<bool>(store->install_palette(grown)));
            expect(static_cast<bool>(store->commit()));
        }
        {
            auto store{world::world_store::open(rwf)};
            expect(static_cast<bool>(store));
            if (store) {
                expect(store->palette().size() == before + 1U);
            }
        }

        const auto compacted{directory.sub("rwf-palette-gc-out")};
        auto gc{anvil::rebuild_palette(rwf, compacted, false, 1)};
        expect(static_cast<bool>(gc));
        if (!gc) {
            return;
        }
        expect(gc->entries_after < gc->entries_before);
        {
            auto store{world::world_store::open(compacted)};
            expect(static_cast<bool>(store));
            if (store) {
                expect(store->palette().size() == before);
            }
        }
        auto left{anvil::scan_world(rwf)};
        auto right{anvil::scan_world(compacted)};
        expect(static_cast<bool>(left));
        expect(static_cast<bool>(right));
        if (left && right) {
            expect(left->size() == right->size());
            if (left->size() == right->size()) {
                for (std::size_t index{0}; index < left->size(); ++index) {
                    expect((*left)[index].dimension ==
                           (*right)[index].dimension);
                    expect((*left)[index].pos == (*right)[index].pos);
                    expect((*left)[index].hash == (*right)[index].hash);
                }
            }
        }
    };

    "aux_names_with_spaces_roundtrip"_test = [] {
        const temp_directory directory;
        const auto source{build_world(directory)};
        expect(static_cast<bool>(
            io::create_directories(source + "/datapacks/My Pack")));
        expect(static_cast<bool>(io::write_text(
            source + "/datapacks/My Pack/pack.mcmeta", "{\"pack\":{}}")));
        expect(static_cast<bool>(
            io::write_text(source + "/players/data/notes pack.txt", "spaces")));
        const auto rwf{directory.sub("rwf-spaces")};
        auto imported{anvil::import_world(source, rwf, false, 1)};
        expect(static_cast<bool>(imported));
        if (!imported) {
            return;
        }
        const auto exported{directory.sub("export-spaces")};
        auto result{anvil::export_world(rwf, exported, false, 1)};
        expect(static_cast<bool>(result));
        if (!result) {
            return;
        }
        expect(
            io::is_regular_file(exported + "/datapacks/My Pack/pack.mcmeta"));
        expect(io::is_regular_file(exported + "/players/data/notes pack.txt"));
    };

    "anvil_rejects_malformed_regions"_test = [] {
        const temp_directory directory;
        const auto source{build_world(directory)};
        const auto region{source +
                          "/dimensions/minecraft/overworld/region/r.0.0.mca"};
        auto original{io::read_file(region)};
        expect(static_cast<bool>(original));
        if (!original) {
            return;
        }
        const auto restore = [&] {
            expect(static_cast<bool>(io::write_file(region, *original)));
        };
        const auto import_error = [&](std::string_view name) {
            const auto result{anvil::import_world(
                source, directory.sub("rwf-" + std::string{name}), false)};
            expect(!result) << name;
            if (result) {
                return;
            }
            expect(result.error().code == refrain::error_code::parse_error)
                << name << " " << result.error().message;
        };
        const auto put_be = [](std::vector<std::byte> &bytes, std::size_t at,
                               std::uint32_t value) {
            bytes[at] = static_cast<std::byte>((value >> 24U) & 0xFFU);
            bytes[at + 1] = static_cast<std::byte>((value >> 16U) & 0xFFU);
            bytes[at + 2] = static_cast<std::byte>((value >> 8U) & 0xFFU);
            bytes[at + 3] = static_cast<std::byte>(value & 0xFFU);
        };

        {
            auto bytes{*original};
            put_be(bytes, 0, (9999U << 8U) | 1U); // slot 0 outside the file
            expect(static_cast<bool>(io::write_file(region, bytes)));
            import_error("oob-location");
            restore();
        }
        {
            auto bytes{*original};
            // Make slot 1 point at slot 0's sectors (overlap).
            std::copy_n(bytes.begin(), 4, bytes.begin() + 4);
            expect(static_cast<bool>(io::write_file(region, bytes)));
            import_error("overlap");
            restore();
        }
        {
            auto bytes{*original};
            // Anvil data starts at 8192 (8 KiB header), not at the RWF offset.
            put_be(bytes, 8192, 0xFFFFFFF0U);
            expect(static_cast<bool>(io::write_file(region, bytes)));
            import_error("bad-length");
            restore();
        }
        {
            auto bytes{*original};
            bytes.resize(8300U);
            expect(static_cast<bool>(io::write_file(region, bytes)));
            import_error("truncated");
            restore();
        }
        {
            write_region_file(
                region,
                {{0, document_bytes(make_chunk_nbt({.x = 5, .z = 0}))}});
            import_error("position-mismatch");
            restore();
        }
    };

    "lz4_multi_block_stream_imports"_test = [] {
        const temp_directory directory;
        const auto source{build_world(directory)};
        const auto region{source +
                          "/dimensions/minecraft/overworld/region/r.0.0.mca"};
        auto document{make_chunk_nbt({.x = 0, .z = 0})};
        set(document, "refrain_multi_block",
            nbt::make_byte_array(nbt::byte_array(200U * 1024U)));
        const auto raw{raw_document_bytes(document)};
        expect(raw.size() > 65536U);
        write_region_file(region, {{0, lz4_block_stream_multi(raw)}}, 4);
        const auto rwf{directory.sub("rwf-lz4-multi")};
        auto imported{anvil::import_world(source, rwf, false, 1)};
        expect(static_cast<bool>(imported));
        if (!imported) {
            return;
        }
        expect(imported->chunks == 1U);
        auto source_digests{anvil::scan_world(source)};
        auto rwf_digests{anvil::scan_world(rwf)};
        expect(static_cast<bool>(source_digests));
        expect(static_cast<bool>(rwf_digests));
        if (source_digests && rwf_digests && !source_digests->empty() &&
            !rwf_digests->empty()) {
            expect((*source_digests)[0].hash == (*rwf_digests)[0].hash);
        }
    };

    "encode_rejects_mod_extensions"_test = [] {
        world::chunk value{};
        value.format_version = world::chunk_format_version;
        value.status = world::chunk_status::full;
        world::section_record section{};
        section.y = 0;
        section.kind = world::section_kind::single_value;
        section.palette.push_back(world::palette_entry{
            .key = "minecraft:air", .count = 0, .state = 0});
        section.biome_kind = world::section_kind::single_value;
        section.biome_palette.push_back("minecraft:plains");
        value.sections.push_back(std::move(section));
        value.extensions.push_back({std::byte{0x01}});
        const auto encoded{world::encode_chunk(value)};
        const bool rejected{!encoded};
        expect(rejected);
        if (encoded) {
            return;
        }
        expect(encoded.error().code == refrain::error_code::unsupported);
    };

    "fsck_repair_leaves_healthy_regions_untouched"_test = [] {
        const temp_directory directory;
        const auto source{build_world(directory)};
        const auto rwf{directory.sub("rwf-healthy")};
        auto imported{anvil::import_world(source, rwf, false, 1)};
        expect(static_cast<bool>(imported));
        if (!imported) {
            return;
        }
        const auto region{rwf +
                          "/dimensions/minecraft/overworld/regions/0.0.rfw"};
        auto before{io::read_file(region)};
        expect(static_cast<bool>(before));
        if (!before) {
            return;
        }
        const auto checked{
            world::rwf::check_region(region, {.x = 0, .z = 0}, true)};
        expect(static_cast<bool>(checked));
        if (checked) {
            expect(!checked->repaired);
            expect(checked->problems.empty());
        }
        auto after{io::read_file(region)};
        expect(static_cast<bool>(after));
        if (after) {
            const bool unchanged{
                before->size() == after->size() &&
                std::equal(before->begin(), before->end(), after->begin())};
            expect(unchanged);
        }
    };

    "external_mcc_roundtrip"_test = [] {
        const temp_directory directory;
        const auto source{build_world(directory)};
        const std::string region{
            source + "/dimensions/minecraft/overworld/region/r.0.0.mca"};
        const auto large{
            document_bytes(make_large_chunk_nbt({.x = 0, .z = 0}))};
        expect(large.size() > (1ULL << 20U));
        write_external_region_file(region, 0, "c.0.0.mcc", large);

        const auto rwf{directory.sub("rwf-external")};
        auto imported{anvil::import_world(source, rwf, false, 1)};
        expect(static_cast<bool>(imported));
        if (!imported) {
            return;
        }
        expect(imported->chunks == 1U);
        auto store{world::world_store::open(rwf)};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        auto chunk{store->read_chunk({.x = 0, .z = 0})};
        expect(static_cast<bool>(chunk));
        if (chunk) {
            expect(chunk->extra.size() >= (1ULL << 20U));
        }

        const auto exported{directory.sub("exported-external")};
        auto result{anvil::export_world(rwf, exported, false, 1)};
        expect(static_cast<bool>(result));
        if (!result) {
            return;
        }
        expect(result->chunks == 1U);
        const auto exported_region{
            exported + "/dimensions/minecraft/overworld/region/r.0.0.mca"};
        const auto external{
            external_path_for(exported_region, {.x = 0, .z = 0})};
        expect(io::is_regular_file(external));
        auto mcc{io::read_file(external)};
        expect(static_cast<bool>(mcc));
        if (mcc) {
            expect(mcc->size() > (1ULL << 20U));
        }
        auto mca{io::read_file(exported_region)};
        expect(static_cast<bool>(mca));
        if (mca) {
            const auto location{get_be32(*mca, 0)};
            const auto offset{location >> 8U};
            expect((location & 0xFFU) == 1U);
            expect(offset >= 2U);
            const auto base{static_cast<std::size_t>(offset) * 4096U};
            expect(base + 5U <= mca->size());
            expect(get_be32(*mca, base) == 1U);
            expect(static_cast<std::uint8_t>((*mca)[base + 4]) == (2U | 0x80U));
        }

        const auto reimported{directory.sub("rwf-external-roundtrip")};
        auto again{anvil::import_world(exported, reimported, false, 1)};
        expect(static_cast<bool>(again));
        if (again) {
            expect(again->chunks == 1U);
        }
    };

    "external_mcc_rejects_missing_truncated_wrong_and_oversize"_test = [] {
        const temp_directory directory;
        const auto source{build_world(directory)};
        const std::string region{
            source + "/dimensions/minecraft/overworld/region/r.0.0.mca"};
        const auto large{
            document_bytes(make_large_chunk_nbt({.x = 0, .z = 0}))};
        const auto repaired{external_path_for(region, {.x = 0, .z = 0})};

        write_external_region_file(region, 0, "c.0.0.mcc", large, 2, false);
        expect(!anvil::import_world(source, directory.sub("rwf-missing"), false,
                                    1));

        write_external_region_file(region, 0, "c.0.0.mcc", {std::byte{0x78}});
        expect(!anvil::import_world(source, directory.sub("rwf-truncated"),
                                    false, 1));

        std::error_code ignored;
        fs::remove(repaired, ignored);
        write_external_region_file(region, 0, "c.1.0.mcc", large);
        expect(
            !anvil::import_world(source, directory.sub("rwf-wrong"), false, 1));
        fs::remove(external_path_for(region, {.x = 1, .z = 0}), ignored);

        write_external_region_file(region, 0, "c.0.0.mcc", {});
        std::error_code resized;
        fs::resize_file(
            repaired, static_cast<std::uintmax_t>(64ULL << 20U) + 1U, resized);
        expect(!resized);
        expect(!anvil::import_world(source, directory.sub("rwf-oversize"),
                                    false, 1));
    };

    "non_overworld_dimensions_are_native"_test = [] {
        const temp_directory directory;
        const auto source{build_world(directory)};
        const auto nether{source + "/dimensions/minecraft/the_nether"};
        expect(static_cast<bool>(io::create_directories(nether + "/region")));
        write_region_file(
            nether + "/region/r.0.0.mca",
            {{0, document_bytes(make_chunk_nbt({.x = 0, .z = 0}, -1, 0, 0))}});
        expect(static_cast<bool>(io::create_directories(nether + "/entities")));
        write_entities_region(nether + "/entities/r.0.0.mca", 0, 0);
        value nether_settings{make_compound()};
        set(nether_settings, "DataVersion", make_int(5023));
        value nether_data{make_compound()};
        set(nether_data, "seed", make_long(7));
        set(nether_settings, "data", std::move(nether_data));
        expect(static_cast<bool>(
            io::create_directories(nether + "/data/minecraft")));
        expect(static_cast<bool>(
            nbt::write_file(nether + "/data/minecraft/world_gen_settings.dat",
                            nether_settings, "", nbt::compression::gzip)));

        const auto rwf{directory.sub("rwf-dim")};
        std::vector<std::byte> global_bytes(1024, std::byte{0x47});
        const auto global_dictionary{
            world::rwf::make_zstd_dictionary(global_bytes)};
        auto imported{
            anvil::import_world(source, rwf, false, 1, 3, &global_dictionary)};
        expect(static_cast<bool>(imported));
        if (!imported) {
            return;
        }
        expect(imported->chunks == 3U);
        expect(imported->regions == 2U);
        expect(!io::exists(rwf + "/vanilla"));
        expect(io::exists(rwf +
                          "/dimensions/minecraft/the_nether/regions/0.0.rfw"));
        expect(io::exists(
            rwf + "/dimensions/minecraft/the_nether/objects/index.rfw"));

        // Replace the nether region's dictionary with a dimension-scoped one:
        // loading/diff/export must select it by dimension, while the overworld
        // keeps using the world-level fallback.
        std::vector<std::byte> nether_bytes(1024, std::byte{0x4e});
        const auto nether_dictionary{
            world::rwf::make_zstd_dictionary(nether_bytes)};
        const auto nether_region{
            rwf + "/dimensions/minecraft/the_nether/regions/0.0.rfw"};
        {
            auto old_region{world::rwf::region_file::load(
                nether_region,
                {.x = 0, .z = 0, .dictionary = &global_dictionary})};
            expect(static_cast<bool>(old_region));
            if (!old_region) {
                return;
            }
            auto payload{old_region->read({.x = 0, .z = 0})};
            expect(static_cast<bool>(payload));
            if (!payload) {
                return;
            }
            expect(static_cast<bool>(old_region->close()));
            std::error_code ignored;
            fs::remove(nether_region, ignored);
            auto fresh{world::rwf::region_file::create(
                nether_region,
                {.x = 0, .z = 0, .dictionary = &nether_dictionary})};
            expect(static_cast<bool>(fresh));
            if (!fresh) {
                return;
            }
            expect(static_cast<bool>(fresh->write(
                {.x = 0, .z = 0}, world::chunk_status::full, *payload)));
            expect(static_cast<bool>(fresh->commit()));
        }
        {
            auto configure{world::world_store::open(rwf)};
            expect(static_cast<bool>(configure));
            if (!configure) {
                return;
            }
            expect(static_cast<bool>(configure->install_dimension_dictionary(
                "minecraft", "the_nether", nether_bytes)));
        }
        auto dimension_store{world::world_store::open(rwf)};
        expect(static_cast<bool>(dimension_store));
        if (dimension_store) {
            const auto *overworld_dictionary{
                dimension_store->dictionary_for("minecraft", "overworld")};
            const auto *stored_nether_dictionary{
                dimension_store->dictionary_for("minecraft", "the_nether")};
            expect(overworld_dictionary != nullptr);
            expect(stored_nether_dictionary != nullptr);
            if (overworld_dictionary != nullptr &&
                stored_nether_dictionary != nullptr) {
                expect(overworld_dictionary->id == global_dictionary.id);
                expect(stored_nether_dictionary->id == nether_dictionary.id);
            }
            auto checked{dimension_store->check(false)};
            expect(static_cast<bool>(checked));
            if (checked) {
                expect(checked->dimensions == 2U);
                expect(checked->regions == 2U);
                expect(checked->chunks == 3U);
                expect(checked->corrupt == 0U);
            }
        }
        auto digests{anvil::scan_world(rwf)};
        expect(static_cast<bool>(digests));
        if (digests) {
            expect(digests->size() == 3U);
            expect((*digests)[0].dimension == "minecraft:overworld");
            expect((*digests)[2].dimension == "minecraft:the_nether");
        }

        const auto exported{directory.sub("export-dim")};
        auto result{anvil::export_world(rwf, exported, false, 1)};
        expect(static_cast<bool>(result));
        if (!result) {
            return;
        }
        expect(result->chunks == 3U);
        expect(result->regions == 2U);
        expect(io::exists(exported +
                          "/dimensions/minecraft/the_nether/region/r.0.0.mca"));
        expect(io::exists(
            exported + "/dimensions/minecraft/the_nether/entities/r.0.0.mca"));
        expect(io::exists(exported +
                          "/dimensions/minecraft/the_nether/data/minecraft/"
                          "world_gen_settings.dat"));
        // The exported nether chunk must keep yPos=0 (not the overworld -4);
        // the semantic hash now covers yPos, so a scan comparison catches it.
        auto exported_digests{anvil::scan_world(exported)};
        expect(static_cast<bool>(exported_digests));
        if (exported_digests) {
            for (const auto &digest : *digests) {
                const auto found{std::find_if(
                    exported_digests->begin(), exported_digests->end(),
                    [&digest](const auto &candidate) {
                        return candidate.dimension == digest.dimension &&
                               candidate.pos == digest.pos;
                    })};
                expect(found != exported_digests->end());
                if (found != exported_digests->end()) {
                    expect(found->hash == digest.hash);
                }
            }
        }
    };

    "parallel_import_export_is_deterministic"_test = [] {
        const temp_directory directory;
        const auto source{build_world(directory)};
        write_region_file(
            source + "/dimensions/minecraft/overworld/region/r.1.0.mca",
            {{0, document_bytes(make_chunk_nbt({.x = 32, .z = 0}))}});
        const auto serial{directory.sub("rwf-serial")};
        const auto parallel{directory.sub("rwf-parallel")};
        auto serial_result{anvil::import_world(source, serial, false, 1)};
        auto parallel_result{anvil::import_world(source, parallel, false, 2)};
        expect(static_cast<bool>(serial_result));
        expect(static_cast<bool>(parallel_result));
        if (!serial_result || !parallel_result) {
            return;
        }
        expect(serial_result->chunks == 3U);
        expect(parallel_result->chunks == 3U);

        const auto compare_tree = [](const std::string &left,
                                     const std::string &right) {
            for (const auto &entry : fs::recursive_directory_iterator(left)) {
                if (!entry.is_regular_file()) {
                    continue;
                }
                const auto relative{fs::relative(entry.path(), left)};
                const auto other{fs::path{right} / relative};
                expect(fs::is_regular_file(other)) << relative.string();
                const auto left_bytes{io::read_file(entry.path().string())};
                const auto right_bytes{io::read_file(other.string())};
                expect(static_cast<bool>(left_bytes)) << relative.string();
                expect(static_cast<bool>(right_bytes)) << relative.string();
                if (left_bytes && right_bytes) {
                    expect(*left_bytes == *right_bytes) << relative.string();
                }
            }
        };
        compare_tree(serial, parallel);

        auto serial_store{world::world_store::open(serial)};
        auto parallel_store{world::world_store::open(parallel)};
        expect(static_cast<bool>(serial_store));
        expect(static_cast<bool>(parallel_store));
        if (!serial_store || !parallel_store) {
            return;
        }
        const auto serial_positions{serial_store->chunk_positions()};
        const auto parallel_positions{parallel_store->chunk_positions()};
        expect(static_cast<bool>(serial_positions));
        expect(static_cast<bool>(parallel_positions));
        if (!serial_positions || !parallel_positions) {
            return;
        }
        expect(serial_positions->size() == 3U);
        expect(*serial_positions == *parallel_positions);
        for (const auto &pos : *serial_positions) {
            const auto left{serial_store->read_chunk(pos)};
            const auto right{parallel_store->read_chunk(pos)};
            expect(static_cast<bool>(left));
            expect(static_cast<bool>(right));
            if (left && right) {
                expect(world::semantic_hash(*left).value_or("") ==
                       world::semantic_hash(*right).value_or(""));
            }
        }

        const auto serial_export{directory.sub("anvil-serial")};
        const auto parallel_export{directory.sub("anvil-parallel")};
        auto export_left{anvil::export_world(serial, serial_export, false, 1)};
        auto export_right{
            anvil::export_world(parallel, parallel_export, false, 2)};
        expect(static_cast<bool>(export_left));
        expect(static_cast<bool>(export_right));
        if (!export_left || !export_right) {
            return;
        }
        expect(export_left->chunks == 3U);
        expect(export_right->chunks == 3U);
        compare_tree(serial_export, parallel_export);
    };

    "import_requires_supported_data_version"_test = [] {
        const temp_directory directory;
        const auto source{directory.sub("source")};
        expect(static_cast<bool>(io::create_directories(source)));
        value data{make_compound()};
        set(data, "DataVersion", make_int(1));
        value root{make_compound()};
        set(root, "Data", std::move(data));
        expect(static_cast<bool>(nbt::write_file(source + "/level.dat", root,
                                                 "", nbt::compression::gzip)));
        const auto target{directory.sub("rwf")};
        const auto result{anvil::import_world(source, target, false)};
        expect(!result);
        if (!result) {
            expect(result.error().code == refrain::error_code::unsupported);
            expect(result.error().message.find("vanilla 26.3") !=
                   std::string::npos);
        }
    };

    "import_rejects_mislabelled_lz4_chunks"_test = [] {
        const temp_directory directory;
        const auto source{build_world(directory)};
        // Rewrite the region with compression type 4 (LZ4) but the same bytes.
        const auto path{source +
                        "/dimensions/minecraft/overworld/region/r.0.0.mca"};
        auto bytes{io::read_file(path)};
        expect(static_cast<bool>(bytes));
        if (bytes) {
            (*bytes)[2 * 4096 + 4] = std::byte{4};
            expect(static_cast<bool>(io::write_file(path, *bytes)));
        }
        const auto result{
            anvil::import_world(source, directory.sub("rwf"), false)};
        expect(!result);
        if (!result) {
            expect(result.error().code == refrain::error_code::parse_error);
        }
    };

    "anvil_accepts_unpadded_final_sector"_test = [] {
        const temp_directory directory;
        const auto source{build_world(directory)};
        const auto path{source +
                        "/dimensions/minecraft/overworld/region/r.0.0.mca"};
        auto bytes{io::read_file(path)};
        expect(static_cast<bool>(bytes));
        if (!bytes) {
            return;
        }
        std::size_t last_end{0};
        for (std::size_t slot{0}; slot < 1024; ++slot) {
            const auto location{get_be32(*bytes, slot * 4U)};
            if (location == 0) {
                continue;
            }
            const auto base{static_cast<std::size_t>(location >> 8U) * 4096U};
            const auto length{get_be32(*bytes, base)};
            last_end = std::max(last_end, base + 4U + length);
        }
        expect(last_end < bytes->size());
        bytes->resize(last_end);
        expect(static_cast<bool>(io::write_file(path, *bytes)));

        auto imported{
            anvil::import_world(source, directory.sub("rwf-unpadded"), false)};
        expect(static_cast<bool>(imported));
        if (imported) {
            expect(imported->chunks == 2U);
        }
    };

    "import_lz4_region_chunks"_test = [] {
        const temp_directory directory;
        const auto source{build_world(directory)};
        const auto path{source +
                        "/dimensions/minecraft/overworld/region/r.0.0.mca"};
        write_region_file(
            path,
            {{0, lz4_block_stream(
                     raw_document_bytes(make_chunk_nbt({.x = 0, .z = 0})))},
             {1,
              lz4_block_stream(
                  raw_document_bytes(make_chunk_nbt({.x = 1, .z = 0})), true)}},
            4);

        const auto rwf{directory.sub("rwf-lz4")};
        auto imported{anvil::import_world(source, rwf, false)};
        expect(static_cast<bool>(imported));
        if (!imported) {
            return;
        }
        expect(imported->chunks == 2U);
        auto source_digests{anvil::scan_world(source)};
        auto rwf_digests{anvil::scan_world(rwf)};
        expect(static_cast<bool>(source_digests));
        expect(static_cast<bool>(rwf_digests));
        if (source_digests && rwf_digests) {
            expect(source_digests->size() == rwf_digests->size());
            if (source_digests->size() == rwf_digests->size()) {
                for (std::size_t index{0}; index < source_digests->size();
                     ++index) {
                    const auto &left{(*source_digests)[index]};
                    const auto &right{(*rwf_digests)[index]};
                    expect(left.dimension == right.dimension);
                    expect(left.pos == right.pos);
                    expect(left.status == right.status);
                    expect(left.hash == right.hash);
                }
            }
        }
    };

    "import_rejects_malformed_lz4_chunks"_test = [] {
        const temp_directory directory;
        const auto source{build_world(directory)};
        const auto path{source +
                        "/dimensions/minecraft/overworld/region/r.0.0.mca"};
        const auto raw{raw_document_bytes(make_chunk_nbt({.x = 0, .z = 0}))};
        const auto valid{lz4_block_stream(raw)};

        const auto expect_parse_error =
            [&](const std::vector<std::byte> &stream, std::string_view name) {
                write_region_file(path, {{0, stream}}, 4);
                auto result{anvil::import_world(
                    source, directory.sub(std::string{name}), false)};
                expect(!result) << std::string{name};
                if (!result) {
                    expect(result.error().code ==
                           refrain::error_code::parse_error)
                        << std::string{name};
                }
            };

        auto bad_checksum{valid};
        const auto compressed_offset{21U};
        bad_checksum[compressed_offset] ^= std::byte{0xFF};
        expect_parse_error(bad_checksum, "rwf-lz4-checksum");

        auto truncated{valid};
        truncated.resize(truncated.size() - 1);
        expect_parse_error(truncated, "rwf-lz4-truncated");

        auto oversized{valid};
        put_le32(oversized, 13, 0xFFFFFFFFU);
        expect_parse_error(oversized, "rwf-lz4-oversized");

        auto bad_magic{valid};
        bad_magic[0] = std::byte{'X'};
        expect_parse_error(bad_magic, "rwf-lz4-magic");
    };

    "light_padding_sections_roundtrip"_test = [] {
        const temp_directory directory;
        const auto source{build_world(directory)};
        value document{make_chunk_nbt({.x = 0, .z = 0})};
        value sections{new_list(tag_type::compound)};
        // The real 26.3 `the_end` case is a bottom padding section with
        // SkyLight only; the top padding is covered with BlockLight.
        push(sections, section_light_padding(-5, true));
        push(sections, section_single(-4, "minecraft:bedrock"));
        push(sections, section_palette(0));
        push(sections, section_light_padding(1, false));
        set(document, "sections", std::move(sections));
        write_region_file(
            source + "/dimensions/minecraft/overworld/region/r.0.0.mca",
            {{0, document_bytes(document)}});

        const auto rwf{directory.sub("rwf")};
        const auto imported{anvil::import_world(source, rwf, false)};
        expect(static_cast<bool>(imported));
        if (!imported) {
            return;
        }
        expect(imported->chunks == 1U);
        auto store{world::world_store::open(rwf)};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        const auto chunk{store->read_chunk({.x = 0, .z = 0})};
        expect(static_cast<bool>(chunk));
        if (!chunk) {
            return;
        }
        // Light is independent of block sections: the two padding layers and
        // the palette section's light are in `light_sections`, while only the
        // two real block sections remain in `sections`.
        expect(chunk->sections.size() == 2U);
        expect(chunk->sections[0].y == -4);
        expect(chunk->sections[1].y == 0);
        expect(chunk->light_sections.size() == 3U);
        expect(chunk->light_sections[0].y == -5);
        expect(chunk->light_sections[0].sky_light.size() ==
               world::light_nibble_bytes);
        expect(chunk->light_sections[0].sky_light[0] == std::byte{0x22});
        expect(chunk->light_sections[1].y == 0);
        expect(chunk->light_sections[2].y == 1);
        expect(chunk->light_sections[2].block_light.size() ==
               world::light_nibble_bytes);
        expect(chunk->light_sections[2].block_light[0] == std::byte{0x11});

        const auto baseline{world::semantic_hash(*chunk)};
        expect(static_cast<bool>(baseline));
        const auto exported{directory.sub("exported")};
        expect(static_cast<bool>(anvil::export_world(rwf, exported, false)));
        const auto rwf2{directory.sub("rwf2")};
        expect(static_cast<bool>(anvil::import_world(exported, rwf2, false)));
        auto store2{world::world_store::open(rwf2)};
        expect(static_cast<bool>(store2));
        if (!store2) {
            return;
        }
        const auto again{store2->read_chunk({.x = 0, .z = 0})};
        expect(static_cast<bool>(again));
        if (again && baseline) {
            expect(*world::semantic_hash(*again) == *baseline);
            expect(again->sections.size() == 2U);
            expect(again->light_sections.size() == 3U);
            expect(again->light_sections[0].y == -5);
            expect(again->light_sections[2].y == 1);
        }
    };

    "compression_profiles_keep_semantics"_test = [] {
        const temp_directory directory;
        const auto source{build_world(directory)};
        const auto fast{directory.sub("rwf-fast")};
        const auto compact{directory.sub("rwf-compact")};
        auto fast_result{anvil::import_world(source, fast, false, 1, 1)};
        auto compact_result{anvil::import_world(source, compact, false, 1, 6)};
        expect(static_cast<bool>(fast_result));
        expect(static_cast<bool>(compact_result));
        if (!fast_result || !compact_result) {
            return;
        }
        expect(fast_result->chunks == compact_result->chunks);
        auto fast_store{world::world_store::open(fast)};
        auto compact_store{world::world_store::open(compact)};
        expect(static_cast<bool>(fast_store));
        expect(static_cast<bool>(compact_store));
        if (!fast_store || !compact_store) {
            return;
        }
        const auto fast_chunk{fast_store->read_chunk({.x = 1, .z = 0})};
        const auto compact_chunk{compact_store->read_chunk({.x = 1, .z = 0})};
        expect(static_cast<bool>(fast_chunk));
        expect(static_cast<bool>(compact_chunk));
        if (fast_chunk && compact_chunk) {
            expect(world::semantic_hash(*fast_chunk).value_or("") ==
                   world::semantic_hash(*compact_chunk).value_or(""));
        }
    };

    "import_overwrite_semantics"_test = [] {
        const temp_directory directory;
        const auto source{build_world(directory)};
        const auto target{directory.sub("rwf")};
        expect(static_cast<bool>(io::create_directories(target)));
        expect(static_cast<bool>(io::write_text(target + "/keep.txt", "x")));
        const auto refused{anvil::import_world(source, target, false)};
        expect(!refused);
        if (!refused) {
            expect(refused.error().code == refrain::error_code::already_exists);
        }
        const auto replaced{anvil::import_world(source, target, true)};
        expect(static_cast<bool>(replaced));
        expect(!io::exists(target + "/keep.txt"));
    };

    "dimension_dictionary_components_are_validated"_test = [] {
        const temp_directory directory;
        auto store{world::world_store::create(directory.sub("world"))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        std::vector<std::byte> bytes(1024, std::byte{0x44});
        expect(!store->install_dimension_dictionary("", "overworld", bytes));
        expect(!store->install_dimension_dictionary("minecraft", "..", bytes));
        expect(!store->install_dimension_dictionary("minecraft", "a/b", bytes));
        expect(
            !store->install_dimension_dictionary("minecraft", "a\\b", bytes));
        expect(!store->install_dimension_dictionary(
            "minecraft", std::string_view{"a\0b", 3}, bytes));
        expect(static_cast<bool>(store->install_dimension_dictionary(
            "minecraft", "the_nether", bytes)));
        expect(io::exists(directory.sub(
            "world/dimensions/minecraft/the_nether/dictionaries/chunk.zdict")));
        const auto *dictionary{
            store->dictionary_for("minecraft", "the_nether")};
        expect(dictionary != nullptr);
        if (dictionary != nullptr) {
            expect(dictionary->id != 0U);
        }
    };

    "world_store_create_and_fsck"_test = [] {
        const temp_directory directory;
        const auto root{directory.sub("world")};
        auto store{world::world_store::create(root)};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        expect(!world::world_store::create(root));
        expect(!world::world_store::open(directory.sub("missing")));

        world::chunk chunk{};
        chunk.format_version = world::chunk_format_version;
        chunk.status = world::chunk_status::full;
        world::section_record section{};
        section.y = 0;
        section.kind = world::section_kind::single_value;
        section.palette.push_back(world::palette_entry{
            .key = "minecraft:air", .count = 0, .state = 0});
        section.biome_kind = world::section_kind::single_value;
        section.biome_palette.push_back("minecraft:plains");
        chunk.sections.push_back(std::move(section));
        expect(static_cast<bool>(store->write_chunk({.x = 0, .z = 0}, chunk)));
        expect(
            static_cast<bool>(store->write_chunk({.x = 33, .z = -1}, chunk)));
        value meta{make_compound()};
        set(meta, "format", make_string("refrain.world-meta/1"));
        auto meta_bytes{nbt::write(meta, "", nbt::compression::none)};
        expect(static_cast<bool>(meta_bytes));
        if (meta_bytes) {
            expect(static_cast<bool>(store->write_meta(*meta_bytes)));
        }
        expect(static_cast<bool>(store->commit()));
        const auto positions{store->chunk_positions()};
        expect(static_cast<bool>(positions));
        if (positions) {
            expect(positions->size() == 2U);
        }
        const auto checked{store->check(false)};
        expect(static_cast<bool>(checked));
        if (checked) {
            expect(checked->regions == 2U);
            expect(checked->chunks == 2U);
            expect(checked->corrupt == 0U);
        }
        // Corrupt one payload byte: fsck must report it, and --repair must not
        // silently drop the chunk.
        const auto region_path{
            root + "/dimensions/minecraft/overworld/regions/0.0.rfw"};
        auto bytes{io::read_file(region_path)};
        expect(static_cast<bool>(bytes));
        if (bytes) {
            (*bytes)[world::rwf::data_offset + 40] = static_cast<std::byte>(
                static_cast<std::uint8_t>(
                    (*bytes)[world::rwf::data_offset + 40]) ^
                0x5A);
            expect(static_cast<bool>(io::write_file(region_path, *bytes)));
        }
        const auto damaged{store->check(false)};
        expect(static_cast<bool>(damaged));
        if (damaged) {
            expect(damaged->corrupt >= 1U);
            expect(!damaged->problems.empty());
        }
    };

    "world_store_bounds_region_handles"_test = [] {
        const temp_directory directory;
        auto store{world::world_store::create(directory.sub("world-handles"))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        world::chunk chunk{};
        chunk.format_version = world::chunk_format_version;
        chunk.status = world::chunk_status::full;
        world::section_record section{};
        section.y = 0;
        section.kind = world::section_kind::single_value;
        section.palette.push_back(world::palette_entry{
            .key = "minecraft:air", .count = 0, .state = 0});
        section.biome_kind = world::section_kind::single_value;
        section.biome_palette.push_back("minecraft:plains");
        chunk.sections.push_back(std::move(section));
        constexpr std::int32_t regions{70};
        for (std::int32_t index{0}; index < regions; ++index) {
            expect(static_cast<bool>(
                store->write_chunk({.x = index * 32, .z = 0}, chunk)));
        }
        expect(static_cast<bool>(store->commit()));
        for (std::int32_t index{0}; index < regions; ++index) {
            const auto read{store->read_chunk({.x = index * 32, .z = 0})};
            expect(static_cast<bool>(read));
        }
    };

    "world_store_rejects_oversized_palette"_test = [] {
        const temp_directory directory;
        auto store{world::world_store::create(directory.sub("world-palette"))};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        world::palette_table table;
        for (std::size_t index{0}; index < 150'000U; ++index) {
            static_cast<void>(
                table.intern(fmt::format("minecraft:refrain_state_{}", index)));
        }
        expect(!store->install_palette(table));
    };

    "world_store_meta_level"_test = [] {
        const temp_directory directory;
        const auto root{directory.sub("world")};
        auto store{world::world_store::create(root)};
        expect(static_cast<bool>(store));
        if (!store) {
            return;
        }
        value data{make_compound()};
        set(data, "DataVersion", make_int(5023));
        set(data, "LevelName", make_string("Meta Test"));
        set(data, "Time", make_long(6000));
        value spawn{make_compound()};
        set(spawn, "pos", nbt::make_int_array({1, 2, 3}));
        set(spawn, "dimension", make_string("minecraft:overworld"));
        set(data, "spawn", std::move(spawn));
        value meta{make_compound()};
        set(meta, "format", make_string("refrain.world-meta/1"));
        set(meta, "level", std::move(data));
        auto bytes{nbt::write(meta, "", nbt::compression::none)};
        expect(static_cast<bool>(bytes));
        if (bytes) {
            expect(static_cast<bool>(store->write_meta(*bytes)));
        }
        value settings{make_compound()};
        set(settings, "DataVersion", make_int(5023));
        value settings_data{make_compound()};
        set(settings_data, "seed", make_long(99));
        set(settings, "data", std::move(settings_data));
        auto settings_bytes{nbt::write(settings, "", nbt::compression::none)};
        expect(static_cast<bool>(settings_bytes));
        if (settings_bytes) {
            expect(static_cast<bool>(store->objects().write(
                "global/data/minecraft/world_gen_settings.dat", *settings_bytes,
                world::object_media::nbt, false)));
            expect(static_cast<bool>(store->objects().commit()));
        }
        const auto level{store->level()};
        expect(static_cast<bool>(level));
        if (level) {
            expect(level->seed == 99);
            expect(level->level_name == "Meta Test");
            expect(level->time == 6000);
            expect(level->spawn_x == 1);
            expect(level->spawn_y == 2);
            expect(level->spawn_z == 3);
        }
    };
};
