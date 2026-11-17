// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/contracts/services/sim.hpp>
#include <refrain/protocol/current.hpp>
#include <refrain/server/server_runtime.hpp>

#include <refrain/session/chunk_tracker.hpp>
#include <refrain/session/play_session.hpp>
#include <refrain/session/player_store.hpp>

#include <refrain/app/content_reload.hpp>
#include <refrain/app/mod_lifecycle.hpp>
#include <refrain/app/mod_management.hpp>
#include <refrain/app/services.hpp>
#include <refrain/core/io.hpp>
#include <refrain/core/log.hpp>
#include <refrain/core/time.hpp>
#include <refrain/data/vanilla.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/mod/rfmod.hpp>
#include <refrain/net/base.hpp>
#include <refrain/net/registry.hpp>
#include <refrain/net/server.hpp>
#include <refrain/server/chunk_generation.hpp>
#include <refrain/server/chunk_packet.hpp>
#include <refrain/server/chunk_service.hpp>
#include <refrain/server/command_bridge.hpp>
#include <refrain/server/console_commands.hpp>
#include <refrain/server/console_service.hpp>
#include <refrain/server/content_reload_inputs.hpp>
#include <refrain/server/entity_broadcaster.hpp>
#include <refrain/server/inventory_sync.hpp>
#include <refrain/server/maintenance_service.hpp>
#include <refrain/server/mod_lifecycle_inputs.hpp>
#include <refrain/server/mod_management_settings.hpp>
#include <refrain/server/play_gateway.hpp>
#include <refrain/server/player_store.hpp>
#include <refrain/server/world_bootstrap.hpp>
#include <refrain/server/world_preflight.hpp>
#include <refrain/sim/replay.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/store.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr refrain::log::logger log_config{"config"};
constexpr refrain::log::logger log_content{"content"};
constexpr refrain::log::logger log_mods{"mods"};
constexpr refrain::log::logger log_net{"net"};
constexpr refrain::log::logger log_permissions{"permissions"};
constexpr refrain::log::logger log_server{"server"};
constexpr refrain::log::logger log_sim{"sim"};
constexpr refrain::log::logger log_world{"world"};

} // namespace

namespace refrain::server {

using app::report_error;
namespace {
constexpr std::uint64_t max_console_file_bytes = 1ULL * 1024ULL * 1024ULL;

/// Human-readable death message. The wording follows the vanilla death keys
/// closely enough for the system chat and the death screen; a translation
/// layer owns localisation later.
[[nodiscard]] auto death_message(const refrain::sim::world &level,
                                 const refrain::sim::player_state &record)
    -> std::string {
    const auto name{record.name.empty() ? std::string{"A player"}
                                        : record.name};
    using source = refrain::sim::damage_source;
    switch (record.last_damage) {
    case source::fall:
        return name + " fell from a high place";
    case source::drown:
        return name + " drowned";
    case source::starve:
        return name + " starved to death";
    case source::out_of_world:
        return name + " fell out of the world";
    case source::in_wall:
        return name + " suffocated in a wall";
    case source::player_attack: {
        const auto *killer{
            record.last_attacker == 0
                ? nullptr
                : level.players().find_by_connection(record.last_attacker)};
        return killer == nullptr || killer->name.empty()
                   ? name + " was slain"
                   : name + " was slain by " + killer->name;
    }
    case source::mob_attack:
        return name + " was slain by a mob";
    case source::generic:
        break;
    }
    return name + " died";
}

/// Process-wide lifecycle flags shared with the console and watchdog threads.
struct runtime_state {
    /// The stdin thread outlives the runtime, so the flag is shared.
    std::shared_ptr<std::atomic<bool>> stop{
        std::make_shared<std::atomic<bool>>(false)};
    std::atomic<std::int64_t> heartbeat_ns{0};
};

/// Snapshot the running composition for the mod management service: identity,
/// loader, whether code runs, where it came from and which services it
/// currently provides.
[[nodiscard]] auto
loaded_composition_snapshot(const server_run_options &options)
    -> std::vector<refrain::app::mod_management_mod> {
    std::vector<refrain::app::mod_management_mod> snapshot{};
    if (options.mod_host == nullptr) {
        return snapshot;
    }
    const auto &host{*options.mod_host};
    const auto &services{host.services()};
    for (const auto &mod : host.mods()) {
        refrain::app::mod_management_mod entry{};
        entry.id = mod.id;
        entry.version = mod.version;
        entry.loader = mod.loader;
        entry.has_code = mod.has_code;
        entry.source = "builtin";
        if (!options.content_store.empty()) {
            auto receipts{refrain::mod::list_receipt_paths_for(
                options.content_store, mod.id)};
            if (receipts) {
                for (const auto &path : *receipts) {
                    auto receipt{refrain::mod::read_receipt(path)};
                    if (!receipt) {
                        continue;
                    }
                    if (receipt->version == mod.version) {
                        entry.source = "installed";
                        break;
                    }
                }
            }
        }
        for (const auto &service : services.active_services()) {
            for (const auto &instance : services.active_instances(service)) {
                const auto provider{
                    services.active_provider(service, instance)};
                if (provider && provider->id == mod.id) {
                    entry.services.push_back(service);
                    break;
                }
            }
        }
        snapshot.push_back(std::move(entry));
    }
    return snapshot;
}

} // namespace

auto run_server(const server_run_options &options,
                const server_service_handles &services, world_port &world)
    -> int {
    log_server.debug("using the {} world port",
                     is_first_party_world(services) ? "direct" : "contract");
    if (options.world_path.empty()) {
        return report_error(refrain::make_error(
            refrain::error_code::invalid_argument,
            "refrain-server: --world is required to start the tick loop"));
    }
    // Mod management configuration: the install/trust policy and the hot
    // lifecycle gates. A malformed file aborts the start instead of running
    // with a weaker policy than the operator wrote.
    auto management_settings{
        refrain::server::load_mod_management_settings(options.config_dir)};
    if (!management_settings) {
        return report_error(management_settings.error());
    }
    // The world is opened below; upgrade a save recorded with an older data
    // version first, or refuse it when no migration provider is active. A
    // current world is untouched by this call.
    if (auto prepared{refrain::server::prepare_world_open(
            options.world_path,
            refrain::server::world_migration_handle{
                .vtable = services.migration,
                .self = services.migration_self,
                .provider = services.migration_provider})};
        !prepared) {
        return report_error(prepared.error());
    }

    const auto level_name{options.world.get_string("level_name", "world")};
    const auto view_distance{static_cast<std::int32_t>(
        options.network.get_int64("view_distance", 10))};
    const auto simulation_distance{static_cast<std::int32_t>(
        options.network.get_int64("simulation_distance", 10))};
    const auto max_players{options.network.get_int64("status.max_players", 20)};
    const auto online_mode{options.network.get_bool("online_mode", true)};
    log_config.info(
        "configured level name '{}', view distance {}, simulation distance {}, "
        "{} max players, online mode {}",
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
    log_config.info(
        "using zstd level {}, watchdog timeout {}ms, autosave every {} ticks",
        zstd_level, watchdog_timeout, autosave_interval);

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
    // Resolved game data paths. The defaults keep a checkout usable for tests
    // and embedders that only set `world_path`; the composition root passes the
    // paths the first-run data directory actually resolved.
    const std::string data_root{options.data_dir.empty()
                                    ? std::string{"content/vanilla/26.3"}
                                    : options.data_dir};
    const std::string datapack_root{options.datapack_root.empty()
                                        ? data_root + "/data"
                                        : options.datapack_root};
    const std::string templates_root{options.templates_root.empty()
                                         ? datapack_root + "/../templates"
                                         : options.templates_root};
    const std::string tables_path{options.tables_path.empty()
                                      ? data_root + "/tables.rvtb"
                                      : options.tables_path};
    const std::string protocol_registry_path{
        options.protocol_registry_path.empty()
            ? data_root + "/protocol_registry.json"
            : options.protocol_registry_path};
    // The content tables are read before the first column loads: the light
    // engine derives every stored light array from them, so a table installed
    // after the spawn area was lit would leave those columns on the fallback.
    std::optional<refrain::data::vanilla_tables> tables{};
    auto table_bytes{refrain::core::io::read_file(tables_path)};
    if (!table_bytes) {
        log_net.warn("vanilla tables are unavailable: {}",
                     table_bytes.error().message);
    } else {
        auto opened{refrain::data::vanilla_tables::open(*table_bytes)};
        if (!opened) {
            log_net.warn("vanilla tables are invalid: {}",
                         opened.error().message);
        } else {
            tables = std::move(*opened);
        }
    }
    if (tables) {
        world.set_item_table(refrain::sim::item_table::from_tables(*tables));
        if (auto *sim_level{world.sim_world()}; sim_level != nullptr) {
            auto light_table{
                refrain::sim::block_light_table::from_tables(*tables)};
            if (light_table.empty()) {
                log_world.warn(
                    "vanilla tables carry no block light data: every block "
                    "state falls back to opaque and emits nothing");
            }
            sim_level->set_block_light_table(std::move(light_table));
        }
    }
    auto boot_projection{world.level()};
    if (!boot_projection) {
        return report_error(boot_projection.error());
    }
    if (!boot_projection->has_chunks) {
        // The store holds no chunk yet, so this is a fresh world: fill the
        // spawn area before the listener accepts joins.
        const auto seed{boot_projection->level.seed};
        const auto spawn{refrain::world::chunk_pos{0, 0}};
        auto generated{refrain::server::generate_spawn_area(
            world,
            refrain::server::world_bootstrap_config{
                .data_root = datapack_root,
                .templates_root = templates_root,
                .seed = seed,
                .radius = 3,
            },
            spawn)};
        if (!generated) {
            return report_error(generated.error());
        }
        log_world.info("generated {} spawn chunks", *generated);
    }
    // Columns the viewport needs but the store does not hold are generated on
    // a fixed worker pool, so walking past the spawn area keeps streaming
    // terrain. The credit level adapts until an operator pins it.
    refrain::server::chunk_service chunks{};
    refrain::server::chunk_generation::config generation_config{
        .data_root = datapack_root,
        .templates_root = templates_root,
        .seed = boot_projection->level.seed,
    };
    generation_config.admission.min_credits =
        static_cast<std::size_t>(std::max<std::int64_t>(
            options.runtime.get_int64("generation_min_credits", 1), 1));
    generation_config.admission.max_credits =
        static_cast<std::size_t>(std::max<std::int64_t>(
            options.runtime.get_int64("generation_max_credits", 8), 1));
    if (auto started{chunks.start_generation(generation_config)}; !started) {
        log_world.warn("chunk generation is unavailable: {}",
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
    auto projection{world.level()};
    if (!projection) {
        return report_error(projection.error());
    }
    log_world.info("opened world '{}' with data version {} and seed {}",
                   options.world_path, projection->level.data_version,
                   projection->level.seed);
    const auto initial_mode{
        projection->level.game_mode >= 0 && projection->level.game_mode <= 3
            ? static_cast<refrain::net::game_mode>(projection->level.game_mode)
            : refrain::net::game_mode::creative};
    world.seed(static_cast<std::uint64_t>(projection->level.seed));
    // Survival and physics state live in the simulation. The level record is
    // the authority for the spawn point, difficulty and hardcore flag; a
    // provider-owned simulation has no in-process world to configure and
    // keeps them itself.
    const auto spawn_x{static_cast<double>(projection->level.spawn_x) + 0.5};
    const auto spawn_y{static_cast<double>(projection->level.spawn_y)};
    const auto spawn_z{static_cast<double>(projection->level.spawn_z) + 0.5};
    if (auto *sim_level{world.sim_world()}; sim_level != nullptr) {
        sim_level->set_spawn_point(refrain::sim::world::spawn_point{
            .x = spawn_x,
            .y = spawn_y,
            .z = spawn_z,
            .yaw = 0.0F,
            .pitch = 0.0F,
        });
        sim_level->set_difficulty(
            projection->level.difficulty >= 0 &&
                    projection->level.difficulty <= 3
                ? static_cast<std::uint32_t>(projection->level.difficulty)
                : 2U);
        sim_level->set_hardcore(projection->level.hardcore);
    }
    // The console and replay scripts drive one synthetic player; it is created
    // before the listener starts, so it is always the first player by entity id
    // and commands that omit a name keep addressing it.
    auto console_player{
        world.add_player(refrain::sim::player_table::registration{
            .connection = refrain::sim::synthetic_connection,
            .name = std::string{"@console"},
            .world = std::string{level_name},
            .x = static_cast<double>(projection->level.spawn_x) + 0.5,
            .y = static_cast<double>(projection->level.spawn_y),
            .z = static_cast<double>(projection->level.spawn_z) + 0.5,
            .game_mode = static_cast<std::uint32_t>(initial_mode),
        })};
    if (!console_player) {
        return report_error(console_player.error());
    }

    // Ticket seeding: the spawn column plus its 3x3 neighbourhood. The
    // tick loop keeps them loaded; a missing chunk in a sparse imported world
    // is a warning, not a fatal startup error.
    const auto spawn_chunk{refrain::sim::chunk_pos{
        projection->level.spawn_x >= 0 ? projection->level.spawn_x / 16
                                       : (projection->level.spawn_x - 15) / 16,
        projection->level.spawn_z >= 0
            ? projection->level.spawn_z / 16
            : (projection->level.spawn_z - 15) / 16}};
    for (std::int32_t dx{-1}; dx <= 1; ++dx) {
        for (std::int32_t dz{-1}; dz <= 1; ++dz) {
            const refrain::sim::chunk_pos pos{spawn_chunk.x + dx,
                                              spawn_chunk.z + dz};
            auto ticketed{
                world.add_ticket(pos, refrain::sim::ticket_kind::spawn)};
            if (!ticketed) {
                log_server.warn(
                    "failed to add a spawn ticket for chunk {},{}: {}", pos.x,
                    pos.z, ticketed.error().message);
            }
        }
    }
    const auto tickets{world.ticket_count(spawn_chunk)};
    log_server.info("loaded {} chunks with {} spawn {}", world.loaded_chunks(),
                    tickets, tickets == 1 ? "ticket" : "tickets");
    // Residency baseline for an operator: the set starts at the pinned spawn
    // columns and is bounded from here by the viewports plus the ticket set.
    log_world.info("resident memory is {}MiB across {} chunks",
                   chunk_service::process_resident_bytes() /
                       (1024ULL * 1024ULL),
                   world.loaded_chunks());

    std::optional<refrain::net::registry_catalog> registries{};
    auto loaded_registries{
        refrain::net::registry_catalog::load(protocol_registry_path)};
    if (!loaded_registries) {
        log_net.warn("registry catalog is unavailable: {}",
                     loaded_registries.error().message);
    } else {
        registries = std::move(*loaded_registries);
    }
    // Hot mod lifecycle: the one service allowed to stage a change of the
    // running composition. It is built from the host the composition root
    // loaded and the catalog inputs that root resolved (content-store roots
    // and the effective auto-scan policy), so a staged catalog resolves
    // exactly like the next server start would. The staging half runs on the
    // command path; the publish is the tick-boundary commit at the top of the
    // loop below and performs no package or store IO.
    const auto lifecycle_inputs{refrain::server::take_mod_lifecycle_inputs()};
    std::optional<refrain::app::mod_lifecycle> mod_lifecycle_service{};
    std::string lifecycle_unavailable_reason{};
    if (options.mod_host == nullptr) {
        lifecycle_unavailable_reason =
            "the runtime was started without a loaded mod host, so the "
            "running composition cannot be changed";
    } else if (!options.mod_host->loaded()) {
        lifecycle_unavailable_reason =
            "the mod host has not loaded a composition, so the running "
            "composition cannot be changed";
    } else if (lifecycle_inputs.has_value() || !options.content_store.empty()) {
        refrain::app::mod_lifecycle_options lifecycle_options{};
        if (lifecycle_inputs.has_value()) {
            lifecycle_options.store_roots = lifecycle_inputs->store_roots;
            lifecycle_options.auto_scan = lifecycle_inputs->auto_scan;
        }
        lifecycle_options.store_root = options.content_store;
        if (lifecycle_options.store_roots.empty() &&
            !options.content_store.empty()) {
            // Embedders and tests that build the options directly do not go
            // through the composition-root handoff; the content store is then
            // the whole catalog.
            lifecycle_options.store_roots.push_back(options.content_store);
        }
        lifecycle_options.side = "server";
        // A staged change must keep every provider key the running
        // composition resolved: the tick loop and the other mods depend on
        // them, so a catalog that drops one is refused instead of staged.
        for (const auto &provider :
             options.mod_host->active_composition().providers) {
            lifecycle_options.required_services.push_back(provider.service);
        }
        lifecycle_options.discovery =
            refrain::mod::discover_options{.require_mods_dir = false};
        mod_lifecycle_service.emplace(*options.mod_host,
                                      std::move(lifecycle_options));
    } else {
        lifecycle_unavailable_reason =
            "the server was started without a mod content store, so the "
            "running composition cannot be re-resolved from installed "
            "receipts";
    }
    // Content view epoch: the composition root built and validated the view
    // the server starts with and published the layer list it was built from.
    // The service rebuilds the same stack off the tick, validates it and
    // publishes it at the tick boundary; `publish_content_view()` exposes the
    // live per-tick snapshot so consumers never hold a view across a tick.
    std::optional<refrain::app::content_reload> content_reload_service{};
    std::string content_reload_unavailable_reason{};
    if (auto content_inputs{refrain::server::take_content_reload_inputs()};
        content_inputs.has_value()) {
        refrain::app::content_reload_options reload_options{};
        reload_options.enabled = management_settings->hot_reload_data;
        reload_options.gate_source = management_settings->admin_source;
        auto sources{std::move(content_inputs->sources)};
        content_reload_service.emplace(
            refrain::app::content_view_snapshot{
                .generation = 1,
                .view = std::move(content_inputs->initial),
            },
            std::move(reload_options),
            [sources = std::move(sources)]()
                -> refrain::result<
                    std::shared_ptr<const refrain::data::content_view>> {
                auto built{refrain::data::content_view::build(sources)};
                if (!built) {
                    return std::unexpected{std::move(built.error())};
                }
                return std::make_shared<const refrain::data::content_view>(
                    std::move(*built));
            });
        refrain::server::publish_content_view(
            content_reload_service->snapshot());
    } else {
        content_reload_unavailable_reason =
            "the composition root did not publish the content layers this "
            "server reads";
    }
    // One mod management service behind the console `mods ...` command and
    // the in-game `/mods` tree, so both frontends report the same results.
    // The hot lifecycle and content reload ports are the only places that may
    // change the running composition or the content view; the command layer
    // keeps its gates, durable store transactions and audit.
    refrain::app::mod_management_context management{};
    management.store = options.content_store;
    management.policy = management_settings->policy;
    management.policy_source = management_settings->policy_source;
    management.admin_source = management_settings->admin_source;
    management.hot_install = management_settings->hot_install;
    management.hot_reload = management_settings->hot_reload;
    management.hot_reload_data = management_settings->hot_reload_data;
    management.loaded = loaded_composition_snapshot(options);
    if (mod_lifecycle_service) {
        management.lifecycle =
            refrain::app::make_mod_lifecycle_port(*mod_lifecycle_service);
    } else {
        management.lifecycle.unavailable_reason = lifecycle_unavailable_reason;
    }
    if (content_reload_service) {
        management.content_reload =
            refrain::app::make_content_reload_port(*content_reload_service);
    } else {
        management.content_reload.unavailable_reason =
            content_reload_unavailable_reason;
    }
    refrain::app::mod_management mods_management{std::move(management)};
    log_mods.info(
        "mod management is available with hot install {}, hot reload {}, "
        "targeting {}; the in-game command requires the 'refrain.mods.admin' "
        "permission node",
        management_settings->hot_install ? "on" : "off",
        management_settings->hot_reload ? "on" : "off",
        options.content_store.empty() ? std::string{"no content store"}
                                      : options.content_store);
    if (mod_lifecycle_service) {
        log_mods.info(
            "the hot lifecycle is wired to {} catalog root(s) with auto-scan "
            "{}",
            mod_lifecycle_service->options().store_roots.size(),
            mod_lifecycle_service->options().auto_scan.value_or(
                options.mod_host->active_composition().spec.auto_scan)
                ? "on"
                : "off");
    } else {
        log_mods.info("the hot lifecycle is unavailable: {}",
                      lifecycle_unavailable_reason);
    }
    if (content_reload_service) {
        const auto live{content_reload_service->snapshot()};
        log_content.info(
            "content reload is {} with generation {} live over {} layer(s)",
            management_settings->hot_reload_data ? "enabled" : "disabled",
            live.generation,
            live.view ? live.view->layer_count() : std::size_t{0});
    } else {
        log_content.info("content reload is unavailable: {}",
                         content_reload_unavailable_reason);
    }

    auto command_environment{
        std::make_shared<refrain::server::command_environment>()};
    command_environment->world = &world;
    command_environment->default_mode = initial_mode;
    command_environment->mods = &mods_management;
    // The permission service is authoritative when the composition provides
    // it: the command filter, the explicit denial path and the tree cache all
    // read through this binding. Without a provider the command nodes fall
    // back to their declared op levels.
    if (services.permissions != nullptr &&
        services.permissions_self != nullptr) {
        refrain::server::bind_permissions(command_environment,
                                          *services.permissions,
                                          services.permissions_self);
        log_permissions.info(
            "command permissions are served by {}, policy revision {}",
            services.permissions_provider.empty()
                ? std::string{"the built-in policy"}
                : services.permissions_provider,
            command_environment->permission_revision());
    } else {
        log_permissions.info(
            "no permission provider is active; commands fall back to their "
            "declared op levels");
    }
    const auto initial_permission_revision{
        command_environment->permission_revision
            ? command_environment->permission_revision()
            : 0U};
    auto published_permission_revision{initial_permission_revision};
    // The first-party port owns the simulation table in this process, so the
    // session forwards to it without a copy. A contract port owns no such
    // table: the provider keeps it and hands the runtime a projection, so the
    // session reads a store that refreshes that projection once per tick.
    std::unique_ptr<refrain::session::player_store> player_store{};
    if (auto *table = world.players(); table != nullptr) {
        player_store =
            std::make_unique<refrain::session::table_player_store>(*table);
    } else {
        player_store = refrain::server::make_world_port_player_store(world);
    }
    refrain::session::play_session players{*player_store};
    // Read the projection once before the listener starts: a provider without
    // a player table fails the boot here, naming the operation, instead of
    // ticking over an empty view.
    if (auto refreshed{players.refresh()}; !refreshed) {
        return report_error(refreshed.error());
    }
    refrain::session::chunk_tracker views{};
    const auto tracking_chunks{static_cast<std::int32_t>(std::max<std::int64_t>(
        1, options.network.get_int64("entity_tracking_range", 512) / 16))};
    using broadcast_budgets = refrain::server::entity_broadcaster::budgets;
    constexpr broadcast_budgets default_broadcast_budgets{};
    const auto broadcast_limits{broadcast_budgets::from_config(
        options.network.get_int64(
            "broadcast.info_bytes_per_tick",
            static_cast<std::int64_t>(
                default_broadcast_budgets.max_info_bytes_per_tick)),
        options.network.get_int64(
            "broadcast.queued_info_bytes",
            static_cast<std::int64_t>(
                default_broadcast_budgets.max_queued_info_bytes)),
        options.network.get_int64(
            "broadcast.visibility_pairs_per_tick",
            static_cast<std::int64_t>(
                default_broadcast_budgets.max_visibility_pairs_per_tick)),
        options.network.get_int64(
            "broadcast.spawns_per_tick",
            static_cast<std::int64_t>(
                default_broadcast_budgets.max_spawns_per_tick)))};
    log_config.info(
        "broadcast is limited to {} info bytes per tick, {} queued info bytes, "
        "{} visibility pairs per tick and {} spawns per tick",
        broadcast_limits.max_info_bytes_per_tick,
        broadcast_limits.max_queued_info_bytes,
        broadcast_limits.max_visibility_pairs_per_tick,
        broadcast_limits.max_spawns_per_tick);
    refrain::server::server_gateway gateway{};
    refrain::server::entity_broadcaster broadcaster{gateway, tracking_chunks,
                                                    broadcast_limits};
    /// Last survival values sent to each connection, keyed by connection so
    /// the emission order is deterministic. The health, food and experience
    /// packets are only sent when a value changed, which is why the runtime
    /// keeps this mirror instead of resending every tick.
    struct survival_sent {
        float health{20.0F};
        std::int32_t food{20};
        float saturation{5.0F};
        float progress{0.0F};
        std::int32_t level{0};
        std::int32_t total{0};
        bool death_sent{false};
    };
    std::map<std::uint64_t, survival_sent> survival_sent{};
    broadcaster.set_damage_handler(
        [&world](std::uint64_t attacker, std::uint64_t target, float damage) {
            auto *sim_level{world.sim_world()};
            if (sim_level == nullptr) {
                return false;
            }
            auto *record{sim_level->players().find_by_connection(target)};
            if (record == nullptr || record->dead) {
                return false;
            }
            return sim_level->damage_player(
                *record, sim::damage_source::player_attack, damage, attacker);
        });
    refrain::net::server *active_listener{nullptr};
    const auto external_join{options.player_join};
    const auto external_leave{options.player_leave};
    refrain::net::server_inputs network_inputs{};
    network_inputs.commands =
        refrain::server::make_command_bridge(command_environment);
    network_inputs.policy.hardcore = projection->level.hardcore;
    network_inputs.policy.difficulty =
        projection->level.difficulty >= 0 && projection->level.difficulty <= 3
            ? static_cast<std::uint32_t>(projection->level.difficulty)
            : 2U;
    network_inputs.policy.difficulty_locked =
        projection->level.difficulty_locked;
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
    const auto spawn_chunk_x{
        static_cast<std::int32_t>(projection->level.spawn_x / 16)};
    const auto spawn_chunk_z{
        static_cast<std::int32_t>(projection->level.spawn_z / 16)};
    // The network shell queues this event from its I/O thread and delivers it
    // from `listener->tick()`, so the session table stays simulation-owned.
    network_inputs.player_join =
        [&players, &views, &broadcaster, &command_environment, external_join,
         spawn_chunk_x, spawn_chunk_z, spawn_x, spawn_y, spawn_z,
         view_distance](const refrain::net::player_identity &identity) {
            const auto mode{
                command_environment->join_state(identity.connection).mode};
            const auto added{players.add_player(
                identity.connection, identity.id, std::string{identity.name},
                std::string{identity.world}, spawn_x, spawn_y, spawn_z,
                static_cast<std::uint32_t>(mode))};
            if (added) {
                static_cast<void>(views.update(identity.connection,
                                               spawn_chunk_x, spawn_chunk_z,
                                               view_distance));
                broadcaster.joined(identity.connection);
            }
            if (external_join) {
                external_join(identity);
            }
        };
    network_inputs.player_leave =
        [&players, &views, &broadcaster, &survival_sent,
         external_leave](const refrain::net::player_identity &identity) {
            const auto leaving{players.player_of(identity.connection)};
            const auto copy{
                leaving == nullptr
                    ? std::nullopt
                    : std::optional<refrain::session::player>{*leaving}};
            static_cast<void>(players.remove_player(identity.connection));
            static_cast<void>(views.remove(identity.connection));
            survival_sent.erase(identity.connection);
            if (copy) {
                broadcaster.left(*copy);
            }
            if (external_leave) {
                external_leave(identity);
            }
        };
    network_inputs.movement = [&players, &views, view_distance, &world,
                               &active_listener](
                                  const refrain::net::movement_event &event) {
        const auto update_view =
            [&views, view_distance](std::uint64_t connection,
                                    const sim::player_state &record) {
                static_cast<void>(views.update(
                    connection,
                    static_cast<std::int32_t>(std::floor(record.x / 16.0)),
                    static_cast<std::int32_t>(std::floor(record.z / 16.0)),
                    std::min(record.view_distance, view_distance)));
            };
        if (auto *sim_level{world.sim_world()}; sim_level != nullptr) {
            auto *record{
                sim_level->players().find_by_connection(event.connection)};
            if (record == nullptr) {
                return;
            }
            const auto verdict{sim_level->submit_player_move(
                *record, sim::player_move_request{
                             .x = event.x,
                             .y = event.y,
                             .z = event.z,
                             .yaw = event.yaw,
                             .pitch = event.pitch,
                             .on_ground = event.on_ground,
                         })};
            if (verdict == sim::player_move_verdict::corrected &&
                active_listener != nullptr) {
                // The client reported a position the world does not
                // allow. Send the authoritative position and keep the
                // teleport confirmation handshake, so the client's next
                // packets are not validated against the stale position.
                active_listener->teleport_player(event.connection, record->x,
                                                 record->y, record->z,
                                                 record->yaw, record->pitch);
            }
            update_view(event.connection, *record);
            return;
        }
        // A provider-owned simulation decides movement itself; the
        // runtime only applies the projection so visibility and chunk
        // streaming keep following the player.
        static_cast<void>(players.update_position(event.connection, event.x,
                                                  event.y, event.z, event.yaw,
                                                  event.pitch));
        if (const auto *player{players.player_of(event.connection)};
            player != nullptr) {
            static_cast<void>(views.update(
                event.connection,
                static_cast<std::int32_t>(std::floor(player->x / 16.0)),
                static_cast<std::int32_t>(std::floor(player->z / 16.0)),
                std::min(player->view_distance, view_distance)));
        }
    };
    network_inputs.input =
        [&world](const refrain::net::player_input_event &event) {
            auto *sim_level{world.sim_world()};
            if (sim_level == nullptr) {
                return;
            }
            if (auto *record{
                    sim_level->players().find_by_connection(event.connection)};
                record != nullptr) {
                record->input = static_cast<sim::player_input>(event.flags);
            }
        };
    network_inputs.flight = [&world, &active_listener](
                                const refrain::net::flight_event &event) {
        auto *sim_level{world.sim_world()};
        if (sim_level == nullptr) {
            return;
        }
        auto *record{sim_level->players().find_by_connection(event.connection)};
        if (record == nullptr) {
            return;
        }
        const auto may_fly{record->game_mode == sim::game_mode_creative ||
                           record->game_mode == sim::game_mode_spectator};
        if (event.flying && may_fly) {
            record->flying = true;
        } else if (!event.flying) {
            record->flying = false;
        } else if (active_listener != nullptr) {
            // A survival client asked to fly: re-send the abilities for
            // its mode, which also clears the client-side flight flag.
            active_listener->set_player_game_mode(
                event.connection,
                static_cast<refrain::net::game_mode>(record->game_mode));
        }
    };
    network_inputs.teleport_confirm =
        [&players, &views, view_distance,
         &world](const refrain::net::teleport_confirm_event &event) {
            if (auto *sim_level{world.sim_world()}; sim_level != nullptr) {
                if (auto *record{sim_level->players().find_by_connection(
                        event.connection)};
                    record != nullptr) {
                    record->x = event.x;
                    record->y = event.y;
                    record->z = event.z;
                    record->yaw = event.yaw;
                    record->pitch = event.pitch;
                    record->vx = 0.0;
                    record->vy = 0.0;
                    record->vz = 0.0;
                    record->on_ground = false;
                    record->has_move_request = false;
                    record->fall_distance = 0.0;
                    record->ground_y = event.y;
                    static_cast<void>(views.update(
                        event.connection,
                        static_cast<std::int32_t>(std::floor(record->x / 16.0)),
                        static_cast<std::int32_t>(std::floor(record->z / 16.0)),
                        std::min(record->view_distance, view_distance)));
                }
                return;
            }
            static_cast<void>(players.update_position(event.connection, event.x,
                                                      event.y, event.z,
                                                      event.yaw, event.pitch));
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
    network_inputs.attack = [&players, &broadcaster, &active_listener,
                             &world](const refrain::net::attack_event &event) {
        if (active_listener == nullptr) {
            return;
        }
        if (auto *sim_level{world.sim_world()}; sim_level != nullptr) {
            const auto *attacker{
                sim_level->players().find_by_connection(event.connection)};
            const auto *target{sim_level->players().find(
                static_cast<sim::entity_id>(event.target_entity))};
            if (attacker == nullptr || target == nullptr ||
                target->id == attacker->id || target->dead ||
                attacker->game_mode == sim::game_mode_spectator) {
                return;
            }
            // Vanilla's player attack reach is three blocks from the eye.
            const auto dx{target->x - attacker->x};
            const auto dy{target->y + 0.9 - (attacker->y + 1.62)};
            const auto dz{target->z - attacker->z};
            if (dx * dx + dy * dy + dz * dz > 9.0) {
                return;
            }
        }
        broadcaster.attack(players, event);
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
    network_inputs.respawn_request = [&world](std::uint64_t connection) {
        auto *sim_level{world.sim_world()};
        if (sim_level == nullptr) {
            // A provider-owned simulation keeps the death state; the runtime
            // cannot verify it, so the request is left to the provider path.
            return true;
        }
        const auto *record{sim_level->players().find_by_connection(connection)};
        return record != nullptr && record->dead;
    };
    network_inputs.respawn = [&players, &broadcaster, &world, spawn_x, spawn_y,
                              spawn_z](std::uint64_t connection) {
        if (auto *sim_level{world.sim_world()}; sim_level != nullptr) {
            if (auto *record{
                    sim_level->players().find_by_connection(connection)};
                record != nullptr) {
                sim_level->respawn_player(*record, spawn_x, spawn_y, spawn_z,
                                          0.0F, 0.0F);
            }
        }
        broadcaster.respawn(players, connection);
    };
    network_inputs.verify_session = options.verify_session;
    network_inputs.seed = projection->level.seed;
    network_inputs.spawn_x =
        static_cast<double>(projection->level.spawn_x) + 0.5;
    network_inputs.spawn_y = static_cast<double>(projection->level.spawn_y);
    network_inputs.spawn_z =
        static_cast<double>(projection->level.spawn_z) + 0.5;
    network_inputs.registry_catalog_path = protocol_registry_path;
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
    // The game-mode rules and the interaction reach are enforced before the
    // world changes: adventure and spectator cannot edit, and an edit must be
    // close to the player's eye. Survival break progress, tool tiers and drops
    // are not modelled yet.
    // TODO: breaking ignores block hardness, the held tool, break progress and
    // neighbour updates; every block becomes air immediately.
    const auto may_edit = [&world](std::uint64_t connection, std::int32_t x,
                                   std::int32_t y, std::int32_t z) {
        auto *sim_level{world.sim_world()};
        if (sim_level == nullptr) {
            return true;
        }
        const auto *record{sim_level->players().find_by_connection(connection)};
        if (record == nullptr || record->dead) {
            return false;
        }
        if (record->game_mode == sim::game_mode_spectator ||
            record->game_mode == sim::game_mode_adventure) {
            return false;
        }
        const auto reach{record->game_mode == sim::game_mode_creative ? 6.0
                                                                      : 5.0};
        const auto dx{static_cast<double>(x) + 0.5 - record->x};
        const auto dy{static_cast<double>(y) + 0.5 - (record->y + 1.62)};
        const auto dz{static_cast<double>(z) + 0.5 - record->z};
        return dx * dx + dy * dy + dz * dz <= reach * reach;
    };
    network_inputs.break_block =
        [apply_block, may_edit](std::uint64_t connection, std::int32_t x,
                                std::int32_t y, std::int32_t z) {
            if (y < std::numeric_limits<std::int16_t>::min() ||
                y > std::numeric_limits<std::int16_t>::max() ||
                !may_edit(connection, x, y, z)) {
                return false;
            }
            return apply_block(
                refrain::sim::block_pos{x, static_cast<std::int16_t>(y), z},
                refrain::sim::air_block);
        };
    // TODO: placing ignores the held item, placement rules and orientation;
    // every placement writes minecraft:stone.
    network_inputs.place_block = [&world, apply_block, may_edit](
                                     std::uint64_t connection, std::int32_t x,
                                     std::int32_t y, std::int32_t z) {
        if (y < std::numeric_limits<std::int16_t>::min() ||
            y > std::numeric_limits<std::int16_t>::max() ||
            !may_edit(connection, x, y, z)) {
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
        log_net.error("failed to start listening on {}:{}: {}",
                      options.network.get_string("bind_host", "0.0.0.0"),
                      options.network.get_int64("port", 25565),
                      network.error().message);
    } else {
        listener = std::move(*network);
        active_listener = listener.get();
        gateway.reset(active_listener);
        command_environment->listener = listener.get();
        log_net.info("listening on {}:{}",
                     options.network.get_string("bind_host", "0.0.0.0"),
                     listener->local_port());
        log_server.info(
            "ready for Minecraft {} clients, type help for console commands "
            "and stop to shut down",
            options.game_version);
    }

    // Chunk streaming owns its state (per-connection viewports and the shared
    // encoded chunk cache) and is attached once the listener and the tables
    // exist.
    if (listener && tables && registries) {
        using chunk_backlog = refrain::server::chunk_streamer::backlog_limits;
        constexpr chunk_backlog default_chunk_backlog{};
        const auto chunk_limits{chunk_backlog::from_config(
            options.network.get_int64(
                "chunk_stream.max_connection_backlog",
                static_cast<std::int64_t>(
                    default_chunk_backlog.max_connection_bytes)),
            options.network.get_int64(
                "chunk_stream.max_total_backlog",
                static_cast<std::int64_t>(
                    default_chunk_backlog.max_total_bytes)))};
        log_config.info(
            "chunk stream backlog is limited to {} bytes per connection and {} "
            "bytes in total",
            chunk_limits.max_connection_bytes, chunk_limits.max_total_bytes);
        chunks.attach(gateway,
                      refrain::server::chunk_packet_source{
                          .world = &world,
                          .tables = &*tables,
                          .registries = &*registries,
                      },
                      chunk_limits);
    }

    // Generated entities follow the column that carries them: the streamer
    // announces them with the normal entity packets when the column is sent and
    // removes them when it leaves the viewport. A port whose provider keeps its
    // columns in another process has no in-process world and therefore no
    // entity records to announce.
    if (auto *sim_level{world.sim_world()}; sim_level != nullptr) {
        chunks.set_entity_source([sim_level](refrain::sim::chunk_pos pos) {
            return sim_level->chunk_entities(pos);
        });
    }

    // Publish the world clock and weather the login burst reads. The clock
    // lives in the simulation thread; the listener copies the values under its
    // own lock.
    net::world_state_snapshot published_world_state{};
    bool have_published_weather{false};
    const auto publish_world_state = [&listener, &world, &published_world_state,
                                      &have_published_weather]() {
        if (!listener) {
            return;
        }
        const auto *clock{world.clock()};
        if (clock == nullptr) {
            return;
        }
        const auto snapshot{clock->snapshot()};
        const auto weather{clock->weather()};
        listener->set_world_state(net::world_state_snapshot{
            .game_time = snapshot.game_time,
            .clock_total_ticks = snapshot.total_ticks,
            .clock_partial_tick = snapshot.partial_tick,
            .clock_rate = snapshot.paused ? 0.0F : snapshot.rate,
            .raining = weather.raining,
            .rain_level = weather.rain_level,
            .thunder_level = weather.thunder_level,
        });
        if (have_published_weather) {
            if (published_world_state.raining != weather.raining) {
                listener->broadcast_game_event(weather.raining ? 1 : 2, 0.0F);
            }
            if (published_world_state.rain_level != weather.rain_level) {
                listener->broadcast_game_event(7, weather.rain_level);
            }
            if (published_world_state.thunder_level != weather.thunder_level) {
                listener->broadcast_game_event(8, weather.thunder_level);
            }
        }
        published_world_state.raining = weather.raining;
        published_world_state.rain_level = weather.rain_level;
        published_world_state.thunder_level = weather.thunder_level;
        have_published_weather = true;
    };
    publish_world_state();

    refrain::server::inventory_sync inventory{
        gateway, tables ? &*tables : nullptr,
        registries ? &*registries : nullptr};

    maintenance_service maintenance{};
    if (auto started{maintenance.start(world, effective)}; !started) {
        return report_error(started.error());
    }
    // A residency unload writes a dirty column synchronously. Wait for the
    // autosave writer to go idle first, so an older snapshot of the same
    // column cannot land on disk after the newer unload flush.
    chunks.set_unload_barrier([&maintenance] { maintenance.drain(); });

    const auto state{std::make_shared<runtime_state>()};
    state->heartbeat_ns.store(refrain::core::now_ns(),
                              std::memory_order_relaxed);

    tick_metrics metrics{};
    console_commands commands{console_dependencies{.world = &world,
                                                   .maintenance = &maintenance,
                                                   .listener = listener.get(),
                                                   .config = options.config,
                                                   .stop = state->stop.get(),
                                                   .mods = &mods_management},
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
                    log_server.warn("tick stalled for {}ms", timeout);
                    warned = true;
                } else if (!stalled && warned) {
                    log_server.info("tick recovered");
                    warned = false;
                }
            }
        }};
    }

    refrain::core::tick_clock clock{};
    clock.start();
    std::int64_t ticks_run{0};
    bool player_projection_failed{false};
    bool provider_validation_failed{false};
    log_server.info("target tick rate is {} tps",
                    refrain::core::ticks_per_second);

    while (!state->stop->load(std::memory_order_acquire)) {
        auto tick{clock.wait_next()};
        if (!tick) {
            log_server.error("{}", tick.error().message);
            break;
        }
        const auto tick_start_ns{refrain::core::now_ns()};
        state->heartbeat_ns.store(refrain::core::now_ns(),
                                  std::memory_order_relaxed);
        // Publish a staged mod composition change at the tick boundary before
        // anything in this tick can call a provider: the registry switch is
        // atomic, retired generations are drained outside the tick and the
        // loaders' unload hooks run here. No package or store IO happens in
        // the tick, that all happened while the change was staged.
        if (auto committed{refrain::app::commit_staged_mod_changes()};
            !committed) {
            log_server.error(
                "failed to commit a staged mod composition change: {}",
                committed.error().message);
        }
        // The world/sim/storage providers are restart-required: the lifecycle
        // refuses to replace them while the loop holds their live state, so a
        // changed generation means the cached tables no longer belong to the
        // committed composition. Re-querying cannot hand over the live world,
        // so stop instead of calling a provider the lifecycle retired.
        if (options.mod_host != nullptr) {
            if (auto valid{refrain::server::validate_cached_providers(
                    services, options.mod_host->services())};
                !valid) {
                log_server.error("{}", valid.error().message);
                provider_validation_failed = true;
                state->stop->store(true, std::memory_order_release);
                break;
            }
        }
        // Publish a finished content view rebuild at the same boundary. The
        // commit only swaps a shared pointer: the pack IO and the validation
        // happened on the builder thread, so a slow layer never blocks the
        // tick. A failed or registry-changing rebuild is dropped and the live
        // generation stays in place; the service logs the outcome.
        if (content_reload_service) {
            const auto committed{content_reload_service->commit()};
            if (committed.published) {
                refrain::server::publish_content_view(
                    content_reload_service->snapshot());
            }
        }
        // The provider ticks its own player table at the end of the loop body,
        // so the projection is re-read here, before this tick's commands and
        // packets read it. A failed read is fatal: every player view below
        // would be stale.
        if (auto refreshed{players.refresh()}; !refreshed) {
            log_server.error("failed to read the player table: {}",
                             refreshed.error().message);
            player_projection_failed = true;
            state->stop->store(true, std::memory_order_release);
            break;
        }
        maintenance.collect_completed();
        console.tick(*tick, ticks_run, *state->stop);
        maintenance.pre_tick(ticks_run);
        if (listener) {
            listener->tick();
            broadcaster.tick(players);
        }
        chunks.tick(world, players, views, tick_start_ns);
        chunks.broadcast_updates(world);
        inventory.tick(players);
        world.tick();
        // Survival packets follow the simulation tick: death, health, hunger
        // and experience are simulation state and the client only sees them
        // through these updates.
        if (auto *sim_level{world.sim_world()}; sim_level != nullptr) {
            sim_level->players().for_each(
                [&sim_level, &survival_sent,
                 &active_listener](refrain::sim::player_state &record) {
                    if (record.connection ==
                        refrain::sim::synthetic_connection) {
                        return;
                    }
                    auto &sent{survival_sent[record.connection]};
                    if (record.dead) {
                        if (sent.death_sent) {
                            return;
                        }
                        sent.death_sent = true;
                        // Kill experience goes to the player that dealt the
                        // final hit, capped like the vanilla player reward.
                        if (record.last_attacker != 0 &&
                            record.last_attacker != record.connection) {
                            auto *killer{
                                sim_level->players().find_by_connection(
                                    record.last_attacker)};
                            if (killer != nullptr && !killer->dead) {
                                refrain::sim::add_experience(
                                    *killer, std::min(sent.level * 7, 100));
                            }
                        }
                        const auto message{death_message(*sim_level, record)};
                        if (active_listener != nullptr) {
                            active_listener->send_player_combat_kill(
                                record.connection,
                                static_cast<std::int32_t>(record.id), message);
                            active_listener->broadcast_system_chat(message);
                            for (const auto &[other_id, other] :
                                 sim_level->players().records()) {
                                if (other.connection !=
                                        refrain::sim::synthetic_connection &&
                                    other.connection != record.connection) {
                                    active_listener->send_entity_event(
                                        other.connection,
                                        static_cast<std::int32_t>(record.id),
                                        3); // death animation
                                }
                            }
                        }
                        return;
                    }
                    const auto respawned{sent.death_sent};
                    sent.death_sent = false;
                    if (respawned || record.health != sent.health ||
                        record.food_level != sent.food ||
                        record.saturation != sent.saturation) {
                        if (active_listener != nullptr) {
                            active_listener->send_player_health(
                                record.connection, record.health,
                                record.food_level, record.saturation);
                        }
                        sent.health = record.health;
                        sent.food = record.food_level;
                        sent.saturation = record.saturation;
                    }
                    if (respawned || record.experience_level != sent.level ||
                        record.total_experience != sent.total ||
                        record.experience_progress != sent.progress) {
                        if (active_listener != nullptr) {
                            active_listener->send_player_experience(
                                record.connection, record.experience_progress,
                                record.experience_level,
                                record.total_experience);
                        }
                        sent.progress = record.experience_progress;
                        sent.level = record.experience_level;
                        sent.total = record.total_experience;
                    }
                });
        }
        publish_world_state();
        // The reference re-synchronises the world clock once per second; the
        // client interpolates the clock in between from the game time it
        // receives with every sync.
        if (ticks_run % 20 == 0 && listener) {
            listener->broadcast_world_time();
        }
        ++ticks_run;
        maintenance.post_tick(ticks_run);
        const auto work_ns{static_cast<std::uint64_t>(refrain::core::now_ns() -
                                                      tick_start_ns)};
        metrics.record(work_ns);
        chunks.observe_tick(work_ns);
        if (options.on_tick) {
            options.on_tick(static_cast<std::uint64_t>(ticks_run));
        }
        // A permission policy change invalidates every cached command tree:
        // rebuild and resend them so a new grant or revoke takes effect in
        // play without a reconnect.
        if (command_environment->permission_revision) {
            const auto policy_revision{
                command_environment->permission_revision()};
            if (policy_revision != published_permission_revision) {
                published_permission_revision = policy_revision;
                if (listener) {
                    const auto player_count{
                        listener->active_connections().size()};
                    listener->resend_commands();
                    log_permissions.info(
                        "permission policy revision changed to {}, resent the "
                        "command tree to {} players",
                        policy_revision, player_count);
                }
            }
        }
        if (options.hash_every > 0 &&
            (ticks_run % static_cast<std::int64_t>(options.hash_every)) == 0) {
            log_sim.info("tick {} state hash is {}", ticks_run,
                         refrain::data::to_hex(world.state_hash()));
        }
        if (options.ticks > 0 && ticks_run >= options.ticks) {
            state->stop->store(true, std::memory_order_release);
            break;
        }
        if ((ticks_run % 20) == 0) {
            log_server.debug(
                "tick {} took {}ms and missed {} ticks", tick->index,
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
    log_sim.info("final state hash at tick {} is {}", world.ticks(),
                 refrain::data::to_hex(world.state_hash()));
    // Residency at the end of the run: the resident set follows the viewports,
    // not the distance the players have walked, and the unload count is what
    // the policy actually released.
    const auto residency{chunks.residency()};
    log_world.info(
        "stopped world: {} resident chunks, peak {}, {} unloaded, resident "
        "memory {}MiB",
        residency.resident_chunks, residency.peak_resident_chunks,
        residency.unloaded_chunks,
        chunk_service::process_resident_bytes() / (1024ULL * 1024ULL));
    auto committed{world.flush()};
    if (!committed) {
        log_server.error("failed to commit the world: {}",
                         committed.error().message);
        return 1;
    }
    if (options.config != nullptr) {
        bool dirty{false};
        for (const auto &name : options.config->names()) {
            dirty = dirty || options.config->dirty(name);
        }
        if (dirty) {
            if (auto saved{options.config->save_all()}; !saved) {
                log_config.error(
                    "failed to save the configuration at shutdown: {}",
                    saved.error().message);
            } else {
                log_config.info("saved all dirty namespaces");
            }
        }
    }
    log_server.info("ran {} ticks", ticks_run);
    return player_projection_failed || provider_validation_failed ? 1 : 0;
}

} // namespace refrain::server
