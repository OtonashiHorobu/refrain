// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/core/config.hpp>
#include <refrain/net/protocol_codec.hpp>
#include <refrain/net/server.hpp>
#include <refrain/net/transport.hpp>
#include <refrain/server/world_port.hpp>

#include <cstdint>
#include <functional>
#include <string>

namespace refrain::mod {
class host;
} // namespace refrain::mod

namespace refrain::server {

// Headless server runtime: a fixed 20 Hz tick loop over an already open
// world, a plain console command queue, and a watchdog. The protocol/sim
// providers plug into the tick loop; this layer owns the
// lifecycle and the console never runs game logic on its own thread.

struct server_run_options {
    /// RWF world directory the world port opens.
    std::string world_path{};
    /// Resolved game data directory: it holds `data/`, the cache marker and
    /// the extracted structure templates. Empty falls back to the checkout
    /// snapshot at `content/vanilla/<version>`.
    std::string data_dir{};
    /// Datapack `data` directory the bootstrap generator reads when the world
    /// has no chunks yet. Empty is `<data_dir>/data`.
    std::string datapack_root{};
    /// Extracted structure templates (`<root>/<namespace>/structure/**.nbt`).
    /// Empty is `<datapack_root>/../templates`; a missing directory still
    /// generates procedural pieces.
    std::string templates_root{};
    /// Tracked block/item table container. Empty is
    /// `<data_dir>/tables.rvtb`; a missing file leaves the runtime without an
    /// item table and logs a warning.
    std::string tables_path{};
    /// Protocol registry catalog. Empty is
    /// `<data_dir>/protocol_registry.json`.
    std::string protocol_registry_path{};
    /// Game version shown in the ready line and the status ping.
    std::string game_version{"26.3"};
    /// Per-namespace configuration views supplied by the host. Empty views
    /// fall back to the built-in defaults.
    core::config::view network{};
    core::config::view world{};
    core::config::view storage{};
    core::config::view runtime{};
    /// Optional transport provider factory. Empty keeps the built-in Asio
    /// TCP transport.
    std::function<result<std::unique_ptr<net::transport>>(
        net::transport_handler, std::size_t)>
        make_transport{};
    /// Optional protocol codec provider. Empty keeps the built-in 777 codec.
    std::shared_ptr<net::protocol_codec> protocol{};
    /// Optional session feature views. Empty keeps static status and sends no
    /// tab list.
    net::status_provider status{};
    net::tab_provider tab{};
    net::tab_encoder encode_tab{};
    net::player_event player_join{};
    net::player_event player_leave{};
    net::session_verifier verify_session{};
    std::function<void(std::uint64_t)> on_tick{};
    /// Called once from the tick loop's thread after the listener has bound,
    /// with the port it actually listens on. A host that leaves the configured
    /// port at zero reads the ephemeral port here instead of searching the log.
    std::function<void(std::uint16_t)> on_listening{};
    /// Host-owned store used by the console `/config` commands and the
    /// shutdown save. Null disables both.
    core::config::store *config{nullptr};
    /// Host configuration directory; the runtime reads the mod management
    /// policy and the hot gates from it. Empty keeps the built-in defaults.
    std::string config_dir{};
    /// Loaded mod host (the running composition). Null leaves `/mods list`
    /// with an empty composition and the hot lifecycle unavailable. The
    /// runtime reads the composition snapshot from it and stages a running
    /// composition change through it; the host belongs to the caller and
    /// outlives this call.
    refrain::mod::host *mod_host{nullptr};
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
/// `services` are the active `refrain:world`, `refrain:sim` and
/// `refrain:storage` providers; `world` is the port built from them. The port
/// owns the world it opens, so it must outlive this call.
///
/// Returns a process exit code (0 on a clean stop). Logging must already be
/// initialized; the function names the tick/console/watchdog threads.
[[nodiscard]] auto run_server(const server_run_options &options,
                              const server_service_handles &services,
                              world_port &world) -> int;

} // namespace refrain::server
