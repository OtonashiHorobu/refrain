// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// `refrain:permissions` provider for the bundled mod `refrain.permissions`.
//
// The mod is statically linked into the host and registered through
// REFR_MOD_STATIC. Its factory publishes the base of the `refrain:permissions`
// chain: the configuration-driven default resolver. Queries answer from an
// immutable policy snapshot, so they are read-only and callable from any
// thread; `reload` re-reads `config/permissions.toml` (the `permissions`
// configuration namespace) and publishes a new snapshot.
//
// The resolver reads the namespace file directly rather than the merged
// configuration view because the policy parser has to reject unknown tables and
// keys, which needs the document tree, and because a reload must be safe to run
// while other threads query the current snapshot.
//
// The provider is also the administration surface of the base resolver: the
// host-side `permissions::admin` interface publishes grant, revoke, groups and
// reload for the operator commands. A change edits the configuration file,
// writes it atomically and only then publishes the new in-memory snapshot, so a
// failed write leaves the running policy untouched.

#include <refrain/contracts/services/permissions_abi.h>
#include <refrain/core/config.hpp>
#include <refrain/core/error.hpp>
#include <refrain/core/io.hpp>
#include <refrain/core/log.hpp>
#include <refrain/host/abi_bridge.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/mod/static_mod.hpp>
#include <refrain/permissions/admin.hpp>
#include <refrain/permissions/persist.hpp>
#include <refrain/permissions/policy.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace abi = refrain::host::abi;
namespace config = refrain::core::config;
namespace io = refrain::core::io;
namespace mod = refrain::mod;
namespace permissions = refrain::permissions;

using refrain::error_code;
using refrain::make_error;
using refrain::services::permissions::permission_result;
using refrain::services::permissions::subject_kind;

constexpr refrain::log::logger log_permissions{"permissions"};

constexpr std::string_view namespace_name{"permissions"};
constexpr std::string_view provider_version{"1.0.0"};
constexpr std::string_view service_id{"refrain:permissions"};

/// Largest configuration file the resolver reads; the policy is a small rule
/// table, so a larger file is a mistake rather than a valid configuration.
constexpr std::uint64_t max_config_bytes{1ULL * 1024ULL * 1024ULL};

/// Contract status code for one resolver error.
[[nodiscard]] auto reload_status(const refrain::error &error) -> std::uint32_t {
    switch (error.code) {
    case error_code::parse_error:
        return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_ERROR_PARSE_ERROR;
    case error_code::io_error:
    case error_code::not_found:
    case error_code::unsupported:
        return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_ERROR_UNAVAILABLE;
    default:
        return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_ERROR_INVALID_ARGUMENT;
    }
}

/// The published policy plus the raw text it was built from. The text lets a
/// reload tell "the file did not change" from "it changed to the same policy",
/// so an unchanged reload neither allocates a snapshot nor bumps the revision.
struct policy_state {
    permissions::policy policy{};
    std::string source_text{};
};

class provider_impl final : public permissions::admin {
  public:
    /// Build the provider for one host load. A host without a mounted
    /// `permissions` namespace serves the built-in policy (deny every
    /// non-console subject) instead of failing: the service is usable before
    /// the operator writes a configuration.
    [[nodiscard]] static auto create(const mod::host &host)
        -> refrain::result<std::unique_ptr<provider_impl>> {
        std::string path{};
        if (const auto *store = host.config(); store != nullptr) {
            auto mounted{store->path(namespace_name)};
            if (mounted) {
                path = std::move(*mounted);
            } else if (mounted.error().code != error_code::not_found) {
                return std::unexpected{make_error(
                    mounted.error().code,
                    "permissions: cannot resolve the configuration file: " +
                        mounted.error().message)};
            }
        }

        auto provider{std::unique_ptr<provider_impl>{new provider_impl{}}};
        provider->path_ = std::move(path);
        auto loaded{provider->read_snapshot()};
        if (!loaded) {
            return std::unexpected{std::move(loaded.error())};
        }
        provider->publish(std::move(*loaded));
        const auto published{provider->published()};
        if (provider->path_.empty()) {
            log_permissions.info(
                "serving the built-in permission policy, no configuration "
                "namespace is mounted: console holds every node and every "
                "other subject is denied");
        } else {
            log_permissions.info(
                "loaded permission policy from '{}', {} subject rule sets and "
                "{} groups",
                provider->path_, published->policy.subjects.size(),
                published->policy.groups.size());
        }
        return provider;
    }

    provider_impl(const provider_impl &) = delete;
    auto operator=(const provider_impl &) -> provider_impl & = delete;
    provider_impl(provider_impl &&) = delete;
    auto operator=(provider_impl &&) -> provider_impl & = delete;
    ~provider_impl() override { permissions::retract_admin(this); }

    [[nodiscard]] auto config_path() const noexcept -> std::string_view {
        return path_;
    }

    /// Answer one query from the current snapshot.
    [[nodiscard]] auto
    query(const refrain_services_permissions_permission_query *query) const
        -> permission_result {
        if (!valid_query(query)) {
            return permission_result::denied;
        }
        const auto snapshot{policy_.load(std::memory_order_acquire)};
        const permissions::subject_view subject{
            .kind = static_cast<subject_kind>(query->subject.kind),
            .id = abi::to_view(query->subject.id),
            .profile_name = abi::to_view(query->subject.profile_name)};
        // Values above the vanilla ladder cannot widen an operator rule
        // further than level 4 already does, so clamp instead of rejecting.
        const auto op_level{std::min<std::uint32_t>(query->op_level, 4)};
        return permissions::evaluate(snapshot->policy, subject,
                                     abi::to_view(query->node), op_level);
    }

    // -- permissions::admin -------------------------------------------------
    [[nodiscard]] auto explain(const permissions::subject_view &subject,
                               std::string_view node,
                               std::uint32_t op_level) const
        -> permissions::decision_explanation override {
        const auto snapshot{policy_.load(std::memory_order_acquire)};
        return permissions::explain(snapshot->policy, subject, node, op_level);
    }

    [[nodiscard]] auto snapshot() const
        -> permissions::policy_snapshot override {
        const auto published{policy_.load(std::memory_order_acquire)};
        return permissions::policy_snapshot{
            .value = published->policy,
            .revision = revision_.load(std::memory_order_acquire),
            .persistent = !path_.empty(),
        };
    }

    [[nodiscard]] auto grant(std::string_view subject, std::string_view rule)
        -> refrain::result<permissions::policy_change> override {
        return change(subject, rule, true);
    }

    [[nodiscard]] auto revoke(std::string_view subject, std::string_view rule)
        -> refrain::result<permissions::policy_change> override {
        return change(subject, rule, false);
    }

    [[nodiscard]] auto groups() const
        -> std::vector<permissions::group_listing> override {
        const auto published{policy_.load(std::memory_order_acquire)};
        return permissions::list_groups(published->policy);
    }

    [[nodiscard]] auto reload()
        -> refrain::result<permissions::reload_outcome> override {
        std::lock_guard lock{reload_mutex_};
        const auto previous{revision_.load(std::memory_order_acquire)};
        if (path_.empty()) {
            // No mounted namespace: the built-in policy is the whole policy, so
            // a reload has nothing to read.
            return permissions::reload_outcome{
                .changed = false,
                .previous_revision = previous,
                .revision = previous,
            };
        }
        auto next{read_snapshot()};
        if (!next) {
            return std::unexpected{std::move(next.error())};
        }
        const auto committed{policy_.load(std::memory_order_acquire)};
        if (next->source_text == committed->source_text) {
            return permissions::reload_outcome{
                .changed = false,
                .previous_revision = previous,
                .revision = previous,
            };
        }
        const auto subject_count{next->policy.subjects.size()};
        const auto group_count{next->policy.groups.size()};
        publish(std::move(*next));
        const auto reloaded{revision_.load(std::memory_order_acquire)};
        log_permissions.info(
            "reloaded permission policy from '{}' at revision {}, {} subject "
            "rule sets and {} groups",
            path_, reloaded, subject_count, group_count);
        return permissions::reload_outcome{
            .changed = true,
            .previous_revision = previous,
            .revision = reloaded,
        };
    }

    [[nodiscard]] auto revision() const noexcept -> std::uint64_t override {
        return revision_.load(std::memory_order_acquire);
    }

    [[nodiscard]] auto persistent() const noexcept -> bool override {
        return !path_.empty();
    }

  private:
    provider_impl() = default;

    /// Apply one grant or revoke to the policy and the configuration file.
    ///
    /// The order is deliberate: validate the rule, edit the file text, validate
    /// the edited document, write it atomically and only then publish the new
    /// snapshot. A failed write therefore leaves the running policy and the
    /// revision untouched, and a successful write is already durable before a
    /// query can observe it.
    [[nodiscard]] auto change(std::string_view subject, std::string_view rule,
                              bool add)
        -> refrain::result<permissions::policy_change> {
        std::lock_guard lock{reload_mutex_};
        if (path_.empty()) {
            return std::unexpected{make_error(
                error_code::unsupported,
                "the permission policy was not loaded from a configuration "
                "file, so a change cannot be persisted")};
        }
        auto parsed_rule{permissions::parse_rule(rule, "permission rule")};
        if (!parsed_rule) {
            return std::unexpected{std::move(parsed_rule.error())};
        }
        if (!permissions::valid_subject_key(subject)) {
            return std::unexpected{make_error(
                error_code::invalid_argument,
                "the permission subject is not a configurable identity")};
        }
        const auto committed{policy_.load(std::memory_order_acquire)};
        auto edited{permissions::edit_grants(committed->source_text, subject,
                                             rule, add)};
        if (!edited) {
            return std::unexpected{std::move(edited.error())};
        }
        const auto canonical_rule{permissions::rule_text(*parsed_rule)};
        const auto unchanged{*edited == committed->source_text};
        if (unchanged) {
            return permissions::policy_change{
                .changed = false,
                .revision = revision_.load(std::memory_order_acquire),
                .subject = std::string{subject},
                .rule = canonical_rule,
            };
        }
        auto document{config::document::parse_text(*edited, path_)};
        if (!document) {
            return std::unexpected{make_error(
                error_code::unknown,
                "the edited permission configuration is not valid TOML")};
        }
        auto next_policy{permissions::parse_policy(*document)};
        if (!next_policy) {
            return std::unexpected{make_error(
                error_code::unknown,
                "the edited permission configuration is not a valid policy")};
        }
        if (auto written{io::atomic_write_text(path_, *edited)}; !written) {
            log_permissions.warn(
                "failed to write the permission policy to '{}', keeping "
                "revision {}: {}",
                path_, revision_.load(std::memory_order_acquire),
                written.error().message);
            return std::unexpected{make_error(
                written.error().code,
                "failed to write the permission configuration file")};
        }
        publish(policy_state{.policy = std::move(*next_policy),
                             .source_text = std::move(*edited)});
        const auto revision{revision_.load(std::memory_order_acquire)};
        log_permissions.info(
            "{} '{}' for {} in the permission policy, revision {}",
            add ? "granted" : "revoked", canonical_rule, subject, revision);
        return permissions::policy_change{
            .changed = true,
            .revision = revision,
            .subject = std::string{subject},
            .rule = canonical_rule,
        };
    }

    /// The currently published snapshot; the caller keeps it alive while it
    /// reads the policy.
    [[nodiscard]] auto published() const
        -> std::shared_ptr<const policy_state> {
        return policy_.load(std::memory_order_acquire);
    }

    /// Read and validate the configuration. A missing file is an empty policy,
    /// which the caller turns into the built-in deny-by-default behaviour.
    [[nodiscard]] auto read_snapshot() -> refrain::result<policy_state> {
        std::string text{};
        if (!path_.empty() && io::exists(path_)) {
            auto read{io::read_text(path_, {.max_bytes = max_config_bytes,
                                            .allow_symlink = false})};
            if (!read) {
                return std::unexpected{make_error(
                    read.error().code, "permissions: cannot read '" + path_ +
                                           "': " + read.error().message)};
            }
            text = std::move(*read);
        }

        policy_state value{};
        value.source_text = std::move(text);
        if (value.source_text.empty()) {
            return value;
        }
        auto document{config::document::parse_text(value.source_text, path_)};
        if (!document) {
            return std::unexpected{
                make_error(error_code::parse_error,
                           "permissions: invalid configuration: " +
                               document.error().message)};
        }
        auto policy{permissions::parse_policy(*document)};
        if (!policy) {
            return std::unexpected{
                make_error(policy.error().code, policy.error().message)};
        }
        value.policy = std::move(*policy);
        return value;
    }

    void publish(policy_state next) {
        policy_.store(std::make_shared<const policy_state>(std::move(next)),
                      std::memory_order_release);
        revision_.fetch_add(1, std::memory_order_acq_rel);
    }

    /// A malformed query is denied instead of being evaluated against the
    /// wrong bytes: the caller must fill the struct sizes the generated
    /// wrapper sets.
    [[nodiscard]] static auto valid_query(
        const refrain_services_permissions_permission_query *query) noexcept
        -> bool {
        if (query == nullptr) {
            return false;
        }
        if (query->struct_size <
            sizeof(refrain_services_permissions_permission_query)) {
            return false;
        }
        if (query->subject.struct_size <
            sizeof(refrain_services_permissions_subject_ref)) {
            return false;
        }
        if (query->context.struct_size <
            sizeof(refrain_services_permissions_permission_context)) {
            return false;
        }
        return query->subject.kind <=
               static_cast<std::uint32_t>(subject_kind::plugin);
    }

  public:
    // -- C ABI slots --------------------------------------------------------
    static auto
    abi_query(void *self,
              const refrain_services_permissions_permission_query *query)
        -> refrain_services_permissions_permission_result {
        const auto *provider{static_cast<const provider_impl *>(self)};
        if (provider == nullptr) {
            return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_DENIED;
        }
        return static_cast<refrain_services_permissions_permission_result>(
            provider->query(query));
    }

    static auto abi_default_result(
        void *self, const refrain_services_permissions_permission_query *query)
        -> refrain_services_permissions_permission_result {
        // The base resolver has no wrapper grants to ignore: its own answer is
        // the chain fallback.
        return abi_query(self, query);
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
        const auto *provider{static_cast<const provider_impl *>(self)};
        return provider == nullptr ? 0U : provider->revision();
    }

    static auto abi_reload(void *self, std::uint64_t *out_value)
        -> refrain_status {
        auto *provider{static_cast<provider_impl *>(self)};
        if (provider == nullptr || out_value == nullptr) {
            return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_ERROR_INVALID_ARGUMENT;
        }
        auto reloaded{provider->reload()};
        if (!reloaded) {
            log_permissions.warn(
                "failed to reload permission policy from '{}': {}; keeping "
                "revision {}",
                provider->config_path(), reloaded.error().message,
                provider->revision());
            return reload_status(reloaded.error());
        }
        *out_value = reloaded->revision;
        return REFRAIN_STATUS_OK;
    }

  private:
    std::string path_{};
    mutable std::mutex reload_mutex_{};
    std::atomic<std::shared_ptr<const policy_state>> policy_{
        std::make_shared<const policy_state>()};
    std::atomic<std::uint64_t> revision_{0};
};

auto permissions_vtable() -> const refrain_services_permissions_vtable & {
    static const refrain_services_permissions_vtable table{
        .struct_size = REFRAIN_SERVICES_PERMISSIONS_VTABLE_SIZE,
        .abi_major = REFRAIN_SERVICES_PERMISSIONS_ABI_MAJOR,
        .abi_minor = REFRAIN_SERVICES_PERMISSIONS_ABI_MINOR,
        .abi_patch = REFRAIN_SERVICES_PERMISSIONS_ABI_PATCH,
        .query = &provider_impl::abi_query,
        .default_result = &provider_impl::abi_default_result,
        .has = &provider_impl::abi_has,
        .revision = &provider_impl::abi_revision,
        .reload = &provider_impl::abi_reload,
    };
    return table;
}

auto make_permissions(refrain::mod::host &host) -> refrain::status {
    // The provider is recreated for each host load (tests create several
    // hosts; production loads exactly one). The host copies the vtable/self
    // pair into its own stable storage, so the host's frozen registry never
    // points at the next host's instance.
    static std::unique_ptr<provider_impl> provider{};
    auto created{provider_impl::create(host)};
    if (!created) {
        provider.reset();
        return std::unexpected{std::move(created.error())};
    }
    provider = std::move(*created);
    const refrain::mod::provider_api api{
        .vtable = &permissions_vtable(),
        .self = provider.get(),
        .abi_major = REFRAIN_SERVICES_PERMISSIONS_ABI_MAJOR};
    auto registered{host.register_provider(service_id, api, provider_version)};
    if (!registered) {
        provider.reset();
        return registered;
    }
    // The service is active, so the management surface answers for this
    // process until the composition replaces the provider.
    permissions::publish_admin(provider.get());
    return refrain::status_ok();
}

} // namespace

REFR_MOD_STATIC("refrain.permissions", "1.0.0", 1, make_permissions);
