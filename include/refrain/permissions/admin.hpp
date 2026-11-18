// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Runtime administration of the built-in permission resolver.
//
// The `refrain:permissions` contract carries queries, the revision and an
// explicit reload; it has no mutation surface, because changing a policy is an
// operator command of the built-in host resolver, not an operation every
// provider in the chain has to implement. This header is the additive,
// host-side interface the built-in base provider publishes for its own
// management commands: read the effective policy, grant or revoke one rule,
// list the configured groups and reload the configuration file.
//
// The interface is C++ value types over the same policy the resolver serves, so
// a management command cannot drift from the evaluation it administers. The
// built-in provider registers its instance for the process; the composition
// root hands the pointer to the command layer. A composition without the base
// provider has no administration surface and the commands report it as
// unavailable.

#pragma once

#include <refrain/permissions/policy.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace refrain::permissions {

/// One successful policy change. `revision` is the published revision after the
/// change; `changed` is false when the requested rule was already present (a
/// grant) or already absent (a revoke), in which case the policy, the file and
/// the revision are untouched.
struct policy_change {
    bool changed{false};
    std::uint64_t revision{0};
    /// Canonical subject key the rule applies to, for example
    /// `player:<uuid>`.
    std::string subject{};
    /// Rule text as written, with the `!` prefix of a deny.
    std::string rule{};
};

/// The current policy value with the revision it is published at and whether a
/// file backs it.
struct policy_snapshot;

/// Outcome of a reload: the revision before and after. `changed` is false when
/// the configuration text did not change, so the revision stayed the same.
struct reload_outcome {
    bool changed{false};
    std::uint64_t previous_revision{0};
    std::uint64_t revision{0};
};

/// Management surface of the built-in base resolver.
///
/// Queries and reads may run on any thread; `grant()`, `revoke()` and
/// `reload()` mutate the configuration and must run on the main thread, like
/// the contract's `reload`. Implementations serialize mutations against each
/// other and publish a new immutable snapshot atomically, so a reader never
/// observes a half-applied change.
class admin {
  public:
    virtual ~admin() = default;

    admin(const admin &) = delete;
    auto operator=(const admin &) -> admin & = delete;

    /// Read one query against the current policy and name the deciding rule.
    [[nodiscard]] virtual auto explain(const subject_view &subject,
                                       std::string_view node,
                                       std::uint32_t op_level) const
        -> decision_explanation = 0;

    /// Read one query with the caller's context and name the deciding rule.
    /// The default implementation ignores the context, so a provider that has
    /// no context rules keeps its answer; the built-in resolver matches its
    /// `[contexts]` predicates.
    [[nodiscard]] virtual auto
    explain(const subject_view &subject, std::string_view node,
            std::uint32_t op_level, const context_view &context) const
        -> decision_explanation {
        static_cast<void>(context);
        return explain(subject, node, op_level);
    }

    /// Copy of the current policy and the revision it is published at.
    [[nodiscard]] virtual auto snapshot() const -> policy_snapshot = 0;

    /// Add one rule to the subject's `[grants]` entry and persist it. `rule` is
    /// a rule node (`node`, `node.*`, `*`) with an optional `!` deny prefix,
    /// the same grammar the policy parser accepts. A rule that is already
    /// present is reported unchanged. When the policy has no configuration
    /// file to persist to, the change is refused with `unsupported`.
    [[nodiscard]] virtual auto grant(std::string_view subject,
                                     std::string_view rule)
        -> result<policy_change> = 0;

    /// Remove one rule from the subject's `[grants]` entry and persist it. A
    /// rule the subject does not hold is reported unchanged. When the policy
    /// has no configuration file to persist to, the change is refused with
    /// `unsupported`.
    [[nodiscard]] virtual auto revoke(std::string_view subject,
                                      std::string_view rule)
        -> result<policy_change> = 0;

    /// Every configured `[groups]` entry with its nodes and members, sorted by
    /// name.
    [[nodiscard]] virtual auto groups() const -> std::vector<group_listing> = 0;

    /// Re-read the configuration file, publish the new policy and return the
    /// old and new revisions. On failure the current snapshot and revision stay
    /// in effect.
    [[nodiscard]] virtual auto reload() -> result<reload_outcome> = 0;

    /// Monotonic revision of the published snapshot.
    [[nodiscard]] virtual auto revision() const -> std::uint64_t = 0;

    /// True when the policy was loaded from a file, so `grant()` and `revoke()`
    /// can persist. A provider built without a mounted `permissions`
    /// configuration namespace serves the built-in policy and reports false.
    [[nodiscard]] virtual auto persistent() const -> bool = 0;

  protected:
    admin() = default;
};

struct policy_snapshot {
    policy value{};
    std::uint64_t revision{0};
    bool persistent{false};
};

/// Publish the management surface of the base resolver for this process.
///
/// The built-in provider calls this after its service registration succeeded
/// and `retract_admin()` when it is torn down. A second registration replaces
/// the first, so a reloaded composition always exposes the running provider;
/// retracting a pointer that is no longer published is a no-op.
void publish_admin(admin *instance);

/// Withdraw `instance` from the published surface when it is the current one.
void retract_admin(admin *instance);

/// Published management surface, or null when the composition has no built-in
/// permission provider.
[[nodiscard]] auto active_admin() -> admin *;

} // namespace refrain::permissions
