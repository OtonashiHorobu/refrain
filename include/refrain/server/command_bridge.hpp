// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <refrain/command/dispatcher.hpp>
#include <refrain/contracts/services/permissions_abi.h>
#include <refrain/net/server.hpp>
#include <refrain/server/world_port.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace refrain::app {
class mod_management;
} // namespace refrain::app

namespace refrain::server {

/// Stable permission key derived from a player profile uuid. The command
/// bridge uses it as the command source identity and as the key of the
/// per-player state (game mode, world, profile uuid) a permission query needs.
[[nodiscard]] auto player_identity_key(const net::uuid &id) -> std::uint64_t;

/// Shared state between the runtime and the network command bridge. The
/// listener pointer is installed after the network server has started.
struct command_environment {
    refrain::command::dispatcher dispatcher{};
    refrain::net::server *listener{nullptr};
    world_port *world{nullptr};

    /// One cached wire tree per (dispatcher revision, permission revision,
    /// subject). A tree is filtered by permission, so players with different
    /// grants must never share one snapshot, and a policy change must not
    /// reuse a snapshot built from the previous policy. `subject` is the
    /// fingerprint of the queried identity: the display name, the vanilla
    /// ladder level and the opaque identity key.
    struct tree_cache_key {
        std::uint64_t dispatcher_revision{0};
        std::uint64_t permission_revision{0};
        std::uint64_t subject{0};

        friend auto operator==(const tree_cache_key &, const tree_cache_key &)
            -> bool = default;
    };

    struct tree_cache_hash {
        [[nodiscard]] auto operator()(const tree_cache_key &key) const noexcept
            -> std::size_t {
            // Same mixing as the identity key, over the revision pair.
            std::uint64_t mixed{key.dispatcher_revision * 1099511628211ULL};
            mixed ^= key.permission_revision * 1469598103934665603ULL;
            mixed ^= key.subject;
            return static_cast<std::size_t>(mixed);
        }
    };

    /// Revisions the tree cache was last filled at. A change in either drops
    /// every entry before a new snapshot is built.
    std::uint64_t tree_cache_dispatcher_revision{0};
    std::uint64_t tree_cache_permission_revision{0};

    mutable std::mutex tree_cache_mutex{};
    std::unordered_map<tree_cache_key,
                       std::shared_ptr<const refrain::net::command_tree>,
                       tree_cache_hash>
        tree_cache{};

    /// Mod management application service shared with the server console. The
    /// `/mods` tree calls it so both frontends report the same results; null
    /// disables the in-game tree.
    refrain::app::mod_management *mods{nullptr};

    /// Vanilla ladder level of a player when no permission provider is
    /// active. It is the fallback of every declared command node in a
    /// composition without `refrain:permissions`; the default keeps the
    /// built-in gameplay commands usable for every player in a development
    /// composition. An active provider makes the policy authoritative and this
    /// level no longer applies.
    refrain::command::permission_level permission{
        refrain::command::level::admin};

    /// Tri-state permission query over the active `refrain:permissions`
    /// provider, injected by `bind_permissions()`. Empty means the composition
    /// has no provider and the dispatcher falls back to the declared op level.
    std::function<refrain::command::permission_decision(
        const refrain::command::source &, std::string_view node)>
        permission_query{};

    /// Monotonic policy revision of that provider, used to key the tree cache
    /// and to resend command trees after a policy change. Empty when no
    /// provider is active.
    std::function<std::uint64_t()> permission_revision{};

    mutable std::mutex mode_mutex{};
    std::unordered_map<std::uint64_t, net::game_mode> modes{};
    std::unordered_map<std::uint64_t, std::uint64_t> key_to_connection{};
    std::unordered_map<std::uint64_t, std::string> identity_uuids{};
    std::unordered_map<std::uint64_t, std::string> identity_worlds{};
    net::game_mode default_mode{net::game_mode::creative};

    [[nodiscard]] auto join_state(std::uint64_t connection) const
        -> net::join_state;
    [[nodiscard]] auto mode_of(std::uint64_t connection) const
        -> net::game_mode;
    void remember_key(std::uint64_t key, std::uint64_t connection,
                      std::string uuid, std::string world_name = {});
    [[nodiscard]] auto connection_for(std::uint64_t key) const
        -> std::optional<std::uint64_t>;
    /// Canonical uuid of the player this identity key was first resolved for;
    /// `nullopt` for a source the bridge never saw join.
    [[nodiscard]] auto uuid_for(std::uint64_t key) const
        -> std::optional<std::string>;
    /// Last world the player was announced in; empty when unknown.
    [[nodiscard]] auto world_for(std::uint64_t key) const -> std::string;
    /// Game mode name of the player for the permission query context
    /// (`survival`, `creative`, `adventure`, `spectator`).
    [[nodiscard]] auto gamemode_name(std::uint64_t key) const -> std::string;

    /// Answer one permission node for a command source through the injected
    /// provider, or `not_handled` when no provider is active.
    [[nodiscard]] auto query_permission(const refrain::command::source &source,
                                        std::string_view node) const
        -> refrain::command::permission_decision;
};

/// Bind the active `refrain:permissions` provider to the command environment:
/// the query the dispatcher filter, the bridge's explicit denial and the tree
/// cache read. Called by the composition root after the provider is active and
/// before `make_command_bridge()`, which installs the filter over the
/// registered nodes; without it the command tree falls back to the declared op
/// levels.
void bind_permissions(std::shared_ptr<command_environment> environment,
                      const refrain_services_permissions_vtable &vtable,
                      void *self);

/// Register the commands implemented by the server runtime.
void register_runtime_commands(
    std::shared_ptr<command_environment> environment);

/// Build the network bridge that forwards parsed commands to the runtime
/// dispatcher and converts its immutable tree/suggestions to wire types.
[[nodiscard]] auto
make_command_bridge(std::shared_ptr<command_environment> environment)
    -> refrain::net::command_bridge;

} // namespace refrain::server
