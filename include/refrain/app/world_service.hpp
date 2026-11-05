// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <cstdint>
#include <string>

namespace refrain::app {

// `refrain-tools world ...` / `worldgen ...` command bodies. Like the
// other services these only parse options, call the world layer and format the
// result; the CLI main owns argv handling.

struct world_import_options {
    std::string from{};
    std::string to{};
    bool force{false};
    unsigned threads{0};                       // 0 = auto, 1 = serial
    std::string compression{"balanced"};       // fast|balanced|compact
    std::string dictionary{};                  // optional zstd dictionary file
    std::string dictionary_scope{"dimension"}; // world|dimension
};

struct world_export_options {
    std::string from{};
    std::string to{};
    bool force{false};
    unsigned threads{0}; // 0 = auto, 1 = serial
};

struct world_fsck_options {
    std::string path{};
    bool repair{false};
};

struct world_compact_options {
    std::string path{};
};

struct world_palette_gc_options {
    std::string from{};
    std::string to{};
    bool force{false};
    unsigned threads{0};
};

struct world_info_options {
    std::string path{};
    bool json{false};
    /// Use `world_store::validate_level()` (required fields/types) instead of
    /// the lenient projection.
    bool strict{false};
};

struct world_diff_options {
    std::string baseline{};
    std::string candidate{};
    int min_match{100};
    bool json{false};
};

struct worldgen_harness_options {
    std::string candidate{};
    std::string oracle{};
    std::string report{};
    int min_match{95};
};

struct worldgen_oracle_options {
    std::string jar{};
    std::string out{};
    std::int64_t seed{0};
    std::string java{"java"};
};

[[nodiscard]] auto run_world_import(const world_import_options &options) -> int;
[[nodiscard]] auto run_world_export(const world_export_options &options) -> int;
[[nodiscard]] auto run_world_fsck(const world_fsck_options &options) -> int;
[[nodiscard]] auto run_world_compact(const world_compact_options &options)
    -> int;
[[nodiscard]] auto run_world_palette_gc(const world_palette_gc_options &options)
    -> int;
[[nodiscard]] auto run_world_info(const world_info_options &options) -> int;
[[nodiscard]] auto run_world_diff(const world_diff_options &options) -> int;
[[nodiscard]] auto run_worldgen_harness(const worldgen_harness_options &options)
    -> int;
[[nodiscard]] auto run_worldgen_oracle(const worldgen_oracle_options &options)
    -> int;

} // namespace refrain::app
