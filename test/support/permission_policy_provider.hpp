// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// In-process `refrain:permissions` provider for command-level tests. It
// answers through the generated C ABI with the real default-resolver policy,
// so a test exercises the same path the composition root binds: contract query
// -> command filter -> tree, completion and execution. `reload()` publishes a
// new policy file text and bumps the revision, which is what a policy change
// does in a running server. The provider also implements the host-side
// `permissions::admin` interface in memory, so the command tests exercise
// grant, revoke, groups and reload without a configuration file.

#pragma once

#include <refrain/contracts/abi.hpp>
#include <refrain/contracts/services/permissions_abi.h>
#include <refrain/core/config.hpp>
#include <refrain/core/error.hpp>
#include <refrain/permissions/admin.hpp>
#include <refrain/permissions/policy.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::test_support {

/// One query the provider answered, so a test can assert the subject and the
/// context the command layer built for it.
struct recorded_permission_query {
    refrain::services::permissions::subject_kind kind{
        refrain::services::permissions::subject_kind::console};
    std::string id{};
    std::string profile_name{};
    std::string node{};
    std::string world{};
    std::string gamemode{};
    std::uint32_t op_level{0};
};

class permission_policy_provider {
  public:
    permission_policy_provider() = default;

    /// Publish `config_text` as the new policy and bump the revision. Returns
    /// false when the document does not parse; the previous policy then stays
    /// in effect, like the built-in provider's failed reload.
    [[nodiscard]] auto reload(std::string_view config_text) -> bool {
        auto document{refrain::core::config::document::parse_text(
            config_text, "permissions.toml")};
        if (!document) {
            return false;
        }
        auto parsed{refrain::permissions::parse_policy(*document)};
        if (!parsed) {
            return false;
        }
        impl_->policy = std::move(*parsed);
        ++impl_->revision;
        return true;
    }

    /// Management surface over the same policy value, for the `/permissions`
    /// command tests. A change bumps the revision like a persisted change.
    [[nodiscard]] auto admin() noexcept -> refrain::permissions::admin * {
        return &admin_;
    }

    /// Replace the policy the next `admin()->reload()` publishes. Returns false
    /// when the text does not parse.
    [[nodiscard]] auto stage_reload(std::string_view config_text) -> bool {
        auto document{refrain::core::config::document::parse_text(
            config_text, "permissions.toml")};
        if (!document) {
            return false;
        }
        auto parsed{refrain::permissions::parse_policy(*document)};
        if (!parsed) {
            return false;
        }
        impl_->staged = std::move(*parsed);
        impl_->staged_valid = true;
        return true;
    }

    /// Queries answered since construction or the last `clear_queries()`.
    [[nodiscard]] auto queries() const
        -> const std::vector<recorded_permission_query> & {
        return impl_->queries;
    }

    void clear_queries() { impl_->queries.clear(); }

    [[nodiscard]] auto vtable() const
        -> const refrain_services_permissions_vtable & {
        static const refrain_services_permissions_vtable table{
            .struct_size = REFRAIN_SERVICES_PERMISSIONS_VTABLE_SIZE,
            .abi_major = REFRAIN_SERVICES_PERMISSIONS_ABI_MAJOR,
            .abi_minor = REFRAIN_SERVICES_PERMISSIONS_ABI_MINOR,
            .abi_patch = REFRAIN_SERVICES_PERMISSIONS_ABI_PATCH,
            .query = &permission_policy_provider::abi_query,
            .default_result = &permission_policy_provider::abi_query,
            .has = &permission_policy_provider::abi_has,
            .revision = &permission_policy_provider::abi_revision,
            .reload = &permission_policy_provider::abi_reload,
        };
        return table;
    }

    [[nodiscard]] auto self() noexcept -> void * { return impl_.get(); }

    [[nodiscard]] auto revision() const -> std::uint64_t {
        return impl_->revision;
    }

  private:
    struct state {
        refrain::permissions::policy policy{};
        std::uint64_t revision{1};
        std::vector<recorded_permission_query> queries{};
        refrain::permissions::policy staged{};
        bool staged_valid{false};
    };

    /// In-memory management surface: the same policy value the ABI queries
    /// read, with a revision bump per change. `reload()` publishes the policy
    /// staged through `stage_reload()`.
    class admin_impl final : public refrain::permissions::admin {
      public:
        explicit admin_impl(state &impl) : impl_{impl} {}

        [[nodiscard]] auto
        explain(const refrain::permissions::subject_view &subject,
                std::string_view node, std::uint32_t op_level) const
            -> refrain::permissions::decision_explanation override {
            return refrain::permissions::explain(impl_.policy, subject, node,
                                                 op_level);
        }

        [[nodiscard]] auto snapshot() const
            -> refrain::permissions::policy_snapshot override {
            return refrain::permissions::policy_snapshot{
                .value = impl_.policy,
                .revision = impl_.revision,
                .persistent = true,
            };
        }

        [[nodiscard]] auto grant(std::string_view subject,
                                 std::string_view rule)
            -> refrain::result<refrain::permissions::policy_change> override {
            return change(subject, rule, true);
        }

        [[nodiscard]] auto revoke(std::string_view subject,
                                  std::string_view rule)
            -> refrain::result<refrain::permissions::policy_change> override {
            return change(subject, rule, false);
        }

        [[nodiscard]] auto groups() const
            -> std::vector<refrain::permissions::group_listing> override {
            return refrain::permissions::list_groups(impl_.policy);
        }

        [[nodiscard]] auto reload()
            -> refrain::result<refrain::permissions::reload_outcome> override {
            const auto previous{impl_.revision};
            if (!impl_.staged_valid) {
                return refrain::permissions::reload_outcome{
                    .changed = false,
                    .previous_revision = previous,
                    .revision = previous,
                };
            }
            impl_.policy = std::move(impl_.staged);
            impl_.staged = {};
            impl_.staged_valid = false;
            ++impl_.revision;
            return refrain::permissions::reload_outcome{
                .changed = true,
                .previous_revision = previous,
                .revision = impl_.revision,
            };
        }

        [[nodiscard]] auto revision() const -> std::uint64_t override {
            return impl_.revision;
        }

        [[nodiscard]] auto persistent() const -> bool override { return true; }

      private:
        [[nodiscard]] auto change(std::string_view subject,
                                  std::string_view rule, bool add)
            -> refrain::result<refrain::permissions::policy_change> {
            auto parsed{
                refrain::permissions::parse_rule(rule, "permission rule")};
            if (!parsed) {
                return std::unexpected{std::move(parsed.error())};
            }
            if (!refrain::permissions::valid_subject_key(subject)) {
                return std::unexpected{
                    refrain::make_error(refrain::error_code::invalid_argument,
                                        "not a configurable subject")};
            }
            auto &rules{impl_.policy.subjects[std::string{subject}]};
            const auto present{
                std::ranges::find(rules.nodes.rules(), *parsed) !=
                rules.nodes.rules().end()};
            if (present == add) {
                return refrain::permissions::policy_change{
                    .changed = false,
                    .revision = impl_.revision,
                    .subject = std::string{subject},
                    .rule = refrain::permissions::rule_text(*parsed),
                };
            }
            if (add) {
                rules.nodes.add(*parsed);
            } else {
                static_cast<void>(rules.nodes.remove(*parsed));
            }
            ++impl_.revision;
            return refrain::permissions::policy_change{
                .changed = true,
                .revision = impl_.revision,
                .subject = std::string{subject},
                .rule = refrain::permissions::rule_text(*parsed),
            };
        }

        state &impl_;
    };

    [[nodiscard]] static auto
    convert(const refrain_services_permissions_permission_query *query,
            state &impl) -> refrain_services_permissions_permission_result {
        const auto kind{
            static_cast<refrain::services::permissions::subject_kind>(
                query->subject.kind)};
        const auto id{refrain::contracts::as_string_view(query->subject.id)};
        const auto profile_name{
            refrain::contracts::as_string_view(query->subject.profile_name)};
        const auto node{refrain::contracts::as_string_view(query->node)};
        impl.queries.push_back(recorded_permission_query{
            .kind = kind,
            .id = std::string{id},
            .profile_name = std::string{profile_name},
            .node = std::string{node},
            .world = std::string{refrain::contracts::as_string_view(
                query->context.world)},
            .gamemode = std::string{refrain::contracts::as_string_view(
                query->context.gamemode)},
            .op_level = query->op_level,
        });
        const auto answer{refrain::permissions::evaluate(
            impl.policy,
            refrain::permissions::subject_view{
                .kind = kind, .id = id, .profile_name = profile_name},
            node, query->op_level)};
        return static_cast<refrain_services_permissions_permission_result>(
            answer);
    }

    static auto
    abi_query(void *self,
              const refrain_services_permissions_permission_query *query)
        -> refrain_services_permissions_permission_result {
        auto *impl{static_cast<state *>(self)};
        if (impl == nullptr || query == nullptr) {
            return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_DENIED;
        }
        return convert(query, *impl);
    }

    static auto
    abi_has(void *self,
            const refrain_services_permissions_permission_query *query)
        -> std::uint8_t {
        return abi_query(self, query) !=
                       REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_DENIED
                   ? 1U
                   : 0U;
    }

    static auto abi_revision(void *self) -> std::uint64_t {
        const auto *impl{static_cast<const state *>(self)};
        return impl == nullptr ? 0U : impl->revision;
    }

    static auto abi_reload(void *, std::uint64_t *) -> refrain_status {
        // The test drives reloads through reload(); the contract entry point
        // has no file to read here.
        return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_ERROR_UNAVAILABLE;
    }

    std::unique_ptr<state> impl_{std::make_unique<state>()};
    admin_impl admin_{*impl_};
};

} // namespace refrain::test_support
