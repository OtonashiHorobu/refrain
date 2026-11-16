// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/kernel/service_registry.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace refrain::kernel {
namespace {

/// Poll interval of the bounded drain wait. A lease is usually held for one
/// call, so the granularity only bounds how late the wait notices an empty
/// counter; it is not a latency path.
constexpr auto drain_poll_interval = std::chrono::milliseconds{1};

} // namespace

auto service_registry::resolve_instance(std::string_view instance)
    -> std::string_view {
    return instance.empty() ? default_instance : instance;
}

auto service_registry::key_of(std::string_view service,
                              std::string_view instance) -> active_key {
    return active_key{std::string{service},
                      std::string{resolve_instance(instance)}};
}

auto service_registry::find_candidate_locked(std::string_view service,
                                             std::string_view provider_id,
                                             std::string_view version) const
    -> const candidate_entry * {
    for (const auto &candidate : candidates_) {
        if (candidate.info.service == service &&
            candidate.info.id == provider_id &&
            (version.empty() || candidate.info.version == version)) {
            return &candidate;
        }
    }
    return nullptr;
}

auto service_registry::find_serial_locked(std::uint64_t serial) const
    -> const candidate_entry * {
    for (const auto &candidate : candidates_) {
        if (candidate.serial == serial) {
            return &candidate;
        }
    }
    return nullptr;
}

auto service_registry::active_locked(const active_key &key) const
    -> const active_entry * {
    const auto slot = active_.find(key);
    return slot == active_.end() ? nullptr : &slot->second;
}

auto service_registry::provider_copy_locked(const active_entry &entry) const
    -> provider_info {
    const auto *candidate = find_serial_locked(entry.serial);
    if (candidate == nullptr) {
        return provider_info{};
    }
    auto copy{candidate->info};
    copy.generation = entry.generation;
    return copy;
}

auto service_registry::in_flight_locked(const active_key &key) const
    -> std::shared_ptr<std::atomic<std::uint64_t>> {
    const auto found = in_flight_.find(key);
    return found == in_flight_.end() ? nullptr : found->second;
}

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
    if (sealed_services_.contains(info.service)) {
        return std::unexpected{make_error(
            error_code::unsupported,
            "service '" + info.service +
                "' is sealed: only the host bootstrap may provide it")};
    }

    if (find_candidate_locked(info.service, info.id, info.version) != nullptr) {
        return std::unexpected{make_error(
            error_code::already_exists,
            "provider '" + info.id + "' for service '" + info.service +
                "' version '" + info.version + "' is already registered")};
    }

    candidates_.push_back(
        candidate_entry{.info = std::move(info), .serial = next_serial_++});
    return status_ok();
}

auto service_registry::add_provider_serial(provider_info info)
    -> result<std::uint64_t> {
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
    if (sealed_services_.contains(info.service)) {
        return std::unexpected{make_error(
            error_code::unsupported,
            "service '" + info.service +
                "' is sealed: only the host bootstrap may provide it")};
    }
    if (find_candidate_locked(info.service, info.id, info.version) != nullptr) {
        return std::unexpected{make_error(
            error_code::already_exists,
            "provider '" + info.id + "' for service '" + info.service +
                "' version '" + info.version + "' is already registered")};
    }
    const auto serial{next_serial_++};
    candidates_.push_back(
        candidate_entry{.info = std::move(info), .serial = serial});
    return serial;
}

auto service_registry::add_staged_provider(provider_info info)
    -> result<std::uint64_t> {
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
    if (sealed_services_.contains(info.service)) {
        return std::unexpected{make_error(
            error_code::unsupported,
            "service '" + info.service +
                "' is sealed: only the host bootstrap may provide it")};
    }
    const auto serial{next_serial_++};
    candidates_.push_back(
        candidate_entry{.info = std::move(info), .serial = serial});
    return serial;
}

auto service_registry::remove_provider(std::uint64_t serial) -> status {
    std::unique_lock lock{mutex_};
    if (frozen_) {
        return std::unexpected{make_error(
            error_code::unsupported,
            std::string_view{
                "service registry is frozen: cannot remove a provider"})};
    }
    const auto *candidate = find_serial_locked(serial);
    if (candidate == nullptr) {
        return std::unexpected{make_error(
            error_code::not_found,
            fmt::format("provider serial {} is not registered", serial))};
    }
    for (const auto &slot : active_) {
        if (slot.second.serial == serial) {
            return std::unexpected{make_error(
                error_code::already_exists,
                fmt::format("provider '{}' for service '{}' instance '{}' is "
                            "active; retire the slot before removing the "
                            "candidate",
                            candidate->info.id, candidate->info.service,
                            slot.first.second))};
        }
    }
    for (const auto &staged : staged_) {
        if (staged.second.has_value() && *staged.second == serial) {
            return std::unexpected{make_error(
                error_code::already_exists,
                fmt::format("provider '{}' for service '{}' instance '{}' has "
                            "a staged activation",
                            candidate->info.id, candidate->info.service,
                            staged.first.second))};
        }
    }
    const auto id{candidate->info.id};
    const auto service{candidate->info.service};
    std::erase_if(candidates_, [serial](const candidate_entry &entry) {
        return entry.serial == serial;
    });
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
    if (sealed_services_.contains(service)) {
        return std::unexpected{
            make_error(error_code::unsupported,
                       "service '" + std::string{service} +
                           "' is sealed and cannot be re-selected")};
    }

    const auto *candidate =
        find_candidate_locked(service, provider_id, version);
    if (candidate == nullptr) {
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
    const auto key{key_of(service, instance)};
    active_.insert_or_assign(
        key, active_entry{.serial = candidate->serial,
                          .generation = slot_generations_[key]});
    dirty_.insert(key);
    staged_.erase(key);
    return status_ok();
}

auto service_registry::stage_active(std::uint64_t serial,
                                    std::string_view service,
                                    std::string_view instance) -> status {
    if (service.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       std::string_view{"service name must not be empty"})};
    }

    std::unique_lock lock{mutex_};
    if (frozen_) {
        return std::unexpected{make_error(
            error_code::unsupported,
            "service registry is frozen: cannot stage an activation")};
    }
    if (sealed_services_.contains(service)) {
        return std::unexpected{make_error(
            error_code::unsupported, "service '" + std::string{service} +
                                         "' is sealed and cannot be staged")};
    }
    const auto *candidate = find_serial_locked(serial);
    if (candidate == nullptr) {
        return std::unexpected{make_error(
            error_code::not_found,
            fmt::format("provider serial {} is not registered", serial))};
    }
    if (candidate->info.service != service) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            fmt::format("provider serial {} belongs to service '{}', not '{}'",
                        serial, candidate->info.service, service))};
    }
    const auto key{key_of(service, instance)};
    staged_.insert_or_assign(key, serial);
    dirty_.erase(key);
    return status_ok();
}

auto service_registry::stage_active(std::string_view service,
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
            "service registry is frozen: cannot stage an activation")};
    }
    if (sealed_services_.contains(service)) {
        return std::unexpected{make_error(
            error_code::unsupported, "service '" + std::string{service} +
                                         "' is sealed and cannot be staged")};
    }
    const auto *candidate =
        find_candidate_locked(service, provider_id, version);
    if (candidate == nullptr) {
        std::string message{"no provider '" + std::string{provider_id} +
                            "' registered for service '" +
                            std::string{service} + "'"};
        if (!version.empty()) {
            message += " version '" + std::string{version} + "'";
        }
        return std::unexpected{
            make_error(error_code::not_found, std::move(message))};
    }
    const auto serial{candidate->serial};
    const auto key{key_of(service, instance)};
    staged_.insert_or_assign(key, serial);
    dirty_.erase(key);
    return status_ok();
}

auto service_registry::stage_retire(std::string_view service,
                                    std::string_view instance) -> status {
    if (service.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       std::string_view{"service name must not be empty"})};
    }

    std::unique_lock lock{mutex_};
    if (frozen_) {
        return std::unexpected{make_error(
            error_code::unsupported,
            "service registry is frozen: cannot stage a retirement")};
    }
    if (sealed_services_.contains(service)) {
        return std::unexpected{make_error(
            error_code::unsupported, "service '" + std::string{service} +
                                         "' is sealed and cannot be retired")};
    }
    const auto key{key_of(service, instance)};
    const auto known{active_.contains(key) ||
                     std::any_of(candidates_.begin(), candidates_.end(),
                                 [service](const candidate_entry &entry) {
                                     return entry.info.service == service;
                                 })};
    if (!known) {
        return std::unexpected{make_error(error_code::not_found,
                                          "service '" + std::string{service} +
                                              "' instance '" + key.second +
                                              "' is not registered")};
    }
    staged_.insert_or_assign(key, std::nullopt);
    dirty_.erase(key);
    return status_ok();
}

auto service_registry::has_staged() const -> bool {
    std::shared_lock lock{mutex_};
    return !staged_.empty();
}

void service_registry::abort_staging() {
    std::unique_lock lock{mutex_};
    staged_.clear();
    quiescing_.clear();
}

auto service_registry::commit_epoch() -> result<std::uint64_t> {
    std::unique_lock lock{mutex_};
    bool changed{false};

    for (const auto &[key, staged] : staged_) {
        if (staged.has_value()) {
            auto &generation = slot_generations_[key];
            ++generation;
            active_.insert_or_assign(
                key, active_entry{.serial = *staged, .generation = generation});
            quiescing_.erase(key);
            if (!in_flight_.contains(key)) {
                in_flight_[key] = std::make_shared<std::atomic<std::uint64_t>>(
                    std::uint64_t{0});
            }
            changed = true;
        } else {
            if (active_.erase(key) > 0) {
                ++slot_generations_[key];
                changed = true;
            }
            quiescing_.erase(key);
        }
    }
    staged_.clear();

    for (const auto &key : dirty_) {
        const auto slot = active_.find(key);
        if (slot == active_.end()) {
            continue;
        }
        slot->second.generation = ++slot_generations_[key];
        if (!in_flight_.contains(key)) {
            in_flight_[key] =
                std::make_shared<std::atomic<std::uint64_t>>(std::uint64_t{0});
        }
        changed = true;
    }
    dirty_.clear();

    if (changed) {
        ++epoch_;
    }
    return epoch_;
}

auto service_registry::epoch() const -> std::uint64_t {
    std::shared_lock lock{mutex_};
    return epoch_;
}

auto service_registry::seal(std::string_view service) -> status {
    if (service.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       std::string_view{"service name must not be empty"})};
    }
    std::unique_lock lock{mutex_};
    if (frozen_) {
        return std::unexpected{make_error(
            error_code::unsupported,
            std::string_view{
                "service registry is frozen: cannot seal a service"})};
    }
    sealed_services_.insert(std::string{service});
    // A sealed service is immutable from now on: discard a change that was
    // staged before the seal, it can never be committed.
    std::erase_if(staged_, [service](const auto &entry) {
        return entry.first.first == service;
    });
    return status_ok();
}

auto service_registry::is_sealed(std::string_view service) const -> bool {
    std::shared_lock lock{mutex_};
    return sealed_services_.contains(std::string{service});
}

auto service_registry::active_provider(std::string_view service,
                                       std::string_view instance) const
    -> std::optional<provider_info> {
    std::shared_lock lock{mutex_};
    const auto *slot = active_locked(key_of(service, instance));
    if (slot == nullptr) {
        return std::nullopt;
    }
    auto copy{provider_copy_locked(*slot)};
    if (copy.service.empty()) {
        return std::nullopt;
    }
    return copy;
}

auto service_registry::query(std::string_view service,
                             std::string_view instance) const
    -> result<provider_handle> {
    std::shared_lock lock{mutex_};
    const auto key{key_of(service, instance)};
    if (quiescing_.contains(key)) {
        return std::unexpected{make_error(
            error_code::stale,
            "service '" + key.first + "' instance '" + key.second +
                "' is quiescing; new calls must wait for the composition "
                "epoch to commit")};
    }
    const auto *slot = active_locked(key);
    if (slot == nullptr) {
        return std::unexpected{make_error(error_code::not_found,
                                          "no active provider for service '" +
                                              key.first + "' instance '" +
                                              key.second + "'")};
    }
    auto provider{provider_copy_locked(*slot)};
    if (provider.service.empty()) {
        return std::unexpected{make_error(
            error_code::unknown,
            fmt::format("active slot for service '{}' instance '{}' references "
                        "an unknown provider serial {}",
                        key.first, key.second, slot->serial))};
    }
    return provider_handle{.provider = std::move(provider),
                           .instance = key.second,
                           .generation = slot->generation,
                           .epoch = epoch_};
}

auto service_registry::validate(const provider_handle &handle) const -> status {
    std::shared_lock lock{mutex_};
    const auto key{key_of(handle.provider.service, handle.instance)};
    if (quiescing_.contains(key)) {
        return std::unexpected{make_error(
            error_code::stale, "service '" + key.first + "' instance '" +
                                   key.second + "' is quiescing")};
    }
    const auto *slot = active_locked(key);
    if (slot == nullptr) {
        return std::unexpected{
            make_error(error_code::stale,
                       "service '" + key.first + "' instance '" + key.second +
                           "' has no active provider at epoch " +
                           std::to_string(epoch_) + "; generation " +
                           std::to_string(handle.generation) + " is stale")};
    }
    if (slot->generation != handle.generation) {
        return std::unexpected{
            make_error(error_code::stale,
                       "service '" + key.first + "' instance '" + key.second +
                           "' generation " + std::to_string(handle.generation) +
                           " was replaced by generation " +
                           std::to_string(slot->generation) + " at epoch " +
                           std::to_string(epoch_))};
    }
    return status_ok();
}

auto service_registry::staged_decision(std::string_view service,
                                       std::string_view instance) const
    -> std::optional<bool> {
    std::shared_lock lock{mutex_};
    const auto pending = staged_.find(key_of(service, instance));
    if (pending == staged_.end()) {
        return std::nullopt;
    }
    return pending->second.has_value();
}

auto service_registry::staged_active_provider(std::string_view service,
                                              std::string_view instance) const
    -> std::optional<provider_info> {
    std::shared_lock lock{mutex_};
    const auto key{key_of(service, instance)};
    const auto pending = staged_.find(key);
    if (pending != staged_.end()) {
        if (!pending->second.has_value()) {
            return std::nullopt;
        }
        const auto *candidate = find_serial_locked(*pending->second);
        if (candidate == nullptr) {
            return std::nullopt;
        }
        return candidate->info;
    }
    const auto *slot = active_locked(key);
    if (slot == nullptr) {
        return std::nullopt;
    }
    auto copy{provider_copy_locked(*slot)};
    if (copy.service.empty()) {
        return std::nullopt;
    }
    return copy;
}

auto service_registry::generation(std::string_view service,
                                  std::string_view instance) const
    -> std::optional<std::uint64_t> {
    std::shared_lock lock{mutex_};
    const auto *slot = active_locked(key_of(service, instance));
    if (slot == nullptr) {
        return std::nullopt;
    }
    return slot->generation;
}

auto service_registry::active_instances(std::string_view service) const
    -> std::vector<std::string> {
    std::shared_lock lock{mutex_};
    std::vector<std::string> instances;
    const auto first =
        active_.lower_bound(active_key{std::string{service}, {}});
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
    const auto *candidate =
        find_candidate_locked(service, provider_id, version);
    if (candidate == nullptr) {
        return std::nullopt;
    }
    return candidate->info;
}

auto service_registry::has_service(std::string_view service) const -> bool {
    std::shared_lock lock{mutex_};
    return std::any_of(candidates_.begin(), candidates_.end(),
                       [service](const candidate_entry &entry) {
                           return entry.info.service == service;
                       });
}

auto service_registry::providers(std::string_view service) const
    -> std::vector<provider_info> {
    std::shared_lock lock{mutex_};
    std::vector<provider_info> result;
    for (const auto &candidate : candidates_) {
        if (candidate.info.service == service) {
            result.push_back(candidate.info);
        }
    }
    return result;
}

auto service_registry::services() const -> std::vector<std::string> {
    std::shared_lock lock{mutex_};
    std::vector<std::string> names;
    names.reserve(candidates_.size());
    for (const auto &candidate : candidates_) {
        names.push_back(candidate.info.service);
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
    if (candidates_.empty()) {
        return "(empty)";
    }

    std::vector<const candidate_entry *> ordered;
    ordered.reserve(candidates_.size());
    for (const auto &candidate : candidates_) {
        ordered.push_back(&candidate);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const candidate_entry *lhs, const candidate_entry *rhs) {
                  if (lhs->info.service != rhs->info.service) {
                      return lhs->info.service < rhs->info.service;
                  }
                  if (lhs->info.id != rhs->info.id) {
                      return lhs->info.id < rhs->info.id;
                  }
                  return lhs->info.version < rhs->info.version;
              });

    std::string out = fmt::format(
        "refrain service registry: {} provider(s), {} active instance(s), "
        "epoch {}\n",
        candidates_.size(), active_.size(), epoch_);
    for (const auto *candidate : ordered) {
        // Active instances of this exact serial, map-ordered by name.
        std::string instances;
        const auto first =
            active_.lower_bound(active_key{candidate->info.service, {}});
        for (auto slot = first; slot != active_.end() &&
                                slot->first.first == candidate->info.service;
             ++slot) {
            if (slot->second.serial == candidate->serial) {
                if (!instances.empty()) {
                    instances += ", ";
                }
                instances += slot->first.second;
            }
        }

        out +=
            fmt::format("{} -> {} [{}] caps=0x{:016x}", candidate->info.service,
                        candidate->info.id, candidate->info.version,
                        candidate->info.capabilities);
        if (!instances.empty()) {
            out += fmt::format(" (active: {})", instances);
        }
        out += '\n';
    }
    return out;
}

auto service_registry::quiesce(std::string_view service,
                               std::string_view instance) -> status {
    std::unique_lock lock{mutex_};
    if (frozen_) {
        return std::unexpected{make_error(
            error_code::unsupported,
            std::string_view{
                "service registry is frozen: cannot quiesce a slot"})};
    }
    const auto key{key_of(service, instance)};
    if (active_locked(key) == nullptr) {
        return std::unexpected{make_error(
            error_code::not_found, "service '" + key.first + "' instance '" +
                                       key.second + "' is not active")};
    }
    quiescing_.insert(key);
    return status_ok();
}

auto service_registry::resume(std::string_view service,
                              std::string_view instance) -> status {
    std::unique_lock lock{mutex_};
    quiescing_.erase(key_of(service, instance));
    return status_ok();
}

auto service_registry::acquire(std::string_view service,
                               std::string_view instance) const
    -> result<provider_lease> {
    std::unique_lock<std::shared_mutex> lock{mutex_};
    const auto key{key_of(service, instance)};
    if (quiescing_.contains(key)) {
        return std::unexpected{
            make_error(error_code::stale,
                       "service '" + key.first + "' instance '" + key.second +
                           "' is quiescing; new calls are rejected")};
    }
    if (active_locked(key) == nullptr) {
        return std::unexpected{make_error(error_code::not_found,
                                          "no active provider for service '" +
                                              key.first + "' instance '" +
                                              key.second + "'")};
    }
    auto &counter = in_flight_[key];
    if (counter == nullptr) {
        counter =
            std::make_shared<std::atomic<std::uint64_t>>(std::uint64_t{0});
    }
    counter->fetch_add(1, std::memory_order_acq_rel);
    return provider_lease{counter};
}

auto service_registry::acquire(const provider_handle &handle) const
    -> result<provider_lease> {
    std::unique_lock<std::shared_mutex> lock{mutex_};
    const auto key{key_of(handle.provider.service, handle.instance)};
    if (quiescing_.contains(key)) {
        return std::unexpected{
            make_error(error_code::stale,
                       "service '" + key.first + "' instance '" + key.second +
                           "' is quiescing; new calls are rejected")};
    }
    const auto *slot = active_locked(key);
    if (slot == nullptr || slot->generation != handle.generation) {
        return std::unexpected{
            make_error(error_code::stale,
                       "service '" + key.first + "' instance '" + key.second +
                           "' generation " + std::to_string(handle.generation) +
                           " is stale at epoch " + std::to_string(epoch_))};
    }
    auto &counter = in_flight_[key];
    if (counter == nullptr) {
        counter =
            std::make_shared<std::atomic<std::uint64_t>>(std::uint64_t{0});
    }
    counter->fetch_add(1, std::memory_order_acq_rel);
    return provider_lease{counter};
}

auto service_registry::drain(std::string_view service,
                             std::string_view instance,
                             std::chrono::milliseconds timeout) const
    -> status {
    std::shared_ptr<std::atomic<std::uint64_t>> counter;
    {
        std::shared_lock lock{mutex_};
        counter = in_flight_locked(key_of(service, instance));
    }
    if (counter == nullptr) {
        return status_ok();
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (counter->load(std::memory_order_acquire) != 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::unexpected{make_error(
                error_code::timeout,
                "service '" + std::string{service} + "' instance '" +
                    std::string{instance} + "' did not drain: " +
                    std::to_string(counter->load(std::memory_order_acquire)) +
                    " call(s) still in flight after " +
                    std::to_string(timeout.count()) + "ms")};
        }
        std::this_thread::sleep_for(drain_poll_interval);
    }
    return status_ok();
}

auto service_registry::drain(std::string_view service,
                             std::chrono::milliseconds timeout) const
    -> status {
    return drain(service, {}, timeout);
}

auto service_registry::in_flight(std::string_view service,
                                 std::string_view instance) const
    -> std::uint64_t {
    std::shared_lock lock{mutex_};
    const auto counter = in_flight_locked(key_of(service, instance));
    return counter == nullptr ? 0 : counter->load(std::memory_order_acquire);
}

auto service_registry::clear() -> status {
    std::unique_lock lock{mutex_};
    if (frozen_) {
        return std::unexpected{make_error(
            error_code::unsupported,
            std::string_view{
                "service registry is frozen: cannot clear providers"})};
    }
    candidates_.clear();
    active_.clear();
    slot_generations_.clear();
    staged_.clear();
    dirty_.clear();
    quiescing_.clear();
    sealed_services_.clear();
    in_flight_.clear();
    epoch_ = 0;
    return status_ok();
}

void service_registry::freeze() {
    std::unique_lock lock{mutex_};
    frozen_ = true;
    staged_.clear();
    quiescing_.clear();
}

auto service_registry::is_frozen() const -> bool {
    std::shared_lock lock{mutex_};
    return frozen_;
}

auto service_registry::provider_count() const -> std::size_t {
    std::shared_lock lock{mutex_};
    return candidates_.size();
}

auto service_registry::active_count() const -> std::size_t {
    std::shared_lock lock{mutex_};
    return active_.size();
}

} // namespace refrain::kernel
