// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Consumer helper for the `refrain:permissions` contract.
//
// The generated wrapper (`refrain/services/permissions.hpp` in the generated
// SDK tree) is the ABI-faithful call surface: struct sizes, borrowed strings
// and the tri-state result. This header builds the query value from ordinary
// C++ values (a subject, a node and an optional context), so a consumer only
// writes
//
//   auto client{permissions::client::bind(api.vtable, api.self)};
//   if (client.has(permissions::subject::player(uuid, name, op_level),
//                  "refrain.mods.admin")) {
//       // ...
//   }
//
// A subject and a context borrow their strings: they must stay alive until the
// call returns. Queries are read-only and may run on any thread; keep the bound
// provider table valid for the lifetime of the client (re-query the service
// when the composition epoch changes, as with any provider table).

#pragma once

#include <refrain/contracts/services/permissions.hpp>
#include <refrain/core/error.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::permissions {

using services::permissions::permission_error;
using services::permissions::permission_result;
using services::permissions::subject_kind;

/// Root of the nodes the permission service owns. Features that manage
/// permissions require `refrain.permissions.admin`; other mods declare their
/// own nodes under their namespace instead.
inline constexpr std::string_view admin_node{"refrain.permissions.admin"};

/// Who a query is about. A player is identified by its canonical profile uuid
/// and, as a fallback, by the profile name; the vanilla operator ladder travels
/// with the subject so the `"op"` default policy and the `[op]` node set can
/// match it.
struct subject {
    subject_kind kind{subject_kind::player};
    std::string_view id{};
    std::string_view profile_name{};
    std::uint32_t op_level{0};

    [[nodiscard]] static auto console() noexcept -> subject {
        return subject{.kind = subject_kind::console};
    }

    [[nodiscard]] static auto player(std::string_view uuid,
                                     std::string_view profile_name = {},
                                     std::uint32_t op_level = 0) noexcept
        -> subject {
        return subject{.kind = subject_kind::player,
                       .id = uuid,
                       .profile_name = profile_name,
                       .op_level = op_level};
    }

    [[nodiscard]] static auto entity(std::string_view uuid) noexcept
        -> subject {
        return subject{.kind = subject_kind::entity, .id = uuid};
    }

    [[nodiscard]] static auto plugin(std::string_view mod_id) noexcept
        -> subject {
        return subject{.kind = subject_kind::plugin, .id = mod_id};
    }
};

/// One extra key/value pair of a query context.
struct context_entry {
    std::string_view key{};
    std::string_view value{};
};

/// Context of a query. The built-in resolver matches its `[contexts]`
/// predicates against this value, and a wrapper provider may match it too; an
/// empty context matches no context rule, so a policy without context rules
/// answers the same way for every context.
struct context {
    std::string_view world{};
    std::string_view gamemode{};
    std::span<const context_entry> extra{};
};

namespace detail {

/// ABI query plus the storage its borrowed slices point at. Built on the stack
/// of one client call.
struct abi_query {
    services::permissions::permission_query value{};
    std::vector<services::permissions::permission_context_entry> extra{};

    abi_query(const subject &who, std::string_view node,
              const context &query_context) {
        extra.reserve(query_context.extra.size());
        for (const auto &entry : query_context.extra) {
            auto abi{services::permissions::make_permission_context_entry()};
            abi.key = contracts::to_borrowed(entry.key);
            abi.value = contracts::to_borrowed(entry.value);
            extra.push_back(abi);
        }

        value = services::permissions::make_permission_query();
        value.subject = services::permissions::make_subject_ref();
        value.subject.kind =
            static_cast<refrain_services_permissions_subject_kind>(who.kind);
        value.subject.id = contracts::to_borrowed(who.id);
        value.subject.profile_name = contracts::to_borrowed(who.profile_name);
        value.context = services::permissions::make_permission_context();
        value.context.world = contracts::to_borrowed(query_context.world);
        value.context.gamemode = contracts::to_borrowed(query_context.gamemode);
        value.context.extra.data = extra.data();
        value.context.extra.len = static_cast<std::uint64_t>(extra.size());
        value.node = contracts::to_borrowed(node);
        value.op_level = who.op_level;
    }
};

} // namespace detail

/// Typed call surface over one active `refrain:permissions` provider.
class client {
  public:
    client(const refrain_services_permissions_vtable &table,
           void *self) noexcept
        : table_{&table}, self_{self} {}

    /// Bind over an opaque provider table/instance pair, for example the
    /// `provider_api` a service lookup returned. `vtable` must be the
    /// generated `refrain_services_permissions_vtable` and stay valid for as
    /// long as the client is used.
    [[nodiscard]] static auto bind(const void *vtable, void *self) -> client {
        return client{
            *static_cast<const refrain_services_permissions_vtable *>(vtable),
            self};
    }

    /// Answer one query from the provider's current policy snapshot.
    [[nodiscard]] auto query(const subject &who, std::string_view node,
                             const context &query_context = {}) const
        -> permission_result {
        const detail::abi_query abi{who, node, query_context};
        return static_cast<permission_result>(table_->query(self_, &abi.value));
    }

    /// Answer the way the base resolver answers on its own, without wrapper
    /// grants.
    [[nodiscard]] auto default_result(const subject &who, std::string_view node,
                                      const context &query_context = {}) const
        -> permission_result {
        const detail::abi_query abi{who, node, query_context};
        return static_cast<permission_result>(
            table_->default_result(self_, &abi.value));
    }

    /// True when the node is not denied, so a caller that layers its own
    /// fallback treats `not_handled` as "maybe".
    [[nodiscard]] auto has(const subject &who, std::string_view node,
                           const context &query_context = {}) const -> bool {
        const detail::abi_query abi{who, node, query_context};
        return table_->has(self_, &abi.value) != 0;
    }

    /// Monotonic policy revision; a cache built from this provider compares it
    /// and rebuilds when it changed.
    [[nodiscard]] auto revision() const -> std::uint64_t {
        return static_cast<std::uint64_t>(table_->revision(self_));
    }

    /// Re-read the provider configuration and return the new revision. The
    /// call reads the host configuration store, so it must run on the main
    /// thread.
    [[nodiscard]] auto reload() const
        -> std::expected<std::uint64_t, permission_error> {
        std::uint64_t revision{0};
        if (const auto code{table_->reload(self_, &revision)};
            code != REFRAIN_STATUS_OK) {
            return std::unexpected{static_cast<permission_error>(code)};
        }
        return revision;
    }

    [[nodiscard]] auto table() const noexcept
        -> const refrain_services_permissions_vtable & {
        return *table_;
    }

    [[nodiscard]] auto self() const noexcept -> void * { return self_; }

  private:
    const refrain_services_permissions_vtable *table_;
    void *self_;
};

} // namespace refrain::permissions
