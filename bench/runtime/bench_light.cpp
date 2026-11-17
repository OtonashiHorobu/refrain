// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Light engine runtime benchmark: generate a deterministic set of overworld
// columns, adopt them into a fresh world and record the initial light pass,
// then run a deterministic block-edit storm and record the incremental light
// updates. The counters come from `sim::light_engine::stats()`, so the numbers
// are the engine's own work rather than a wall-clock estimate.
//
// The benchmark is data-driven: it needs the extracted datapack root in
// `REFRAIN_VANILLA_WORLDGEN` (or `--data-root`) for generation and the content
// table container next to it for block light properties. Without them it
// prints a skip line and exits successfully, so it can live in a CI image that
// does not ship game data.
//
// The block-edit storm places and breaks a torch in the air cell above the
// highest block of the centre column of every chunk. A torch is a light source
// with a fourteen-block reach, so each edit relaxes the open air around it:
// the same deterministic workload on every machine, independent of the terrain
// the seed happens to produce.
//
// Example:
//   REFRAIN_VANILLA_WORLDGEN=content/vanilla/26.3/data
//   ./build/Release/bin/refrain-bench-light --chunks 64 --edits 8 --repeat 3
//
// Numbers measured on a machine that is also compiling or serving are
// indicative only; compare runs from the same idle benchmark host.

#include <refrain/core/io.hpp>
#include <refrain/data/vanilla.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/proto_bridge.hpp>
#include <refrain/world/store.hpp>
#include <refrain/worldgen/chunk_generator.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace refrain;
using clock_type = std::chrono::steady_clock;

/// Highest block Y the benchmark edits in; the overworld build limit is 320.
constexpr std::int32_t overworld_top_y{319};

struct options {
    std::string data_root{};
    std::string tables_path{};
    std::string templates_root{};
    std::string output{};
    /// Fixed biome key. Empty runs the dimension's biome source, like a
    /// production world; a fixed biome makes a smoke run cheaper.
    std::string biome{};
    worldgen::jlong seed{12345};
    /// Columns to generate and adopt, rounded up to the nearest square: the
    /// production generator works on square tiles, so the workload is a square
    /// of `ceil(sqrt(chunks))` by `ceil(sqrt(chunks))` columns.
    std::int32_t chunks{64};
    /// Torch place/break pairs per column.
    std::int32_t edits{8};
    std::int32_t repeat{1};
    std::int32_t origin_x{0};
    std::int32_t origin_z{0};
    /// Edge in chunks of one generated tile. The production runtime uses 8;
    /// a smaller value makes a smoke run cheaper at the cost of less
    /// representative chunk-ready numbers.
    std::int32_t tile_size{8};
    worldgen::generation_target target{worldgen::generation_target::full};
};

[[nodiscard]] auto skip_message(std::string_view reason) -> int {
    fmt::print("bench-light: skipped, {}\n", reason);
    return 0;
}

[[nodiscard]] auto parse_integer(std::string_view text, std::int64_t &out)
    -> bool {
    if (text.empty()) {
        return false;
    }
    const auto *begin = text.data();
    const auto *end = begin + text.size();
    const auto result = std::from_chars(begin, end, out);
    return result.ec == std::errc{} && result.ptr == end;
}

[[nodiscard]] auto parse_target(std::string_view text)
    -> std::optional<worldgen::generation_target> {
    if (text == "terrain") {
        return worldgen::generation_target::terrain;
    }
    if (text == "full") {
        return worldgen::generation_target::full;
    }
    return std::nullopt;
}

/// Edge of the square workload that holds at least `chunks` columns.
[[nodiscard]] auto square_side(std::int32_t chunks) -> std::int32_t {
    std::int32_t side{1};
    while (side * side < chunks) {
        ++side;
    }
    return side;
}

/// The square workload in ascending x then z order: the order the chunk
/// service applies generated results in.
[[nodiscard]] auto make_positions(const options &config)
    -> std::vector<world::chunk_pos> {
    const auto side{square_side(config.chunks)};
    std::vector<world::chunk_pos> positions{};
    positions.reserve(static_cast<std::size_t>(side) *
                      static_cast<std::size_t>(side));
    for (std::int32_t x{0}; x < side; ++x) {
        for (std::int32_t z{0}; z < side; ++z) {
            positions.push_back(world::chunk_pos{.x = config.origin_x + x,
                                                 .z = config.origin_z + z});
        }
    }
    return positions;
}

[[nodiscard]] auto milliseconds(clock_type::duration duration) -> double {
    return std::chrono::duration<double, std::milli>(duration).count();
}

[[nodiscard]] auto nanoseconds(clock_type::duration duration) -> std::uint64_t {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
}

/// Unique scratch directory for one benchmark process.
[[nodiscard]] auto scratch_root() -> std::filesystem::path {
    const auto stamp{clock_type::now().time_since_epoch().count()};
    return std::filesystem::temp_directory_path() /
           fmt::format("refrain_bench_light_{}", stamp);
}

/// True when the state at `pos` is one of the air states. Absent blocks and
/// the three air keys all count as air; every other state is a real block.
[[nodiscard]] auto is_air(const sim::world &owner, const world::block_pos &pos)
    -> bool {
    const auto id{owner.block_at(pos)};
    if (id == sim::air_block) {
        return true;
    }
    const auto key{owner.registry().key(id)};
    return key == "minecraft:cave_air" || key == "minecraft:void_air";
}

/// Y of the air cell above the highest real block of the centre column of one
/// chunk. The benchmark places its torch there, so the edit always happens in
/// open air regardless of what the seed generated.
[[nodiscard]] auto edit_height(sim::world &owner, const world::chunk_pos &pos)
    -> std::int16_t {
    const auto *column{owner.find_chunk(pos)};
    if (column == nullptr) {
        return 100;
    }
    const auto base_x{pos.x * world::section_edge + 8};
    const auto base_z{pos.z * world::section_edge + 8};
    const auto top{column->sections.empty()
                       ? 0
                       : static_cast<std::int32_t>(column->sections.back().y) *
                                 world::section_edge +
                             15};
    for (auto y{std::min(top, overworld_top_y)}; y >= column->y_pos * 16; --y) {
        const world::block_pos cell{
            .x = base_x, .y = static_cast<std::int16_t>(y), .z = base_z};
        if (!is_air(owner, cell)) {
            return static_cast<std::int16_t>(std::min(y + 1, top));
        }
    }
    return 100;
}

struct emit_fn {
    std::ostream *stream{nullptr};

    void operator()(const std::string &line) const {
        fmt::print("{}\n", line);
        if (stream != nullptr) {
            *stream << line << '\n';
        }
    }
};

/// One repeat of the workload. Returns false when generation or adoption
/// failed, which the caller reports as an error rather than a skipped run.
[[nodiscard]] auto run_repeat(const options &config,
                              worldgen::chunk_generator &generator,
                              const data::vanilla_tables &tables,
                              std::span<const world::chunk_pos> positions,
                              std::int32_t repeat_index,
                              const std::filesystem::path &root,
                              const emit_fn &emit) -> bool {
    auto created{world::world_store::create(root.string())};
    if (!created) {
        fmt::print(stderr, "bench-light: cannot create the world store: {}\n",
                   created.error().message);
        return false;
    }
    auto owner{sim::world::attach(*created)};
    owner.set_block_light_table(sim::block_light_table::from_tables(tables));
    const auto torch{owner.registry().intern("minecraft:torch")};
    const auto air{sim::air_block};

    // --- initial light ----------------------------------------------------
    // The production runtime generates whole tiles and adopts the columns in
    // ascending position order; the workload does the same with one worker so
    // the light cost is the only variable.
    owner.light().reset_stats();
    const auto load_begin{clock_type::now()};
    const auto side{square_side(config.chunks)};
    auto protos{generator.generate_area_tiled(
        worldgen::chunk_pos{.x = config.origin_x, .z = config.origin_z}, side,
        side, 1)};
    const auto generate_wall{clock_type::now() - load_begin};
    if (!protos) {
        fmt::print(stderr, "bench-light: tile generation failed: {}\n",
                   protos.error().message);
        return false;
    }
    if (protos->size() != positions.size()) {
        fmt::print(stderr, "bench-light: generated {} columns, expected {}\n",
                   protos->size(), positions.size());
        return false;
    }
    const auto convert_begin{clock_type::now()};
    std::vector<std::pair<world::chunk_pos, world::chunk>> columns{};
    columns.reserve(protos->size());
    for (auto &proto : *protos) {
        if (proto == nullptr) {
            fmt::print(stderr, "bench-light: the generator returned an empty "
                               "column\n");
            return false;
        }
        const world::chunk_pos position{.x = proto->chunk_x(),
                                        .z = proto->chunk_z()};
        auto column{world::from_proto_chunk(*proto, generator.biome_names())};
        if (!column) {
            fmt::print(stderr, "bench-light: chunk conversion failed: {}\n",
                       column.error().message);
            return false;
        }
        columns.emplace_back(position, std::move(*column));
    }
    const auto convert_wall{clock_type::now() - convert_begin};
    // The chunk service applies generated columns in ascending position order,
    // never in completion order.
    std::sort(columns.begin(), columns.end(),
              [](const auto &left, const auto &right) {
                  return left.first < right.first;
              });
    const auto adopt_begin{clock_type::now()};
    for (auto &[position, column] : columns) {
        auto adopted{owner.insert_chunk(position, std::move(column))};
        if (!adopted) {
            fmt::print(stderr, "bench-light: chunk adoption failed: {}\n",
                       adopted.error().message);
            return false;
        }
    }
    const auto adopt_wall{clock_type::now() - adopt_begin};
    const auto chunk_ready_ns{
        nanoseconds(generate_wall + convert_wall + adopt_wall)};
    const auto initial{owner.light().stats()};
    const auto column_count{static_cast<double>(positions.size())};
    emit(fmt::format(
        "RESULT scenario=initial_light repeat={} seed={} chunks={} tile={} "
        "target={} columns_lit={} initial_drain_calls={} initial_nodes={} "
        "initial_nodes_per_column={:.1f} initial_ns={} "
        "initial_ns_per_column={:.0f} initial_peak_pending={} "
        "generate_ms={:.1f} convert_ms={:.1f} adopt_ms={:.1f} "
        "chunk_ready_ns={} chunk_ready_ms={:.1f} "
        "chunk_ready_ms_per_column={:.2f}",
        repeat_index, config.seed, positions.size(), config.tile_size,
        config.target == worldgen::generation_target::terrain ? "terrain"
                                                              : "full",
        initial.columns_lit, initial.initial_drain_calls, initial.initial_nodes,
        static_cast<double>(initial.initial_nodes) / column_count,
        initial.initial_ns,
        static_cast<double>(initial.initial_ns) / column_count,
        initial.pending_peak, milliseconds(generate_wall),
        milliseconds(convert_wall), milliseconds(adopt_wall), chunk_ready_ns,
        static_cast<double>(chunk_ready_ns) / 1.0e6,
        static_cast<double>(chunk_ready_ns) / 1.0e6 / column_count));

    // --- deterministic block-edit storm ----------------------------------
    std::vector<std::int16_t> edit_ys{};
    edit_ys.reserve(positions.size());
    for (const auto &position : positions) {
        edit_ys.push_back(edit_height(owner, position));
    }
    owner.light().reset_stats();
    const auto edit_begin{clock_type::now()};
    std::uint64_t applied{0};
    for (std::size_t index{0}; index < positions.size(); ++index) {
        const auto &position = positions[index];
        const world::block_pos cell{.x = position.x * world::section_edge + 8,
                                    .y = edit_ys[index],
                                    .z = position.z * world::section_edge + 8};
        for (std::int32_t edit{0}; edit < config.edits; ++edit) {
            // Place on even edits and break on odd ones, so every edit changes
            // the cell and the storm covers both the increase and the decrease
            // path.
            const auto value{edit % 2 == 0 ? torch : air};
            auto status{owner.set_block(cell, value)};
            if (!status) {
                fmt::print(stderr,
                           "bench-light: block edit failed at ({}, {}, {}): "
                           "{}\n",
                           cell.x, cell.y, cell.z, status.error().message);
                return false;
            }
            ++applied;
        }
    }
    const auto edit_wall{clock_type::now() - edit_begin};
    const auto edits{owner.light().stats()};
    const auto edit_count{static_cast<double>(applied)};
    emit(fmt::format(
        "RESULT scenario=edit_storm repeat={} seed={} chunks={} "
        "edits_per_column="
        "{} edits={} block_edits={} drain_calls={} drain_nodes={} "
        "drain_nodes_per_edit={:.1f} drain_ns={} drain_ns_per_edit={:.0f} "
        "pending_peak={} edit_wall_ms={:.1f}",
        repeat_index, config.seed, positions.size(), config.edits, applied,
        edits.block_edits, edits.drain_calls, edits.drain_nodes,
        static_cast<double>(edits.drain_nodes) / edit_count, edits.drain_ns,
        static_cast<double>(edits.drain_ns) / edit_count, edits.pending_peak,
        milliseconds(edit_wall)));

    // --- tick backstop ----------------------------------------------------
    // Every block edit drained its update inline, so the backstop the tick
    // runs for work queued outside a tick has nothing left to process.
    const auto before_tick{owner.light().stats()};
    owner.tick();
    const auto after_tick{owner.light().stats()};
    emit(fmt::format(
        "RESULT scenario=tick_backstop repeat={} seed={} chunks={} "
        "tick_drain_calls={} tick_drain_nodes={} pending={}",
        repeat_index, config.seed, positions.size(),
        after_tick.drain_calls - before_tick.drain_calls,
        after_tick.drain_nodes - before_tick.drain_nodes, after_tick.pending));
    return true;
}

[[nodiscard]] auto parse_options(int argc, char **argv)
    -> std::optional<options> {
    options config{};
    if (const char *root = std::getenv("REFRAIN_VANILLA_WORLDGEN");
        root != nullptr) {
        config.data_root = root;
    }
    if (const char *templates = std::getenv("REFRAIN_VANILLA_STRUCTURES");
        templates != nullptr) {
        config.templates_root = templates;
    }
    if (const char *tables = std::getenv("REFRAIN_VANILLA_TABLES");
        tables != nullptr) {
        config.tables_path = tables;
    }
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        const auto next =
            [&](std::string_view name) -> std::optional<std::string_view> {
            if (index + 1 >= argc) {
                fmt::print(stderr, "bench-light: {} needs a value\n", name);
                return std::nullopt;
            }
            return std::string_view{argv[++index]};
        };
        if (argument == "--help" || argument == "-h") {
            fmt::print(
                "refrain-bench-light: light engine benchmark\n"
                "  --data-root <path>       datapack data root (default: env "
                "REFRAIN_VANILLA_WORLDGEN)\n"
                "  --tables <path>          content table container (default: "
                "env REFRAIN_VANILLA_TABLES, else <data-root>/../tables.rvtb)\n"
                "  --templates-root <path>  template root (default: env "
                "REFRAIN_VANILLA_STRUCTURES)\n"
                "  --biome <key>            fixed biome (default: the "
                "dimension biome source)\n"
                "  --seed <n>               world seed (default 12345)\n"
                "  --chunks <n>             columns to generate and adopt, "
                "rounded up to a square (default 64)\n"
                "  --tile-size <n>          generator tile edge in chunks "
                "(default 8, the production unit)\n"
                "  --edits <n>              torch place/break edits per column "
                "(default 8)\n"
                "  --repeat <n>             workload repeats, each on a fresh "
                "world (default 1)\n"
                "  --origin-x/--origin-z    area origin in chunks (default 0)\n"
                "  --target <name>          terrain or full (default full)\n"
                "  --output <path>          append the result lines here\n");
            return std::nullopt;
        }
        if (argument == "--data-root") {
            const auto value = next(argument);
            if (!value) {
                return std::nullopt;
            }
            config.data_root = *value;
        } else if (argument == "--tables") {
            const auto value = next(argument);
            if (!value) {
                return std::nullopt;
            }
            config.tables_path = *value;
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
        } else if (argument == "--seed") {
            const auto value = next(argument);
            std::int64_t parsed{0};
            if (!value || !parse_integer(*value, parsed)) {
                return std::nullopt;
            }
            config.seed = parsed;
        } else if (argument == "--chunks") {
            const auto value = next(argument);
            std::int64_t parsed{0};
            if (!value || !parse_integer(*value, parsed) || parsed < 1 ||
                parsed > 1024) {
                return std::nullopt;
            }
            config.chunks = static_cast<std::int32_t>(parsed);
        } else if (argument == "--edits") {
            const auto value = next(argument);
            std::int64_t parsed{0};
            if (!value || !parse_integer(*value, parsed) || parsed < 1 ||
                parsed > 4096) {
                return std::nullopt;
            }
            config.edits = static_cast<std::int32_t>(parsed);
        } else if (argument == "--tile-size") {
            const auto value = next(argument);
            std::int64_t parsed{0};
            if (!value || !parse_integer(*value, parsed) || parsed < 1 ||
                parsed > 64) {
                return std::nullopt;
            }
            config.tile_size = static_cast<std::int32_t>(parsed);
        } else if (argument == "--repeat") {
            const auto value = next(argument);
            std::int64_t parsed{0};
            if (!value || !parse_integer(*value, parsed) || parsed < 1 ||
                parsed > 100) {
                return std::nullopt;
            }
            config.repeat = static_cast<std::int32_t>(parsed);
        } else if (argument == "--origin-x") {
            const auto value = next(argument);
            std::int64_t parsed{0};
            if (!value || !parse_integer(*value, parsed)) {
                return std::nullopt;
            }
            config.origin_x = static_cast<std::int32_t>(parsed);
        } else if (argument == "--origin-z") {
            const auto value = next(argument);
            std::int64_t parsed{0};
            if (!value || !parse_integer(*value, parsed)) {
                return std::nullopt;
            }
            config.origin_z = static_cast<std::int32_t>(parsed);
        } else if (argument == "--target") {
            const auto value = next(argument);
            const auto target = value ? parse_target(*value) : std::nullopt;
            if (!target) {
                return std::nullopt;
            }
            config.target = *target;
        } else if (argument == "--output") {
            const auto value = next(argument);
            if (!value) {
                return std::nullopt;
            }
            config.output = *value;
        } else {
            fmt::print(stderr, "bench-light: unknown argument {}\n", argument);
            return std::nullopt;
        }
    }
    if (config.tables_path.empty() && !config.data_root.empty()) {
        config.tables_path =
            (std::filesystem::path{config.data_root} / ".." / "tables.rvtb")
                .lexically_normal()
                .string();
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
        return skip_message(
            "REFRAIN_VANILLA_WORLDGEN is not set and --data-root is absent");
    }
    if (config.tables_path.empty()) {
        return skip_message("the content table container path is unknown");
    }
    if (!core::io::exists(config.tables_path)) {
        return skip_message(
            fmt::format("the content table container {} does not exist",
                        config.tables_path));
    }
    auto tables_bytes{core::io::read_file(config.tables_path)};
    if (!tables_bytes) {
        return skip_message(
            fmt::format("the content table container {} cannot be read: {}",
                        config.tables_path, tables_bytes.error().message));
    }
    auto tables{data::vanilla_tables::open(*tables_bytes)};
    if (!tables) {
        return skip_message(
            fmt::format("the content table container {} is invalid: {}",
                        config.tables_path, tables.error().message));
    }
    if (sim::block_light_table::from_tables(*tables).empty()) {
        return skip_message(
            fmt::format("the content table container {} carries no block "
                        "light data",
                        config.tables_path));
    }
    auto created{
        worldgen::chunk_generator::create(worldgen::chunk_generator_config{
            .data_root = config.data_root,
            .templates_root = config.templates_root,
            .seed = config.seed,
            .kind = worldgen::dimension_kind::overworld,
            .fixed_biome = config.biome,
            .target = config.target,
            .tile_size = config.tile_size,
        })};
    if (!created) {
        fmt::print(stderr, "bench-light: chunk generator creation failed: {}\n",
                   created.error().message);
        return 1;
    }
    auto generator = std::move(*created);

    std::ofstream output{};
    std::ostream *stream{nullptr};
    if (!config.output.empty()) {
        output.open(config.output, std::ios::app);
        if (!output) {
            fmt::print(stderr, "bench-light: cannot open output file {}\n",
                       config.output);
            return 1;
        }
        stream = &output;
    }
    const emit_fn emit{.stream = stream};
    fmt::print("bench-light: seed={} biome={} chunks={} tile={} edits={} "
               "repeat={} target={} tables={}\n",
               config.seed,
               config.biome.empty() ? "(dimension source)" : config.biome,
               config.chunks, config.tile_size, config.edits, config.repeat,
               config.target == worldgen::generation_target::terrain ? "terrain"
                                                                     : "full",
               config.tables_path);

    const auto positions{make_positions(config)};
    const auto root{scratch_root()};
    bool ok{true};
    for (std::int32_t repeat{0}; repeat < config.repeat; ++repeat) {
        const auto repeat_root{root / fmt::format("repeat-{}", repeat)};
        if (!run_repeat(config, *generator, *tables, positions, repeat,
                        repeat_root, emit)) {
            ok = false;
            break;
        }
    }
    std::error_code error{};
    std::filesystem::remove_all(root, error);
    return ok ? 0 : 1;
}
