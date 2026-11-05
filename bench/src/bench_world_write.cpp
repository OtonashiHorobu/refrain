// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Benchmark for chunk-record write cost: full chunk-record writes versus a
// runtime partial edit. The benchmark deliberately separates the two paths:
//   * `world_store::write_chunk` always re-encodes/compresses a complete chunk
//     record (the full-record baseline);
//   * `sim::world::write_pending` encodes only the dirty section after a
//     `set_block` edit (clean sections are reused) and commits the region.
//
// Run with `refrain-bench --benchmark_filter=world_write`. The results inform
// whether record-level delta is worth a format change; they are not a CI
// pass/fail threshold.

#include <refrain/sim/world.hpp>
#include <refrain/world/chunk_codec.hpp>
#include <refrain/world/store.hpp>

#include <benchmark/benchmark.h>

#include <fmt/format.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

namespace {

namespace fs = std::filesystem;
namespace sim = refrain::sim;
namespace world = refrain::world;

constexpr std::int32_t section_count{8};
constexpr std::int32_t section_volume{4096};

[[nodiscard]] auto scratch_directory(std::string_view name) -> fs::path {
    static std::uint64_t sequence{0};
    const auto stamp{static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count())};
    auto path{
        fs::temp_directory_path() /
        fmt::format("refrain-bench-world-{}-{}-{}", name, stamp, sequence++)};
    std::error_code error;
    fs::create_directories(path, error);
    return path;
}

[[nodiscard]] auto make_chunk() -> world::chunk {
    world::chunk chunk{};
    chunk.format_version = world::chunk_format_version;
    chunk.status = world::chunk_status::full;
    chunk.y_pos = 0;
    for (std::int32_t index{0}; index < section_count; ++index) {
        world::section_record section{};
        section.y = static_cast<std::int8_t>(index);
        section.kind = world::section_kind::single_value;
        section.bits_per_entry = 0;
        section.block_count = static_cast<std::uint16_t>(section_volume);
        section.biome_kind = world::section_kind::single_value;
        section.biome_bits_per_entry = 0;
        section.biome_palette.push_back("minecraft:plains");
        section.palette.push_back(world::palette_entry{
            .key = "minecraft:stone",
            .count = static_cast<std::uint16_t>(section_volume),
            .state = 0,
        });
        chunk.sections.push_back(std::move(section));
    }
    return chunk;
}

class runtime_write_fixture : public benchmark::Fixture {
  public:
    void SetUp(const benchmark::State &) override {
        root = scratch_directory("runtime");
        auto store{world::world_store::create(root.string())};
        if (!store) {
            return;
        }
        auto written{store->write_chunk(world::chunk_pos{0, 0}, make_chunk())};
        if (!written) {
            return;
        }
        static_cast<void>(store->commit());
        // Build the same storage chunk a one-section edit produces; the
        // full-store benchmark re-encodes all of it, the runtime benchmark
        // reuses the clean decoded sections.
        auto runtime{sim::world::open(root.string())};
        if (!runtime) {
            return;
        }
        if (auto ensured{runtime->ensure_chunk({0, 0})}; !ensured) {
            return;
        }
        const auto dirt{runtime->registry().intern("minecraft:dirt")};
        if (auto edited{runtime->set_block({0, 2, 3}, dirt)}; !edited) {
            return;
        }
        const auto pending{runtime->snapshot_pending()};
        if (pending.size() != 1U) {
            return;
        }
        auto encoded{runtime->encode_snapshot(pending.front())};
        if (!encoded) {
            return;
        }
        full_chunk = std::move(encoded->chunk);
    }

    void TearDown(const benchmark::State &) override {
        std::error_code error;
        fs::remove_all(root, error);
    }

    fs::path root{};
    std::optional<world::chunk> full_chunk{};
};

} // namespace

// Encoding-only comparison: `world::encode_chunk` always serializes the whole
// chunk from the storage model, while `sim::world::encode_snapshot` reuses the
// decoded record for clean sections and rebuilds only the dirty one.
class encode_fixture : public benchmark::Fixture {
  public:
    void TearDown(const benchmark::State &) override {
        pending.clear();
        world.reset();
        std::error_code error;
        fs::remove_all(root, error);
    }

  protected:
    void prepare(std::int32_t dirty_sections) {
        root = scratch_directory("encode");
        auto store{world::world_store::create(root.string())};
        if (!store) {
            return;
        }
        static_cast<void>(
            store->write_chunk(world::chunk_pos{0, 0}, make_chunk()));
        static_cast<void>(store->commit());
        auto opened{sim::world::open(root.string())};
        if (!opened) {
            return;
        }
        world = std::make_unique<sim::world>(std::move(*opened));
        if (auto ensured{world->ensure_chunk({0, 0})}; !ensured) {
            return;
        }
        const auto dirt{world->registry().intern("minecraft:dirt")};
        for (std::int32_t section{0}; section < dirty_sections; ++section) {
            if (auto edited{world->set_block(
                    {0, static_cast<std::int16_t>(section * 16 + 2), 3}, dirt)};
                !edited) {
                return;
            }
        }
        pending = world->snapshot_pending();
    }

    fs::path root{};
    std::unique_ptr<sim::world> world{};
    std::vector<sim::pending_snapshot> pending{};
};

class full_encode_fixture : public encode_fixture {
  public:
    void SetUp(const benchmark::State &) override { prepare(section_count); }
};

class partial_encode_fixture : public encode_fixture {
  public:
    void SetUp(const benchmark::State &) override { prepare(1); }
};

BENCHMARK_F(full_encode_fixture, world_encode_full_runtime)
(benchmark::State &state) {
    if (!world || pending.size() != 1U) {
        state.SkipWithError("full encode fixture setup failed");
        return;
    }
    for (auto iteration : state) {
        static_cast<void>(iteration);
        auto encoded{world->encode_snapshot(pending.front())};
        if (!encoded) {
            state.SkipWithError("encode_snapshot failed");
            return;
        }
        benchmark::DoNotOptimize(encoded->chunk.sections.size());
    }
}

BENCHMARK_F(partial_encode_fixture, world_encode_partial_reuse)
(benchmark::State &state) {
    if (!world || pending.size() != 1U) {
        state.SkipWithError("partial encode fixture setup failed");
        return;
    }
    for (auto iteration : state) {
        static_cast<void>(iteration);
        auto encoded{world->encode_snapshot(pending.front())};
        if (!encoded) {
            state.SkipWithError("encode_snapshot failed");
            return;
        }
        benchmark::DoNotOptimize(encoded->chunk.sections.size());
    }
}

BENCHMARK_F(runtime_write_fixture, world_write_full_store)
(benchmark::State &state) {
    for (auto iteration : state) {
        static_cast<void>(iteration);
        auto store{world::world_store::open(root.string())};
        if (!store) {
            state.SkipWithError("world_store::open failed");
            return;
        }
        if (!full_chunk.has_value()) {
            state.SkipWithError("full chunk fixture setup failed");
            return;
        }
        auto written{store->write_chunk(world::chunk_pos{1, 1}, *full_chunk)};
        if (!written) {
            state.SkipWithError("write_chunk failed");
            return;
        }
        if (auto committed{store->commit()}; !committed) {
            state.SkipWithError("commit failed");
            return;
        }
    }
}

BENCHMARK_F(runtime_write_fixture, world_write_partial_runtime)
(benchmark::State &state) {
    for (auto iteration : state) {
        static_cast<void>(iteration);
        auto runtime{sim::world::open(root.string())};
        if (!runtime) {
            state.SkipWithError("sim::world::open failed");
            return;
        }
        if (auto ensured{runtime->ensure_chunk({0, 0})}; !ensured) {
            state.SkipWithError("ensure_chunk failed");
            return;
        }
        const auto dirt{runtime->registry().intern("minecraft:dirt")};
        if (auto edited{runtime->set_block({0, 2, 3}, dirt)}; !edited) {
            state.SkipWithError("set_block failed");
            return;
        }
        auto pending{runtime->snapshot_pending()};
        auto written{runtime->write_pending(pending)};
        if (!written) {
            state.SkipWithError("write_pending failed");
            return;
        }
        runtime->mark_clean(*written);
    }
}
