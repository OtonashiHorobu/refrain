// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/config.hpp>

#include <cstdint>
#include <string>

namespace refrain::server {

// Headless server runtime: a fixed 20 Hz tick loop over an already open
// world, a plain console command queue, and a watchdog. The protocol/sim
// providers plug into the tick loop; this layer owns the
// lifecycle and the console never runs game logic on its own thread.

struct server_run_options {
    /// RWF world directory (`world_store::open`).
    std::string world_path{};
    /// Per-namespace configuration views supplied by the host. Empty views
    /// fall back to the built-in defaults.
    core::config::view network{};
    core::config::view world{};
    core::config::view storage{};
    core::config::view runtime{};
    /// Host-owned store used by the console `/config` commands and the
    /// shutdown save. Null disables both.
    core::config::store *config{nullptr};
    /// Run exactly this many ticks, then stop. 0 = until `stop`/EOF.
    std::int64_t ticks{0};
    /// Optional batch command file (one console command per line, all at tick
    /// 0).
    std::string command_file{};
    /// Optional tick-indexed replay script (`<tick> <command>` per line).
    std::string script_file{};
    /// Log the state hash every N ticks; 0 disables. The final hash is always
    /// logged at shutdown.
    std::uint64_t hash_every{0};
    /// Default zstd level for regions opened by the runtime (1..19).
    int world_zstd_level{3};
    /// Adaptive zstd policy (levels 1..19, radii in chunks, near <= mid).
    int zstd_near_level{1};
    int zstd_mid_level{3};
    int zstd_far_level{6};
    std::int32_t zstd_near_radius{2};
    std::int32_t zstd_mid_radius{8};
    /// 0 disables the watchdog.
    std::uint32_t watchdog_timeout_ms{5000};
    /// Flush dirty chunks every N ticks; 0 disables periodic autosave.
    std::uint64_t autosave_interval_ticks{6000};
    /// Try runtime region compaction every N ticks; 0 disables.
    std::uint64_t compact_interval_ticks{0};
    /// Minimum region bytes before compaction is considered.
    std::uint64_t compact_min_bytes{1ULL * 1024ULL * 1024ULL};
    std::uint32_t compact_garbage_percent{40};
    /// Compact aux object/blob stores every N ticks; 0 disables.
    std::uint64_t store_compact_interval_ticks{0};
    /// Optional content store for periodic mark-sweep GC.
    std::string content_store{};
    bool content_gc_apply{false};
    int content_gc_keep_days{7};
    std::uint64_t content_gc_budget_ms{2000};
    std::uint64_t content_gc_interval_ticks{0};
};

/// Run the server until a console `stop`, the tick limit, or EOF.
///
/// Returns a process exit code (0 on a clean stop). Logging must already be
/// initialized; the function names the tick/console/watchdog threads.
[[nodiscard]] auto run_server(const server_run_options &options) -> int;

} // namespace refrain::server
