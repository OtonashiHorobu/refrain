// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Anvil (vanilla Java Edition) world import/export for RWF v1.
//
// Scope: the 26.3 world layout only — `level.dat`,
// `dimensions/<ns>/<dim>/{region,entities,poi,data}`, `players/data/*.dat`,
// `data/**`, `datapacks/**`; non-overworld dimensions are imported/exported
// natively. Region compression: gzip/zlib/none and LZ4 (`type 4`, lz4-java
// block stream) on import, plus external `.mcc` chunks; export writes zlib.
// The importer requires DataVersion 5023 (Minecraft 26.3).
//
// The mapping between the NBT chunk and `world::chunk` is lossless: import and
// export preserve every modelled field in both directions.

#pragma once

#include <refrain/core/error.hpp>
#include <refrain/world/model.hpp>
#include <refrain/world/rwf.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::world::anvil {

/// DataVersion written on export and required on import.
inline constexpr std::int32_t supported_data_version = 5023;

/// Automatic zstd dictionary training scope. `dimension` trains one dictionary
/// per dimension (stored under that dimension); `world` trains one dictionary
/// over every dimension and stores it as the world-level fallback. An explicit
/// `--dictionary` always wins and is installed as the world-level fallback.
enum class dictionary_scope : std::uint8_t { world, dimension };

struct import_result {
    std::size_t regions{0};
    std::size_t chunks{0};
    std::size_t entity_chunks{0};
    std::size_t poi_chunks{0};
    std::size_t playerdata{0};
    std::size_t datapack_files{0};
    std::size_t aux_files{0};
};

struct export_result {
    std::size_t regions{0};
    std::size_t chunks{0};
    std::size_t entity_chunks{0};
    std::size_t poi_chunks{0};
    std::size_t playerdata{0};
    std::size_t datapack_files{0};
    std::size_t aux_files{0};
};

/// Detected on-disk world layout.
enum class world_format {
    anvil, ///< 26.3 layout (`dimensions/minecraft/overworld/region/`)
    rwf,   ///< `meta.rfw` + `regions/*.rfw`
};

/// Detect the layout of `root` (`anvil` when `level.dat` exists, otherwise
/// `rwf` when `meta.rfw` or `regions/` exists).
[[nodiscard]] auto detect_world(std::string_view root) -> result<world_format>;

/// One chunk's semantic identity, used by `world diff` and the worldgen
/// harness. `hash` is `world::semantic_hash` of the decoded chunk.
struct chunk_digest {
    std::string dimension{"minecraft:overworld"};
    chunk_pos pos{};
    std::string status{};
    std::string hash{};
};

/// Decode every chunk of a 26.3 Anvil or RWF world into digests, sorted by
/// coordinate. Both layouts are normalised through `world::chunk`, so the
/// comparison is storage-independent.
[[nodiscard]] auto scan_world(std::string_view root)
    -> result<std::vector<chunk_digest>>;

/// Import `source` (Anvil layout) into `target` (RWF layout). With
/// `overwrite`, an existing `target` directory is removed first.
/// `threads`: 0 = auto (jobs executor), 1 = serial, N = N workers. Regions are
/// independent and each worker owns its own region file; output bytes do not
/// depend on the thread count.
[[nodiscard]] auto
import_world(std::string_view source, std::string_view target, bool overwrite,
             unsigned threads = 1, int zstd_level = 3,
             const rwf::zstd_dictionary *dictionary = nullptr,
             dictionary_scope scope = dictionary_scope::dimension)
    -> result<import_result>;

/// Export `source` (RWF layout) into `target` (Anvil layout).
/// `threads` follows `import_world`; each worker owns its own read handle and
/// writes a disjoint Anvil region file.
[[nodiscard]] auto export_world(std::string_view source,
                                std::string_view target, bool overwrite,
                                unsigned threads = 1) -> result<export_result>;

struct palette_gc_result {
    std::size_t entries_before{0};
    std::size_t entries_after{0};
    std::size_t chunks{0};
    std::size_t regions{0};
    std::uint64_t bytes_before{0};
    std::uint64_t bytes_after{0};
};

/// Rebuild `source` (RWF) into a fresh `target` (RWF) through the Anvil
/// round-trip, which recomputes the world palette from reachable block/biome
/// keys (dropping entries that no chunk references) and retrains the zstd
/// dictionaries. The source world is left untouched; `target` must be absent
/// (or `overwrite`). This is an offline compaction: it needs temp disk space
/// for an Anvil copy and must not run against a live server.
[[nodiscard]] auto rebuild_palette(std::string_view source,
                                   std::string_view target, bool overwrite,
                                   unsigned threads = 1, int zstd_level = 3)
    -> result<palette_gc_result>;

} // namespace refrain::world::anvil
