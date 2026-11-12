// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// `refrain:world` provider for `refrain.world.vanilla`.
//
// The process hosts one world, so the contract is a singleton: the provider
// owns an optional `sim::world` attached by `open_world()` to the store the
// runtime opened once. A world view never opens the level directory itself, so
// every provider of a composition shares one store. Chunk views are borrowed
// from `sections_` and valid only for the call that produced them.

#include <refrain/contracts/abi.hpp>
#include <refrain/contracts/services/world_abi.h>

#include <refrain/core/error.hpp>
#include <refrain/host/abi_bridge.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/mod/static_mod.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/store.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace abi = refrain::host::abi;

/// Status rows for the codes the `refrain:world` contract distinguishes;
/// every other error is an internal failure.
constexpr abi::status_entry world_statuses[]{
    {refrain::error_code::not_found,
     REFRAIN_SERVICES_WORLD_WORLD_ERROR_CHUNK_NOT_LOADED},
    {refrain::error_code::invalid_argument,
     REFRAIN_SERVICES_WORLD_WORLD_ERROR_OUT_OF_BOUNDS},
    {refrain::error_code::unsupported,
     REFRAIN_SERVICES_WORLD_WORLD_ERROR_NOT_IMPLEMENTED},
};

struct world_provider {
    std::optional<refrain::sim::world> world{};
    std::vector<refrain_core_chunk_section_view> sections{};

    static auto open_world(void *self,
                           const refrain_services_world_world_config *config)
        -> refrain_status {
        auto *provider{static_cast<world_provider *>(self)};
        if (provider == nullptr || config == nullptr ||
            config->struct_size != REFRAIN_SERVICES_WORLD_WORLD_CONFIG_SIZE) {
            return REFRAIN_SERVICES_WORLD_WORLD_ERROR_OPEN_FAILED;
        }
        if (provider->world.has_value()) {
            return REFRAIN_SERVICES_WORLD_WORLD_ERROR_ALREADY_OPEN;
        }
        auto *store{
            static_cast<refrain::world::world_store *>(config->storage.store)};
        if (store == nullptr) {
            // TODO: adopt chunks through the `refrain:storage` ABI so a
            // storage provider that keeps its store private can host this
            // world; today the view needs the in-process store and refuses
            // with the named error instead of opening the directory itself.
            return REFRAIN_SERVICES_WORLD_WORLD_ERROR_STORAGE_UNAVAILABLE;
        }
        auto opened{refrain::sim::world::attach(*store)};
        opened.seed(static_cast<std::uint64_t>(config->seed));
        provider->world.emplace(std::move(opened));
        return REFRAIN_STATUS_OK;
    }

    static auto close_world(void *self) -> refrain_status {
        auto *provider{static_cast<world_provider *>(self)};
        if (provider == nullptr || !provider->world.has_value()) {
            return REFRAIN_SERVICES_WORLD_WORLD_ERROR_CHUNK_NOT_LOADED;
        }
        auto flushed{provider->world->flush()};
        provider->world.reset();
        provider->sections.clear();
        return flushed ? REFRAIN_STATUS_OK
                       : abi::to_status(
                             flushed.error(), world_statuses,
                             REFRAIN_SERVICES_WORLD_WORLD_ERROR_INTERNAL);
    }

    static auto chunk_at(void *self, const refrain_core_chunk_pos *pos,
                         refrain_core_chunk_view *out) -> refrain_status {
        auto *provider{static_cast<world_provider *>(self)};
        if (provider == nullptr || pos == nullptr || out == nullptr ||
            !provider->world.has_value()) {
            return REFRAIN_SERVICES_WORLD_WORLD_ERROR_CHUNK_NOT_LOADED;
        }
        const refrain::sim::chunk_pos key{pos->x, pos->z};
        auto ensured{provider->world->ensure_chunk(key)};
        if (!ensured) {
            return REFRAIN_SERVICES_WORLD_WORLD_ERROR_CHUNK_NOT_LOADED;
        }
        const auto *chunk{*ensured};
        provider->sections.clear();
        provider->sections.reserve(chunk->sections.size());
        for (const auto &section : chunk->sections) {
            auto &view{provider->sections.emplace_back()};
            view.struct_size = REFRAIN_CORE_CHUNK_SECTION_VIEW_SIZE;
            view.section_y = section.y;
            view.block_count =
                section.uniform ? static_cast<std::uint16_t>(
                                      section.value == refrain::sim::air_block
                                          ? 0
                                          : refrain::world::section_volume)
                                : section.base.block_count;
            view.bits_per_entry = 0;
            view.flags = 0;
            view.block_entities =
                refrain_core_slice_block_pos{.data = nullptr, .len = 0};
        }
        out->struct_size = REFRAIN_CORE_CHUNK_VIEW_SIZE;
        out->pos = *pos;
        out->status =
            static_cast<refrain_core_chunk_status>(chunk->storage.status);
        out->section_count =
            static_cast<std::uint32_t>(provider->sections.size());
        out->sections = refrain_core_slice_chunk_section_view{
            .data = provider->sections.data(),
            .len = provider->sections.size(),
        };
        return REFRAIN_STATUS_OK;
    }

    static auto block_at(void *self, const refrain_core_block_pos *pos)
        -> std::uint32_t {
        auto *provider{static_cast<world_provider *>(self)};
        if (provider == nullptr || pos == nullptr ||
            !provider->world.has_value()) {
            return refrain::sim::air_block;
        }
        return provider->world->block_at(refrain::sim::block_pos{
            pos->x, static_cast<std::int16_t>(pos->y), pos->z});
    }

    static auto height_at(void *self, const refrain_core_block_pos *pos)
        -> std::int32_t {
        auto *provider{static_cast<world_provider *>(self)};
        if (provider == nullptr || pos == nullptr ||
            !provider->world.has_value()) {
            return -1;
        }
        for (std::int32_t y{world_limit}; y >= -world_limit; --y) {
            const auto block{provider->world->block_at(refrain::sim::block_pos{
                pos->x, static_cast<std::int16_t>(y), pos->z})};
            if (provider->world->block_solid(block)) {
                return y;
            }
        }
        return -1;
    }

    static auto is_chunk_loaded(void *self, const refrain_core_chunk_pos *pos)
        -> std::uint8_t {
        auto *provider{static_cast<world_provider *>(self)};
        return provider != nullptr && pos != nullptr &&
                       provider->world.has_value() &&
                       provider->world->find_chunk(
                           refrain::sim::chunk_pos{pos->x, pos->z}) != nullptr
                   ? 1
                   : 0;
    }

    static auto apply_block_changes(void *self,
                                    refrain_core_slice_block_change changes,
                                    std::uint32_t *out_value)
        -> refrain_status {
        auto *provider{static_cast<world_provider *>(self)};
        if (provider == nullptr || out_value == nullptr ||
            !provider->world.has_value()) {
            return REFRAIN_SERVICES_WORLD_WORLD_ERROR_CHUNK_NOT_LOADED;
        }
        std::vector<refrain::sim::block_change> batch{};
        batch.reserve(static_cast<std::size_t>(changes.len));
        for (std::uint64_t index{0}; index < changes.len; ++index) {
            const auto &change{changes.data[index]};
            batch.push_back(refrain::sim::block_change{
                .pos =
                    refrain::sim::block_pos{
                        change.pos.x, static_cast<std::int16_t>(change.pos.y),
                        change.pos.z},
                .old_state = change.old_state,
                .new_state = change.new_state});
        }
        auto applied{provider->world->set_blocks(batch)};
        if (!applied) {
            return abi::to_status(applied.error(), world_statuses,
                                  REFRAIN_SERVICES_WORLD_WORLD_ERROR_INTERNAL);
        }
        *out_value = static_cast<std::uint32_t>(*applied);
        return REFRAIN_STATUS_OK;
    }

    static auto world_tick(void *self) -> std::int64_t {
        auto *provider{static_cast<world_provider *>(self)};
        return provider != nullptr && provider->world.has_value()
                   ? provider->world->ticks()
                   : 0;
    }

    static constexpr std::int32_t world_limit{refrain::world::max_section_y *
                                                  refrain::world::section_edge +
                                              refrain::world::section_edge - 1};
};

auto world_vtable() -> const refrain_services_world_vtable & {
    static const refrain_services_world_vtable table{
        .struct_size = REFRAIN_SERVICES_WORLD_VTABLE_SIZE,
        .abi_major = REFRAIN_SERVICES_WORLD_ABI_MAJOR,
        .abi_minor = REFRAIN_SERVICES_WORLD_ABI_MINOR,
        .abi_patch = REFRAIN_SERVICES_WORLD_ABI_PATCH,
        .open_world = &world_provider::open_world,
        .close_world = &world_provider::close_world,
        .chunk_at = &world_provider::chunk_at,
        .block_at = &world_provider::block_at,
        .height_at = &world_provider::height_at,
        .is_chunk_loaded = &world_provider::is_chunk_loaded,
        .apply_block_changes = &world_provider::apply_block_changes,
        .world_tick = &world_provider::world_tick,
    };
    return table;
}

auto make_world_vanilla(refrain::mod::host &host) -> refrain::status {
    static std::unique_ptr<world_provider> provider{};
    provider = std::make_unique<world_provider>();
    const refrain::mod::provider_api api{
        .vtable = &world_vtable(),
        .self = provider.get(),
        .abi_major = REFRAIN_SERVICES_WORLD_ABI_MAJOR,
    };
    auto status{host.register_provider("refrain:world", api, "1.0.0")};
    if (!status) {
        provider.reset();
        return status;
    }
    return refrain::status_ok();
}

} // namespace

REFR_MOD_STATIC("refrain.world.vanilla", "1.0.0", 1, make_world_vanilla);
