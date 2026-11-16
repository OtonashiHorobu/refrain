// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Player projection read cost (Google Benchmark 1.9.5).
//
// The runtime reads the whole player table once per tick through the world
// port: `player_count()` plus one indexed `player_at()` per record. This
// benchmark builds a contract composition over a thousand-player table and
// measures both the raw projection read and the session store refresh it
// feeds, so the number can be compared with the 50ms tick budget.
//
// Run with:
//   ./build/Release/bin/refrain-bench --benchmark_filter=player_projection
//
// A number from a local iteration build (LTO off, baseline architecture) is
// indicative only; benchmark-role numbers come from the benchmark host.

#include <refrain/contracts/abi.hpp>
#include <refrain/contracts/services/sim_abi.h>
#include <refrain/contracts/services/storage_abi.h>
#include <refrain/contracts/services/world_abi.h>
#include <refrain/server/player_store.hpp>
#include <refrain/server/world_port.hpp>
#include <refrain/session/player_store.hpp>
#include <refrain/sim/player.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/create.hpp>
#include <refrain/world/store.hpp>

#include <benchmark/benchmark.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

namespace contracts = refrain::contracts;
namespace server = refrain::server;
namespace sim = refrain::sim;
namespace world = refrain::world;

constexpr std::size_t player_count = 1000;

struct fake_provider {
    sim::world *world{nullptr};
    std::uint64_t next_handle{1};
};

refrain_status
fake_storage_open(void *self,
                  const refrain_services_storage_world_open_config *config,
                  refrain_services_storage_storage_handle *out) {
    if (self == nullptr || config == nullptr || out == nullptr) {
        return REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_OPEN_FAILED;
    }
    auto &provider{*static_cast<fake_provider *>(self)};
    *out = refrain_services_storage_storage_handle{};
    out->struct_size = REFRAIN_SERVICES_STORAGE_STORAGE_HANDLE_SIZE;
    out->world = provider.next_handle++;
    return REFRAIN_STATUS_OK;
}

refrain_status fake_storage_close(void *, refrain_core_handle_storage_world) {
    return REFRAIN_STATUS_OK;
}

refrain_status fake_storage_read(void *, refrain_core_handle_storage_world,
                                 const refrain_services_storage_chunk_key *,
                                 refrain_core_chunk_buffer *,
                                 refrain_services_storage_chunk_read_result *) {
    return REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_NOT_FOUND;
}

refrain_status fake_storage_write(void *, refrain_core_handle_storage_world,
                                  const refrain_services_storage_chunk_key *,
                                  refrain_bytes) {
    return REFRAIN_STATUS_OK;
}

refrain_status fake_storage_sync(void *, refrain_core_handle_storage_world) {
    return REFRAIN_STATUS_OK;
}

std::uint8_t fake_storage_is_open(void *, refrain_core_handle_storage_world) {
    return 1;
}

refrain_status fake_world_open(void *,
                               const refrain_services_world_world_config *) {
    return REFRAIN_STATUS_OK;
}

refrain_status fake_world_close(void *) { return REFRAIN_STATUS_OK; }

refrain_status fake_sim_open(void *self,
                             const refrain_services_sim_sim_config *config,
                             refrain_services_sim_handle_sim_world *out) {
    if (self == nullptr || config == nullptr || out == nullptr) {
        return REFRAIN_SERVICES_SIM_SIM_ERROR_INVALID_HANDLE;
    }
    auto &provider{*static_cast<fake_provider *>(self)};
    *out = provider.next_handle++;
    return REFRAIN_STATUS_OK;
}

refrain_status fake_sim_close(void *, refrain_services_sim_handle_sim_world) {
    return REFRAIN_STATUS_OK;
}

std::uint32_t fake_sim_player_count(void *self,
                                    refrain_services_sim_handle_sim_world) {
    auto *provider{static_cast<fake_provider *>(self)};
    return provider == nullptr
               ? 0U
               : static_cast<std::uint32_t>(provider->world->players().size());
}

refrain_status fake_sim_player_at(void *self,
                                  refrain_services_sim_handle_sim_world,
                                  std::uint32_t index,
                                  refrain_services_sim_player_info *out) {
    auto *provider{static_cast<fake_provider *>(self)};
    if (provider == nullptr || out == nullptr) {
        return REFRAIN_SERVICES_SIM_SIM_ERROR_INVALID_ARGUMENT;
    }
    const auto *record{provider->world->players().nth(index)};
    if (record == nullptr) {
        return REFRAIN_SERVICES_SIM_SIM_ERROR_OUT_OF_BOUNDS;
    }
    out->struct_size = REFRAIN_SERVICES_SIM_PLAYER_INFO_SIZE;
    out->connection = record->connection;
    out->profile_id = contracts::to_borrowed(std::span<const std::uint8_t>{
        record->profile_id.data(), record->profile_id.size()});
    out->name = contracts::to_borrowed(std::string_view{record->name});
    out->world = contracts::to_borrowed(std::string_view{record->world});
    out->entity_id = record->id;
    out->x = record->x;
    out->y = record->y;
    out->z = record->z;
    out->yaw = record->yaw;
    out->pitch = record->pitch;
    out->on_ground = record->on_ground ? 1 : 0;
    out->game_mode = record->game_mode;
    out->health = record->health;
    out->view_distance = record->view_distance;
    return REFRAIN_STATUS_OK;
}

const refrain_services_sim_vtable fake_sim_table{
    .struct_size = REFRAIN_SERVICES_SIM_VTABLE_SIZE,
    .abi_major = REFRAIN_SERVICES_SIM_ABI_MAJOR,
    .abi_minor = REFRAIN_SERVICES_SIM_ABI_MINOR,
    .abi_patch = REFRAIN_SERVICES_SIM_ABI_PATCH,
    .open_world = &fake_sim_open,
    .close_world = &fake_sim_close,
    .tick = nullptr,
    .ticks = nullptr,
    .state_hash = nullptr,
    .set_blocks = nullptr,
    .spawn_entity = nullptr,
    .remove_entity = nullptr,
    .player_count = &fake_sim_player_count,
    .player_at = &fake_sim_player_at,
    .player_by_connection = nullptr,
    .add_player = nullptr,
    .remove_player = nullptr,
    .set_player_transform = nullptr,
    .set_player_position = nullptr,
    .set_player_view_distance = nullptr,
    .move_player = nullptr,
    .jump = nullptr,
};

const refrain_services_world_vtable fake_world_table{
    .struct_size = REFRAIN_SERVICES_WORLD_VTABLE_SIZE,
    .abi_major = REFRAIN_SERVICES_WORLD_ABI_MAJOR,
    .abi_minor = REFRAIN_SERVICES_WORLD_ABI_MINOR,
    .abi_patch = REFRAIN_SERVICES_WORLD_ABI_PATCH,
    .open_world = &fake_world_open,
    .close_world = &fake_world_close,
    .chunk_at = nullptr,
    .block_at = nullptr,
    .height_at = nullptr,
    .is_chunk_loaded = nullptr,
    .apply_block_changes = nullptr,
    .world_tick = nullptr,
    .level = nullptr,
    .zstd_level = nullptr,
    .set_zstd_level = nullptr,
    .set_zstd_policy = nullptr,
};

const refrain_services_storage_vtable fake_storage_table{
    .struct_size = REFRAIN_SERVICES_STORAGE_VTABLE_SIZE,
    .abi_major = REFRAIN_SERVICES_STORAGE_ABI_MAJOR,
    .abi_minor = REFRAIN_SERVICES_STORAGE_ABI_MINOR,
    .abi_patch = REFRAIN_SERVICES_STORAGE_ABI_PATCH,
    .open_world = &fake_storage_open,
    .close_world = &fake_storage_close,
    .read_chunk = &fake_storage_read,
    .write_chunk = &fake_storage_write,
    .sync = &fake_storage_sync,
    .is_open = &fake_storage_is_open,
};

[[nodiscard]] auto scratch_directory() -> std::filesystem::path {
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    auto path{std::filesystem::temp_directory_path() /
              ("refrain-bench-players-" + std::to_string(stamp))};
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return path;
}

[[nodiscard]] auto open_bench_world(const std::filesystem::path &directory)
    -> std::optional<sim::world> {
    static_cast<void>(world::create_world(directory.string(), 42, "world"));
    auto created{sim::world::open(directory.string())};
    if (!created) {
        return std::nullopt;
    }
    return std::optional<sim::world>{std::move(*created)};
}

/// One opened world with a thousand registered players and the port that
/// projects it. The fixture owns all of it; the timing loop only reads.
struct bench_world {
    std::filesystem::path directory{scratch_directory()};
    std::optional<sim::world> opened{open_bench_world(directory)};
    fake_provider provider{};
    std::unique_ptr<server::world_port> port{};
    std::unique_ptr<refrain::session::player_store> store{};

    bench_world() {
        if (!opened.has_value()) {
            return;
        }
        provider.world = &*opened;
        for (std::size_t index{0}; index < player_count; ++index) {
            static_cast<void>(
                opened->add_player(sim::player_table::registration{
                    .connection = index + 1,
                    .name = "bench-" + std::to_string(index),
                    .world = "overworld",
                    .x = static_cast<double>(index),
                    .y = 64.0,
                    .z = static_cast<double>(index) * 2.0,
                    .game_mode = 0,
                    .inventory_slots = 36,
                }));
        }
        const server::server_service_handles handles{
            .world = &fake_world_table,
            .world_self = &provider,
            .sim = &fake_sim_table,
            .sim_self = &provider,
            .storage = &fake_storage_table,
            .storage_self = &provider,
            .world_provider = "bench.world",
            .sim_provider = "bench.sim",
            .storage_provider = "bench.storage",
        };
        port = server::make_world_port(handles);
        if (port != nullptr) {
            static_cast<void>(port->open(directory.string()));
            store = server::make_world_port_player_store(*port);
        }
    }

    ~bench_world() {
        if (port != nullptr) {
            port->close();
        }
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    bench_world(const bench_world &) = delete;
    auto operator=(const bench_world &) -> bench_world & = delete;
    bench_world(bench_world &&) = delete;
    auto operator=(bench_world &&) -> bench_world & = delete;
};

void bench_player_projection(benchmark::State &state) {
    bench_world bench{};
    if (bench.port == nullptr) {
        state.SkipWithError("the player projection bench world did not open");
        return;
    }
    state.SetLabel("contract port, 1000 players");
    for (auto _ : state) {
        const auto count{bench.port->player_count()};
        if (!count) {
            state.SkipWithError(count.error().message.c_str());
            return;
        }
        std::uint64_t checksum{0};
        for (std::uint32_t index{0}; index < *count; ++index) {
            const auto projected{bench.port->player_at(index)};
            if (!projected) {
                state.SkipWithError(projected.error().message.c_str());
                return;
            }
            checksum += projected->entity_id + projected->name.size();
        }
        benchmark::DoNotOptimize(checksum);
    }
    state.SetItemsProcessed(state.iterations() *
                            static_cast<std::int64_t>(player_count));
}
BENCHMARK(bench_player_projection)->Unit(benchmark::kMicrosecond);

void bench_player_store_refresh(benchmark::State &state) {
    bench_world bench{};
    if (bench.store == nullptr) {
        state.SkipWithError("the player projection bench world did not open");
        return;
    }
    state.SetLabel("contract store refresh, 1000 players");
    for (auto _ : state) {
        const auto refreshed{bench.store->refresh()};
        if (!refreshed) {
            state.SkipWithError(refreshed.error().message.c_str());
            return;
        }
        benchmark::DoNotOptimize(bench.store->size());
    }
    state.SetItemsProcessed(state.iterations() *
                            static_cast<std::int64_t>(player_count));
}
BENCHMARK(bench_player_store_refresh)->Unit(benchmark::kMicrosecond);

} // namespace
