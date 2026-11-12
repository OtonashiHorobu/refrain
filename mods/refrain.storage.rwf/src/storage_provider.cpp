// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// `refrain:storage` provider for `refrain.storage.rwf`.
//
// Payloads crossing the service ABI use the self-contained inline-palette TLV
// form: RWF's own storage keeps the compact world-palette ids, but a service
// consumer cannot be expected to share that table. `decode_chunk` accepts both
// forms, so a payload written here can also be inspected by the CLI.

#include <refrain/contracts/abi.hpp>
#include <refrain/contracts/services/storage_abi.h>

#include <refrain/core/error.hpp>
#include <refrain/host/abi_bridge.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/mod/static_mod.hpp>
#include <refrain/world/chunk_codec.hpp>
#include <refrain/world/store.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace abi = refrain::host::abi;

/// Status rows for the codes the `refrain:storage` contract distinguishes;
/// anything else surfaces as an open failure.
constexpr abi::status_entry storage_statuses[]{
    {refrain::error_code::not_found,
     REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_NOT_FOUND},
    {refrain::error_code::parse_error,
     REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_CORRUPT_DATA},
    {refrain::error_code::version_mismatch,
     REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_VERSION_MISMATCH},
    {refrain::error_code::io_error,
     REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_IO_ERROR},
};

struct storage_provider {
    struct entry {
        refrain::world::world_store store;
        std::vector<std::byte> read_buffer;
    };

    std::map<std::uint64_t, entry> worlds{};
    std::uint64_t next_handle{1};

    [[nodiscard]] auto find(std::uint64_t handle) noexcept -> entry * {
        const auto found{worlds.find(handle)};
        return found == worlds.end() ? nullptr : &found->second;
    }

    static auto
    open_world(void *self,
               const refrain_services_storage_world_open_config *cfg,
               refrain_core_handle_storage_world *out_value) -> refrain_status {
        if (self == nullptr || cfg == nullptr || out_value == nullptr ||
            cfg->struct_size !=
                REFRAIN_SERVICES_STORAGE_WORLD_OPEN_CONFIG_SIZE) {
            return REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_OPEN_FAILED;
        }
        const auto path{abi::to_view(cfg->world_path)};
        if (path.empty()) {
            return REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_OPEN_FAILED;
        }
        auto &provider{*static_cast<storage_provider *>(self)};
        auto store{refrain::world::world_store::open(path)};
        if (!store) {
            return abi::to_status(
                store.error(), storage_statuses,
                REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_OPEN_FAILED);
        }
        const auto handle{provider.next_handle++};
        provider.worlds.emplace(
            handle, entry{.store = std::move(*store), .read_buffer = {}});
        *out_value = handle;
        return REFRAIN_STATUS_OK;
    }

    static auto close_world(void *self,
                            refrain_core_handle_storage_world handle)
        -> refrain_status {
        auto *provider{static_cast<storage_provider *>(self)};
        if (provider == nullptr) {
            return REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_OPEN_FAILED;
        }
        const auto found{provider->worlds.find(handle)};
        if (found == provider->worlds.end()) {
            return REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_NOT_FOUND;
        }
        if (auto committed{found->second.store.commit()}; !committed) {
            return abi::to_status(
                committed.error(), storage_statuses,
                REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_OPEN_FAILED);
        }
        provider->worlds.erase(found);
        return REFRAIN_STATUS_OK;
    }

    static auto read_chunk(void *self, refrain_core_handle_storage_world handle,
                           const refrain_services_storage_chunk_key *key,
                           refrain_core_chunk_buffer *buffer,
                           refrain_services_storage_chunk_read_result *out)
        -> refrain_status {
        auto *provider{static_cast<storage_provider *>(self)};
        if (provider == nullptr || key == nullptr || buffer == nullptr ||
            out == nullptr) {
            return REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_IO_ERROR;
        }
        auto *world{provider->find(handle)};
        if (world == nullptr) {
            return REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_NOT_FOUND;
        }
        auto chunk{world->store.read_chunk(
            refrain::world::chunk_pos{key->pos.x, key->pos.z})};
        if (!chunk) {
            return abi::to_status(
                chunk.error(), storage_statuses,
                REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_OPEN_FAILED);
        }
        auto payload{refrain::world::encode_chunk(
            *chunk, nullptr, true,
            refrain::world::chunk_palette_mode::inline_keys)};
        if (!payload) {
            return abi::to_status(
                payload.error(), storage_statuses,
                REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_OPEN_FAILED);
        }
        world->read_buffer = std::move(*payload);
        buffer->pos = key->pos;
        buffer->generation = 0;
        buffer->word_count =
            static_cast<std::uint32_t>(world->read_buffer.size() / 8U);
        buffer->data = abi::to_bytes(std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t *>(world->read_buffer.data()),
            world->read_buffer.size()});
        out->bytes_read = static_cast<std::uint32_t>(world->read_buffer.size());
        out->format_version = key->format_version;
        out->record_version = 0;
        return REFRAIN_STATUS_OK;
    }

    static auto write_chunk(void *self,
                            refrain_core_handle_storage_world handle,
                            const refrain_services_storage_chunk_key *key,
                            refrain_bytes payload) -> refrain_status {
        auto *provider{static_cast<storage_provider *>(self)};
        if (provider == nullptr || key == nullptr) {
            return REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_IO_ERROR;
        }
        auto *world{provider->find(handle)};
        if (world == nullptr) {
            return REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_NOT_FOUND;
        }
        const auto bytes{abi::to_span(payload)};
        auto chunk{refrain::world::decode_chunk(std::span<const std::byte>{
            reinterpret_cast<const std::byte *>(bytes.data()), bytes.size()})};
        if (!chunk) {
            return abi::to_status(
                chunk.error(), storage_statuses,
                REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_OPEN_FAILED);
        }
        auto written{world->store.write_chunk(
            refrain::world::chunk_pos{key->pos.x, key->pos.z}, *chunk)};
        return written
                   ? REFRAIN_STATUS_OK
                   : abi::to_status(
                         written.error(), storage_statuses,
                         REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_OPEN_FAILED);
    }

    static auto sync(void *self, refrain_core_handle_storage_world handle)
        -> refrain_status {
        auto *provider{static_cast<storage_provider *>(self)};
        if (provider == nullptr) {
            return REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_OPEN_FAILED;
        }
        auto *world{provider->find(handle)};
        if (world == nullptr) {
            return REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_NOT_FOUND;
        }
        auto committed{world->store.commit()};
        return committed
                   ? REFRAIN_STATUS_OK
                   : abi::to_status(
                         committed.error(), storage_statuses,
                         REFRAIN_SERVICES_STORAGE_STORAGE_ERROR_OPEN_FAILED);
    }

    static auto is_open(void *self, refrain_core_handle_storage_world handle)
        -> std::uint8_t {
        auto *provider{static_cast<storage_provider *>(self)};
        return provider != nullptr && provider->find(handle) != nullptr ? 1 : 0;
    }
};

auto storage_vtable() -> const refrain_services_storage_vtable & {
    static const refrain_services_storage_vtable table{
        .struct_size = REFRAIN_SERVICES_STORAGE_VTABLE_SIZE,
        .abi_major = REFRAIN_SERVICES_STORAGE_ABI_MAJOR,
        .abi_minor = REFRAIN_SERVICES_STORAGE_ABI_MINOR,
        .abi_patch = REFRAIN_SERVICES_STORAGE_ABI_PATCH,
        .open_world = &storage_provider::open_world,
        .close_world = &storage_provider::close_world,
        .read_chunk = &storage_provider::read_chunk,
        .write_chunk = &storage_provider::write_chunk,
        .sync = &storage_provider::sync,
        .is_open = &storage_provider::is_open,
    };
    return table;
}

auto make_storage_rwf(refrain::mod::host &host) -> refrain::status {
    static std::unique_ptr<storage_provider> provider{};
    provider = std::make_unique<storage_provider>();
    const refrain::mod::provider_api api{
        .vtable = &storage_vtable(),
        .self = provider.get(),
        .abi_major = REFRAIN_SERVICES_STORAGE_ABI_MAJOR,
    };
    auto status{host.register_provider("refrain:storage", api, "1.0.0")};
    if (!status) {
        provider.reset();
        return status;
    }
    return refrain::status_ok();
}

} // namespace

REFR_MOD_STATIC("refrain.storage.rwf", "1.0.0", 1, make_storage_rwf);
