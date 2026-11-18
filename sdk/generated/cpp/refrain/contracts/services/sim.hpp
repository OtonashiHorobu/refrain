// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: a4cbf5b67e2cecb5

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_SIM_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_SIM_HPP

#include <refrain/contracts/services/sim_abi.h>
#include <refrain/contracts/abi.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <refrain/contracts/core.hpp>
#include <refrain/contracts/services/storage.hpp>
#include <refrain/contracts/event.hpp>
#include <refrain/core/error.hpp>
#include <refrain/core/schedule.hpp>

namespace refrain::services::sim {

enum class sim_error : std::uint32_t { // enum 'sim_error'
    invalid_handle = 1,
    already_open = 2,
    not_found = 3,
    io_error = 4,
    internal = 5,
    out_of_bounds = 6,
    invalid_argument = 7,
    storage_unavailable = 8,
};

namespace detail {
struct sim_world_tag;
} // namespace detail

using sim_world = ::refrain::contracts::handle<detail::sim_world_tag>;

/// Published once per tick, after the player/RNG step.
struct tick_event {
    std::int64_t tick;
};

static_assert(sizeof(tick_event) == 8);
static_assert(alignof(tick_event) == 8);
static_assert(offsetof(tick_event, tick) == 0);
inline constexpr std::string_view tick_event_id =
    "refrain:event/refrain/services/sim/tick_event";
inline constexpr std::uint64_t tick_event_type = REFRAIN_SERVICES_SIM_EVENT_TICK_EVENT_TYPE;

/// One block mutation that changed the world, published once per applied
/// position in ascending position order.
struct block_changed {
    std::int32_t x;
    std::int32_t y;
    std::int32_t z;
    std::uint32_t old_state;
    std::uint32_t new_state;
};

static_assert(sizeof(block_changed) == 20);
static_assert(alignof(block_changed) == 4);
static_assert(offsetof(block_changed, x) == 0);
static_assert(offsetof(block_changed, y) == 4);
static_assert(offsetof(block_changed, z) == 8);
static_assert(offsetof(block_changed, old_state) == 12);
static_assert(offsetof(block_changed, new_state) == 16);
inline constexpr std::string_view block_changed_id =
    "refrain:event/refrain/services/sim/block_changed";
inline constexpr std::uint64_t block_changed_type = REFRAIN_SERVICES_SIM_EVENT_BLOCK_CHANGED_TYPE;

/// An entity was added to the world.
struct entity_spawned {
    /// Process-local entity id.
    std::uint64_t id;
    /// Namespaced entity type key, for example `minecraft:pig`.
    std::string_view type;
    double x;
    double y;
    double z;
};

static_assert(sizeof(entity_spawned) == 48);
static_assert(alignof(entity_spawned) == 8);
static_assert(offsetof(entity_spawned, id) == 0);
static_assert(offsetof(entity_spawned, type) == 8);
static_assert(offsetof(entity_spawned, x) == 24);
static_assert(offsetof(entity_spawned, y) == 32);
static_assert(offsetof(entity_spawned, z) == 40);
inline constexpr std::string_view entity_spawned_id =
    "refrain:event/refrain/services/sim/entity_spawned";
inline constexpr std::uint64_t entity_spawned_type = REFRAIN_SERVICES_SIM_EVENT_ENTITY_SPAWNED_TYPE;

/// An entity was removed from the world.
struct entity_removed {
    /// Process-local entity id.
    std::uint64_t id;
};

static_assert(sizeof(entity_removed) == 8);
static_assert(alignof(entity_removed) == 8);
static_assert(offsetof(entity_removed, id) == 0);
inline constexpr std::string_view entity_removed_id =
    "refrain:event/refrain/services/sim/entity_removed";
inline constexpr std::uint64_t entity_removed_type = REFRAIN_SERVICES_SIM_EVENT_ENTITY_REMOVED_TYPE;

using sim_config = ::refrain_services_sim_sim_config;
/// Zero-initialized `sim_config` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_sim_config() noexcept -> sim_config {
    sim_config value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
/// Read-only projection of one player record. A provider fills it from the
/// record it owns; the name and the profile uuid are borrowed for the duration
/// of the call, like the level projection's level name.
using player_info = ::refrain_services_sim_player_info;
/// Zero-initialized `player_info` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_player_info() noexcept -> player_info {
    player_info value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
/// Everything needed to register one player. The provider allocates the
/// entity id and keeps the record; the caller reads it back through the
/// projection.
using player_registration = ::refrain_services_sim_player_registration;
/// Zero-initialized `player_registration` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_player_registration() noexcept -> player_registration {
    player_registration value{};
    ::refrain::contracts::init_struct(value);
    return value;
}

static_assert(sizeof(sim_config) == 72);
static_assert(alignof(sim_config) == 8);
static_assert(offsetof(sim_config, struct_size) == 0);
static_assert(offsetof(sim_config, storage) == 8);
static_assert(offsetof(sim_config, seed) == 64);
static_assert(sizeof(player_info) == 120);
static_assert(alignof(player_info) == 8);
static_assert(offsetof(player_info, struct_size) == 0);
static_assert(offsetof(player_info, connection) == 8);
static_assert(offsetof(player_info, profile_id) == 16);
static_assert(offsetof(player_info, name) == 32);
static_assert(offsetof(player_info, world) == 48);
static_assert(offsetof(player_info, entity_id) == 64);
static_assert(offsetof(player_info, x) == 72);
static_assert(offsetof(player_info, y) == 80);
static_assert(offsetof(player_info, z) == 88);
static_assert(offsetof(player_info, yaw) == 96);
static_assert(offsetof(player_info, pitch) == 100);
static_assert(offsetof(player_info, on_ground) == 104);
static_assert(offsetof(player_info, game_mode) == 108);
static_assert(offsetof(player_info, health) == 112);
static_assert(offsetof(player_info, view_distance) == 116);
static_assert(sizeof(player_registration) == 96);
static_assert(alignof(player_registration) == 8);
static_assert(offsetof(player_registration, struct_size) == 0);
static_assert(offsetof(player_registration, connection) == 8);
static_assert(offsetof(player_registration, profile_id) == 16);
static_assert(offsetof(player_registration, name) == 32);
static_assert(offsetof(player_registration, world) == 48);
static_assert(offsetof(player_registration, x) == 64);
static_assert(offsetof(player_registration, y) == 72);
static_assert(offsetof(player_registration, z) == 80);
static_assert(offsetof(player_registration, game_mode) == 88);
static_assert(offsetof(player_registration, inventory_slots) == 92);

inline constexpr std::string_view service_id = "refrain:sim";
inline constexpr std::uint32_t abi_major = 1;
inline constexpr std::uint32_t abi_minor = 1;
inline constexpr std::uint32_t abi_patch = 0;
inline constexpr ::refrain::contracts::side_kind side = ::refrain::contracts::side_kind::server;
inline constexpr ::refrain::contracts::thread_kind thread = ::refrain::contracts::thread_kind::sim;
inline constexpr ::refrain::contracts::composition_kind composition = ::refrain::contracts::composition_kind::exclusive;
inline constexpr std::string_view capabilities = "tick,replay,deterministic-rng,block-events,entity-events";
inline constexpr std::string_view required_services = "refrain:core>=1.0.0,refrain:world>=1.0.0,refrain:storage>=1.0.0";
inline constexpr std::string_view service_ids[] = {"refrain:sim"};

/// Type-safe wrapper around `refrain_services_sim_vtable`. The provider (host) owns the table and `self`; this class is a borrowed call surface.
class provider final {
  public:
    provider(const ::refrain_services_sim_vtable &table, void *self) noexcept
        : table_{&table}, self_{self} {}

    provider(const provider &) = delete;
    auto operator=(const provider &) -> provider & = delete;

    [[nodiscard]] auto table() const noexcept
        -> const ::refrain_services_sim_vtable & {
        return *table_;
    }

    [[nodiscard]] auto self() const noexcept -> void * {
        return self_;
    }

    /// Open a sim world over the given store. The provider never opens the
    /// level directory itself; the store was opened once by the runtime.
    [[nodiscard]] auto open_world(const ::refrain::services::sim::sim_config &config) const -> std::expected<::refrain::services::sim::sim_world, ::refrain::services::sim::sim_error> {
        const auto abi_config = ::refrain::contracts::with_struct_size(config);
        ::refrain_services_sim_handle_sim_world value{};
        if (const auto code = table_->open_world(self_, &abi_config, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::sim::sim_error>(code));
        }
        return ::refrain::services::sim::sim_world{value};
    }

    [[nodiscard]] auto close_world(::refrain::services::sim::sim_world world) const -> std::expected<void, ::refrain::services::sim::sim_error> {
        if (const auto code = table_->close_world(self_, world.raw()); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::sim::sim_error>(code));
        }
        return {};
    }

    /// Advance exactly one tick.
    [[nodiscard]] auto tick(::refrain::services::sim::sim_world world) const -> std::expected<void, ::refrain::services::sim::sim_error> {
        if (const auto code = table_->tick(self_, world.raw()); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::sim::sim_error>(code));
        }
        return {};
    }

    [[nodiscard]] auto ticks(::refrain::services::sim::sim_world world) const -> std::int64_t {
        return static_cast<std::int64_t>(table_->ticks(self_, world.raw()));
    }

    /// Lowercase hex BLAKE3 of the deterministic replay state.
    [[nodiscard]] auto state_hash(::refrain::services::sim::sim_world world) const -> std::string_view {
        return ::refrain::contracts::as_string_view(table_->state_hash(self_, world.raw()));
    }

    /// Apply a batch of block mutations and publish one `block_changed` event
    /// per applied change. Returns the number of changes that modified the
    /// world; a change that writes the current state is skipped.
    [[nodiscard]] auto set_blocks(::refrain::services::sim::sim_world world, std::span<const ::refrain::core::block_change> changes) const -> std::expected<std::uint32_t, ::refrain::services::sim::sim_error> {
        ::uint32_t value{};
        if (const auto code = table_->set_blocks(self_, world.raw(), refrain_core_slice_block_change{changes.data(), static_cast<std::uint64_t>(changes.size())}, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::sim::sim_error>(code));
        }
        return static_cast<std::uint32_t>(value);
    }

    /// Spawn an entity at the given feet position and publish
    /// `entity_spawned`. The id is allocated in ascending order.
    [[nodiscard]] auto spawn_entity(::refrain::services::sim::sim_world world, std::string_view type, double x, double y, double z) const -> std::expected<std::uint64_t, ::refrain::services::sim::sim_error> {
        ::uint64_t value{};
        if (const auto code = table_->spawn_entity(self_, world.raw(), ::refrain::contracts::to_borrowed(type), x, y, z, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::sim::sim_error>(code));
        }
        return static_cast<std::uint64_t>(value);
    }

    /// Remove one entity and publish `entity_removed`. Returns false when the
    /// id is unknown.
    [[nodiscard]] auto remove_entity(::refrain::services::sim::sim_world world, std::uint64_t id) const -> bool {
        return (table_->remove_entity(self_, world.raw(), id) != 0);
    }

    /// Number of player records. `player_at` enumerates them in ascending
    /// entity id order, which is the order the state hash covers.
    /// 
    /// A count plus an indexed accessor is the read shape because every field
    /// but the name and the profile uuid is a fixed-size value: a packed
    /// buffer would need either a per-call buffer whose size the consumer must
    /// guess or an allocation the provider owns, and neither carries a
    /// borrowed string without a second layout. A provider callback per player
    /// would put provider code on the runtime's call stack for a read the
    /// runtime already batches once per tick. The indexed form keeps one
    /// dispatch per record, lets a caller stop at the count it read, and
    /// leaves the string lifetime rule identical to the level projection.
    [[nodiscard]] auto player_count(::refrain::services::sim::sim_world world) const -> std::uint32_t {
        return static_cast<std::uint32_t>(table_->player_count(self_, world.raw()));
    }

    /// One player record by index, in ascending entity id order. The name and
    /// the profile uuid are borrowed for the duration of the call, like the
    /// level projection's level name.
    [[nodiscard]] auto player_at(::refrain::services::sim::sim_world world, std::uint32_t index) const -> std::expected<::refrain::services::sim::player_info, ::refrain::services::sim::sim_error> {
        ::refrain_services_sim_player_info value{};
        ::refrain::contracts::init_struct(value);
        if (const auto code = table_->player_at(self_, world.raw(), index, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::sim::sim_error>(code));
        }
        return value;
    }

    /// One player record by connection. The write calls are connection
    /// addressed because a packet names a connection, not a table index; this
    /// is the read a caller uses to refresh one record after a write.
    [[nodiscard]] auto player_by_connection(::refrain::services::sim::sim_world world, std::uint64_t connection) const -> std::expected<::refrain::services::sim::player_info, ::refrain::services::sim::sim_error> {
        ::refrain_services_sim_player_info value{};
        ::refrain::contracts::init_struct(value);
        if (const auto code = table_->player_by_connection(self_, world.raw(), connection, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::sim::sim_error>(code));
        }
        return value;
    }

    /// Register a player record and allocate its entity id. Fails with
    /// `invalid_argument` for an empty name or a connection that already owns
    /// a player.
    [[nodiscard]] auto add_player(::refrain::services::sim::sim_world world, const ::refrain::services::sim::player_registration &record) const -> std::expected<std::uint64_t, ::refrain::services::sim::sim_error> {
        const auto abi_record = ::refrain::contracts::with_struct_size(record);
        ::uint64_t value{};
        if (const auto code = table_->add_player(self_, world.raw(), &abi_record, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::sim::sim_error>(code));
        }
        return static_cast<std::uint64_t>(value);
    }

    /// Remove the player owned by `connection`. Returns false when the
    /// connection owns no record.
    [[nodiscard]] auto remove_player(::refrain::services::sim::sim_world world, std::uint64_t connection) const -> bool {
        return (table_->remove_player(self_, world.raw(), connection) != 0);
    }

    /// Set position and rotation in one call, the way a movement packet
    /// updates both at once. Velocity is left untouched.
    [[nodiscard]] auto set_player_transform(::refrain::services::sim::sim_world world, std::uint64_t connection, double x, double y, double z, float yaw, float pitch) const -> std::expected<void, ::refrain::services::sim::sim_error> {
        if (const auto code = table_->set_player_transform(self_, world.raw(), connection, x, y, z, yaw, pitch); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::sim::sim_error>(code));
        }
        return {};
    }

    /// Set position and clear velocity and the ground flag, the teleport
    /// semantics the console move command uses.
    [[nodiscard]] auto set_player_position(::refrain::services::sim::sim_world world, std::uint64_t connection, double x, double y, double z) const -> std::expected<void, ::refrain::services::sim::sim_error> {
        if (const auto code = table_->set_player_position(self_, world.raw(), connection, x, y, z); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::sim::sim_error>(code));
        }
        return {};
    }

    /// Replace the view distance the provider keeps for the player.
    [[nodiscard]] auto set_player_view_distance(::refrain::services::sim::sim_world world, std::uint64_t connection, std::int32_t distance) const -> std::expected<void, ::refrain::services::sim::sim_error> {
        if (const auto code = table_->set_player_view_distance(self_, world.raw(), connection, distance); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::sim::sim_error>(code));
        }
        return {};
    }

    /// Move by a relative offset with the provider's collision response.
    /// Returns false when the move was fully blocked.
    [[nodiscard]] auto move_player(::refrain::services::sim::sim_world world, std::uint64_t connection, double dx, double dy, double dz) const -> std::expected<bool, ::refrain::services::sim::sim_error> {
        ::uint8_t value{};
        if (const auto code = table_->move_player(self_, world.raw(), connection, dx, dy, dz, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::sim::sim_error>(code));
        }
        return (value != 0);
    }

    /// Make the player jump when it stands on the ground.
    [[nodiscard]] auto jump(::refrain::services::sim::sim_world world, std::uint64_t connection) const -> std::expected<void, ::refrain::services::sim::sim_error> {
        if (const auto code = table_->jump(self_, world.raw(), connection); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::sim::sim_error>(code));
        }
        return {};
    }

  private:
    const ::refrain_services_sim_vtable *table_;
    void *self_;
};

inline constexpr std::array<::refrain::core::schedule::task_descriptor, 0>
    schedule_tasks{};

} // namespace refrain::services::sim

namespace refrain::contracts {

template <> struct event_traits<::refrain::services::sim::tick_event> {
    static constexpr bool is_event = true;
    static constexpr std::string_view id =
        ::refrain::services::sim::tick_event_id;
    static constexpr bool batch = false;
    static constexpr bool cancellable = false;
};

template <> struct event_traits<::refrain::services::sim::block_changed> {
    static constexpr bool is_event = true;
    static constexpr std::string_view id =
        ::refrain::services::sim::block_changed_id;
    static constexpr bool batch = false;
    static constexpr bool cancellable = false;
};

template <> struct event_traits<::refrain::services::sim::entity_spawned> {
    static constexpr bool is_event = true;
    static constexpr std::string_view id =
        ::refrain::services::sim::entity_spawned_id;
    static constexpr bool batch = false;
    static constexpr bool cancellable = false;
};

template <> struct event_traits<::refrain::services::sim::entity_removed> {
    static constexpr bool is_event = true;
    static constexpr std::string_view id =
        ::refrain::services::sim::entity_removed_id;
    static constexpr bool batch = false;
    static constexpr bool cancellable = false;
};

} // namespace refrain::contracts

#endif // REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_SIM_HPP
