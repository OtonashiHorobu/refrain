// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/permissions/chain.hpp>

#include <fmt/format.h>

#include <cstddef>
#include <utility>

namespace refrain::permissions {

namespace {

namespace contract = services::permissions;

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
        const auto answer{wrapper.vtable->query(wrapper.self, &value)};
        if (answer !=
            REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_NOT_HANDLED) {
            return static_cast<permission_result>(answer);
        }
    }
    return static_cast<permission_result>(
        base_.vtable->query(base_.self, &value));
}

auto provider_chain::default_result(
    const contract::permission_query &value) const -> permission_result {
    if (!valid_query(&value)) {
        return permission_result::denied;
    }
    return static_cast<permission_result>(
        base_.vtable->default_result(base_.self, &value));
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
    return value->subject.kind <=
           static_cast<std::uint32_t>(contract::subject_kind::plugin);
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
