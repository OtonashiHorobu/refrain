// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Composition helper for the `refrain:permissions` chain.
//
// The contract declares `composition = chain`: an optional wrapper provider may
// answer only the nodes it owns and answer `not_handled` for everything else,
// in which case the walk continues with the next provider until the base
// resolver answers. This header turns a set of already-registered provider
// tables into one provider surface with that walk. The composition root owns
// the links and passes the resulting table/instance pair to the command layer
// exactly like a single provider, so the command layer has no permissions
// dependency and a composition without the service still falls back to the
// declared op levels.
//
// Order: the stored wrappers are consulted first, outermost wrapper first, and
// the base resolves the nodes every wrapper forwarded. A provider may answer
// `allowed` or `denied` for a node it owns, which ends the walk; a wrapper can
// therefore narrow a base grant with a deny and extend the base with a grant
// for its own nodes. A malformed query is answered `denied` before any link is
// called, so a broken caller cannot turn a wrapper's parsing into a grant: the
// chain validates the struct sizes, the subject kind, the concrete node and the
// whole context (world, game mode, extra entries with their length, reserved
// and duplicate key rules) before the walk, and an answer outside the
// tri-state enum counts as a denial.
//
// Lifetime: a chain borrows its links. The owner keeps every linked provider
// table and instance valid for as long as the chain is published, and changes
// the link set by building a new chain instead of mutating the old one. A
// composition that retires a link while queries may still run against it must
// drain those queries first, the same rule the host applies to any cached
// provider generation.
//
// Revision and reload: the chain revision is the base revision plus the sum of
// the wrapper revisions. Every link's revision is monotonic by contract, so the
// sum changes exactly when one of them changed and a cache keyed by it
// invalidates on any wrapper change as well as on a base policy change. Reload
// walks the links in query order, starting with the base; the first failing
// link stops the walk, and the error names it. Links that already reloaded keep
// their new snapshot because a provider has no rollback, which the caller
// reports to the operator.

#pragma once

#include <refrain/contracts/services/permissions.hpp>
#include <refrain/core/error.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::permissions {

using services::permissions::permission_result;

/// One link of a permission chain: the provider id used in diagnostics, the
/// host-owned ABI table and the instance pointer the table expects.
struct provider_link {
    std::string id{};
    const refrain_services_permissions_vtable *vtable{nullptr};
    void *self{nullptr};
};

/// Build one link. The id is diagnostic only (a wrapper that fails to reload is
/// named by it); the table and instance must stay valid for as long as the
/// chain is used.
[[nodiscard]] auto
make_provider_link(std::string_view id,
                   const refrain_services_permissions_vtable &table, void *self)
    -> provider_link;

/// True when the link carries the table and instance every operation needs.
[[nodiscard]] auto complete(const provider_link &link) noexcept -> bool;

/// Outcome of `provider_chain::reload()`. `changed` is true when the composed
/// revision moved, so a configuration that reloads to the same rules is
/// reported as unchanged.
struct chain_reload_outcome {
    bool changed{false};
    std::uint64_t previous_revision{0};
    std::uint64_t revision{0};
};

/// Ordered, immutable chain of permission providers plus its base resolver.
///
/// The chain is a value: links are fixed at construction and never mutated. A
/// composition root that needs a new link set builds a new chain and replaces
/// the provider it publishes; concurrent readers keep the old value alive.
class provider_chain {
  public:
    /// Build a chain. `wrappers` are consulted in the given order and `base`
    /// answers whatever they forward. Fails with `invalid_argument` when a link
    /// is incomplete, so a half-built composition cannot answer queries.
    [[nodiscard]] static auto create(std::vector<provider_link> wrappers,
                                     provider_link base)
        -> result<provider_chain>;

    provider_chain(provider_chain &&) noexcept = default;
    auto operator=(provider_chain &&) noexcept -> provider_chain & = default;
    provider_chain(const provider_chain &) = delete;
    auto operator=(const provider_chain &) -> provider_chain & = delete;
    ~provider_chain() = default;

    /// Answer one query: the first wrapper answer that is not `not_handled`
    /// wins, otherwise the base answer is returned. Read-only and callable from
    /// any thread; the links are fixed for the lifetime of the chain.
    [[nodiscard]] auto
    query(const services::permissions::permission_query &value) const
        -> permission_result;

    /// Answer the way the base resolver answers alone, ignoring wrapper grants.
    /// A consumer that layers its own fallback uses this for the nodes no
    /// provider handled.
    [[nodiscard]] auto
    default_result(const services::permissions::permission_query &value) const
        -> permission_result;

    /// Tri-state convenience predicate: true unless the composed answer is
    /// `denied`.
    [[nodiscard]] auto
    has(const services::permissions::permission_query &value) const -> bool;

    /// Composed monotonic revision: the base revision plus the sum of the
    /// wrapper revisions.
    [[nodiscard]] auto revision() const -> std::uint64_t;

    /// Reload every link: the base first, then the wrappers in query order. The
    /// first failure stops the walk and names the link when it is a wrapper;
    /// links that already reloaded keep their new snapshot.
    [[nodiscard]] auto reload() const -> result<chain_reload_outcome>;

    [[nodiscard]] auto wrappers() const noexcept
        -> const std::vector<provider_link> & {
        return wrappers_;
    }

    [[nodiscard]] auto base() const noexcept -> const provider_link & {
        return base_;
    }

    /// Provider surface of the chain, so a composition root can publish it as
    /// one provider without owning the walk itself.
    [[nodiscard]] auto vtable() const
        -> const refrain_services_permissions_vtable &;

    /// The instance pointer `vtable()` expects; the chain must stay alive.
    [[nodiscard]] auto self() noexcept -> void * { return this; }

  private:
    provider_chain(std::vector<provider_link> wrappers, provider_link base);

    [[nodiscard]] static auto
    valid_query(const services::permissions::permission_query *value) noexcept
        -> bool;

    std::vector<provider_link> wrappers_{};
    provider_link base_{};
};

} // namespace refrain::permissions
