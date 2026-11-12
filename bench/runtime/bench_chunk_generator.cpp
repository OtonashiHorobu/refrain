// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Full-chunk runtime benchmark: the production `chunk_generator` over a
// rectangle of chunks, for the terrain and full targets and a fixed worker
// count. Reports load time, throughput, per-chunk latency and peak resident
// memory so a change can be compared against a recorded run.
//
// The benchmark is data-driven: it needs the extracted datapack root in
// `REFRAIN_VANILLA_WORLDGEN` and, for template-backed structures, the template
// root in `REFRAIN_VANILLA_STRUCTURES`. Without the datapack root it prints a
// skip line and exits successfully, so it can live in a CI image that does not
// ship game data.
//
// Example:
//   REFRAIN_VANILLA_WORLDGEN=/data REFRAIN_VANILLA_STRUCTURES=/templates
//   ./build/Release/bin/refrain-bench-runtime --targets terrain,full
//       --chunks 16,32,64 --workers 1,2,4,8 --repeat 3

#include <refrain/worldgen/chunk_generator.hpp>
#include <refrain/worldgen/stage_profile.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace refrain;
using namespace refrain::worldgen;
using clock_type = std::chrono::steady_clock;

struct options {
    std::string data_root{};
    std::string templates_root{};
    std::string biome{"minecraft:plains"};
    std::string dimension{"overworld"};
    dimension_kind kind{dimension_kind::overworld};
    std::vector<generation_target> targets{generation_target::terrain,
                                           generation_target::full};
    std::vector<jint> chunk_counts{16, 32, 64};
    std::vector<std::size_t> worker_counts{1, 2, 4, 8};
    jlong seed{12345};
    jint origin_x{0};
    jint origin_z{0};
    jint repeat{3};
    std::string output{};
    bool chunk_hash{false};
    bool stages{false};
};

auto hash_bytes(std::uint64_t hash, std::string_view text) -> std::uint64_t {
    constexpr std::uint64_t prime = 1099511628211ULL;
    for (const char character : text) {
        hash ^= static_cast<unsigned char>(character);
        hash *= prime;
    }
    return hash;
}

auto hash_integer(std::uint64_t hash, std::uint64_t value) -> std::uint64_t {
    constexpr std::uint64_t prime = 1099511628211ULL;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        hash ^= (value >> shift) & 0xFFU;
        hash *= prime;
    }
    return hash;
}

auto skip_message() -> int {
    fmt::print("bench-chunk-generator: skipped, REFRAIN_VANILLA_WORLDGEN is "
               "not set\n");
    return 0;
}

auto parse_integer(std::string_view text, jlong &out) -> bool {
    if (text.empty()) {
        return false;
    }
    const auto *begin = text.data();
    const auto *end = begin + text.size();
    const auto result = std::from_chars(begin, end, out);
    return result.ec == std::errc{} && result.ptr == end;
}

auto split_list(std::string_view text) -> std::vector<std::string_view> {
    std::vector<std::string_view> parts;
    while (true) {
        const auto comma = text.find(',');
        parts.push_back(text.substr(0, comma));
        if (comma == std::string_view::npos) {
            return parts;
        }
        text.remove_prefix(comma + 1);
    }
}

auto parse_target(std::string_view text) -> std::optional<generation_target> {
    if (text == "terrain") {
        return generation_target::terrain;
    }
    if (text == "features") {
        return generation_target::features;
    }
    if (text == "full") {
        return generation_target::full;
    }
    return std::nullopt;
}

auto parse_dimension(std::string_view text) -> std::optional<dimension_kind> {
    if (text == "overworld") {
        return dimension_kind::overworld;
    }
    if (text == "the_nether" || text == "nether") {
        return dimension_kind::nether;
    }
    if (text == "the_end" || text == "end") {
        return dimension_kind::end;
    }
    return std::nullopt;
}

auto target_name(generation_target target) -> std::string_view {
    switch (target) {
    case generation_target::terrain:
        return "terrain";
    case generation_target::features:
        return "features";
    case generation_target::full:
        return "full";
    }
    return "terrain";
}

auto read_status_kib(std::string_view key) -> std::uint64_t {
    auto file = std::fopen("/proc/self/status", "r");
    if (file == nullptr) {
        return 0;
    }
    char line[256];
    std::uint64_t value = 0;
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        const std::string_view text{line};
        if (!text.starts_with(key)) {
            continue;
        }
        auto digits = text.substr(key.size());
        const auto space = digits.find_first_not_of(" \t");
        if (space != std::string_view::npos) {
            digits.remove_prefix(space);
        }
        const auto result = std::from_chars(
            digits.data(), digits.data() + digits.size(), value);
        if (result.ec != std::errc{}) {
            value = 0;
        }
        break;
    }
    std::fclose(file);
    return value;
}

auto read_peak_rss_kib() -> std::uint64_t { return read_status_kib("VmHWM:"); }

auto read_rss_kib() -> std::uint64_t { return read_status_kib("VmRSS:"); }

auto milliseconds(clock_type::duration duration) -> double {
    return std::chrono::duration<double, std::milli>(duration).count();
}

auto percentile(std::vector<double> values, double fraction) -> double {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(
        fraction * static_cast<double>(values.size() - 1));
    return values[index];
}

auto rectangle_for(jint chunks) -> std::pair<jint, jint> {
    jint width = 1;
    while (width * width < chunks) {
        ++width;
    }
    const jint depth = (chunks + width - 1) / width;
    return {width, depth};
}

auto make_positions(jint origin_x, jint origin_z, jint width, jint depth)
    -> std::vector<chunk_pos> {
    std::vector<chunk_pos> positions;
    positions.reserve(static_cast<std::size_t>(width) *
                      static_cast<std::size_t>(depth));
    for (jint x = 0; x < width; ++x) {
        for (jint z = 0; z < depth; ++z) {
            positions.push_back(
                chunk_pos{.x = origin_x + x, .z = origin_z + z});
        }
    }
    return positions;
}

struct scenario_result {
    std::string target{};
    jint chunks{0};
    std::size_t workers{0};
    double create_ms{0.0};
    double cold_batch_ms{0.0};
    double median_batch_ms{0.0};
    double warm_chunks_per_second{0.0};
    double cold_chunks_per_second{0.0};
    double p50_ms_per_chunk{0.0};
    double p95_ms_per_chunk{0.0};
    std::uint64_t rss_kib{0};
    std::uint64_t peak_rss_kib{0};
};

auto run_scenario(const options &config, generation_target target, jint chunks,
                  std::size_t workers, std::ostream *stream)
    -> std::optional<scenario_result> {
    const auto [width, depth] = rectangle_for(chunks);
    const auto create_begin = clock_type::now();
    auto created = chunk_generator::create(chunk_generator_config{
        .data_root = config.data_root,
        .templates_root = config.templates_root,
        .seed = config.seed,
        .kind = config.kind,
        .fixed_biome = config.biome,
        .target = target,
        .structure_sets = {},
        .reference_world = false,
    });
    const auto create_ms = milliseconds(clock_type::now() - create_begin);
    if (!created) {
        fmt::print(stderr, "bench-chunk-generator: create failed: {}\n",
                   created.error().message);
        return std::nullopt;
    }
    auto generator = std::move(*created);
    std::atomic<block_state_id> sink{0};
    const auto observe = [&sink](const std::unique_ptr<proto_chunk> &chunk) {
        if (chunk != nullptr) {
            sink.store(chunk->state_at(8, 0, 8), std::memory_order_relaxed);
        }
    };

    std::vector<double> batch_ms;
    batch_ms.reserve(static_cast<std::size_t>(config.repeat));
    std::vector<double> per_chunk_ms;
    per_chunk_ms.reserve(static_cast<std::size_t>(chunks));
    double cold_batch_ms = 0.0;

    const auto positions =
        make_positions(config.origin_x, config.origin_z, width, depth);
    for (jint repeat = 0; repeat < config.repeat; ++repeat) {
        const auto begin = clock_type::now();
        if (workers == 1) {
            // The serial path is also the per-chunk latency sample: it runs
            // the same stage sequence the batch entry point uses for one
            // worker.
            for (const auto &position : positions) {
                const auto chunk_begin = clock_type::now();
                auto generated = generator->generate(position);
                if (!generated) {
                    fmt::print(stderr,
                               "bench-chunk-generator: generate failed: {}\n",
                               generated.error().message);
                    return std::nullopt;
                }
                if (repeat == 0) {
                    per_chunk_ms.push_back(
                        milliseconds(clock_type::now() - chunk_begin));
                }
                observe(*generated);
            }
            const auto elapsed = milliseconds(clock_type::now() - begin);
            batch_ms.push_back(elapsed);
        } else {
            auto generated = generator->generate_area(
                chunk_pos{.x = config.origin_x, .z = config.origin_z}, width,
                depth, workers);
            if (!generated) {
                fmt::print(stderr,
                           "bench-chunk-generator: generate_area failed: {}\n",
                           generated.error().message);
                return std::nullopt;
            }
            const auto elapsed = milliseconds(clock_type::now() - begin);
            batch_ms.push_back(elapsed);
            for (const auto &chunk : *generated) {
                observe(chunk);
            }
        }
        if (repeat == 0) {
            cold_batch_ms = batch_ms.back();
        }
    }

    std::vector<double> sorted = batch_ms;
    std::sort(sorted.begin(), sorted.end());
    const double median_batch_ms = sorted[sorted.size() / 2U];
    const double warm_chunks_per_second =
        static_cast<double>(chunks) * 1000.0 / median_batch_ms;
    const double cold_chunks_per_second =
        static_cast<double>(chunks) * 1000.0 / cold_batch_ms;
    if (workers != 1) {
        // The batch entry point does not expose per-chunk completion times, so
        // the tail statistics use each batch's per-chunk amortised time.
        per_chunk_ms.clear();
        for (const double batch : batch_ms) {
            per_chunk_ms.push_back(batch / static_cast<double>(chunks));
        }
    }

    scenario_result result;
    result.target = std::string{target_name(target)};
    result.chunks = chunks;
    result.workers = workers;
    result.create_ms = create_ms;
    result.cold_batch_ms = cold_batch_ms;
    result.median_batch_ms = median_batch_ms;
    result.warm_chunks_per_second = warm_chunks_per_second;
    result.cold_chunks_per_second = cold_chunks_per_second;
    result.p50_ms_per_chunk = percentile(per_chunk_ms, 0.50);
    result.p95_ms_per_chunk = percentile(per_chunk_ms, 0.95);
    result.rss_kib = read_rss_kib();
    result.peak_rss_kib = read_peak_rss_kib();

    const auto line = fmt::format(
        "RESULT target={} chunks={} workers={} create_ms={:.1f} "
        "cold_batch_ms={:.1f} median_batch_ms={:.1f} "
        "cold_chunks_per_s={:.3f} warm_chunks_per_s={:.3f} "
        "p50_ms_per_chunk={:.1f} p95_ms_per_chunk={:.1f} rss_mib={:.1f} "
        "peak_rss_mib={:.1f}",
        result.target, result.chunks, result.workers, result.create_ms,
        result.cold_batch_ms, result.median_batch_ms,
        result.cold_chunks_per_second, result.warm_chunks_per_second,
        result.p50_ms_per_chunk, result.p95_ms_per_chunk,
        static_cast<double>(result.rss_kib) / 1024.0,
        static_cast<double>(result.peak_rss_kib) / 1024.0);
    fmt::print("{}\n", line);
    if (stream != nullptr) {
        *stream << line << '\n';
    }
    return result;
}

/// Runs one batch per repeat and prints the wall-clock time of each stage the
/// generator reports. The rows are a decomposition of one batch: a row that
/// encloses another stage (`feature.decorate` around `feature.placement` and
/// the feature types) is not a sibling of it.
auto run_stage_scenario(const options &config, generation_target target,
                        jint chunks, std::ostream *stream) -> bool {
    const auto [width, depth] = rectangle_for(chunks);
    auto created = chunk_generator::create(chunk_generator_config{
        .data_root = config.data_root,
        .templates_root = config.templates_root,
        .seed = config.seed,
        .kind = config.kind,
        .fixed_biome = config.biome,
        .target = target,
        .structure_sets = {},
        .reference_world = false,
    });
    if (!created) {
        fmt::print(stderr, "bench-chunk-generator: create failed: {}\n",
                   created.error().message);
        return false;
    }
    auto generator = std::move(*created);
    stage_profile profile;
    generator->set_stage_profile(&profile);
    std::atomic<block_state_id> sink{0};
    const auto positions =
        make_positions(config.origin_x, config.origin_z, width, depth);
    const auto emit = [stream](const std::string &line) {
        fmt::print("{}\n", line);
        if (stream != nullptr) {
            *stream << line << '\n';
        }
    };
    const auto chunk_count = static_cast<double>(positions.size());
    for (jint repeat = 0; repeat < config.repeat; ++repeat) {
        profile.clear();
        const auto begin = clock_type::now();
        for (const auto &position : positions) {
            auto generated = generator->generate(position);
            if (!generated) {
                fmt::print(stderr,
                           "bench-chunk-generator: generate failed: {}\n",
                           generated.error().message);
                return false;
            }
            sink.store(generated->get()->state_at(8, 0, 8),
                       std::memory_order_relaxed);
        }
        const auto elapsed = milliseconds(clock_type::now() - begin);
        emit(fmt::format("STAGE_BATCH target={} batch={} chunks={} workers=1 "
                         "measured_ms={:.3f} per_chunk_ms={:.3f}",
                         target_name(target), repeat, chunks, elapsed,
                         elapsed / chunk_count));
        for (const auto &entry : profile.stages()) {
            emit(fmt::format("STAGE target={} batch={} name={} total_ms={:.3f} "
                             "per_chunk_ms={:.3f} samples={}",
                             target_name(target), repeat, entry.name,
                             static_cast<double>(entry.nanoseconds) / 1.0e6,
                             static_cast<double>(entry.nanoseconds) / 1.0e6 /
                                 chunk_count,
                             entry.samples));
        }
        for (const auto &[type, entry] : profile.features()) {
            emit(fmt::format(
                "FEATURE_STAGE target={} batch={} type={} total_ms={:.3f} "
                "per_chunk_ms={:.3f} samples={}",
                target_name(target), repeat, type,
                static_cast<double>(entry.nanoseconds) / 1.0e6,
                static_cast<double>(entry.nanoseconds) / 1.0e6 / chunk_count,
                entry.samples));
        }
    }
    return true;
}

auto chunk_digest(const proto_chunk &chunk) -> std::uint64_t {
    const auto &table = chunk.table();
    auto hash = std::uint64_t{0xcbf29ce484222325ULL};
    hash = hash_integer(hash, static_cast<std::uint64_t>(chunk.chunk_x()));
    hash = hash_integer(hash, static_cast<std::uint64_t>(chunk.chunk_z()));
    hash = hash_integer(hash, static_cast<std::uint64_t>(chunk.status()));
    for (jint y = chunk.min_y(); y < chunk.min_y() + chunk.height(); ++y) {
        for (jint z = 0; z < 16; ++z) {
            for (jint x = 0; x < 16; ++x) {
                hash = hash_bytes(hash, table.name(chunk.state_at(x, y, z)));
            }
        }
    }
    for (jint y = chunk.min_y(); y < chunk.min_y() + chunk.height(); y += 4) {
        for (jint z = 0; z < 16; z += 4) {
            for (jint x = 0; x < 16; x += 4) {
                hash = hash_integer(hash, chunk.biome_at(x, y, z));
            }
        }
    }
    for (const auto &start : chunk.structure_starts()) {
        hash = hash_bytes(hash, start.structure);
        hash = hash_bytes(hash, start.step);
        hash = hash_integer(hash, static_cast<std::uint64_t>(start.chunk_x));
        hash = hash_integer(hash, static_cast<std::uint64_t>(start.chunk_z));
        hash = hash_integer(hash, static_cast<std::uint64_t>(start.min_x));
        hash = hash_integer(hash, static_cast<std::uint64_t>(start.min_y));
        hash = hash_integer(hash, static_cast<std::uint64_t>(start.min_z));
        hash = hash_integer(hash, static_cast<std::uint64_t>(start.max_x));
        hash = hash_integer(hash, static_cast<std::uint64_t>(start.max_y));
        hash = hash_integer(hash, static_cast<std::uint64_t>(start.max_z));
        for (const auto &piece : start.pieces) {
            hash = hash_bytes(hash, piece.type);
        }
    }
    hash = hash_integer(
        hash, static_cast<std::uint64_t>(chunk.block_entities().size()));
    for (const auto &[pos, entity] : chunk.block_entities()) {
        (void)entity;
        hash = hash_integer(hash, static_cast<std::uint64_t>(pos.x));
        hash = hash_integer(hash, static_cast<std::uint64_t>(pos.y));
        hash = hash_integer(hash, static_cast<std::uint64_t>(pos.z));
    }
    for (const auto &section : chunk.post_processing()) {
        for (const auto packed : section) {
            hash = hash_integer(hash, packed);
        }
    }
    return hash;
}

auto run_chunk_hash(const options &config, generation_target target,
                    std::ostream *stream) -> bool {
    auto created = chunk_generator::create(chunk_generator_config{
        .data_root = config.data_root,
        .templates_root = config.templates_root,
        .seed = config.seed,
        .kind = config.kind,
        .fixed_biome = config.biome,
        .target = target,
        .structure_sets = {},
        .reference_world = false,
    });
    if (!created) {
        fmt::print(stderr, "bench-chunk-generator: create failed: {}\n",
                   created.error().message);
        return false;
    }
    auto generator = std::move(*created);
    const auto [width, depth] = rectangle_for(config.chunk_counts.front());
    const auto positions =
        make_positions(config.origin_x, config.origin_z, width, depth);
    auto total = std::uint64_t{0xcbf29ce484222325ULL};
    for (const auto &position : positions) {
        auto generated = generator->generate(position);
        if (!generated) {
            fmt::print(stderr, "bench-chunk-generator: generate failed: {}\n",
                       generated.error().message);
            return false;
        }
        const auto digest = chunk_digest(**generated);
        total = hash_integer(total, digest);
        const auto line = fmt::format(
            "CHUNK_HASH target={} seed={} pos={},{} digest={:016x}",
            target_name(target), config.seed, position.x, position.z, digest);
        fmt::print("{}\n", line);
        if (stream != nullptr) {
            *stream << line << '\n';
        }
    }
    const auto line = fmt::format(
        "CHUNK_HASH_TOTAL target={} seed={} chunks={} digest={:016x}",
        target_name(target), config.seed, positions.size(), total);
    fmt::print("{}\n", line);
    if (stream != nullptr) {
        *stream << line << '\n';
    }
    return true;
}

auto parse_options(int argc, char **argv) -> std::optional<options> {
    options config;
    if (const char *root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        root != nullptr) {
        config.data_root = root;
    }
    if (const char *templates = std::getenv("REFRAIN_VANILLA_STRUCTURES");
        templates != nullptr) {
        config.templates_root = templates;
    }
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        const auto next =
            [&](std::string_view name) -> std::optional<std::string_view> {
            if (index + 1 >= argc) {
                fmt::print(stderr, "bench-chunk-generator: {} needs a value\n",
                           name);
                return std::nullopt;
            }
            return std::string_view{argv[++index]};
        };
        if (argument == "--help" || argument == "-h") {
            fmt::print(
                "refrain-bench-runtime: full-chunk generator benchmark\n"
                "  --data-root <path>       datapack data root (default: env "
                "REFRAIN_VANILLA_WORLDGEN)\n"
                "  --templates-root <path>  template root (default: env "
                "REFRAIN_VANILLA_STRUCTURES)\n"
                "  --seed <n>               world seed (default 12345)\n"
                "  --biome <key>            fixed biome (default "
                "minecraft:plains)\n"
                "  --dimension <kind>       overworld, the_nether or the_end\n"
                "  --targets <list>         terrain,features,full (default "
                "terrain,full)\n"
                "  --chunks <list>          chunk counts (default 16,32,64)\n"
                "  --workers <list>         batch worker counts (default "
                "1,2,4,8)\n"
                "  --repeat <n>             batches per scenario (default 3)\n"
                "  --origin-x/--origin-z   area origin (default 0)\n"
                "  --output <path>          append the result lines here\n"
                "  --chunk-hash             hash the generated chunk set "
                "instead "
                "of timing it\n"
                "  --stages                 print the wall-clock time of each "
                "generation stage per batch\n");
            return std::nullopt;
        }
        if (argument == "--data-root") {
            const auto value = next(argument);
            if (!value) {
                return std::nullopt;
            }
            config.data_root = *value;
        } else if (argument == "--templates-root") {
            const auto value = next(argument);
            if (!value) {
                return std::nullopt;
            }
            config.templates_root = *value;
        } else if (argument == "--biome") {
            const auto value = next(argument);
            if (!value) {
                return std::nullopt;
            }
            config.biome = *value;
        } else if (argument == "--dimension") {
            const auto value = next(argument);
            const auto kind = value ? parse_dimension(*value) : std::nullopt;
            if (!value || !kind) {
                return std::nullopt;
            }
            config.dimension = std::string{*value};
            config.kind = *kind;
        } else if (argument == "--seed") {
            const auto value = next(argument);
            if (!value || !parse_integer(*value, config.seed)) {
                return std::nullopt;
            }
        } else if (argument == "--origin-x") {
            const auto value = next(argument);
            jlong parsed = 0;
            if (!value || !parse_integer(*value, parsed)) {
                return std::nullopt;
            }
            config.origin_x = static_cast<jint>(parsed);
        } else if (argument == "--origin-z") {
            const auto value = next(argument);
            jlong parsed = 0;
            if (!value || !parse_integer(*value, parsed)) {
                return std::nullopt;
            }
            config.origin_z = static_cast<jint>(parsed);
        } else if (argument == "--repeat") {
            const auto value = next(argument);
            jlong parsed = 0;
            if (!value || !parse_integer(*value, parsed) || parsed < 1) {
                return std::nullopt;
            }
            config.repeat = static_cast<jint>(parsed);
        } else if (argument == "--targets") {
            const auto value = next(argument);
            if (!value) {
                return std::nullopt;
            }
            config.targets.clear();
            for (const auto part : split_list(*value)) {
                const auto target = parse_target(part);
                if (!target) {
                    return std::nullopt;
                }
                config.targets.push_back(*target);
            }
        } else if (argument == "--chunks") {
            const auto value = next(argument);
            if (!value) {
                return std::nullopt;
            }
            config.chunk_counts.clear();
            for (const auto part : split_list(*value)) {
                jlong parsed = 0;
                if (!parse_integer(part, parsed) || parsed <= 0) {
                    return std::nullopt;
                }
                config.chunk_counts.push_back(static_cast<jint>(parsed));
            }
        } else if (argument == "--workers") {
            const auto value = next(argument);
            if (!value) {
                return std::nullopt;
            }
            config.worker_counts.clear();
            for (const auto part : split_list(*value)) {
                jlong parsed = 0;
                if (!parse_integer(part, parsed) || parsed <= 0) {
                    return std::nullopt;
                }
                config.worker_counts.push_back(
                    static_cast<std::size_t>(parsed));
            }
        } else if (argument == "--chunk-hash") {
            config.chunk_hash = true;
        } else if (argument == "--stages") {
            config.stages = true;
        } else if (argument == "--output") {
            const auto value = next(argument);
            if (!value) {
                return std::nullopt;
            }
            config.output = *value;
        } else {
            fmt::print(stderr, "bench-chunk-generator: unknown argument {}\n",
                       argument);
            return std::nullopt;
        }
    }
    if (config.chunk_counts.empty() || config.worker_counts.empty() ||
        config.targets.empty()) {
        return std::nullopt;
    }
    return config;
}

} // namespace

auto main(int argc, char **argv) -> int {
    const auto parsed = parse_options(argc, argv);
    if (!parsed) {
        return 0;
    }
    const auto &config = *parsed;
    if (config.data_root.empty()) {
        return skip_message();
    }
    fmt::print("bench-chunk-generator: seed={} biome={} dimension={} "
               "templates={}\n",
               config.seed, config.biome, config.dimension,
               config.templates_root.empty() ? "(none)"
                                             : config.templates_root);

    std::ofstream output;
    std::ostream *stream = nullptr;
    if (!config.output.empty()) {
        output.open(config.output, std::ios::app);
        if (!output) {
            fmt::print(stderr,
                       "bench-chunk-generator: cannot open output file {}\n",
                       config.output);
            return 1;
        }
        stream = &output;
    }

    if (config.chunk_hash) {
        for (const auto target : config.targets) {
            if (!run_chunk_hash(config, target, stream)) {
                return 1;
            }
        }
        return 0;
    }

    if (config.stages) {
        for (const auto target : config.targets) {
            for (const auto chunks : config.chunk_counts) {
                if (!run_stage_scenario(config, target, chunks, stream)) {
                    return 1;
                }
            }
        }
        return 0;
    }

    for (const auto target : config.targets) {
        for (const auto chunks : config.chunk_counts) {
            for (const auto workers : config.worker_counts) {
                const auto result =
                    run_scenario(config, target, chunks, workers, stream);
                if (!result) {
                    return 1;
                }
            }
        }
    }
    const auto rss = read_peak_rss_kib();
    fmt::print("SUMMARY peak_rss_mib={:.1f}\n",
               static_cast<double>(rss) / 1024.0);
    if (stream != nullptr) {
        *stream << fmt::format("SUMMARY peak_rss_mib={:.1f}",
                               static_cast<double>(rss) / 1024.0)
                << '\n';
    }
    return 0;
}
