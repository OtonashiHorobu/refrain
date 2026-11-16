// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/console_commands.hpp>

#include <refrain/app/mod_management.hpp>
#include <refrain/core/log.hpp>
#include <refrain/core/schedule.hpp>
#include <refrain/net/server.hpp>
#include <refrain/server/maintenance_service.hpp>
#include <refrain/sim/world.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace {

constexpr refrain::log::logger log_config{"config"};
constexpr refrain::log::logger log_mods{"mods"};
constexpr refrain::log::logger log_net{"net"};
constexpr refrain::log::logger log_schedule{"schedule"};
constexpr refrain::log::logger log_server{"server"};
constexpr refrain::log::logger log_sim{"sim"};
constexpr refrain::log::logger log_storage{"storage"};

} // namespace

namespace refrain::server {

namespace {

void print_console_help() {
    log_server.info(
        "available commands are stop, save, tps, hash, pos [name], "
        "move [name] <dx> <dy> <dz>, walk [name] <dx> <dz>, jump [name], "
        "gamemode <mode> [name], heal [name], food [name] <level>, "
        "damage [name] <amount>, xp [name] <points>, "
        "dig <x> <y> <z>, place <x> <y> <z> <block>, "
        "give [name] <item> <count>, inv [name], "
        "spawn <type> <x> <y> <z>, entities, kill <entity-id>, palette-gc, "
        "storage, time query|set|add|pause|resume, "
        "weather clear|rain|thunder [duration], schedules, "
        "config <namespace> get|set <key> [value], "
        "config save|reload [namespace], config names, "
        "mods list|installed|verify <package>|install <package> "
        "[confirm]|uninstall <id>[@version] [confirm]|reload "
        "<id>[@version]|trust [list|pin <id>|clear <id>]; install, uninstall "
        "and trust changes need confirm, --dry-run reports without changing "
        "state");
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
        log_config.warn("the host configuration store is unavailable");
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
        log_config.info("{} configuration namespaces are available: {}",
                        names.size(), joined);
        return;
    }
    if (tokens.size() >= 2 && tokens[1] == "save") {
        if (tokens.size() == 2) {
            if (auto saved{config->save_all()}; !saved) {
                log_config.error(
                    "failed to save all dirty configuration namespaces: {}",
                    saved.error().message);
            } else {
                log_config.info("saved dirty namespaces");
            }
        } else if (auto saved{config->save(tokens[2])}; !saved) {
            log_config.error("failed to save namespace '{}': {}", tokens[2],
                             saved.error().message);
        } else {
            log_config.info("saved namespace '{}'", tokens[2]);
        }
        return;
    }
    if (tokens.size() >= 2 && tokens[1] == "reload") {
        if (tokens.size() == 2) {
            if (auto loaded{config->reload_all()}; !loaded) {
                log_config.error(
                    "failed to reload all configuration namespaces: {}",
                    loaded.error().message);
            } else {
                log_config.info("reloaded the configuration, but running "
                                "subsystems keep their startup snapshot until "
                                "restart");
            }
        } else if (auto loaded{config->reload(tokens[2])}; !loaded) {
            log_config.error("failed to reload namespace '{}': {}", tokens[2],
                             loaded.error().message);
        } else {
            log_config.info("reloaded namespace '{}', but the change takes "
                            "effect after a restart",
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
            log_config.error("failed to set {}.{}: {}", name_space, key,
                             result.error().message);
        } else {
            log_config.info(
                "updated {}.{} in memory, 'config save' persists it and "
                "running subsystems pick it up after restart",
                name_space, key);
        }
        return;
    }
    log_config.warn("unknown action '{}'", action);
}

/// Run one `mods ...` line through the shared application service. The
/// command writes its lines through the `mods` logger, and reports a failure at
/// error level with the same message the in-game command shows the player.
void execute_mods_command(std::span<const std::string_view> tokens,
                          refrain::app::mod_management *mods) {
    if (mods == nullptr) {
        log_mods.warn("mod management is unavailable");
        return;
    }
    refrain::app::mod_management_actor actor{};
    actor.kind = refrain::app::mod_management_actor_kind::console;
    actor.name = "@console";
    const auto report{mods->execute(tokens, actor)};
    for (const auto &line : report.lines) {
        log_mods.info("{}", line);
    }
    if (!report.ok) {
        log_mods.error("{}", report.error);
    }
}

/// Copy one protocol-facing projection into the record shape the console
/// commands print. The projection carries no inventory or velocity; commands
/// that need the inventory read the in-process table instead.
[[nodiscard]] auto player_record(const player_projection &projected)
    -> refrain::sim::player_state {
    refrain::sim::player_state record{};
    record.id = projected.entity_id;
    record.connection = projected.connection;
    record.profile_id = projected.profile_id;
    record.name = projected.name;
    record.world = projected.world;
    record.x = projected.x;
    record.y = projected.y;
    record.z = projected.z;
    record.yaw = projected.yaw;
    record.pitch = projected.pitch;
    record.on_ground = projected.on_ground;
    record.game_mode = projected.game_mode;
    record.health = projected.health;
    record.view_distance = projected.view_distance;
    return record;
}

/// Resolve the player a console command addresses. An empty name selects the
/// first player by entity id (the console's default); the server registers the
/// synthetic replay player before any client can connect, so a replay never
/// changes target when clients join.
///
/// The selection is a projection copy: the direct port reads the in-process
/// table and a contract port reads the provider's projection. The command then
/// addresses the record by connection, so the copy stays enough for a write.
[[nodiscard]] auto select_player(world_port &world, std::string_view name)
    -> std::optional<refrain::sim::player_state> {
    auto count{world.player_count()};
    if (!count) {
        log_server.warn("failed to read the player list: {}",
                        count.error().message);
        return std::nullopt;
    }
    for (std::uint32_t index{0}; index < *count; ++index) {
        auto projected{world.player_at(index)};
        if (!projected) {
            log_server.warn("failed to read player {}: {}", index,
                            projected.error().message);
            return std::nullopt;
        }
        if (name.empty() || projected->name == name) {
            return player_record(*projected);
        }
    }
    return std::nullopt;
}

/// The in-process table, or null when the active provider keeps it. Commands
/// that touch the inventory need the table and name the gap otherwise.
[[nodiscard]] auto player_table_of(world_port &world)
    -> refrain::sim::player_table * {
    return world.players();
}

} // namespace

void tick_metrics::record(std::uint64_t work_ns) {
    const auto us{static_cast<std::uint32_t>(work_ns / 1'000ULL)};
    samples[count % sample_capacity] = us;
    ++count;
    total_us += us;
    max_us = std::max(max_us, static_cast<std::uint64_t>(us));
}

console_commands::console_commands(console_dependencies dependencies,
                                   tick_metrics &metrics)
    : dependencies_{dependencies}, metrics_{metrics} {}

void console_commands::execute(std::string_view line,
                               const refrain::core::tick_info &tick) {
    auto &world{*dependencies_.world};
    if (line == "stop") {
        log_server.info("stopping the server");
        dependencies_.stop->store(true, std::memory_order_release);
        return;
    }
    if (line == "palette-gc") {
        // Quiesced maintenance: this runs on the sim thread, so no tick or
        // console command overlaps it. Drain the writer first; the rebuild
        // closes/reopens the store and reloads the previously loaded chunks.
        dependencies_.maintenance->drain();
        auto rebuilt{world.rebuild_palette(1)};
        if (!rebuilt) {
            log_server.error("failed to rebuild the world palette: {}",
                             rebuilt.error().message);
            // A failed reopen leaves the store invalid; continuing to tick
            // would operate on a closed world.
            if (!world.store_valid()) {
                dependencies_.stop->store(true, std::memory_order_release);
            }
            return;
        }
        log_server.info(
            "rebuilt the world palette, entries went from {} to {} and bytes "
            "from {} to {}",
            rebuilt->entries_before, rebuilt->entries_after,
            rebuilt->bytes_before, rebuilt->bytes_after);
        return;
    }
    if (line == "save") {
        // Drain the background writer first: a snapshot taken before this
        // command could otherwise be written after flush() and resurrect an
        // older chunk revision.
        dependencies_.maintenance->drain();
        auto committed{world.flush()};
        if (!committed) {
            log_storage.error("{}", committed.error().message);
        } else {
            log_storage.info("saved all dirty chunks");
        }
        return;
    }
    if (line == "net") {
        if (dependencies_.listener == nullptr) {
            log_server.warn("net is unavailable");
            return;
        }
        const auto stats{dependencies_.listener->stats()};
        const bool one_player{stats.connections == 1};
        log_net.info("{} {} {} connected, received {} packets and {} bytes, "
                     "sent {} packets and {} bytes",
                     stats.connections, one_player ? "player" : "players",
                     one_player ? "is" : "are", stats.packets_in,
                     stats.bytes_in, stats.packets_out, stats.bytes_out);
        return;
    }
    if (line == "storage") {
        if (dependencies_.maintenance == nullptr) {
            log_storage.warn("storage is unavailable");
            return;
        }
        const auto status{dependencies_.maintenance->writer_status()};
        if (status.consecutive_failures == 0) {
            log_storage.info("autosave is healthy");
            return;
        }
        log_storage.warn(
            "autosave has failed {} times in a row, the chunks stay dirty and "
            "are retried: {}",
            status.consecutive_failures, status.last_error);
        return;
    }
    if (line == "tps") {
        const auto samples{
            std::min(metrics_.count, tick_metrics::sample_capacity)};
        std::vector<std::uint32_t> sorted(metrics_.samples.begin(),
                                          metrics_.samples.begin() + samples);
        std::sort(sorted.begin(), sorted.end());
        const auto percentile = [&](double fraction) {
            if (sorted.empty()) {
                return std::uint64_t{0};
            }
            const auto index{static_cast<std::size_t>(
                static_cast<double>(sorted.size() - 1) * fraction)};
            return static_cast<std::uint64_t>(sorted[index]);
        };
        const auto average{metrics_.count == 0
                               ? 0.0
                               : static_cast<double>(metrics_.total_us) /
                                     static_cast<double>(metrics_.count) /
                                     1000.0};
        log_server.info(
            "tick {} ran with an interval of {}ms and missed {} ticks, work "
            "averaged {:.2f}ms with p99 {:.2f}ms and max {:.2f}ms across {} "
            "samples",
            tick.index,
            std::chrono::duration_cast<std::chrono::milliseconds>(tick.actual)
                .count(),
            tick.missed, average,
            static_cast<double>(percentile(0.99)) / 1000.0,
            static_cast<double>(metrics_.max_us) / 1000.0, samples);
        return;
    }
    if (line == "hash") {
        log_sim.info("state hash is {}",
                     refrain::data::to_hex(world.state_hash()));
        return;
    }
    if (line == "time" || line.starts_with("time ")) {
        auto *clock{world.clock()};
        if (clock == nullptr) {
            log_server.warn("the active world provider keeps no clock");
            return;
        }
        const auto tokens{split_console_line(line)};
        const auto publish = [&] {
            if (dependencies_.listener == nullptr) {
                return;
            }
            const auto snapshot{clock->snapshot()};
            const auto weather{clock->weather()};
            dependencies_.listener->set_world_state(net::world_state_snapshot{
                .game_time = snapshot.game_time,
                .clock_total_ticks = snapshot.total_ticks,
                .clock_partial_tick = snapshot.partial_tick,
                .clock_rate = snapshot.paused ? 0.0F : snapshot.rate,
                .raining = weather.raining,
                .rain_level = weather.rain_level,
                .thunder_level = weather.thunder_level,
            });
            dependencies_.listener->broadcast_world_time();
        };
        const auto named_ticks =
            [](std::string_view name) -> std::optional<std::int64_t> {
            if (name == "day" || name == "morning") {
                return 1000;
            }
            if (name == "noon") {
                return 6000;
            }
            if (name == "night") {
                return 13000;
            }
            if (name == "midnight") {
                return 18000;
            }
            return std::nullopt;
        };
        if (tokens.size() == 1 ||
            (tokens.size() == 2 && tokens[1] == "query")) {
            const auto snapshot{clock->snapshot()};
            log_server.info(
                "game time is {} ticks and the overworld clock is at {} ticks, "
                "{}",
                snapshot.game_time, snapshot.total_ticks,
                snapshot.paused ? "paused" : "running");
            return;
        }
        if (tokens.size() >= 3 && tokens[1] == "set") {
            std::int64_t ticks{0};
            if (const auto named{named_ticks(tokens[2])}; named) {
                ticks = *named;
            } else if (!parse_number(tokens[2], ticks)) {
                log_server.warn("time set expects a tick count or day, noon, "
                                "night or midnight");
                return;
            }
            clock->set_total_ticks(ticks);
            publish();
            log_server.info("set the overworld clock to {} ticks", ticks);
            return;
        }
        if (tokens.size() >= 3 && tokens[1] == "add") {
            std::int64_t ticks{0};
            if (!parse_number(tokens[2], ticks)) {
                log_server.warn("time add expects a tick count");
                return;
            }
            clock->add_total_ticks(ticks);
            publish();
            log_server.info("added {} ticks to the overworld clock", ticks);
            return;
        }
        if (tokens.size() == 2 && tokens[1] == "pause") {
            clock->set_paused(true);
            publish();
            log_server.info("paused the overworld clock");
            return;
        }
        if (tokens.size() == 2 && tokens[1] == "resume") {
            clock->set_paused(false);
            publish();
            log_server.info("resumed the overworld clock");
            return;
        }
        log_server.warn("time expects query, set <ticks|day|noon|night|"
                        "midnight>, add <ticks>, pause or resume");
        return;
    }
    if (line == "weather" || line.starts_with("weather ")) {
        auto *clock{world.clock()};
        if (clock == nullptr) {
            log_server.warn("the active world provider keeps no weather");
            return;
        }
        const auto tokens{split_console_line(line)};
        if (tokens.size() < 2) {
            const auto weather{clock->weather()};
            log_server.info("weather is {}, rain level {:.2f}, thunder level "
                            "{:.2f}",
                            weather.raining
                                ? (weather.thundering ? "thunder" : "rain")
                                : "clear",
                            static_cast<double>(weather.rain_level),
                            static_cast<double>(weather.thunder_level));
            return;
        }
        std::int32_t duration{6000};
        if (tokens.size() >= 3 && !parse_number(tokens[2], duration)) {
            log_server.warn("weather expects an optional duration in ticks");
            return;
        }
        duration = std::max<std::int32_t>(duration, 1);
        if (tokens[1] == "clear") {
            clock->set_weather(false, false, duration, 0, 0);
        } else if (tokens[1] == "rain") {
            clock->set_weather(true, false, 0, duration, 0);
        } else if (tokens[1] == "thunder") {
            clock->set_weather(true, true, 0, duration, duration);
        } else {
            log_server.warn("weather expects clear, rain or thunder");
            return;
        }
        if (dependencies_.listener != nullptr) {
            const auto weather{clock->weather()};
            dependencies_.listener->set_world_state(net::world_state_snapshot{
                .game_time = clock->snapshot().game_time,
                .clock_total_ticks = clock->snapshot().total_ticks,
                .clock_partial_tick = clock->snapshot().partial_tick,
                .clock_rate =
                    clock->snapshot().paused ? 0.0F : clock->snapshot().rate,
                .raining = weather.raining,
                .rain_level = weather.rain_level,
                .thunder_level = weather.thunder_level,
            });
            dependencies_.listener->broadcast_game_event(
                weather.raining ? 1 : 2, 0.0F);
            dependencies_.listener->broadcast_game_event(7, weather.rain_level);
            dependencies_.listener->broadcast_game_event(8,
                                                         weather.thunder_level);
        }
        log_server.info("set the weather to {}", tokens[1]);
        return;
    }
    const auto tokens{split_console_line(line)};
    if ((tokens.size() == 1 || tokens.size() == 2) && tokens[0] == "inv") {
        const auto selected{
            select_player(world, tokens.size() == 2 ? tokens[1] : "")};
        if (!selected) {
            log_server.warn("no player is connected");
            return;
        }
        auto *players{player_table_of(world)};
        if (players == nullptr) {
            log_server.warn("the active world provider exposes no player "
                            "inventory");
            return;
        }
        const auto *player{players->find(selected->id)};
        if (player == nullptr) {
            log_server.warn("no player is connected");
            return;
        }
        if (player->inventory.empty()) {
            log_server.info("the inventory of {} is empty", player->name);
        } else {
            for (const auto &stack : player->inventory) {
                log_server.info("{} holds {} x{}", player->name, stack.item,
                                stack.count);
            }
        }
        return;
    }
    if ((tokens.size() == 1 || tokens.size() == 2) && tokens[0] == "pos") {
        const auto player{
            select_player(world, tokens.size() == 2 ? tokens[1] : "")};
        if (!player) {
            log_server.warn("no player is connected");
            return;
        }
        log_server.info("{} is at x={:.1f} y={:.1f} z={:.1f}", player->name,
                        player->x, player->y, player->z);
        return;
    }
    if (line == "entities") {
        const auto *entities{world.entities()};
        if (entities == nullptr || entities->empty()) {
            log_server.info("no entities are loaded");
        } else {
            for (const auto &[id, value] : *entities) {
                log_server.info(
                    "entity #{} is {} at x={:.1f} y={:.1f} z={:.1f}", id,
                    value.type, value.x, value.y, value.z);
            }
        }
        return;
    }
    if (!tokens.empty() && tokens[0] == "config") {
        execute_config_command(tokens, dependencies_.config);
        return;
    }
    if (!tokens.empty() && tokens[0] == "mods") {
        execute_mods_command(
            std::span<const std::string_view>{tokens}.subspan(1),
            dependencies_.mods);
        return;
    }
    if ((tokens.size() == 1 || tokens.size() == 2) && tokens[0] == "jump") {
        const auto player{
            select_player(world, tokens.size() == 2 ? tokens[1] : "")};
        if (!player) {
            log_server.warn("no player is connected");
            return;
        }
        auto jumped{world.jump(player->connection)};
        if (!jumped) {
            log_server.warn("failed to make {} jump: {}", player->name,
                            jumped.error().message);
            return;
        }
        auto after{world.player(player->connection)};
        log_server.info("{} jumped and is {}", player->name,
                        after && after->on_ground ? "on the ground"
                                                  : "in the air");
        return;
    }
    const auto selected_record =
        [&world](std::string_view name) -> refrain::sim::player_state * {
        const auto selected{select_player(world, std::string{name})};
        if (!selected) {
            return nullptr;
        }
        auto *sim_level{world.sim_world()};
        if (sim_level == nullptr) {
            return nullptr;
        }
        return sim_level->players().find_by_connection(selected->connection);
    };
    if ((tokens.size() == 2 || tokens.size() == 3) && tokens[0] == "gamemode") {
        const auto named{tokens.size() == 3};
        static constexpr std::array<std::string_view, 4> names{
            "survival", "creative", "adventure", "spectator"};
        std::uint32_t mode{0};
        const auto requested{tokens[1]};
        bool known{false};
        for (std::uint32_t index{0}; index < names.size(); ++index) {
            if (requested == names[index]) {
                mode = index;
                known = true;
                break;
            }
        }
        if (!known) {
            std::uint32_t numeric{0};
            if (parse_number(requested, numeric) && numeric <= 3) {
                mode = numeric;
                known = true;
            }
        }
        if (!known) {
            log_server.warn("gamemode expects survival, creative, adventure or "
                            "spectator");
            return;
        }
        auto *record{selected_record(named ? tokens[2] : "")};
        if (record == nullptr) {
            log_server.warn("no player is connected");
            return;
        }
        auto *sim_level{world.sim_world()};
        if (sim_level == nullptr) {
            log_server.warn("the active world provider keeps no in-process "
                            "player state");
            return;
        }
        refrain::sim::set_player_game_mode(*sim_level, *record, mode);
        if (dependencies_.listener != nullptr &&
            record->connection != refrain::sim::synthetic_connection) {
            dependencies_.listener->set_player_game_mode(
                record->connection, static_cast<refrain::net::game_mode>(mode));
        }
        log_server.info("{} now plays in game mode {}", record->name,
                        names[mode]);
        return;
    }
    if ((tokens.size() == 1 || tokens.size() == 2) && tokens[0] == "heal") {
        auto *record{selected_record(tokens.size() == 2 ? tokens[1] : "")};
        auto *sim_level{world.sim_world()};
        if (record == nullptr || sim_level == nullptr) {
            log_server.warn("no player is connected");
            return;
        }
        if (record->dead) {
            log_server.warn("{} is dead; respawn before healing", record->name);
            return;
        }
        record->health = 20.0F;
        log_server.info("{} is at full health", record->name);
        return;
    }
    if ((tokens.size() == 2 || tokens.size() == 3) && tokens[0] == "food") {
        const auto named{tokens.size() == 3};
        std::int32_t level{0};
        if (!parse_number(tokens[named ? 2 : 1], level) || level < 0 ||
            level > 20) {
            log_server.warn("food expects a level between 0 and 20");
            return;
        }
        auto *record{selected_record(named ? tokens[1] : "")};
        if (record == nullptr) {
            log_server.warn("no player is connected");
            return;
        }
        record->food_level = level;
        record->saturation =
            std::min(record->saturation, static_cast<float>(level));
        log_server.info("{} now has {} food", record->name, level);
        return;
    }
    if ((tokens.size() == 2 || tokens.size() == 3) && tokens[0] == "damage") {
        const auto named{tokens.size() == 3};
        double amount{0.0};
        if (!parse_number(tokens[named ? 2 : 1], amount) || amount <= 0.0) {
            log_server.warn("damage expects a positive amount");
            return;
        }
        auto *record{selected_record(named ? tokens[1] : "")};
        auto *sim_level{world.sim_world()};
        if (record == nullptr || sim_level == nullptr) {
            log_server.warn("no player is connected");
            return;
        }
        static_cast<void>(sim_level->damage_player(
            *record, refrain::sim::damage_source::generic,
            static_cast<float>(amount)));
        log_server.info("{} took {} damage and is at {} health", record->name,
                        amount, record->health);
        return;
    }
    if ((tokens.size() == 2 || tokens.size() == 3) && tokens[0] == "xp") {
        const auto named{tokens.size() == 3};
        std::int32_t points{0};
        if (!parse_number(tokens[named ? 2 : 1], points) || points <= 0) {
            log_server.warn("xp expects a positive amount");
            return;
        }
        auto *record{selected_record(named ? tokens[1] : "")};
        auto *sim_level{world.sim_world()};
        if (record == nullptr || sim_level == nullptr) {
            log_server.warn("no player is connected");
            return;
        }
        refrain::sim::add_experience(*record, points);
        log_server.info("{} gained {} experience and is at level {}",
                        record->name, points, record->experience_level);
        return;
    }
    if ((tokens.size() == 3 || tokens.size() == 4) && tokens[0] == "give") {
        const bool named{tokens.size() == 4};
        const auto selected{select_player(world, named ? tokens[1] : "")};
        if (!selected) {
            log_server.warn("no player is connected");
            return;
        }
        std::uint32_t count{0};
        if (!parse_number(tokens[named ? 3 : 2], count) || count == 0) {
            log_server.warn("give expects a positive count");
            return;
        }
        const auto item{tokens[named ? 2 : 1]};
        auto *players{player_table_of(world)};
        if (players == nullptr) {
            log_server.warn("the active world provider exposes no player "
                            "inventory");
            return;
        }
        auto *player{players->find(selected->id)};
        if (player == nullptr) {
            log_server.warn("no player is connected");
            return;
        }
        auto added{world.add_item(*player, item, count)};
        if (!added) {
            log_server.warn("failed to give {} to {}: {}", item, player->name,
                            added.error().message);
            return;
        }
        log_server.info("{} received {} x{}", player->name, item, count);
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
        auto spawned{world.spawn_entity(tokens[1], x, y, z)};
        if (!spawned) {
            log_server.warn("failed to spawn {}: {}", tokens[1],
                            spawned.error().message);
            return;
        }
        log_server.info("spawned entity #{} of type {}", *spawned, tokens[1]);
        return;
    }
    if (tokens.size() == 2 && tokens[0] == "kill") {
        refrain::sim::entity_id id{0};
        if (!parse_number(tokens[1], id)) {
            log_server.warn("kill expects an entity id");
            return;
        }
        if (!world.remove_entity(id)) {
            log_server.warn("no entity #{} is loaded", id);
            return;
        }
        log_server.info("killed entity #{}", id);
        return;
    }
    if ((tokens.size() == 3 || tokens.size() == 4) && tokens[0] == "walk") {
        const bool named{tokens.size() == 4};
        const auto player{select_player(world, named ? tokens[1] : "")};
        if (!player) {
            log_server.warn("no player is connected");
            return;
        }
        double dx{0.0};
        double dz{0.0};
        if (!parse_number(tokens[named ? 2 : 1], dx) ||
            !parse_number(tokens[named ? 3 : 2], dz)) {
            log_server.warn("walk expects two numbers");
            return;
        }
        const auto moved{world.move_player(player->connection, dx, 0.0, dz)};
        if (!moved) {
            log_server.warn("failed to move {}: {}", player->name,
                            moved.error().message);
            return;
        }
        // Report the committed position, not the requested one: the provider
        // resolves collision, so the record it returns is the answer.
        const auto after{world.player(player->connection)};
        log_server.info("{} {} x={:.1f} y={:.1f} z={:.1f}", player->name,
                        *moved ? "moved to" : "was blocked at",
                        after ? after->x : player->x,
                        after ? after->y : player->y,
                        after ? after->z : player->z);
        return;
    }
    if ((tokens.size() == 4 || tokens.size() == 5) && tokens[0] == "move") {
        const bool named{tokens.size() == 5};
        const auto player{select_player(world, named ? tokens[1] : "")};
        if (!player) {
            log_server.warn("no player is connected");
            return;
        }
        double dx{0.0};
        double dy{0.0};
        double dz{0.0};
        if (!parse_number(tokens[named ? 2 : 1], dx) ||
            !parse_number(tokens[named ? 3 : 2], dy) ||
            !parse_number(tokens[named ? 4 : 3], dz)) {
            log_server.warn("move expects three numbers");
            return;
        }
        auto moved{world.set_player_position(player->connection, player->x + dx,
                                             player->y + dy, player->z + dz)};
        if (!moved) {
            log_server.warn("failed to move {}: {}", player->name,
                            moved.error().message);
            return;
        }
        const auto after{world.player(player->connection)};
        log_server.info("{} is now at x={:.1f} y={:.1f} z={:.1f}", player->name,
                        after ? after->x : player->x,
                        after ? after->y : player->y,
                        after ? after->z : player->z);
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
            log_server.warn("the y coordinate {} is outside the valid block "
                            "range",
                            y);
            return;
        }
        auto value{refrain::sim::air_block};
        std::optional<std::string> gained{};
        if (tokens[0] == "dig") {
            const auto existing{world.block_at(
                refrain::sim::block_pos{x, static_cast<std::int16_t>(y), z})};
            if (existing != refrain::sim::air_block) {
                gained = std::string{world.block_key(existing)};
            }
        } else if (tokens[0] == "place") {
            value = world.intern_block(std::string{tokens[4]});
        }
        auto applied{world.set_block(
            refrain::sim::block_pos{x, static_cast<std::int16_t>(y), z},
            value)};
        if (!applied) {
            log_server.warn("failed to {} the block at {}, {}, {}: {}",
                            tokens[0] == "dig" ? "break" : "place", x, y, z,
                            applied.error().message);
            return;
        }
        // The block change is the contract-served part; the inventory side
        // effects below need the in-process table.
        // TODO: item operations are a separate contract surface, so a
        // provider-backed composition cannot apply the console's inventory
        // effects of a dig or a place yet.
        auto *players{player_table_of(world)};
        auto *player{players != nullptr ? players->first() : nullptr};
        if (gained && player != nullptr) {
            (void)world.add_item(*player, *gained, 1);
        }
        if (tokens[0] == "dig" && player != nullptr) {
            // Mining is the console player's use of its tool. There is no
            // selected hotbar slot yet, so the first damageable stack stands
            // in for the main hand.
            //
            // TODO: damage the main-hand item only when it is the correct tool
            // for the broken block, and wire the same rule into the network
            // block-break path, which does not pass the acting connection.
            const auto *items{world.items()};
            if (items != nullptr) {
                for (std::size_t index{0}; index < player->inventory.size();
                     ++index) {
                    if (refrain::sim::stack_max_damage(player->inventory[index],
                                                       *items) != 0) {
                        (void)world.damage_item(*player, index);
                        break;
                    }
                }
            }
        }
        if (tokens[0] == "place" && player != nullptr) {
            // The server remains creative-permissive: consume when available,
            // but do not reject a scripted placement without a prior `give`.
            (void)world.remove_item(*player, tokens[4], 1);
        }
        log_server.info("set block at {},{},{} to {}", x, y, z,
                        tokens[0] == "place" ? tokens[4] : "air");
        return;
    }
    if (line == "help") {
        print_console_help();
        return;
    }
    if (line == "schedules") {
        if (dependencies_.maintenance == nullptr) {
            log_schedule.info("the maintenance scheduler is unavailable");
            return;
        }
        const auto tasks{dependencies_.maintenance->tasks()};
        if (tasks.empty()) {
            log_schedule.info("no scheduled tasks are registered");
            return;
        }
        for (const auto &task : tasks) {
            log_schedule.info(
                "schedule {}.{}: phase {}, interval {} ticks, enabled {}, runs "
                "{}, failures {}, coalesced {}, last tick {}",
                task.service, task.method,
                refrain::core::schedule::phase_name(task.phase_of),
                task.interval_ticks, task.enabled ? "yes" : "no",
                task.stats.runs, task.stats.failures, task.stats.coalesced,
                task.stats.last_tick);
            if (!task.stats.last_error.empty()) {
                log_schedule.info("schedule {}.{} last failed with: {}",
                                  task.service, task.method,
                                  task.stats.last_error);
            }
        }
        return;
    }
    log_server.warn("unknown command '{}'", line);
}

} // namespace refrain::server
