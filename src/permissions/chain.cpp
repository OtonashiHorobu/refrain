// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/permissions/chain.hpp>

#include <refrain/permissions/policy.hpp>

#include <fmt/format.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace refrain::permissions {

namespace {

namespace contract = services::permissions;

/// Highest value the tri-state contract defines. A provider that answers with
/// any other value cannot name its answer, so the walk treats it as a denial
/// instead of casting it to an enumerator with a different meaning.
constexpr std::uint32_t max_permission_result{
    static_cast<std::uint32_t>(contract::permission_result::not_handled)};

[[nodiscard]] auto normalize(refrain_services_permissions_permission_result raw)
    -> permission_result {
    const auto value{static_cast<std::uint32_t>(raw)};
    if (value > max_permission_result) {
        return permission_result::denied;
    }
    return static_cast<permission_result>(value);
}

/// Validate the query context against the policy's context grammar without
/// allocating: every extra entry must have the contract struct size, the slice
/// must be bounded, and the assembled view must pass `valid_context` (lengths,
/// reserved keys and duplicates).
[[nodiscard]] auto
valid_query_context(const contract::permission_context &context) noexcept
    -> bool {
    if (context.world.len > permissions::max_context_length ||
        context.gamemode.len > permissions::max_context_length) {
        return false;
    }
    if (context.extra.len > permissions::max_context_entries) {
        return false;
    }
    if (context.extra.len != 0 && context.extra.data == nullptr) {
        return false;
    }
    std::array<context_value_view, permissions::max_context_entries> entries{};
    for (std::uint64_t index{0}; index < context.extra.len; ++index) {
        const auto &entry{context.extra.data[index]};
        if (entry.struct_size <
            sizeof(refrain_services_permissions_permission_context_entry)) {
            return false;
        }
        entries[index] = context_value_view{
            .key = refrain::contracts::as_string_view(entry.key),
            .value = refrain::contracts::as_string_view(entry.value)};
    }
    const context_view view{
        .world = refrain::contracts::as_string_view(context.world),
        .gamemode = refrain::contracts::as_string_view(context.gamemode),
        .extra = std::span<const context_value_view>{
            entries.data(), static_cast<std::size_t>(context.extra.len)}};
    return permissions::valid_context(view);
}

[[nodiscard]] auto reload_error(const provider_link &link, refrain_status code)
    -> error {
    auto reason{error_code::unknown};
    switch (static_cast<contract::permission_error>(code)) {
    case contract::permission_error::invalid_argument:
        reason = error_code::invalid_argument;
        break;
    case contract::permission_error::parse_error:
        reason = error_code::parse_error;
        break;
    case contract::permission_error::not_initialized:
        reason = error_code::not_initialized;
        break;
    case contract::permission_error::unavailable:
        reason = error_code::unsupported;
        break;
    }
    return make_error(reason, fmt::format("permission provider '{}' failed to "
                                          "reload (error {})",
                                          link.id, code));
}

} // namespace

auto make_provider_link(std::string_view id,
                        const refrain_services_permissions_vtable &table,
                        void *self) -> provider_link {
    return provider_link{.id = std::string{id}, .vtable = &table, .self = self};
}

auto complete(const provider_link &link) noexcept -> bool {
    return !link.id.empty() && link.vtable != nullptr && link.self != nullptr &&
           link.vtable->query != nullptr &&
           link.vtable->default_result != nullptr &&
           link.vtable->has != nullptr && link.vtable->revision != nullptr &&
           link.vtable->reload != nullptr;
}

provider_chain::provider_chain(std::vector<provider_link> wrappers,
                               provider_link base)
    : wrappers_{std::move(wrappers)}, base_{std::move(base)} {}

auto provider_chain::create(std::vector<provider_link> wrappers,
                            provider_link base) -> result<provider_chain> {
    if (!complete(base)) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "the permission chain base provider is incomplete")};
    }
    for (const auto &wrapper : wrappers) {
        if (!complete(wrapper)) {
            return std::unexpected{make_error(
                error_code::invalid_argument,
                fmt::format("the permission chain wrapper '{}' is incomplete",
                            wrapper.id))};
        }
    }
    return provider_chain{std::move(wrappers), std::move(base)};
}

auto provider_chain::query(const contract::permission_query &value) const
    -> permission_result {
    if (!valid_query(&value)) {
        return permission_result::denied;
    }
    for (const auto &wrapper : wrappers_) {
        const auto answer{
            normalize(wrapper.vtable->query(wrapper.self, &value))};
        if (answer != permission_result::not_handled) {
            return answer;
        }
    }
    return normalize(base_.vtable->query(base_.self, &value));
}

auto provider_chain::default_result(
    const contract::permission_query &value) const -> permission_result {
    if (!valid_query(&value)) {
        return permission_result::denied;
    }
    return normalize(base_.vtable->default_result(base_.self, &value));
}

auto provider_chain::has(const contract::permission_query &value) const
    -> bool {
    return query(value) != permission_result::denied;
}

auto provider_chain::revision() const -> std::uint64_t {
    auto composed{
        static_cast<std::uint64_t>(base_.vtable->revision(base_.self))};
    for (const auto &wrapper : wrappers_) {
        composed +=
            static_cast<std::uint64_t>(wrapper.vtable->revision(wrapper.self));
    }
    return composed;
}

auto provider_chain::reload() const -> result<chain_reload_outcome> {
    const auto previous{revision()};
    const auto reloaded{[](const provider_link &link) -> status {
        auto revision{std::uint64_t{0}};
        const auto code{link.vtable->reload(link.self, &revision)};
        if (code != REFRAIN_STATUS_OK) {
            return std::unexpected{reload_error(link, code)};
        }
        return status_ok();
    }};
    if (auto base_reload{reloaded(base_)}; !base_reload) {
        return std::unexpected{std::move(base_reload.error())};
    }
    for (const auto &wrapper : wrappers_) {
        if (auto wrapper_reload{reloaded(wrapper)}; !wrapper_reload) {
            return std::unexpected{std::move(wrapper_reload.error())};
        }
    }
    const auto next{revision()};
    return chain_reload_outcome{
        .changed = next != previous,
        .previous_revision = previous,
        .revision = next,
    };
}

auto provider_chain::valid_query(
    const contract::permission_query *value) noexcept -> bool {
    if (value == nullptr) {
        return false;
    }
    if (value->struct_size < sizeof(contract::permission_query)) {
        return false;
    }
    if (value->subject.struct_size < sizeof(contract::subject_ref)) {
        return false;
    }
    if (value->context.struct_size < sizeof(contract::permission_context)) {
        return false;
    }
    if (value->subject.kind >
        static_cast<std::uint32_t>(contract::subject_kind::plugin)) {
        return false;
    }
    if (!permissions::valid_node(
            refrain::contracts::as_string_view(value->node))) {
        return false;
    }
    return valid_query_context(value->context);
}

namespace {

auto abi_query(void *self,
               const refrain_services_permissions_permission_query *value)
    -> refrain_services_permissions_permission_result {
    const auto *chain{static_cast<const provider_chain *>(self)};
    if (chain == nullptr || value == nullptr) {
        return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_DENIED;
    }
    return static_cast<refrain_services_permissions_permission_result>(
        chain->query(*value));
}

auto abi_default_result(
    void *self, const refrain_services_permissions_permission_query *value)
    -> refrain_services_permissions_permission_result {
    const auto *chain{static_cast<const provider_chain *>(self)};
    if (chain == nullptr || value == nullptr) {
        return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_DENIED;
    }
    return static_cast<refrain_services_permissions_permission_result>(
        chain->default_result(*value));
}

auto abi_has(void *self,
             const refrain_services_permissions_permission_query *value)
    -> std::uint8_t {
    const auto *chain{static_cast<const provider_chain *>(self)};
    if (chain == nullptr || value == nullptr) {
        return 0;
    }
    return chain->has(*value) ? 1U : 0U;
}

auto abi_revision(void *self) -> std::uint64_t {
    const auto *chain{static_cast<const provider_chain *>(self)};
    return chain == nullptr ? 0U : chain->revision();
}

auto abi_reload(void *self, std::uint64_t *out_value) -> refrain_status {
    auto *chain{static_cast<provider_chain *>(self)};
    if (chain == nullptr || out_value == nullptr) {
        return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_ERROR_INVALID_ARGUMENT;
    }
    auto result{chain->reload()};
    if (!result) {
        return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_ERROR_UNAVAILABLE;
    }
    *out_value = result->revision;
    return REFRAIN_STATUS_OK;
}

} // namespace

auto provider_chain::vtable() const
    -> const refrain_services_permissions_vtable & {
    static const refrain_services_permissions_vtable table{
        .struct_size = REFRAIN_SERVICES_PERMISSIONS_VTABLE_SIZE,
        .abi_major = REFRAIN_SERVICES_PERMISSIONS_ABI_MAJOR,
        .abi_minor = REFRAIN_SERVICES_PERMISSIONS_ABI_MINOR,
        .abi_patch = REFRAIN_SERVICES_PERMISSIONS_ABI_PATCH,
        .query = &abi_query,
        .default_result = &abi_default_result,
        .has = &abi_has,
        .revision = &abi_revision,
        .reload = &abi_reload,
    };
    return table;
}

} // namespace refrain::permissions
