// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/kernel/service_registry.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::kernel {
namespace {

/// Empty instance strings address the reserved default composition slot.
[[nodiscard]] auto resolve_instance(std::string_view instance)
    -> std::string_view {
    return instance.empty() ? service_registry::default_instance : instance;
}

/// Copy of the first matching candidate; `version` empty matches any version.
[[nodiscard]] auto find_candidate(const std::vector<provider_info> &candidates,
                                  std::string_view service,
                                  std::string_view provider_id,
                                  std::string_view version)
    -> std::optional<provider_info> {
    const auto match = std::find_if(
        candidates.begin(), candidates.end(),
        [service, provider_id, version](const provider_info &candidate) {
            return candidate.service == service &&
                   candidate.id == provider_id &&
                   (version.empty() || candidate.version == version);
        });
    if (match == candidates.end()) {
        return std::nullopt;
    }
    return *match;
}

} // namespace

auto service_registry::add_provider(provider_info info) -> status {
    if (info.service.empty() || info.id.empty()) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            std::string_view{
                "provider requires a non-empty service name and provider id"})};
    }

    std::unique_lock lock{mutex_};
    if (frozen_) {
        return std::unexpected{make_error(
            error_code::unsupported,
            "service registry is frozen: cannot register provider '" + info.id +
                "' for service '" + info.service + "'")};
    }

    const auto duplicate =
        std::any_of(providers_.begin(), providers_.end(),
                    [&info](const provider_info &candidate) {
                        return candidate.service == info.service &&
                               candidate.id == info.id &&
                               candidate.version == info.version;
                    });
    if (duplicate) {
        return std::unexpected{make_error(
            error_code::already_exists,
            "provider '" + info.id + "' for service '" + info.service +
                "' version '" + info.version + "' is already registered")};
    }

    providers_.push_back(std::move(info));
    return status_ok();
}

auto service_registry::select_active(std::string_view service,
                                     std::string_view provider_id,
                                     std::string_view version,
                                     std::string_view instance) -> status {
    if (service.empty() || provider_id.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       std::string_view{
                           "service name and provider id must not be empty"})};
    }

    std::unique_lock lock{mutex_};
    if (frozen_) {
        return std::unexpected{make_error(
            error_code::unsupported,
            "service registry is frozen: cannot select active provider '" +
                std::string{provider_id} + "' for service '" +
                std::string{service} + "' instance '" +
                std::string{resolve_instance(instance)} + "'")};
    }

    const auto candidate =
        find_candidate(providers_, service, provider_id, version);
    if (!candidate) {
        std::string message{"no provider '" + std::string{provider_id} +
                            "' registered for service '" +
                            std::string{service} + "'"};
        if (!version.empty()) {
            message += " version '" + std::string{version} + "'";
        }
        return std::unexpected{
            make_error(error_code::not_found, std::move(message))};
    }

    // Re-selecting an existing (service, instance) slot updates it in place;
    // selecting a new instance adds a slot (instanced composition).
    active_.insert_or_assign(
        active_key{std::string{service},
                   std::string{resolve_instance(instance)}},
        active_entry{.provider_id = std::string{provider_id},
                     .version = candidate->version});
    return status_ok();
}

auto service_registry::active_provider(std::string_view service,
                                       std::string_view instance) const
    -> std::optional<provider_info> {
    std::shared_lock lock{mutex_};
    const auto slot = active_.find(active_key{
        std::string{service}, std::string{resolve_instance(instance)}});
    if (slot == active_.end()) {
        return std::nullopt;
    }
    return find_candidate(providers_, service, slot->second.provider_id,
                          slot->second.version);
}

auto service_registry::active_instances(std::string_view service) const
    -> std::vector<std::string> {
    std::shared_lock lock{mutex_};
    std::vector<std::string> instances;
    const auto first =
        active_.lower_bound(active_key{std::string{service}, std::string{}});
    // std::map orders by (service, instance): the range for one service is
    // contiguous and already sorted by instance name.
    for (auto slot = first;
         slot != active_.end() && slot->first.first == service; ++slot) {
        instances.push_back(slot->first.second);
    }
    return instances;
}

auto service_registry::find(std::string_view service,
                            std::string_view provider_id,
                            std::string_view version) const
    -> std::optional<provider_info> {
    std::shared_lock lock{mutex_};
    return find_candidate(providers_, service, provider_id, version);
}

auto service_registry::has_service(std::string_view service) const -> bool {
    std::shared_lock lock{mutex_};
    return std::any_of(providers_.begin(), providers_.end(),
                       [service](const provider_info &candidate) {
                           return candidate.service == service;
                       });
}

auto service_registry::providers(std::string_view service) const
    -> std::vector<provider_info> {
    std::shared_lock lock{mutex_};
    std::vector<provider_info> result;
    for (const auto &candidate : providers_) {
        if (candidate.service == service) {
            result.push_back(candidate);
        }
    }
    return result;
}

auto service_registry::services() const -> std::vector<std::string> {
    std::shared_lock lock{mutex_};
    std::vector<std::string> names;
    names.reserve(providers_.size());
    for (const auto &candidate : providers_) {
        names.push_back(candidate.service);
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

auto service_registry::active_services() const -> std::vector<std::string> {
    std::shared_lock lock{mutex_};
    std::vector<std::string> names;
    names.reserve(active_.size());
    for (const auto &slot : active_) {
        // Instance-major within a service: only push when the service changes.
        if (names.empty() || names.back() != slot.first.first) {
            names.push_back(slot.first.first);
        }
    }
    return names;
}

auto service_registry::dump() const -> std::string {
    std::shared_lock lock{mutex_};
    if (providers_.empty()) {
        return "(empty)";
    }

    std::vector<const provider_info *> ordered;
    ordered.reserve(providers_.size());
    for (const auto &candidate : providers_) {
        ordered.push_back(&candidate);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const provider_info *lhs, const provider_info *rhs) {
                  if (lhs->service != rhs->service) {
                      return lhs->service < rhs->service;
                  }
                  if (lhs->id != rhs->id) {
                      return lhs->id < rhs->id;
                  }
                  return lhs->version < rhs->version;
              });

    std::string out = fmt::format(
        "refrain service registry: {} provider(s), {} active instance(s)\n",
        providers_.size(), active_.size());
    for (const auto *candidate : ordered) {
        // Active instances of this exact candidate, map-ordered by name.
        std::string instances;
        const auto first =
            active_.lower_bound(active_key{candidate->service, std::string{}});
        for (auto slot = first;
             slot != active_.end() && slot->first.first == candidate->service;
             ++slot) {
            if (slot->second.provider_id == candidate->id &&
                slot->second.version == candidate->version) {
                if (!instances.empty()) {
                    instances += ", ";
                }
                instances += slot->first.second;
            }
        }

        out += fmt::format("{} -> {} [{}] caps=0x{:016x}", candidate->service,
                           candidate->id, candidate->version,
                           candidate->capabilities);
        if (!instances.empty()) {
            out += fmt::format(" (active: {})", instances);
        }
        out += '\n';
    }
    return out;
}

auto service_registry::clear() -> status {
    std::unique_lock lock{mutex_};
    if (frozen_) {
        return std::unexpected{make_error(
            error_code::unsupported,
            std::string_view{
                "service registry is frozen: cannot clear providers"})};
    }
    providers_.clear();
    active_.clear();
    return status_ok();
}

void service_registry::freeze() {
    std::unique_lock lock{mutex_};
    frozen_ = true;
}

auto service_registry::is_frozen() const -> bool {
    std::shared_lock lock{mutex_};
    return frozen_;
}

auto service_registry::provider_count() const -> std::size_t {
    std::shared_lock lock{mutex_};
    return providers_.size();
}

auto service_registry::active_count() const -> std::size_t {
    std::shared_lock lock{mutex_};
    return active_.size();
}

} // namespace refrain::kernel
