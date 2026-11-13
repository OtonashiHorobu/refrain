// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/contracts/services/sim.hpp>
#include <refrain/protocol/current.hpp>
#include <refrain/server/server_runtime.hpp>

#include <refrain/session/chunk_tracker.hpp>
#include <refrain/session/play_session.hpp>

#include <refrain/app/services.hpp>
#include <refrain/core/io.hpp>
#include <refrain/core/log.hpp>
#include <refrain/core/time.hpp>
#include <refrain/data/vanilla.hpp>
#include <refrain/net/registry.hpp>
#include <refrain/net/server.hpp>
#include <refrain/server/chunk_generation.hpp>
#include <refrain/server/chunk_packet.hpp>
#include <refrain/server/chunk_service.hpp>
#include <refrain/server/command_bridge.hpp>
#include <refrain/server/console_commands.hpp>
#include <refrain/server/console_service.hpp>
#include <refrain/server/entity_broadcaster.hpp>
#include <refrain/server/inventory_sync.hpp>
#include <refrain/server/maintenance_service.hpp>
#include <refrain/server/play_gateway.hpp>
#include <refrain/server/world_bootstrap.hpp>
#include <refrain/sim/replay.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/store.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr refrain::log::logger log_config{"config"};
constexpr refrain::log::logger log_net{"net"};
constexpr refrain::log::logger log_server{"server"};
constexpr refrain::log::logger log_sim{"sim"};
constexpr refrain::log::logger log_world{"world"};

} // namespace

namespace refrain::server {

using app::report_error;
namespace {
constexpr std::uint64_t max_console_file_bytes = 1ULL * 1024ULL * 1024ULL;

/// Process-wide lifecycle flags shared with the console and watchdog threads.
struct runtime_state {
    /// The stdin thread outlives the runtime, so the flag is shared.
    std::shared_ptr<std::atomic<bool>> stop{
        std::make_shared<std::atomic<bool>>(false)};
    std::atomic<std::int64_t> heartbeat_ns{0};
};

} // namespace

auto run_server(const server_run_options &options,
                const server_service_handles &services, world_port &world)
    -> int {
    log_server.debug("world port={}",
                     is_first_party_world(services) ? "direct" : "contract");
    if (options.world_path.empty()) {
        return report_error(refrain::make_error(
            refrain::error_code::invalid_argument,
            "refrain-server: --world is required to start the tick loop"));
    }

    const auto level_name{options.world.get_string("level_name", "world")};
    const auto view_distance{static_cast<std::int32_t>(
        options.network.get_int64("view_distance", 10))};
    const auto simulation_distance{static_cast<std::int32_t>(
        options.network.get_int64("simulation_distance", 10))};
    const auto max_players{options.network.get_int64("status.max_players", 20)};
    const auto online_mode{options.network.get_bool("online_mode", false)};
    log_config.info("level={} view={} sim={} max_players={} online_mode={}",
                    level_name, view_distance, simulation_distance, max_players,
                    online_mode);
    const auto zstd_level{static_cast<int>(
        options.storage.get_int64("zstd_level", options.world_zstd_level))};
    const auto zstd_near_level{static_cast<int>(
        options.storage.get_int64("zstd_near_level", options.zstd_near_level))};
    const auto zstd_mid_level{static_cast<int>(
        options.storage.get_int64("zstd_mid_level", options.zstd_mid_level))};
    const auto zstd_far_level{static_cast<int>(
        options.storage.get_int64("zstd_far_level", options.zstd_far_level))};
    const auto zstd_near_radius{
        static_cast<std::int32_t>(options.storage.get_int64(
            "zstd_near_radius", options.zstd_near_radius))};
    const auto zstd_mid_radius{static_cast<std::int32_t>(
        options.storage.get_int64("zstd_mid_radius", options.zstd_mid_radius))};
    const auto autosave_interval{
        static_cast<std::uint64_t>(options.storage.get_int64(
            "autosave_interval_ticks",
            static_cast<std::int64_t>(options.autosave_interval_ticks)))};
    const auto compact_interval{
        static_cast<std::uint64_t>(options.storage.get_int64(
            "compact_interval_ticks",
            static_cast<std::int64_t>(options.compact_interval_ticks)))};
    const auto store_compact_interval{
        static_cast<std::uint64_t>(options.storage.get_int64(
            "store_compact_interval_ticks",
            static_cast<std::int64_t>(options.store_compact_interval_ticks)))};
    const auto compact_min_bytes{
        static_cast<std::uint64_t>(options.storage.get_int64(
            "compact_min_bytes",
            static_cast<std::int64_t>(options.compact_min_bytes)))};
    const auto compact_garbage_percent{
        static_cast<std::uint32_t>(options.storage.get_int64(
            "compact_garbage_percent", options.compact_garbage_percent))};
    const auto watchdog_timeout{
        static_cast<std::uint32_t>(options.runtime.get_int64(
            "watchdog_timeout_ms", options.watchdog_timeout_ms))};
    log_config.info("zstd={}, watchdog={}ms, autosave={}", zstd_level,
                    watchdog_timeout, autosave_interval);

    // Keep the maintenance worker's option view consistent with the resolved
    // configuration values.
    server_run_options effective{options};
    effective.world_zstd_level = zstd_level;
    effective.zstd_near_level = zstd_near_level;
    effective.zstd_mid_level = zstd_mid_level;
    effective.zstd_far_level = zstd_far_level;
    effective.zstd_near_radius = zstd_near_radius;
    effective.zstd_mid_radius = zstd_mid_radius;
    effective.watchdog_timeout_ms = watchdog_timeout;
    effective.autosave_interval_ticks = autosave_interval;
    effective.compact_interval_ticks = compact_interval;
    effective.store_compact_interval_ticks = store_compact_interval;
    effective.compact_min_bytes = compact_min_bytes;
    effective.compact_garbage_percent = compact_garbage_percent;

    if (auto opened{world.open(options.world_path)}; !opened) {
        return report_error(opened.error());
    }
    const std::string datapack_root{options.datapack_root.empty()
                                        ? "content/vanilla/26.3/data"
                                        : options.datapack_root};
    auto level_info{world.level()};
    if (!level_info) {
        return report_error(level_info.error());
    }
    if (world.loaded_chunks() == 0) {
        // A world created from a seed starts empty; fill the spawn area
        // before the listener accepts joins.
        const auto seed{level_info->seed};
        const auto spawn{refrain::world::chunk_pos{0, 0}};
        auto generated{refrain::server::generate_spawn_area(
            world,
            refrain::server::world_bootstrap_config{
                .data_root = datapack_root,
                .templates_root = datapack_root + "/../templates",
                .seed = seed,
                .radius = 3,
            },
            spawn)};
        if (!generated) {
            return report_error(generated.error());
        }
        refrain::log::info("[world]: generated {} spawn chunk(s)", *generated);
    }
    // Columns the viewport needs but the store does not hold are generated on
    // a fixed worker pool, so walking past the spawn area keeps streaming
    // terrain. The credit level adapts until an operator pins it.
    refrain::server::chunk_service chunks{};
    refrain::server::chunk_generation::config generation_config{
        .data_root = datapack_root,
        .templates_root = datapack_root + "/../templates",
        .seed = level_info->seed,
    };
    generation_config.admission.min_credits =
        static_cast<std::size_t>(std::max<std::int64_t>(
            options.runtime.get_int64("generation_min_credits", 1), 1));
    generation_config.admission.max_credits =
        static_cast<std::size_t>(std::max<std::int64_t>(
            options.runtime.get_int64("generation_max_credits", 8), 1));
    if (auto started{chunks.start_generation(generation_config)}; !started) {
        refrain::log::warn("[world]: generation unavailable: {}",
                           started.error().message);
    }
    if (auto configured{world.set_zstd_level(zstd_level)}; !configured) {
        return report_error(configured.error());
    }
    if (auto configured{world.set_zstd_policy(
            refrain::sim::zstd_policy{.near_level = zstd_near_level,
                                      .mid_level = zstd_mid_level,
                                      .far_level = zstd_far_level,
                                      .near_radius = zstd_near_radius,
                                      .mid_radius = zstd_mid_radius})};
        !configured) {
        return report_error(configured.error());
    }
    auto level{world.level()};
    if (!level) {
        return report_error(level.error());
    }
    log_world.info("{} dataversion={} seed={}", options.world_path,
                   level->data_version, level->seed);
    const auto initial_mode{
        level->game_mode >= 0 && level->game_mode <= 3
            ? static_cast<refrain::net::game_mode>(level->game_mode)
            : refrain::net::game_mode::creative};
    world.seed(static_cast<std::uint64_t>(level->seed));
    // The console and replay scripts drive one synthetic player; it is created
    // before the listener starts, so it is always the first player by entity id
    // and commands that omit a name keep addressing it.
    auto console_player{
        world.add_player(refrain::sim::player_table::registration{
            .connection = refrain::sim::synthetic_connection,
            .name = std::string{"@console"},
            .world = std::string{level_name},
            .x = static_cast<double>(level->spawn_x) + 0.5,
            .y = static_cast<double>(level->spawn_y),
            .z = static_cast<double>(level->spawn_z) + 0.5,
            .game_mode = static_cast<std::uint32_t>(initial_mode),
        })};
    if (!console_player) {
        return report_error(console_player.error());
    }

    // Ticket seeding: the spawn column plus its 3x3 neighbourhood. The
    // tick loop keeps them loaded; a missing chunk in a sparse imported world
    // is a warning, not a fatal startup error.
    const auto spawn_chunk{refrain::sim::chunk_pos{
        level->spawn_x >= 0 ? level->spawn_x / 16 : (level->spawn_x - 15) / 16,
        level->spawn_z >= 0 ? level->spawn_z / 16
                            : (level->spawn_z - 15) / 16}};
    for (std::int32_t dx{-1}; dx <= 1; ++dx) {
        for (std::int32_t dz{-1}; dz <= 1; ++dz) {
            const refrain::sim::chunk_pos pos{spawn_chunk.x + dx,
                                              spawn_chunk.z + dz};
            auto ticketed{
                world.add_ticket(pos, refrain::sim::ticket_kind::spawn)};
            if (!ticketed) {
                log_server.warn("{},{}: {}", pos.x, pos.z,
                                ticketed.error().message);
            }
        }
    }
    log_server.info("{} chunks {} tickets", world.loaded_chunks(),
                    world.ticket_count(spawn_chunk));

    std::optional<refrain::data::vanilla_tables> tables{};
    auto table_bytes{
        refrain::core::io::read_file("content/vanilla/26.3/tables.rvtb")};
    if (!table_bytes) {
        log_net.warn("vanilla tables unavailable: {}",
                     table_bytes.error().message);
    } else {
        auto opened{refrain::data::vanilla_tables::open(*table_bytes)};
        if (!opened) {
            log_net.warn("vanilla tables invalid: {}", opened.error().message);
        } else {
            tables = std::move(*opened);
        }
    }
    if (tables) {
        world.set_item_table(refrain::sim::item_table::from_tables(*tables));
    }
    std::optional<refrain::net::registry_catalog> registries{};
    auto loaded_registries{refrain::net::registry_catalog::load(
        "content/vanilla/26.3/protocol_registry.json")};
    if (!loaded_registries) {
        log_net.warn("registry catalog unavailable: {}",
                     loaded_registries.error().message);
    } else {
        registries = std::move(*loaded_registries);
    }
    auto command_environment{
        std::make_shared<refrain::server::command_environment>()};
    command_environment->world = &world;
    command_environment->default_mode = initial_mode;
    auto *player_table{world.players()};
    if (player_table == nullptr) {
        return report_error(refrain::make_error(
            refrain::error_code::unsupported,
            "the active world provider exposes no player table"));
    }
    refrain::session::play_session players{*player_table};
    refrain::session::chunk_tracker views{};
    const auto tracking_chunks{static_cast<std::int32_t>(std::max<std::int64_t>(
        1, options.network.get_int64("entity_tracking_range", 512) / 16))};
    refrain::server::server_gateway gateway{};
    refrain::server::entity_broadcaster broadcaster{gateway, tracking_chunks};
    refrain::net::server *active_listener{nullptr};
    const auto external_join{options.player_join};
    const auto external_leave{options.player_leave};
    refrain::net::server_inputs network_inputs{};
    network_inputs.commands =
        refrain::server::make_command_bridge(command_environment);
    network_inputs.policy.hardcore = level->hardcore;
    network_inputs.policy.difficulty =
        level->difficulty >= 0 && level->difficulty <= 3
            ? static_cast<std::uint32_t>(level->difficulty)
            : 2U;
    network_inputs.policy.difficulty_locked = level->difficulty_locked;
    network_inputs.join_state =
        [command_environment](const refrain::net::player_identity &identity) {
            return command_environment->join_state(identity.connection);
        };
    network_inputs.fallback_bind_host =
        options.network.get_string("bind_host", "0.0.0.0");
    network_inputs.fallback_port =
        static_cast<std::uint16_t>(options.network.get_int64("port", 25565));
    network_inputs.fallback_compression = static_cast<std::int32_t>(
        options.network.get_int64("compression_threshold", 256));
    network_inputs.fallback_view_distance = static_cast<std::int32_t>(
        options.network.get_int64("view_distance", 10));
    network_inputs.fallback_simulation_distance = static_cast<std::int32_t>(
        options.network.get_int64("simulation_distance", 10));
    network_inputs.fallback_max_players = static_cast<std::int32_t>(
        options.network.get_int64("status.max_players", 20));
    network_inputs.fallback_online_mode =
        options.network.get_bool("online_mode", true);
    network_inputs.fallback_secure_chat =
        options.network.get_bool("enforce_secure_profile", true);
    network_inputs.fallback_motd =
        options.network.get_string("status.motd", "A Refrain Server");
    network_inputs.make_transport = options.make_transport;
    network_inputs.protocol = options.protocol;
    network_inputs.status = options.status;
    network_inputs.tab = options.tab;
    network_inputs.encode_tab = options.encode_tab;
    const auto spawn_chunk_x{static_cast<std::int32_t>(level->spawn_x / 16)};
    const auto spawn_chunk_z{static_cast<std::int32_t>(level->spawn_z / 16)};
    // The network shell queues this event from its I/O thread and delivers it
    // from `listener->tick()`, so the session table stays simulation-owned.
    network_inputs.player_join =
        [&players, &views, &broadcaster, &active_listener, external_join,
         spawn_chunk_x, spawn_chunk_z,
         view_distance](const refrain::net::player_identity &identity) {
            const auto added{players.add_player(
                identity.connection, identity.id, std::string{identity.name},
                std::string{identity.world}, 0.0, 64.0, 0.0, 1)};
            if (added) {
                static_cast<void>(views.update(identity.connection,
                                               spawn_chunk_x, spawn_chunk_z,
                                               view_distance));
            }
            if (added && active_listener != nullptr) {
                broadcaster.joined(players, identity.connection);
            }
            if (external_join) {
                external_join(identity);
            }
        };
    network_inputs.player_leave =
        [&players, &views, &broadcaster,
         external_leave](const refrain::net::player_identity &identity) {
            const auto leaving{players.player_of(identity.connection)};
            const auto copy{
                leaving == nullptr
                    ? std::nullopt
                    : std::optional<refrain::session::player>{*leaving}};
            static_cast<void>(players.remove_player(identity.connection));
            static_cast<void>(views.remove(identity.connection));
            if (copy) {
                broadcaster.left(players, *copy);
            }
            if (external_leave) {
                external_leave(identity);
            }
        };
    network_inputs.movement = [&players, &views, view_distance](
                                  const refrain::net::movement_event &event) {
        static_cast<void>(players.update_position(event.connection, event.x,
                                                  event.y, event.z, event.yaw,
                                                  event.pitch));
        const auto player{players.player_of(event.connection)};
        if (player != nullptr) {
            static_cast<void>(views.update(
                event.connection,
                static_cast<std::int32_t>(std::floor(player->x / 16.0)),
                static_cast<std::int32_t>(std::floor(player->z / 16.0)),
                std::min(player->view_distance, view_distance)));
        }
    };
    network_inputs.view_distance =
        [&players, &views,
         view_distance](const refrain::net::view_distance_event &event) {
            // The server owns the viewport: a client asking for more than the
            // configured distance gets the configured one, and a client asking
            // for nothing still gets the minimum renderable square.
            const auto clamped_distance{
                std::clamp(event.distance, std::int32_t{2}, view_distance)};
            static_cast<void>(
                players.set_view_distance(event.connection, clamped_distance));
            const auto player{players.player_of(event.connection)};
            if (player != nullptr) {
                static_cast<void>(views.update(
                    event.connection,
                    static_cast<std::int32_t>(std::floor(player->x / 16.0)),
                    static_cast<std::int32_t>(std::floor(player->z / 16.0)),
                    clamped_distance));
            }
        };
    network_inputs.attack = [&players, &broadcaster, &active_listener](
                                const refrain::net::attack_event &event) {
        if (active_listener != nullptr) {
            broadcaster.attack(players, event);
        }
    };
    network_inputs.animation = [&players, &broadcaster,
                                &active_listener](std::uint64_t connection,
                                                  std::int8_t action) {
        if (active_listener != nullptr) {
            broadcaster.animation(players, connection, action);
        }
    };
    network_inputs.play_ready =
        [&players, &broadcaster,
         &active_listener](const refrain::net::player_identity &identity) {
            if (active_listener != nullptr) {
                broadcaster.play_ready(players, identity.connection);
            }
        };
    network_inputs.respawn = [&players,
                              &broadcaster](std::uint64_t connection) {
        broadcaster.respawn(players, connection);
    };
    network_inputs.verify_session = options.verify_session;
    network_inputs.seed = level->seed;
    network_inputs.spawn_x = static_cast<double>(level->spawn_x) + 0.5;
    network_inputs.spawn_y = static_cast<double>(level->spawn_y);
    network_inputs.spawn_z = static_cast<double>(level->spawn_z) + 0.5;
    network_inputs.registry_catalog_path =
        "content/vanilla/26.3/protocol_registry.json";
    // Apply one block mutation and publish it on the contract's event type.
    // The world records the previous state; forwarding the same triple keeps a
    // listener's view identical to the state the world committed.
    const auto apply_block = [&world](const refrain::sim::block_pos &pos,
                                      refrain::sim::block_state_id value) {
        const auto before{world.block_at(pos)};
        const auto applied{
            world.set_blocks(std::array<refrain::sim::block_change, 1>{
                refrain::sim::block_change{
                    .pos = pos, .old_state = before, .new_state = value}})};
        return applied.has_value();
    };
    // TODO: breaking ignores block hardness, the held tool, drops and
    // neighbour updates; every block becomes air immediately.
    network_inputs.break_block = [apply_block](std::int32_t x, std::int32_t y,
                                               std::int32_t z) {
        if (y < std::numeric_limits<std::int16_t>::min() ||
            y > std::numeric_limits<std::int16_t>::max()) {
            return false;
        }
        return apply_block(
            refrain::sim::block_pos{x, static_cast<std::int16_t>(y), z},
            refrain::sim::air_block);
    };
    // TODO: placing ignores the held item, placement rules and orientation;
    // every placement writes minecraft:stone.
    network_inputs.place_block = [&world, apply_block](std::int32_t x,
                                                       std::int32_t y,
                                                       std::int32_t z) {
        if (y < std::numeric_limits<std::int16_t>::min() ||
            y > std::numeric_limits<std::int16_t>::max()) {
            return false;
        }
        const auto stone{world.intern_block(std::string{"minecraft:stone"})};
        return apply_block(
            refrain::sim::block_pos{x, static_cast<std::int16_t>(y), z}, stone);
    };
    auto network{refrain::net::server::start(std::move(network_inputs),
                                             options.network)};
    std::unique_ptr<refrain::net::server> listener{};
    if (!network) {
        log_net.error("listener failed: {}", network.error().message);
    } else {
        listener = std::move(*network);
        active_listener = listener.get();
        gateway.reset(active_listener);
        command_environment->listener = listener.get();
        log_net.info("listening on {}:{}",
                     options.network.get_string("bind_host", "0.0.0.0"),
                     listener->local_port());
    }

    // Chunk streaming owns its state (per-connection viewports and the shared
    // encoded chunk cache) and is attached once the listener and the tables
    // exist.
    if (listener && tables && registries) {
        chunks.attach(gateway, refrain::server::chunk_packet_source{
                                   .world = &world,
                                   .tables = &*tables,
                                   .registries = &*registries,
                               });
    }

    refrain::server::inventory_sync inventory{
        gateway, tables ? &*tables : nullptr,
        registries ? &*registries : nullptr};

    maintenance_service maintenance{};
    if (auto started{maintenance.start(world, effective)}; !started) {
        return report_error(started.error());
    }

    const auto state{std::make_shared<runtime_state>()};
    state->heartbeat_ns.store(refrain::core::now_ns(),
                              std::memory_order_relaxed);

    tick_metrics metrics{};
    console_commands commands{console_dependencies{.world = &world,
                                                   .maintenance = &maintenance,
                                                   .listener = listener.get(),
                                                   .config = options.config,
                                                   .stop = state->stop.get()},
                              metrics};
    console_service console{[&commands](std::string_view line,
                                        const refrain::core::tick_info &tick) {
        commands.execute(line, tick);
    }};

    if (!options.command_file.empty() && !options.script_file.empty()) {
        return report_error(refrain::make_error(
            refrain::error_code::invalid_argument,
            "refrain-server: --cmd-file and --script are mutually exclusive"));
    }
    if (!options.script_file.empty()) {
        auto text{refrain::core::io::read_text(
            options.script_file,
            {.max_bytes = max_console_file_bytes, .allow_symlink = false})};
        if (!text) {
            return report_error(text.error());
        }
        auto parsed{refrain::sim::parse_replay_script(*text)};
        if (!parsed) {
            return report_error(parsed.error());
        }
        console.load_replay(std::move(*parsed));
    }
    if (!options.command_file.empty()) {
        auto text{refrain::core::io::read_text(
            options.command_file,
            {.max_bytes = max_console_file_bytes, .allow_symlink = false})};
        if (!text) {
            return report_error(text.error());
        }
        console.push_text(*text);
    } else if (options.script_file.empty()) {
        console.start_stdin(state->stop);
    }

    std::thread watchdog{};
    if (watchdog_timeout > 0) {
        watchdog = std::thread{[state, timeout = watchdog_timeout] {
            refrain::log::set_thread_name("watchdog");
            const auto timeout_ns{static_cast<std::int64_t>(timeout) *
                                  1'000'000LL};
            bool warned{false};
            while (!state->stop->load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds{250});
                const auto heartbeat{
                    state->heartbeat_ns.load(std::memory_order_relaxed)};
                const auto stalled{heartbeat != 0 &&
                                   refrain::core::now_ns() - heartbeat >
                                       timeout_ns};
                if (stalled && !warned) {
                    log_server.warn("stalled {}ms", timeout);
                    warned = true;
                } else if (!stalled && warned) {
                    log_server.info("recovered");
                    warned = false;
                }
            }
        }};
    }

    refrain::core::tick_clock clock{};
    clock.start();
    std::int64_t ticks_run{0};
    log_server.info("{} tps", refrain::core::ticks_per_second);

    while (!state->stop->load(std::memory_order_acquire)) {
        auto tick{clock.wait_next()};
        if (!tick) {
            log_server.error("{}", tick.error().message);
            break;
        }
        const auto tick_start_ns{refrain::core::now_ns()};
        state->heartbeat_ns.store(refrain::core::now_ns(),
                                  std::memory_order_relaxed);
        maintenance.collect_completed();
        console.tick(*tick, ticks_run, *state->stop);
        maintenance.pre_tick(ticks_run);
        if (listener) {
            listener->tick();
            broadcaster.tick(players);
        }
        chunks.tick(world, views, tick_start_ns);
        inventory.tick(players);
        world.tick();
        ++ticks_run;
        maintenance.post_tick(ticks_run);
        const auto work_ns{static_cast<std::uint64_t>(refrain::core::now_ns() -
                                                      tick_start_ns)};
        metrics.record(work_ns);
        chunks.observe_tick(work_ns);
        if (options.on_tick) {
            options.on_tick(static_cast<std::uint64_t>(ticks_run));
        }
        if (options.hash_every > 0 &&
            (ticks_run % static_cast<std::int64_t>(options.hash_every)) == 0) {
            log_sim.info("tick={} hash={}", ticks_run,
                         refrain::data::to_hex(world.state_hash()));
        }
        if (options.ticks > 0 && ticks_run >= options.ticks) {
            log_server.info("{}", ticks_run);
            state->stop->store(true, std::memory_order_release);
            break;
        }
        if ((ticks_run % 20) == 0) {
            log_server.debug(
                "tick {}: {}ms missed={}", tick->index,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    tick->actual)
                    .count(),
                tick->missed);
        }
    }

    state->stop->store(true, std::memory_order_release);
    if (watchdog.joinable()) {
        watchdog.join();
    }
    // Stop accepting connections before the world teardown so no handler can
    // observe a half-destroyed store.
    if (listener) {
        listener->stop();
        listener.reset();
    }
    // Shutdown order: the world writer first, then the content queue worker
    // (so no unlink overlaps store teardown), and only then the final flush.
    maintenance.stop();
    log_sim.info("final tick={} hash={}", world.ticks(),
                 refrain::data::to_hex(world.state_hash()));
    auto committed{world.flush()};
    if (!committed) {
        log_server.error("commit failed: {}", committed.error().message);
        return 1;
    }
    if (options.config != nullptr) {
        bool dirty{false};
        for (const auto &name : options.config->names()) {
            dirty = dirty || options.config->dirty(name);
        }
        if (dirty) {
            if (auto saved{options.config->save_all()}; !saved) {
                log_config.error("shutdown save failed: {}",
                                 saved.error().message);
            } else {
                log_config.info("saved");
            }
        }
    }
    log_server.info("{} ticks", ticks_run);
    return 0;
}

} // namespace refrain::server
