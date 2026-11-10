// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// RWF world directory (`world/`): `meta.rfw`,
// `dimensions/<ns>/<dim>/{regions,blobs,objects,dictionaries}` and root
// `objects/` (global data/players/assets/datapacks).
//
// The store owns open region handles (one per region, single-writer), decodes
// chunk payloads through `world::decode_chunk` and exposes `meta.rfw` as an
// opaque NBT document. `level()` and `read_gameplay()` project the subset of
// vanilla `level.dat` that the CLI and the server need; `write_gameplay()`
// rewrites only the gameplay tags of that document.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/world/model.hpp>
#include <refrain/world/palette_table.hpp>
#include <refrain/world/rwf.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::world {

/// `refrain.world-meta/1` format marker inside `meta.rfw`.
inline constexpr std::string_view meta_format = "refrain.world-meta/1";

/// Projection of `meta.level` (the vanilla `level.dat` `Data` compound).
struct level_info {
    std::int32_t data_version{0};
    std::int64_t seed{0};
    std::int32_t spawn_x{0};
    std::int32_t spawn_y{0};
    std::int32_t spawn_z{0};
    std::int64_t time{0};
    std::string level_name;
    /// `GameType` (0 survival, 1 creative, 2 adventure, 3 spectator).
    std::int32_t game_mode{1};
    /// `hardcore` (vanilla stores it as a byte).
    bool hardcore{false};
    /// `Difficulty` (0 peaceful, 1 easy, 2 normal, 3 hard).
    std::int32_t difficulty{2};
    /// `DifficultyLocked` (vanilla stores it as a byte).
    bool difficulty_locked{false};
};

/// Editable gameplay subset of `meta.level`; the vanilla tag names are
/// `GameType`, `hardcore`, `Difficulty` and `DifficultyLocked`.
struct gameplay_settings {
    std::int32_t game_mode{1};
    bool hardcore{false};
    std::int32_t difficulty{2};
    bool difficulty_locked{false};
};

struct world_stats {
    std::size_t regions{0};
    std::size_t dimensions{0};
    std::size_t objects{0};
    std::size_t chunks{0};
    std::size_t entity_chunks{0};
    std::size_t poi_chunks{0};
    std::size_t playerdata{0};
    std::size_t datapack_files{0};
    std::size_t aux_files{0};
    std::uint64_t bytes{0};
};

/// Palette reference usage across all chunks (the input to an in-place
/// palette GC; see `rebuild_palette_in_place` in
/// `refrain/world/maintenance.hpp` and the offline `world palette-gc`).
struct palette_usage {
    std::size_t table_entries{0};
    std::size_t referenced_entries{0};
    std::size_t chunks{0};
};

struct world_compact {
    object_store_stats objects{};
    object_store_stats blobs{};
    bool repaired{false};
};

struct world_check {
    std::size_t regions{0};
    std::size_t dimensions{0};
    std::size_t chunks{0};
    std::size_t corrupt{0};
    std::uint64_t garbage_bytes{0};
    bool repaired{false};
    std::vector<std::string> problems;
};

class world_store {
  public:
    world_store() noexcept;
    ~world_store();

    world_store(world_store &&other) noexcept;
    auto operator=(world_store &&other) noexcept -> world_store &;

    world_store(const world_store &) = delete;
    auto operator=(const world_store &) -> world_store & = delete;

    /// Create an empty world directory. Fails with `already_exists` when the
    /// path exists and is not an empty directory.
    [[nodiscard]] static auto create(std::string_view root)
        -> result<world_store>;

    /// Open an existing world directory (meta may be absent until written).
    [[nodiscard]] static auto open(std::string_view root)
        -> result<world_store>;

    [[nodiscard]] auto valid() const noexcept -> bool;
    [[nodiscard]] auto root() const -> const std::string &;

    /// Default zstd level for regions opened after this call (1..19). The
    /// server exposes it as a compression profile; `write_chunk(..., level)`
    /// overrides it for one record and `sim::world` chooses levels adaptively.
    [[nodiscard]] auto set_zstd_level(int level) -> status;

    /// Default zstd level applied to regions opened from now on (the value
    /// last accepted by `set_zstd_level`, or `rwf::default_zstd_level`).
    [[nodiscard]] auto zstd_level() const noexcept -> int;

    /// Install the world-level fallback zstd dictionary for subsequent region
    /// writes; it is stored under `dictionaries/chunk.zdict` and loaded on
    /// open. Dimensions with their own dictionary ignore the fallback.
    [[nodiscard]] auto install_dictionary(std::span<const std::byte> bytes)
        -> status;

    /// Install a dimension-scoped dictionary at
    /// `dimensions/<namespace>/<dimension>/dictionaries/chunk.zdict`.
    [[nodiscard]] auto
    install_dimension_dictionary(std::string_view namespace_name,
                                 std::string_view dimension,
                                 std::span<const std::byte> bytes) -> status;

    /// World-level fallback dictionary (null when absent).
    [[nodiscard]] auto dictionary() const -> const rwf::zstd_dictionary *;

    /// Dictionary used by a dimension: its scoped dictionary when present,
    /// otherwise the world-level fallback. Null when neither exists.
    [[nodiscard]] auto dictionary_for(std::string_view namespace_name,
                                      std::string_view dimension) const
        -> const rwf::zstd_dictionary *;

    /// Install a palette table (used by import before chunk encoding).
    [[nodiscard]] auto install_palette(palette_table table) -> status;

    [[nodiscard]] auto palette() const -> const palette_table &;

    /// Count distinct palette ids referenced by every stored chunk.
    [[nodiscard]] auto palette_usage_stats() -> result<palette_usage>;

    /// Raw `meta.rfw` payload (NBT document, uncompressed).
    [[nodiscard]] auto read_meta() -> result<std::vector<std::byte>>;
    [[nodiscard]] auto write_meta(std::span<const std::byte> payload) -> status;

    /// Parsed view of `meta.level` (lenient projection).
    [[nodiscard]] auto level() -> result<level_info>;

    /// Gameplay fields of `meta.level`, using the same lenient projection as
    /// `level()`: missing tags keep their default and tags stored as a byte or
    /// an int are accepted. Numeric values outside [0, 3] also keep the
    /// default instead of failing the read.
    [[nodiscard]] auto read_gameplay() -> result<gameplay_settings>;

    /// Replace the four gameplay tags of `meta.level` and atomically rewrite
    /// `meta.rwf`; every other tag and the single-record container are left
    /// intact. `game_mode` and `difficulty` are validated against [0, 3]
    /// before the file is read. A missing `meta.rwf` is an error (the store
    /// never creates a world here). Callers serialize this with other writes
    /// to the same world.
    [[nodiscard]] auto write_gameplay(const gameplay_settings &settings)
        -> status;

    /// Strict identity validation of `meta.level`: DataVersion, spawn.pos,
    /// Time and LevelName must exist with the expected NBT types; a
    /// `world_gen_settings.dat` that exists must parse and carry `data.seed`.
    /// Returns the same projection as `level()` when valid.
    [[nodiscard]] auto validate_level() -> result<level_info>;

    [[nodiscard]] auto read_chunk(const chunk_pos &pos) -> result<chunk>;
    /// Write a chunk using the region's configured zstd level.
    [[nodiscard]] auto write_chunk(const chunk_pos &pos, const chunk &value)
        -> status;

    /// Write a chunk with an explicit zstd level in [1, 19] (adaptive writes).
    /// The region codec and dictionary still apply; levels outside the range
    /// fail with `invalid_argument` before anything is encoded.
    [[nodiscard]] auto write_chunk(const chunk_pos &pos, const chunk &value,
                                   int zstd_level) -> status;

    [[nodiscard]] auto contains(const chunk_pos &pos) -> bool;

    /// Stored chunk coordinates, sorted. Scans the region index files.
    [[nodiscard]] auto chunk_positions() -> result<std::vector<chunk_pos>>;

    /// Commit every open region (group commit).
    [[nodiscard]] auto commit() -> status;

    /// Compact the aux object store and blob store (drop dead objects).
    [[nodiscard]] auto compact() -> result<world_compact>;

    /// Rewrite overworld region files whose garbage reaches `min_bytes` and
    /// `min_percent` of the file size. Returns the number of compacted regions.
    /// Callers must serialize this with writes (the server does it under the
    /// runtime world's store mutex).
    [[nodiscard]] auto compact_regions(std::uint64_t min_bytes,
                                       unsigned min_percent)
        -> result<std::size_t>;

    [[nodiscard]] auto blobs() -> rwf::blob_store &;

    /// Path-addressed aux objects (`objects/`): data/players/icon/datapacks.
    [[nodiscard]] auto objects() -> object_store &;

    [[nodiscard]] auto stats() -> result<world_stats>;

    /// Validate every region, chunk payload, blob object and `meta.rfw`; with
    /// `repair` rebuilds damaged region indexes and `meta.rfw` is left
    /// untouched.
    [[nodiscard]] auto check(bool repair) -> result<world_check>;

  private:
    struct impl;
    explicit world_store(std::unique_ptr<impl> state);

    std::unique_ptr<impl> impl_;
};

/// `chunk_pos` for a region-local slot (used by region scanning).
[[nodiscard]] auto position_for_slot(std::int32_t region_x,
                                     std::int32_t region_z, std::uint32_t slot,
                                     std::int32_t region_edge) -> chunk_pos;

} // namespace refrain::world
