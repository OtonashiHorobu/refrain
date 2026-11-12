// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/protocol/current.hpp>
#include <refrain/server/server_runtime.hpp>

#include <refrain/session/chunk_tracker.hpp>
#include <refrain/session/play_session.hpp>

#include <refrain/app/services.hpp>
#include <refrain/core/io.hpp>
#include <refrain/core/log.hpp>
#include <refrain/core/schedule.hpp>
#include <refrain/core/time.hpp>
#include <refrain/data/vanilla.hpp>
#include <refrain/net/registry.hpp>
#include <refrain/net/server.hpp>
#include <refrain/server/chunk_generation.hpp>
#include <refrain/server/chunk_packet.hpp>
#include <refrain/server/chunk_streamer.hpp>
#include <refrain/server/chunk_view.hpp>
#include <refrain/server/command_bridge.hpp>
#include <refrain/server/entity_broadcaster.hpp>
#include <refrain/server/inventory_sync.hpp>
#include <refrain/server/maintenance_service.hpp>
#include <refrain/server/play_gateway.hpp>
#include <refrain/server/world_bootstrap.hpp>
#include <refrain/sim/replay.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/store.hpp>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace {

constexpr refrain::log::logger log_config{"config"};
constexpr refrain::log::logger log_content{"content"};
constexpr refrain::log::logger log_net{"net"};
constexpr refrain::log::logger log_schedule{"schedule"};
constexpr refrain::log::logger log_server{"server"};
constexpr refrain::log::logger log_sim{"sim"};
constexpr refrain::log::logger log_storage{"storage"};
constexpr refrain::log::logger log_world{"world"};
constexpr refrain::log::logger log_chunk_gen{"chunk-gen"};

} // namespace

namespace refrain::server {

using app::report_error;
namespace {

constexpr std::size_t max_console_commands = 1024;
constexpr std::uint64_t max_console_file_bytes = 1ULL * 1024ULL * 1024ULL;

struct runtime_state {
    mutable std::mutex commands_mutex{};
    std::vector<std::string> commands{};
    std::atomic<bool> stop{false};
    std::atomic<std::int64_t> heartbeat_ns{0};
    refrain::core::config::store *config{nullptr};
    static constexpr std::size_t tick_samples{2048};
    std::array<std::uint32_t, tick_samples> tick_us{};
    std::size_t tick_count{0};
    std::uint64_t tick_total_us{0};
    std::uint64_t tick_max_us{0};
};

void record_tick_work(runtime_state &state, std::uint64_t work_ns) {
    const auto us{static_cast<std::uint32_t>(work_ns / 1'000ULL)};
    state.tick_us[state.tick_count % runtime_state::tick_samples] = us;
    ++state.tick_count;
    state.tick_total_us += us;
    state.tick_max_us =
        std::max(state.tick_max_us, static_cast<std::uint64_t>(us));
}

void push_command(const std::shared_ptr<runtime_state> &state,
                  std::string line) {
    if (line.empty()) {
        return;
    }
    const std::lock_guard lock{state->commands_mutex};
    if (state->commands.size() < max_console_commands) {
        state->commands.push_back(std::move(line));
    }
}

[[nodiscard]] auto drain_commands(const std::shared_ptr<runtime_state> &state)
    -> std::vector<std::string> {
    std::vector<std::string> drained{};
    const std::lock_guard lock{state->commands_mutex};
    drained.swap(state->commands);
    return drained;
}

void push_text_commands(const std::shared_ptr<runtime_state> &state,
                        std::string_view text) {
    std::size_t start{0};
    while (start <= text.size()) {
        const auto newline{text.find('\n', start)};
        const auto end{newline == std::string_view::npos ? text.size()
                                                         : newline};
        auto line{std::string{text.substr(start, end - start)}};
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        push_command(state, std::move(line));
        if (newline == std::string_view::npos) {
            break;
        }
        start = newline + 1;
    }
}

void start_console_thread(const std::shared_ptr<runtime_state> &state) {
    std::thread console{[state] {
        refrain::log::set_thread_name("console");
        std::string line{};
        try {
            while (!state->stop.load(std::memory_order_acquire) &&
                   std::getline(std::cin, line)) {
                push_command(state, line);
            }
        } catch (...) {
            // The console is diagnostics: a failed read must not terminate the
            // server; the tick loop can still be stopped by `--ticks`/signal.
        }
    }};
    // A blocking stdin read cannot be woken portably. The console thread only
    // touches the shared state, so it is safe to detach at shutdown and let
    // process exit reclaim it.
    console.detach();
}

void print_console_help() {
    log_server.info(
        "commands: stop, save, tps, hash, pos, move <dx> <dy> <dz>, "
        "walk <dx> <dz>, jump, dig <x> <y> <z>, place <x> <y> <z> <block>, "
        "give <item> <count>, inv, spawn <type> <x> <y> <z>, entities, "
        "kill <entity-id>, palette-gc, schedules, config <namespace> "
        "get|set <key> [value], config save|reload [namespace], "
        "config names");
}

[[nodiscard]] auto split_console_line(std::string_view line)
    -> std::vector<std::string_view> {
    std::vector<std::string_view> tokens{};
    std::size_t position{0};
    while (position < line.size()) {
        while (position < line.size() && line[position] == ' ') {
            ++position;
        }
        if (position == line.size()) {
            break;
        }
        const auto start{position};
        while (position < line.size() && line[position] != ' ') {
            ++position;
        }
        tokens.push_back(line.substr(start, position - start));
    }
    return tokens;
}

template <typename T>
[[nodiscard]] auto parse_number(std::string_view text, T &out) -> bool {
    const auto parsed{
        std::from_chars(text.data(), text.data() + text.size(), out)};
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

[[nodiscard]] auto parse_config_value(std::string_view text)
    -> std::variant<bool, std::int64_t, double, std::string> {
    if (text == "true") {
        return true;
    }
    if (text == "false") {
        return false;
    }
    std::int64_t integer{0};
    const auto int_parsed{
        std::from_chars(text.data(), text.data() + text.size(), integer)};
    if (int_parsed.ec == std::errc{} &&
        int_parsed.ptr == text.data() + text.size()) {
        return integer;
    }
    double real{0.0};
    const auto real_parsed{
        std::from_chars(text.data(), text.data() + text.size(), real)};
    if (real_parsed.ec == std::errc{} &&
        real_parsed.ptr == text.data() + text.size()) {
        return real;
    }
    return std::string{text};
}

void execute_config_command(std::span<const std::string_view> tokens,
                            refrain::core::config::store *config) {
    namespace cfg = refrain::core::config;
    if (config == nullptr) {
        log_config.warn("no host configuration store");
        return;
    }
    if (tokens.size() >= 2 && tokens[1] == "names") {
        const auto names{config->names()};
        std::string joined{};
        for (const auto &name : names) {
            if (!joined.empty()) {
                joined += ' ';
            }
            joined += name;
        }
        log_config.info("namespaces={} list={}", names.size(), joined);
        return;
    }
    if (tokens.size() >= 2 && tokens[1] == "save") {
        if (tokens.size() == 2) {
            if (auto saved{config->save_all()}; !saved) {
                log_config.error("save failed: {}", saved.error().message);
            } else {
                log_config.info("saved dirty namespaces");
            }
        } else if (auto saved{config->save(tokens[2])}; !saved) {
            log_config.error("save '{}' failed: {}", tokens[2],
                             saved.error().message);
        } else {
            log_config.info("saved '{}'", tokens[2]);
        }
        return;
    }
    if (tokens.size() >= 2 && tokens[1] == "reload") {
        if (tokens.size() == 2) {
            if (auto loaded{config->reload_all()}; !loaded) {
                log_config.error("reload failed: {}", loaded.error().message);
            } else {
                log_config.info("reloaded; running subsystems keep their "
                                "startup snapshot until restart");
            }
        } else if (auto loaded{config->reload(tokens[2])}; !loaded) {
            log_config.error("reload '{}' failed: {}", tokens[2],
                             loaded.error().message);
        } else {
            log_config.info("reloaded '{}'; restart required to apply",
                            tokens[2]);
        }
        return;
    }
    if (tokens.size() < 4) {
        log_config.warn(
            "usage: config <namespace> get <key> | set <key> <value> "
            "| save [namespace] | reload [namespace] | names");
        return;
    }
    const auto name_space{tokens[1]};
    const auto action{tokens[2]};
    const auto key{tokens[3]};
    auto module{config->module(name_space)};
    if (!module) {
        log_config.warn("unknown namespace '{}'", name_space);
        return;
    }
    if (action == "get") {
        const auto kind{module->kind(key)};
        if (kind == cfg::value_kind::missing) {
            log_config.warn("{}.{} is not set", name_space, key);
        } else if (kind == cfg::value_kind::boolean) {
            log_config.info("{}.{} = {}", name_space, key,
                            module->get_bool(key, false));
        } else if (kind == cfg::value_kind::integer) {
            log_config.info("{}.{} = {}", name_space, key,
                            module->get_int64(key, 0));
        } else if (kind == cfg::value_kind::floating) {
            log_config.info("{}.{} = {}", name_space, key,
                            module->get_double(key, 0.0));
        } else if (kind == cfg::value_kind::string) {
            log_config.info("{}.{} = '{}'", name_space, key,
                            module->get_string(key, ""));
        } else if (kind == cfg::value_kind::array) {
            const auto values{module->get_strings(key)};
            log_config.info("{}.{} = {} strings", name_space, key,
                            values.size());
        } else {
            log_config.info("{}.{} = <table>", name_space, key);
        }
        return;
    }
    if (action == "set") {
        if (tokens.size() < 5) {
            log_config.warn("set expects a value");
            return;
        }
        const auto value{parse_config_value(tokens[4])};
        const auto result{std::visit(
            [&](const auto &typed) -> status {
                using T = std::decay_t<decltype(typed)>;
                if constexpr (std::is_same_v<T, bool>) {
                    return config->set_bool(name_space, key, typed);
                } else if constexpr (std::is_same_v<T, std::int64_t>) {
                    return config->set_int64(name_space, key, typed);
                } else if constexpr (std::is_same_v<T, double>) {
                    return config->set_double(name_space, key, typed);
                } else {
                    return config->set_string(name_space, key, typed);
                }
            },
            value)};
        if (!result) {
            log_config.error("set failed: {}", result.error().message);
        } else {
            log_config.info(
                "{}.{} updated in memory; 'config save' persists it "
                "and subsystems pick it up after restart",
                name_space, key);
        }
        return;
    }
    log_config.warn("unknown action '{}'", action);
}

void execute_console_command(const std::string &line,
                             const std::shared_ptr<runtime_state> &state,
                             refrain::sim::world &store,
                             maintenance_service *maintenance,
                             const refrain::core::tick_info &tick,
                             refrain::net::server *listener) {
    if (line == "stop") {
        log_server.info("stop");
        state->stop.store(true, std::memory_order_release);
        return;
    }
    if (line == "palette-gc") {
        // Quiesced maintenance: this runs on the sim thread, so no tick or
        // console command overlaps it. Drain the writer first; the rebuild
        // closes/reopens the store and reloads the previously loaded chunks.
        maintenance->drain();
        auto rebuilt{
            store.rebuild_palette_in_place(1, store.store().zstd_level())};
        if (!rebuilt) {
            log_server.error("palette-gc failed: {}", rebuilt.error().message);
            // A failed reopen leaves the store invalid; continuing to tick
            // would operate on a closed world.
            if (!store.store().valid()) {
                state->stop.store(true, std::memory_order_release);
            }
            return;
        }
        log_server.info("palette-gc: entries {}->{} bytes {}->{}",
                        rebuilt->entries_before, rebuilt->entries_after,
                        rebuilt->bytes_before, rebuilt->bytes_after);
        return;
    }
    if (line == "save") {
        // Drain the background writer first: a snapshot taken before this
        // command could otherwise be written after flush() and resurrect an
        // older chunk revision.
        maintenance->drain();
        auto committed{store.flush()};
        if (!committed) {
            log_storage.error("{}", committed.error().message);
        } else {
            log_storage.info("ok");
        }
        return;
    }
    if (line == "net") {
        if (listener == nullptr) {
            log_server.warn("net is unavailable");
            return;
        }
        const auto stats{listener->stats()};
        log_net.info("players={} in={} out={} bytes_in={} bytes_out={}",
                     stats.connections, stats.packets_in, stats.packets_out,
                     stats.bytes_in, stats.bytes_out);
        return;
    }
    if (line == "tps") {
        const auto samples{
            std::min(state->tick_count, runtime_state::tick_samples)};
        std::vector<std::uint32_t> sorted(state->tick_us.begin(),
                                          state->tick_us.begin() + samples);
        std::sort(sorted.begin(), sorted.end());
        const auto percentile = [&](double fraction) {
            if (sorted.empty()) {
                return std::uint64_t{0};
            }
            const auto index{static_cast<std::size_t>(
                static_cast<double>(sorted.size() - 1) * fraction)};
            return static_cast<std::uint64_t>(sorted[index]);
        };
        const auto average{state->tick_count == 0
                               ? 0.0
                               : static_cast<double>(state->tick_total_us) /
                                     static_cast<double>(state->tick_count) /
                                     1000.0};
        log_server.info(
            "{} interval={}ms missed={} work_avg={:.2f}ms "
            "work_p99={:.2f}ms work_max={:.2f}ms samples={}",
            tick.index,
            std::chrono::duration_cast<std::chrono::milliseconds>(tick.actual)
                .count(),
            tick.missed, average,
            static_cast<double>(percentile(0.99)) / 1000.0,
            static_cast<double>(state->tick_max_us) / 1000.0, samples);
        return;
    }
    if (line == "hash") {
        log_sim.info("{}", refrain::data::to_hex(store.state_hash()));
        return;
    }
    if (line == "inv") {
        if (store.player().inventory.empty()) {
            log_server.info("empty");
        } else {
            for (const auto &stack : store.player().inventory) {
                log_server.info("{} x{}", stack.item, stack.count);
            }
        }
        return;
    }
    if (line == "pos") {
        const auto &player{store.player()};
        log_server.info("x={:.1f} y={:.1f} z={:.1f}", player.x, player.y,
                        player.z);
        return;
    }
    if (line == "entities") {
        if (store.entities().empty()) {
            log_server.info("none");
        } else {
            for (const auto &[id, value] : store.entities()) {
                log_server.info("entity {} {} x={:.1f} y={:.1f} z={:.1f}", id,
                                value.type, value.x, value.y, value.z);
            }
        }
        return;
    }
    const auto tokens{split_console_line(line)};
    if (!tokens.empty() && tokens[0] == "config") {
        execute_config_command(tokens, state->config);
        return;
    }
    if (line == "jump") {
        store.jump();
        log_server.info("on_ground={}", store.player().on_ground);
        return;
    }
    if (tokens.size() == 3 && tokens[0] == "give") {
        std::uint32_t count{0};
        if (!parse_number(tokens[2], count) || count == 0) {
            log_server.warn("give expects a positive count");
            return;
        }
        auto added{store.add_item(tokens[1], count)};
        if (!added) {
            log_server.warn("give failed: {}", added.error().message);
            return;
        }
        log_server.info("{} x{}", tokens[1], count);
        return;
    }
    if (tokens.size() == 5 && tokens[0] == "spawn") {
        double x{0.0};
        double y{0.0};
        double z{0.0};
        if (!parse_number(tokens[2], x) || !parse_number(tokens[3], y) ||
            !parse_number(tokens[4], z)) {
            log_server.warn("spawn expects numeric coordinates");
            return;
        }
        auto spawned{store.spawn_entity(tokens[1], x, y, z)};
        if (!spawned) {
            log_server.warn("spawn failed: {}", spawned.error().message);
            return;
        }
        log_server.info("{} #{}", tokens[1], *spawned);
        return;
    }
    if (tokens.size() == 2 && tokens[0] == "kill") {
        refrain::sim::entity_id id{0};
        if (!parse_number(tokens[1], id)) {
            log_server.warn("kill expects an entity id");
            return;
        }
        if (!store.remove_entity(id)) {
            log_server.warn("no entity {}", id);
            return;
        }
        log_server.info("#{}", id);
        return;
    }
    if (tokens.size() == 3 && tokens[0] == "walk") {
        double dx{0.0};
        double dz{0.0};
        if (!parse_number(tokens[1], dx) || !parse_number(tokens[2], dz)) {
            log_server.warn("walk expects two numbers");
            return;
        }
        const auto moved{store.move_player(dx, 0.0, dz)};
        log_server.info("{:.1f},{:.1f},{:.1f} {}", store.player().x,
                        store.player().y, store.player().z,
                        moved ? "moved" : "blocked");
        return;
    }
    if (tokens.size() == 4 && tokens[0] == "move") {
        double dx{0.0};
        double dy{0.0};
        double dz{0.0};
        if (!parse_number(tokens[1], dx) || !parse_number(tokens[2], dy) ||
            !parse_number(tokens[3], dz)) {
            log_server.warn("move expects three numbers");
            return;
        }
        auto &player{store.player()};
        store.set_player_position(player.x + dx, player.y + dy, player.z + dz);
        log_server.info("{:.1f},{:.1f},{:.1f}", player.x, player.y, player.z);
        return;
    }
    if ((tokens.size() == 4 && tokens[0] == "dig") ||
        (tokens.size() == 5 && tokens[0] == "place")) {
        std::int32_t x{0};
        std::int32_t y{0};
        std::int32_t z{0};
        if (!parse_number(tokens[1], x) || !parse_number(tokens[2], y) ||
            !parse_number(tokens[3], z)) {
            log_server.warn("{} expects integer coordinates", tokens[0]);
            return;
        }
        if (y < std::numeric_limits<std::int16_t>::min() ||
            y > std::numeric_limits<std::int16_t>::max()) {
            log_server.warn("block y={} is out of range", y);
            return;
        }
        auto value{refrain::sim::air_block};
        std::optional<std::string> gained{};
        if (tokens[0] == "dig") {
            const auto existing{store.block_at(
                refrain::sim::block_pos{x, static_cast<std::int16_t>(y), z})};
            if (existing != refrain::sim::air_block) {
                gained = std::string{store.registry().key(existing)};
            }
        } else if (tokens[0] == "place") {
            value = store.registry().intern(std::string{tokens[4]});
        }
        auto applied{store.set_block(
            refrain::sim::block_pos{x, static_cast<std::int16_t>(y), z},
            value)};
        if (!applied) {
            log_server.warn("{} failed: {}", tokens[0],
                            applied.error().message);
            return;
        }
        if (gained) {
            (void)store.add_item(*gained, 1);
        }
        if (tokens[0] == "place") {
            // The server remains creative-permissive: consume when available,
            // but do not reject a scripted placement without a prior `give`.
            (void)store.remove_item(tokens[4], 1);
        }
        log_server.info("{}: {},{},{} -> {}", tokens[0], x, y, z,
                        tokens[0] == "place" ? tokens[4] : "air");
        return;
    }
    if (line == "help") {
        print_console_help();
        return;
    }
    if (line == "schedules") {
        if (maintenance == nullptr) {
            log_schedule.info("unavailable");
            return;
        }
        const auto tasks{maintenance->tasks()};
        if (tasks.empty()) {
            log_schedule.info("none");
            return;
        }
        for (const auto &task : tasks) {
            log_schedule.info(
                "{}.{} phase={} interval={} enabled={} runs={} "
                "failures={} coalesced={} last_tick={}",
                task.service, task.method,
                refrain::core::schedule::phase_name(task.phase_of),
                task.interval_ticks, task.enabled ? "yes" : "no",
                task.stats.runs, task.stats.failures, task.stats.coalesced,
                task.stats.last_tick);
            if (!task.stats.last_error.empty()) {
                log_schedule.info("{}.{} last_error={}", task.service,
                                  task.method, task.stats.last_error);
            }
        }
        return;
    }
    log_server.warn("unknown '{}'", line);
}

} // namespace

auto run_server(const server_run_options &options) -> int {
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

    auto store{refrain::sim::world::open(options.world_path)};
    if (!store) {
        return report_error(store.error());
    }
    const std::string datapack_root{options.datapack_root.empty()
                                        ? "content/vanilla/26.3/data"
                                        : options.datapack_root};
    auto level_info{store->store().level()};
    if (!level_info) {
        return report_error(level_info.error());
    }
    if (store->loaded_chunks() == 0) {
        // A world created from a seed starts empty; fill the spawn area
        // before the listener accepts joins.
        const auto seed{level_info->seed};
        const auto spawn{refrain::world::chunk_pos{0, 0}};
        auto generated{refrain::server::generate_spawn_area(
            store->store(),
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
    // a worker, so walking past the spawn area keeps streaming terrain.
    std::unique_ptr<refrain::server::chunk_generation> generation{};
    if (auto started{refrain::server::chunk_generation::start(
            refrain::server::chunk_generation::config{
                .data_root = datapack_root,
                .templates_root = datapack_root + "/../templates",
                .seed = level_info->seed,
            })};
        started) {
        generation = std::move(*started);
    } else {
        refrain::log::warn("[world]: generation unavailable: {}",
                           started.error().message);
    }
    if (auto configured{store->store().set_zstd_level(zstd_level)};
        !configured) {
        return report_error(configured.error());
    }
    if (auto configured{store->set_zstd_policy(
            refrain::sim::zstd_policy{.near_level = zstd_near_level,
                                      .mid_level = zstd_mid_level,
                                      .far_level = zstd_far_level,
                                      .near_radius = zstd_near_radius,
                                      .mid_radius = zstd_mid_radius})};
        !configured) {
        return report_error(configured.error());
    }
    auto level{store->store().level()};
    if (!level) {
        return report_error(level.error());
    }
    log_world.info("{} dataversion={} seed={}", options.world_path,
                   level->data_version, level->seed);
    store->seed(static_cast<std::uint64_t>(level->seed));
    store->set_player_position(static_cast<double>(level->spawn_x) + 0.5,
                               static_cast<double>(level->spawn_y),
                               static_cast<double>(level->spawn_z) + 0.5);

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
                store->add_ticket(pos, refrain::sim::ticket_kind::spawn)};
            if (!ticketed) {
                log_server.warn("{},{}: {}", pos.x, pos.z,
                                ticketed.error().message);
            }
        }
    }
    log_server.info("{} chunks {} tickets", store->loaded_chunks(),
                    store->ticket_count(spawn_chunk));

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
    command_environment->world = &store->store();
    const auto initial_mode{
        level->game_mode >= 0 && level->game_mode <= 3
            ? static_cast<refrain::net::game_mode>(level->game_mode)
            : refrain::net::game_mode::creative};
    command_environment->default_mode = initial_mode;
    refrain::session::play_session players{};
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
    // TODO: breaking ignores block hardness, the held tool, drops and
    // neighbour updates; every block becomes air immediately.
    network_inputs.break_block =
        [world = &*store](std::int32_t x, std::int32_t y, std::int32_t z) {
            if (y < std::numeric_limits<std::int16_t>::min() ||
                y > std::numeric_limits<std::int16_t>::max()) {
                return false;
            }
            return world
                ->set_block(
                    refrain::sim::block_pos{x, static_cast<std::int16_t>(y), z},
                    refrain::sim::air_block)
                .has_value();
        };
    // TODO: placing ignores the held item, placement rules and orientation;
    // every placement writes minecraft:stone.
    network_inputs.place_block =
        [world = &*store](std::int32_t x, std::int32_t y, std::int32_t z) {
            if (y < std::numeric_limits<std::int16_t>::min() ||
                y > std::numeric_limits<std::int16_t>::max()) {
                return false;
            }
            const auto stone{
                world->registry().intern(std::string{"minecraft:stone"})};
            return world
                ->set_block(
                    refrain::sim::block_pos{x, static_cast<std::int16_t>(y), z},
                    stone)
                .has_value();
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
    // encoded chunk cache) and is created once the listener and the tables
    // exist.
    std::optional<refrain::server::chunk_streamer> streamer{};
    if (listener && tables && registries) {
        streamer.emplace(*listener, refrain::server::chunk_packet_source{
                                        .world = &*store,
                                        .tables = &*tables,
                                        .registries = &*registries,
                                    });
        streamer->set_chunk_requester(
            [&generation](std::int32_t x, std::int32_t z) {
                return generation != nullptr &&
                       generation->request(refrain::world::chunk_pos{x, z});
            });
    }

    refrain::server::inventory_sync inventory{gateway,
                                              tables ? &*tables : nullptr};

    maintenance_service maintenance{};
    if (auto started{maintenance.start(*store, effective)}; !started) {
        return report_error(started.error());
    }

    const auto state{std::make_shared<runtime_state>()};
    state->config = options.config;
    state->heartbeat_ns.store(refrain::core::now_ns(),
                              std::memory_order_relaxed);

    if (!options.command_file.empty() && !options.script_file.empty()) {
        return report_error(refrain::make_error(
            refrain::error_code::invalid_argument,
            "refrain-server: --cmd-file and --script are mutually exclusive"));
    }
    std::vector<refrain::sim::replay_step> replay{};
    std::size_t replay_index{0};
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
        replay = std::move(*parsed);
    }
    if (!options.command_file.empty()) {
        auto text{refrain::core::io::read_text(
            options.command_file,
            {.max_bytes = max_console_file_bytes, .allow_symlink = false})};
        if (!text) {
            return report_error(text.error());
        }
        push_text_commands(state, *text);
    } else if (options.script_file.empty()) {
        start_console_thread(state);
    }

    std::thread watchdog{};
    if (watchdog_timeout > 0) {
        watchdog = std::thread{[state, timeout = watchdog_timeout] {
            refrain::log::set_thread_name("watchdog");
            const auto timeout_ns{static_cast<std::int64_t>(timeout) *
                                  1'000'000LL};
            bool warned{false};
            while (!state->stop.load(std::memory_order_acquire)) {
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

    while (!state->stop.load(std::memory_order_acquire)) {
        auto tick{clock.wait_next()};
        if (!tick) {
            log_server.error("{}", tick.error().message);
            break;
        }
        const auto tick_start_ns{refrain::core::now_ns()};
        state->heartbeat_ns.store(refrain::core::now_ns(),
                                  std::memory_order_relaxed);
        maintenance.collect_completed();
        for (const auto &line : drain_commands(state)) {
            execute_console_command(line, state, *store, &maintenance, *tick,
                                    listener.get());
        }
        while (replay_index < replay.size() &&
               replay[replay_index].tick == ticks_run) {
            execute_console_command(replay[replay_index].command, state, *store,
                                    &maintenance, *tick, listener.get());
            if (state->stop.load(std::memory_order_acquire)) {
                break;
            }
            ++replay_index;
        }
        maintenance.pre_tick(ticks_run);
        if (listener) {
            listener->tick();
            broadcaster.tick(players);
        }
        if (generation) {
            for (auto &[pos, column] : generation->take_results()) {
                auto inserted{store->insert_chunk(pos, std::move(column))};
                if (!inserted) {
                    log_chunk_gen.debug("insert ({}, {}) failed: {}", pos.x,
                                        pos.z, inserted.error().message);
                    continue;
                }
                if (streamer) {
                    streamer->chunk_ready(pos.x, pos.z);
                }
            }
        }
        if (streamer) {
            streamer->tick(views);
        }
        inventory.tick(players);
        store->tick();
        ++ticks_run;
        maintenance.post_tick(ticks_run);
        record_tick_work(*state, static_cast<std::uint64_t>(
                                     refrain::core::now_ns() - tick_start_ns));
        if (options.on_tick) {
            options.on_tick(static_cast<std::uint64_t>(ticks_run));
        }
        if (options.hash_every > 0 &&
            (ticks_run % static_cast<std::int64_t>(options.hash_every)) == 0) {
            log_sim.info("tick={} hash={}", ticks_run,
                         refrain::data::to_hex(store->state_hash()));
        }
        if (options.ticks > 0 && ticks_run >= options.ticks) {
            log_server.info("{}", ticks_run);
            state->stop.store(true, std::memory_order_release);
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

    state->stop.store(true, std::memory_order_release);
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
    log_sim.info("final tick={} hash={}", store->ticks(),
                 refrain::data::to_hex(store->state_hash()));
    auto committed{store->flush()};
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
