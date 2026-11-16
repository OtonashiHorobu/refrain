// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/kernel/service_registry.hpp>
#include <refrain/mod/host.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace refrain::mod {

/// A provider table a tick-loop consumer keeps across ticks, together with the
/// registry generation it was read from.
///
/// Ordinary service providers are replaced on composition epochs. A consumer
/// that cached a raw vtable/self pair would keep calling the retired
/// generation after the tick-boundary commit, so it caches this object
/// instead: `refresh()` runs at the tick boundary, re-reads the committed
/// generation and publishes a new snapshot only when the generation changed.
///
/// A call on another thread reads an immutable `table` snapshot, so a swap
/// never tears a call: the old generation stays readable for as long as a
/// caller holds the snapshot. The registry can retire a generation while a
/// snapshot is still being used, so a caller that must not call a retired
/// implementation has to re-read after the publication point; the tick
/// boundary is that point.
///
/// Sealed host tables have generation zero and never change; a consumer may
/// keep them for the process lifetime and needs no cache at all.
class provider_cache {
  public:
    /// One immutable provider table plus the generation it was committed as.
    struct table {
        const void *vtable{nullptr};
        void *self{nullptr};
        std::uint64_t generation{0};
    };

    provider_cache() = default;

    /// Bind to `(service, instance)` of `registry` and publish the committed
    /// table when the slot is active. `registry` must outlive the cache.
    provider_cache(const kernel::service_registry &registry,
                   std::string service, std::string instance = {})
        : registry_{&registry}, service_{std::move(service)},
          instance_{std::move(instance)} {
        static_cast<void>(refresh());
    }

    provider_cache(const provider_cache &) = delete;
    auto operator=(const provider_cache &) -> provider_cache & = delete;
    provider_cache(provider_cache &&) = delete;
    auto operator=(provider_cache &&) -> provider_cache & = delete;
    ~provider_cache() = default;

    /// Re-read the committed slot. Publishes a new snapshot when the active
    /// generation changed and clears the snapshot when the slot has no active
    /// provider. A slot that is quiescing for a staged change keeps the
    /// current snapshot: the committed generation is still the one calls must
    /// use until the commit.
    ///
    /// Must be called from one thread (the tick loop); readers of `current()`
    /// may run on any thread concurrently.
    auto refresh() -> bool {
        if (registry_ == nullptr) {
            return false;
        }
        auto found{registry_->query(service_, instance_)};
        if (!found) {
            if (found.error().code == error_code::stale) {
                // Quiescing: the committed generation is unchanged and still
                // the one to call until the staged change commits.
                return current() != nullptr;
            }
            publish({});
            return false;
        }
        const auto *api{
            static_cast<const provider_api *>(found->provider.instance)};
        if (api == nullptr || api->vtable == nullptr || api->self == nullptr) {
            publish({});
            return false;
        }
        publish(table{.vtable = api->vtable,
                      .self = api->self,
                      .generation = found->generation});
        return true;
    }

    /// The published snapshot, or null when the slot has no committed
    /// provider. The returned object stays valid while the caller holds it.
    [[nodiscard]] auto current() const -> std::shared_ptr<const table> {
        return current_.load(std::memory_order_acquire);
    }

    /// Generation of the published snapshot; zero when there is none.
    [[nodiscard]] auto generation() const -> std::uint64_t {
        const auto snapshot{current()};
        return snapshot == nullptr ? 0 : snapshot->generation;
    }

    [[nodiscard]] auto service() const noexcept -> std::string_view {
        return service_;
    }

    [[nodiscard]] auto instance() const noexcept -> std::string_view {
        return instance_;
    }

  private:
    void publish(table next) {
        const auto current_snapshot{current()};
        if (current_snapshot != nullptr && next.vtable != nullptr &&
            current_snapshot->generation == next.generation &&
            current_snapshot->vtable == next.vtable &&
            current_snapshot->self == next.self) {
            // Same committed generation: keep the snapshot, no allocation on
            // the ordinary tick.
            return;
        }
        if (next.vtable == nullptr) {
            current_.store({}, std::memory_order_release);
            return;
        }
        current_.store(std::make_shared<const table>(std::move(next)),
                       std::memory_order_release);
    }

    const kernel::service_registry *registry_{nullptr};
    std::string service_{};
    std::string instance_{};
    mutable std::atomic<std::shared_ptr<const table>> current_{};
};

} // namespace refrain::mod
