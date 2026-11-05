// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/app/world_service.hpp>

#include <refrain/app/services.hpp>
#include <refrain/core/io.hpp>
#include <refrain/data/json.hpp>
#include <refrain/platform/subprocess.hpp>
#include <refrain/world/anvil.hpp>
#include <refrain/world/store.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace refrain::app {
namespace {

namespace io = refrain::core::io;
namespace world = refrain::world;
namespace json = refrain::data::json;

struct comparison {
    std::size_t total{0};
    std::size_t matched{0};
    std::size_t missing{0};
    std::size_t extra{0};
    std::vector<std::string> mismatches;
};

[[nodiscard]] auto
compare(const std::vector<world::anvil::chunk_digest> &baseline,
        const std::vector<world::anvil::chunk_digest> &candidate)
    -> comparison {
    std::map<std::pair<std::string, world::chunk_pos>,
             const world::anvil::chunk_digest *>
        left;
    for (const auto &digest : baseline) {
        left.emplace(std::pair{digest.dimension, digest.pos}, &digest);
    }
    std::map<std::pair<std::string, world::chunk_pos>,
             const world::anvil::chunk_digest *>
        right;
    for (const auto &digest : candidate) {
        right.emplace(std::pair{digest.dimension, digest.pos}, &digest);
    }
    comparison result{};
    constexpr std::size_t max_reported = 16;
    for (const auto &[key, digest] : left) {
        const auto &pos{key.second};
        ++result.total;
        const auto found{right.find(key)};
        if (found == right.end()) {
            ++result.missing;
            if (result.mismatches.size() < max_reported) {
                result.mismatches.push_back(
                    fmt::format("{}:{} missing from candidate (status {})",
                                digest->dimension, world::format_chunk_pos(pos),
                                digest->status));
            }
            continue;
        }
        if (found->second->hash == digest->hash) {
            ++result.matched;
            continue;
        }
        if (result.mismatches.size() < max_reported) {
            result.mismatches.push_back(fmt::format(
                "{}:{} hash mismatch (status {})", digest->dimension,
                world::format_chunk_pos(pos), digest->status));
        }
    }
    for (const auto &[key, digest] : right) {
        const auto &pos{key.second};
        if (left.contains(key)) {
            continue;
        }
        ++result.total;
        ++result.extra;
        if (result.mismatches.size() < max_reported) {
            result.mismatches.push_back(fmt::format(
                "{}:{} only in candidate (status {})", digest->dimension,
                world::format_chunk_pos(pos), digest->status));
        }
    }
    return result;
}

[[nodiscard]] auto percent(std::size_t matched, std::size_t total) -> double {
    if (total == 0) {
        return 100.0;
    }
    return 100.0 * static_cast<double>(matched) / static_cast<double>(total);
}

[[nodiscard]] auto require_directory(std::string_view path,
                                     std::string_view what) -> bool {
    if (path.empty()) {
        fmt::print(stderr, "error: {} is required\n", what);
        return false;
    }
    return true;
}

} // namespace

auto run_world_import(const world_import_options &options) -> int {
    if (!require_directory(options.from, "--from") ||
        !require_directory(options.to, "--to")) {
        return 2;
    }
    int zstd_level{3};
    if (options.compression == "fast") {
        zstd_level = 1;
    } else if (options.compression == "compact") {
        zstd_level = 6;
    } else if (options.compression != "balanced") {
        fmt::print(stderr, "error: unknown compression profile '{}'\n",
                   options.compression);
        return 2;
    }
    world::anvil::dictionary_scope dictionary_scope{
        world::anvil::dictionary_scope::dimension};
    if (options.dictionary_scope == "world") {
        dictionary_scope = world::anvil::dictionary_scope::world;
    } else if (options.dictionary_scope != "dimension") {
        fmt::print(stderr, "error: unknown dictionary scope '{}'\n",
                   options.dictionary_scope);
        return 2;
    }
    world::rwf::zstd_dictionary dictionary{};
    const world::rwf::zstd_dictionary *dictionary_ptr{nullptr};
    if (!options.dictionary.empty()) {
        auto bytes{io::read_file(options.dictionary,
                                 {.max_bytes = world::rwf::max_record_bytes,
                                  .allow_symlink = false})};
        if (!bytes) {
            return report_error(bytes.error());
        }
        dictionary = world::rwf::make_zstd_dictionary(*bytes);
        dictionary_ptr = &dictionary;
    }
    auto result{world::anvil::import_world(
        options.from, options.to, options.force, options.threads, zstd_level,
        dictionary_ptr, dictionary_scope)};
    if (!result) {
        return report_error(result.error());
    }
    fmt::print("world import: {} chunks, {} entities, {} poi, {} playerdata, "
               "{} datapack files, {} aux files\n",
               result->chunks, result->entity_chunks, result->poi_chunks,
               result->playerdata, result->datapack_files, result->aux_files);
    return 0;
}

auto run_world_export(const world_export_options &options) -> int {
    if (!require_directory(options.from, "--from") ||
        !require_directory(options.to, "--to")) {
        return 2;
    }
    auto result{world::anvil::export_world(options.from, options.to,
                                           options.force, options.threads)};
    if (!result) {
        return report_error(result.error());
    }
    fmt::print("world export: {} chunks, {} entities, {} poi, {} playerdata, "
               "{} datapack files, {} aux files\n",
               result->chunks, result->entity_chunks, result->poi_chunks,
               result->playerdata, result->datapack_files, result->aux_files);
    return 0;
}

auto run_world_fsck(const world_fsck_options &options) -> int {
    if (!require_directory(options.path, "world path")) {
        return 2;
    }
    auto store{world::world_store::open(options.path)};
    if (!store) {
        return report_error(store.error());
    }
    auto report{store->check(options.repair)};
    if (!report) {
        return report_error(report.error());
    }
    fmt::print("world fsck: regions={} chunks={} dimensions={} corrupt={} "
               "garbage={} repaired={}\n",
               report->regions, report->chunks, report->dimensions,
               report->corrupt, report->garbage_bytes,
               report->repaired ? "yes" : "no");
    for (const auto &problem : report->problems) {
        fmt::print("problem: {}\n", problem);
    }
    // Repaired regions no longer report their fixed problems; anything left
    // (corrupt meta, object stores, unreadable blobs) still fails the check.
    if (!report->problems.empty()) {
        return 1;
    }
    return 0;
}

auto run_world_compact(const world_compact_options &options) -> int {
    if (!require_directory(options.path, "world path")) {
        return 2;
    }
    auto store{world::world_store::open(options.path)};
    if (!store) {
        return report_error(store.error());
    }
    auto report{store->compact()};
    if (!report) {
        return report_error(report.error());
    }
    fmt::print("world compact: objects={} segments={} live={} garbage={}; "
               "blobs={} segments={} live={} garbage={}\n",
               report->objects.objects, report->objects.segments,
               report->objects.live_bytes, report->objects.garbage_bytes,
               report->blobs.objects, report->blobs.segments,
               report->blobs.live_bytes, report->blobs.garbage_bytes);
    return 0;
}

auto run_world_palette_gc(const world_palette_gc_options &options) -> int {
    if (!require_directory(options.from, "--from") ||
        !require_directory(options.to, "--to")) {
        return 2;
    }
    auto result{world::anvil::rebuild_palette(options.from, options.to,
                                              options.force, options.threads)};
    if (!result) {
        return report_error(result.error());
    }
    fmt::print("world palette-gc: entries {} -> {}, chunks={}, regions={}, "
               "bytes {} -> {}\n",
               result->entries_before, result->entries_after, result->chunks,
               result->regions, result->bytes_before, result->bytes_after);
    return 0;
}

auto run_world_info(const world_info_options &options) -> int {
    if (!require_directory(options.path, "world path")) {
        return 2;
    }
    auto store{world::world_store::open(options.path)};
    if (!store) {
        return report_error(store.error());
    }
    auto stats{store->stats()};
    if (!stats) {
        return report_error(stats.error());
    }
    auto level{options.strict ? store->validate_level() : store->level()};
    if (!level) {
        return report_error(level.error());
    }
    auto palette{store->palette_usage_stats()};
    if (!palette) {
        return report_error(palette.error());
    }
    if (options.json) {
        json::value::object_type spawn;
        spawn.emplace("x", json::value{level->spawn_x});
        spawn.emplace("y", json::value{level->spawn_y});
        spawn.emplace("z", json::value{level->spawn_z});
        json::value::object_type info;
        info.emplace("format", json::value{"refrain.world-info/1"});
        info.emplace("layout", json::value{"rwf"});
        info.emplace("data_version", json::value{level->data_version});
        info.emplace("seed", json::value{level->seed});
        info.emplace("spawn", json::value{std::move(spawn)});
        info.emplace("time", json::value{level->time});
        info.emplace("level_name", json::value{level->level_name});
        info.emplace("regions",
                     json::value{static_cast<std::int64_t>(stats->regions)});
        info.emplace("chunks",
                     json::value{static_cast<std::int64_t>(stats->chunks)});
        info.emplace("entity_chunks", json::value{static_cast<std::int64_t>(
                                          stats->entity_chunks)});
        info.emplace("poi_chunks",
                     json::value{static_cast<std::int64_t>(stats->poi_chunks)});
        info.emplace("playerdata",
                     json::value{static_cast<std::int64_t>(stats->playerdata)});
        info.emplace("datapack_files", json::value{static_cast<std::int64_t>(
                                           stats->datapack_files)});
        info.emplace("aux_files",
                     json::value{static_cast<std::int64_t>(stats->aux_files)});
        info.emplace("dimensions",
                     json::value{static_cast<std::int64_t>(stats->dimensions)});
        info.emplace("objects",
                     json::value{static_cast<std::int64_t>(stats->objects)});
        info.emplace("bytes",
                     json::value{static_cast<std::int64_t>(stats->bytes)});
        info.emplace("palette_entries", json::value{static_cast<std::int64_t>(
                                            palette->table_entries)});
        info.emplace("palette_referenced",
                     json::value{static_cast<std::int64_t>(
                         palette->referenced_entries)});
        fmt::print("{}\n", json::value{std::move(info)}.dump());
        return 0;
    }
    fmt::print("world: {}\n", options.path);
    fmt::print("layout: rwf\n");
    fmt::print("data_version: {}\n", level->data_version);
    fmt::print("seed: {}\n", level->seed);
    fmt::print("spawn: {} {} {}\n", level->spawn_x, level->spawn_y,
               level->spawn_z);
    fmt::print("time: {}\n", level->time);
    fmt::print("level_name: {}\n", level->level_name);
    fmt::print("regions: {}\n", stats->regions);
    fmt::print("chunks: {}\n", stats->chunks);
    fmt::print("entities: {}\n", stats->entity_chunks);
    fmt::print("poi: {}\n", stats->poi_chunks);
    fmt::print("playerdata: {}\n", stats->playerdata);
    fmt::print("datapack_files: {}\n", stats->datapack_files);
    fmt::print("aux_files: {}\n", stats->aux_files);
    fmt::print("dimensions: {}\n", stats->dimensions);
    fmt::print("objects: {}\n", stats->objects);
    fmt::print("bytes: {}\n", stats->bytes);
    fmt::print("palette: table={}, referenced={}\n", palette->table_entries,
               palette->referenced_entries);
    return 0;
}

auto run_world_diff(const world_diff_options &options) -> int {
    if (!require_directory(options.baseline, "baseline world") ||
        !require_directory(options.candidate, "candidate world")) {
        return 2;
    }
    auto baseline{world::anvil::scan_world(options.baseline)};
    if (!baseline) {
        return report_error(baseline.error());
    }
    auto candidate{world::anvil::scan_world(options.candidate)};
    if (!candidate) {
        return report_error(candidate.error());
    }
    const auto result{compare(*baseline, *candidate)};
    const auto match{percent(result.matched, result.total)};
    if (options.json) {
        json::value::array_type mismatches;
        mismatches.reserve(result.mismatches.size());
        for (const auto &mismatch : result.mismatches) {
            mismatches.emplace_back(json::value{mismatch});
        }
        json::value::object_type report;
        report.emplace("format", json::value{"refrain.world-diff/1"});
        report.emplace("chunks",
                       json::value{static_cast<std::int64_t>(result.total)});
        report.emplace("matched",
                       json::value{static_cast<std::int64_t>(result.matched)});
        report.emplace("mismatched", json::value{static_cast<std::int64_t>(
                                         result.total - result.matched)});
        report.emplace("missing",
                       json::value{static_cast<std::int64_t>(result.missing)});
        report.emplace("extra",
                       json::value{static_cast<std::int64_t>(result.extra)});
        report.emplace("match_rate", json::value{match / 100.0});
        report.emplace("mismatches", json::value{std::move(mismatches)});
        fmt::print("{}\n", json::value{std::move(report)}.dump());
    } else {
        fmt::print("world diff: chunks={} matched={} mismatched={} missing={} "
                   "extra={} match={:.2f}%\n",
                   result.total, result.matched, result.total - result.matched,
                   result.missing, result.extra, match);
        for (const auto &mismatch : result.mismatches) {
            fmt::print("mismatch: {}\n", mismatch);
        }
    }
    if (options.min_match > 0 &&
        match + 1e-9 < static_cast<double>(options.min_match)) {
        return 1;
    }
    return 0;
}

auto run_worldgen_harness(const worldgen_harness_options &options) -> int {
    if (!require_directory(options.candidate, "--candidate") ||
        !require_directory(options.oracle, "--oracle")) {
        return 2;
    }
    auto oracle{world::anvil::scan_world(options.oracle)};
    if (!oracle) {
        return report_error(oracle.error());
    }
    auto candidate{world::anvil::scan_world(options.candidate)};
    if (!candidate) {
        return report_error(candidate.error());
    }
    const auto result{compare(*oracle, *candidate)};
    const auto match{percent(result.matched, result.total)};
    std::map<std::string, std::pair<std::size_t, std::size_t>> by_status;
    std::map<std::pair<std::string, world::chunk_pos>,
             const world::anvil::chunk_digest *>
        candidate_map;
    for (const auto &digest : *candidate) {
        candidate_map.emplace(std::pair{digest.dimension, digest.pos}, &digest);
    }
    for (const auto &digest : *oracle) {
        auto &[chunks, matched] = by_status[digest.status];
        ++chunks;
        const auto found{
            candidate_map.find(std::pair{digest.dimension, digest.pos})};
        if (found != candidate_map.end() &&
            found->second->hash == digest.hash) {
            ++matched;
        }
    }
    fmt::print("worldgen harness: chunks={} matched={} match={:.2f}% "
               "min_match={}%\n",
               result.total, result.matched, match, options.min_match);
    for (const auto &[status, counts] : by_status) {
        fmt::print("status {}: {}/{} ({:.2f}%)\n", status, counts.second,
                   counts.first, percent(counts.second, counts.first));
    }
    for (const auto &mismatch : result.mismatches) {
        fmt::print("mismatch: {}\n", mismatch);
    }
    if (!options.report.empty()) {
        json::value::array_type statuses;
        for (const auto &[status, counts] : by_status) {
            json::value::object_type entry;
            entry.emplace("status", json::value{status});
            entry.emplace("chunks",
                          json::value{static_cast<std::int64_t>(counts.first)});
            entry.emplace("matched", json::value{static_cast<std::int64_t>(
                                         counts.second)});
            entry.emplace(
                "match_rate",
                json::value{percent(counts.second, counts.first) / 100.0});
            statuses.emplace_back(json::value{std::move(entry)});
        }
        json::value::object_type report;
        report.emplace("format", json::value{"refrain.worldgen-report/1"});
        report.emplace("chunks",
                       json::value{static_cast<std::int64_t>(result.total)});
        report.emplace("matched",
                       json::value{static_cast<std::int64_t>(result.matched)});
        report.emplace("mismatched", json::value{static_cast<std::int64_t>(
                                         result.total - result.matched)});
        report.emplace("match_rate", json::value{match / 100.0});
        report.emplace("by_status", json::value{std::move(statuses)});
        if (auto written{io::write_text(
                options.report, json::value{std::move(report)}.dump() + "\n")};
            !written) {
            return report_error(written.error());
        }
    }
    if (options.min_match > 0 &&
        match + 1e-9 < static_cast<double>(options.min_match)) {
        return 1;
    }
    return 0;
}

auto run_worldgen_oracle(const worldgen_oracle_options &options) -> int {
    if (options.jar.empty()) {
        fmt::print(stderr, "error: --jar is required\n");
        return 2;
    }
    if (options.out.empty()) {
        fmt::print(stderr, "error: --out is required\n");
        return 2;
    }
    if (!io::is_regular_file(options.jar)) {
        fmt::print(stderr, "error: '{}' is not a file\n", options.jar);
        return 1;
    }
    if (auto created{io::create_directories(options.out)}; !created) {
        return report_error(created.error());
    }
    const auto properties{
        fmt::format("level-name=world\nlevel-seed={}\nlevel-type="
                    "minecraft:normal\nonline-mode=false\n"
                    "sync-chunk-writes=true\nview-distance=2\n"
                    "spawn-protection=0\nmax-players=1\n"
                    "pause-when-empty-seconds=0\n",
                    options.seed)};
    if (auto written{io::write_text(
            fmt::format("{}/server.properties", options.out), properties)};
        !written) {
        return report_error(written.error());
    }
    if (auto written{io::write_text(fmt::format("{}/eula.txt", options.out),
                                    "eula=true\n")};
        !written) {
        return report_error(written.error());
    }
    platform::subprocess_options process{};
    process.program = options.java;
    process.arguments = {"-Xmx2G", "-jar", options.jar, "nogui"};
    process.working_directory = options.out;
    process.stdin_text = "stop\n";
    auto started{platform::run_process(process)};
    if (!started) {
        return report_error(started.error());
    }
    if (started->exit_code != 0) {
        fmt::print(stderr, "error: vanilla server exited with {}\n",
                   started->exit_code);
        return 1;
    }
    const auto world{fmt::format("{}/world", options.out)};
    if (!io::is_directory(world)) {
        fmt::print(stderr, "error: vanilla server did not create '{}'\n",
                   world);
        return 1;
    }
    fmt::print("worldgen oracle: {} (seed={})\n", world, options.seed);
    return 0;
}

} // namespace refrain::app
