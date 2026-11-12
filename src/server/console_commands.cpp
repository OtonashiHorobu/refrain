// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/console_commands.hpp>

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
    auto &store{*dependencies_.world};
    if (line == "stop") {
        log_server.info("stop");
        dependencies_.stop->store(true, std::memory_order_release);
        return;
    }
    if (line == "palette-gc") {
        // Quiesced maintenance: this runs on the sim thread, so no tick or
        // console command overlaps it. Drain the writer first; the rebuild
        // closes/reopens the store and reloads the previously loaded chunks.
        dependencies_.maintenance->drain();
        auto rebuilt{
            store.rebuild_palette_in_place(1, store.store().zstd_level())};
        if (!rebuilt) {
            log_server.error("palette-gc failed: {}", rebuilt.error().message);
            // A failed reopen leaves the store invalid; continuing to tick
            // would operate on a closed world.
            if (!store.store().valid()) {
                dependencies_.stop->store(true, std::memory_order_release);
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
        dependencies_.maintenance->drain();
        auto committed{store.flush()};
        if (!committed) {
            log_storage.error("{}", committed.error().message);
        } else {
            log_storage.info("ok");
        }
        return;
    }
    if (line == "net") {
        if (dependencies_.listener == nullptr) {
            log_server.warn("net is unavailable");
            return;
        }
        const auto stats{dependencies_.listener->stats()};
        log_net.info("players={} in={} out={} bytes_in={} bytes_out={}",
                     stats.connections, stats.packets_in, stats.packets_out,
                     stats.bytes_in, stats.bytes_out);
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
            "{} interval={}ms missed={} work_avg={:.2f}ms "
            "work_p99={:.2f}ms work_max={:.2f}ms samples={}",
            tick.index,
            std::chrono::duration_cast<std::chrono::milliseconds>(tick.actual)
                .count(),
            tick.missed, average,
            static_cast<double>(percentile(0.99)) / 1000.0,
            static_cast<double>(metrics_.max_us) / 1000.0, samples);
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
        execute_config_command(tokens, dependencies_.config);
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
        if (dependencies_.maintenance == nullptr) {
            log_schedule.info("unavailable");
            return;
        }
        const auto tasks{dependencies_.maintenance->tasks()};
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

} // namespace refrain::server
