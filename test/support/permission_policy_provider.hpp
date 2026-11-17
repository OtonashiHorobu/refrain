// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// In-process `refrain:permissions` provider for command-level tests. It
// answers through the generated C ABI with the real default-resolver policy,
// so a test exercises the same path the composition root binds: contract query
// -> command filter -> tree, completion and execution. `reload()` publishes a
// new policy file text and bumps the revision, which is what a policy change
// does in a running server.

#pragma once

#include <refrain/contracts/abi.hpp>
#include <refrain/contracts/services/permissions_abi.h>
#include <refrain/core/config.hpp>
#include <refrain/permissions/policy.hpp>

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
};

} // namespace refrain::test_support
