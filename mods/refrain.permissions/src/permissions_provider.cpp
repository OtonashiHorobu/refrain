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
//
// The provider answers through a chain: the committed composition may declare
// wrapper providers in front of this base, and the provider composes them into
// one `permissions::provider_chain` per composition epoch. Wrappers answer only
// the nodes they own and answer `not_handled` for everything else, so a
// deployment can extend the policy without replacing the base resolver. The
// composed revision is the base revision plus every wrapper revision, so a
// change in either invalidates the command-tree cache, and `reload` reloads the
// base and then the wrappers.
//
// The chain is resolved from the thread-safe service registry, not from the
// gamepack plan: the base occupies the reserved default instance and every
// declared wrapper occupies its own `chain:<position>` instance, so a wrapper
// generation can be reloaded, replaced or retired while the base keeps
// answering. A registry snapshot reports the wrapper set together with the
// epoch that published it, so a rebuilt chain always describes one committed
// generation and can never re-resolve a retired predecessor from stale
// candidate order. The snapshot also carries the generation of every slot; a
// query leases each wrapper slot before walking the chain and holds the leases
// until the answer is complete, so a commit that retires a wrapper drains the
// walk before unloading it. A query that races the commit without being able to
// lease a link answers from the base resolver alone for that one call, and the
// next epoch rebuilds the chain. A failed stage changes neither the registry
// nor the running chain. The provider is created during the host load and is
// first queried after `load()` returns, when the composition is committed. A
// query issued by a mod factory earlier in the same load answers from the
// base-only chain and is not cached, so it cannot pin a wrapper-less link set.

#include <refrain/contracts/services/permissions_abi.h>
#include <refrain/core/config.hpp>
#include <refrain/core/error.hpp>
#include <refrain/core/io.hpp>
#include <refrain/core/log.hpp>
#include <refrain/host/abi_bridge.hpp>
#include <refrain/kernel/service_registry.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/mod/static_mod.hpp>
#include <refrain/permissions/admin.hpp>
#include <refrain/permissions/chain.hpp>
#include <refrain/permissions/persist.hpp>
#include <refrain/permissions/policy.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::permissions::test_seam {

/// Test-only seam. A test increments this counter to make the next chain
/// refresh publish a fresh chain instance even though the configured link set
/// did not change. It lets the snapshot lifetime and the monotonic revision be
/// stressed without a host that can publish a link change; production code
/// never touches it.
std::atomic<std::uint64_t> forced_chain_rebuilds{0};

/// Test-only seam. When set, `query()` calls it after acquiring the published
/// chain snapshot and before walking it. A test sets it to force the snapshot
/// to be replaced at exactly that point; production code never touches it.
std::function<void()> query_probe{};

} // namespace refrain::permissions::test_seam

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
constexpr std::string_view provider_id{"refrain.permissions"};
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
    case error_code::stale:
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

/// Format a count with the noun form the count requires: logging never prints
/// a `(s)` marker.
[[nodiscard]] auto count_noun(std::size_t count, std::string_view singular,
                              std::string_view plural) -> std::string {
    return fmt::format("{} {}", count, count == 1 ? singular : plural);
}

/// One resolved chain wrapper: the callable link plus the registry handle it
/// was read from. The handle names the exact provider generation, so a walk can
/// lease it before calling the table and a commit that retires that generation
/// drains the walk instead of freeing the table underneath it.
struct chain_link {
    std::size_t position{0};
    permissions::provider_link link{};
    refrain::kernel::provider_handle handle{};
};

/// One published link set: the composition epoch it was resolved at, the chain
/// itself, the registry handles its links were read from and a monotonic
/// offset. The offset keeps the facade revision increasing when a link-set
/// change lowers the raw sum (for example when a wrapper with a higher revision
/// is removed); it only changes together with the chain in one atomically
/// published value. The value is immutable once published, and a reader pins it
/// for the duration of one walk.
struct chain_state {
    std::uint64_t epoch{0};
    std::uint64_t revision_offset{0};
    std::shared_ptr<const permissions::provider_chain> chain{};
    /// Query order: outermost wrapper first; empty for the base-only chain.
    std::vector<chain_link> links{};
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
        provider->host_ = &host;
        provider->path_ = std::move(path);
        auto loaded{provider->read_snapshot()};
        if (!loaded) {
            return std::unexpected{std::move(loaded.error())};
        }
        provider->publish(std::move(*loaded));
        auto fallback{provider->base_only_chain()};
        if (!fallback) {
            return std::unexpected{std::move(fallback.error())};
        }
        provider->fallback_chain_ = std::move(*fallback);
        provider->fallback_state_ = std::make_shared<const chain_state>(
            chain_state{.epoch = 0,
                        .revision_offset = 0,
                        .chain = provider->fallback_chain_});
        provider->chain_.store(provider->fallback_state_,
                               std::memory_order_release);
        const auto published{provider->published()};
        if (provider->path_.empty()) {
            log_permissions.info(
                "serving the built-in permission policy, no configuration "
                "namespace is mounted: console holds every node and every "
                "other subject is denied");
        } else {
            log_permissions.info(
                "loaded permission policy from '{}', {}, {} and {}",
                provider->path_,
                count_noun(published->policy.subjects.size(),
                           "subject rule set", "subject rule sets"),
                count_noun(published->policy.context_subjects.size(),
                           "context rule set", "context rule sets"),
                count_noun(published->policy.groups.size(), "group", "groups"));
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

    /// Answer one query through the current composed chain. The walk pins every
    /// wrapper generation with a registry lease before the first call, so a
    /// commit that retires a wrapper has to drain this walk first and the table
    /// stays valid until the answer is complete. When the link set is being
    /// replaced at this instant, the base resolver answers this query alone and
    /// the next epoch rebuilds the chain: a table retired by a committed epoch
    /// is never called.
    [[nodiscard]] auto
    query(const refrain_services_permissions_permission_query *value) const
        -> permission_result {
        if (!valid_query(value)) {
            return permission_result::denied;
        }
        const auto walk{begin_walk()};
        if (!walk.complete) {
            return query_base(value);
        }
        if (permissions::test_seam::query_probe) {
            permissions::test_seam::query_probe();
        }
        return walk.state->chain->query(*value);
    }

    /// Answer the way the base resolver answers alone, ignoring wrapper grants.
    [[nodiscard]] auto default_result(
        const refrain_services_permissions_permission_query *value) const
        -> permission_result {
        return query_base(value);
    }

    /// Tri-state predicate over the composed answer.
    [[nodiscard]] auto
    has(const refrain_services_permissions_permission_query *value) const
        -> bool {
        return query(value) != permission_result::denied;
    }

    /// Monotonic composed revision of the base policy, the wrapper revisions
    /// and the link-set changes. A cache keyed by it invalidates on a policy
    /// change, a wrapper change and a wrapper removal alike. The last published
    /// value is a floor: a walk that cannot pin its links (a commit is
    /// replacing one at this instant) reports the floor instead of dropping,
    /// and a link-set change that lowers the raw sum is offset past it.
    [[nodiscard]] auto revision() const -> std::uint64_t {
        const auto walk{begin_walk()};
        if (!walk.complete) {
            const auto floor{last_revision_.load(std::memory_order_acquire)};
            return floor != 0 ? floor : base_revision();
        }
        const auto raw{walk.state->chain->revision() +
                       walk.state->revision_offset};
        const auto previous{
            last_revision_.fetch_max(raw, std::memory_order_acq_rel)};
        return std::max(previous, raw);
    }

    /// Reload the composed chain: the base policy first, then the wrappers in
    /// query order. The walk leases every wrapper for the duration, so a
    /// concurrent epoch cannot retire one while it reloads. The reported
    /// revisions are the monotonic facade values, so a caller can compare them
    /// with `revision()`.
    [[nodiscard]] auto reload_collect()
        -> refrain::result<permissions::chain_reload_outcome> {
        const auto walk{begin_walk()};
        if (!walk.complete) {
            return std::unexpected{make_error(
                error_code::stale, "the permission chain is being replaced at "
                                   "a composition epoch; "
                                   "retry the reload after the commit")};
        }
        auto result{walk.state->chain->reload()};
        if (!result) {
            return std::unexpected{std::move(result.error())};
        }
        result->previous_revision += walk.state->revision_offset;
        result->revision += walk.state->revision_offset;
        return result;
    }

    // -- permissions::admin ------------------------------------------------
    [[nodiscard]] auto explain(const permissions::subject_view &subject,
                               std::string_view node,
                               std::uint32_t op_level) const
        -> permissions::decision_explanation override {
        return explain(subject, node, op_level, {});
    }

    [[nodiscard]] auto explain(const permissions::subject_view &subject,
                               std::string_view node, std::uint32_t op_level,
                               const permissions::context_view &context) const
        -> permissions::decision_explanation override {
        const auto snapshot{policy_.load(std::memory_order_acquire)};
        return permissions::explain(snapshot->policy, subject, node, op_level,
                                    context);
    }

    [[nodiscard]] auto snapshot() const
        -> permissions::policy_snapshot override {
        const auto published{policy_.load(std::memory_order_acquire)};
        return permissions::policy_snapshot{
            .value = published->policy,
            .revision = revision(),
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
        auto reloaded{reload_collect()};
        if (!reloaded) {
            return std::unexpected{std::move(reloaded.error())};
        }
        return permissions::reload_outcome{
            .changed = reloaded->changed,
            .previous_revision = reloaded->previous_revision,
            .revision = reloaded->revision,
        };
    }

    [[nodiscard]] auto persistent() const noexcept -> bool override {
        return !path_.empty();
    }

  private:
    provider_impl() = default;

    // -- base resolver -----------------------------------------------------

    /// Answer one query against the published base policy alone.
    [[nodiscard]] auto
    query_base(const refrain_services_permissions_permission_query *query) const
        -> permission_result {
        if (!valid_query(query)) {
            return permission_result::denied;
        }
        std::vector<permissions::context_value_view> extra{};
        const auto context{context_view_of(*query, extra)};
        if (!context.has_value()) {
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
                                     abi::to_view(query->node), op_level,
                                     *context);
    }

    /// Revision of the published base policy alone; the chain sums it with the
    /// wrapper revisions.
    [[nodiscard]] auto base_revision() const noexcept -> std::uint64_t {
        return revision_.load(std::memory_order_acquire);
    }

    /// Re-read the configuration file and publish the new base policy. On
    /// failure the current snapshot and revision stay in effect.
    [[nodiscard]] auto reload_base()
        -> refrain::result<permissions::reload_outcome> {
        std::lock_guard lock{reload_mutex_};
        const auto previous{base_revision()};
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
        const auto reloaded{base_revision()};
        log_permissions.info(
            "reloaded permission policy from '{}' at base revision {}, {} "
            "and {}",
            path_, reloaded,
            count_noun(subject_count, "subject rule set", "subject rule sets"),
            count_noun(group_count, "group", "groups"));
        return permissions::reload_outcome{
            .changed = true,
            .previous_revision = previous,
            .revision = reloaded,
        };
    }

    // -- composed chain ----------------------------------------------------

    /// The provider surface of the base resolver alone; a chain link points at
    /// this table so a wrapper can forward to the base without re-entering the
    /// chain.
    [[nodiscard]] static auto base_table()
        -> const refrain_services_permissions_vtable & {
        static const refrain_services_permissions_vtable table{
            .struct_size = REFRAIN_SERVICES_PERMISSIONS_VTABLE_SIZE,
            .abi_major = REFRAIN_SERVICES_PERMISSIONS_ABI_MAJOR,
            .abi_minor = REFRAIN_SERVICES_PERMISSIONS_ABI_MINOR,
            .abi_patch = REFRAIN_SERVICES_PERMISSIONS_ABI_PATCH,
            .query = &provider_impl::base_abi_query,
            .default_result = &provider_impl::base_abi_query,
            .has = &provider_impl::base_abi_has,
            .revision = &provider_impl::base_abi_revision,
            .reload = &provider_impl::base_abi_reload,
        };
        return table;
    }

    [[nodiscard]] auto base_link() const -> permissions::provider_link {
        return permissions::make_provider_link(
            provider_id, base_table(), const_cast<provider_impl *>(this));
    }

    /// A chain holding only the base resolver, used before the host finished
    /// loading and when the composition declares no wrapper.
    [[nodiscard]] auto base_only_chain() const
        -> refrain::result<std::shared_ptr<const permissions::provider_chain>> {
        auto chain{permissions::provider_chain::create({}, base_link())};
        if (!chain) {
            return std::unexpected{std::move(chain.error())};
        }
        return std::make_shared<const permissions::provider_chain>(
            std::move(*chain));
    }

    /// Resolve the committed wrapper set of the service through the registry.
    ///
    /// The registry snapshot reports every active slot of the service together
    /// with the epoch that published them, so the resolved set and the
    /// committed generation agree on one commitment point: a rebuild cannot mix
    /// a retired wrapper with its replacement, and a candidate the commit
    /// retired is never re-resolved from stale registration order. Only slots
    /// named `chain:<position>` are wrappers; the reserved default instance is
    /// the base and stays out of the link set.
    [[nodiscard]] auto
    compose_links(const refrain::kernel::service_snapshot &snapshot) const
        -> std::vector<chain_link> {
        std::vector<chain_link> links{};
        for (const auto &slot : snapshot.slots) {
            const auto position{mod::chain_wrapper_position(slot.instance)};
            if (!position.has_value()) {
                continue;
            }
            const auto *api{
                static_cast<const mod::provider_api *>(slot.provider.instance)};
            if (api == nullptr || api->vtable == nullptr ||
                api->self == nullptr) {
                log_permissions.warn(
                    "permission chain wrapper slot '{}' holds an incomplete "
                    "provider table; the base resolver answers without it",
                    slot.instance);
                continue;
            }
            links.push_back(chain_link{
                .position = *position,
                .link = permissions::make_provider_link(
                    fmt::format("{}@{}", slot.provider.id,
                                slot.provider.version),
                    *static_cast<const refrain_services_permissions_vtable *>(
                        api->vtable),
                    api->self),
                .handle = slot,
            });
        }
        // Query order is outermost first: the declared wrapper list is ordered
        // innermost to outermost, and the outermost wrapper has the highest
        // position.
        std::sort(links.begin(), links.end(),
                  [](const chain_link &lhs, const chain_link &rhs) {
                      return lhs.position > rhs.position;
                  });
        return links;
    }

    /// Lease every link for the duration of one callback and report its result.
    /// A wrapper whose slot is quiescing or was replaced fails the lease;
    /// `nullopt` tells the caller to fall back to the base resolver instead of
    /// calling a table a commit is retiring.
    template <typename Callback>
    [[nodiscard]] auto with_leases(const std::vector<chain_link> &links,
                                   Callback &&callback) const
        -> std::optional<std::uint64_t> {
        std::vector<refrain::kernel::provider_lease> leases{};
        leases.reserve(links.size());
        for (const auto &link : links) {
            auto lease{host_->services().acquire(link.handle)};
            if (!lease) {
                return std::nullopt;
            }
            leases.push_back(std::move(*lease));
        }
        return callback();
    }

    /// True when both chains ask the same provider instances in the same
    /// order. The instance pointer distinguishes a replacement generation that
    /// reuses the same id and version from the committed one, so a reloaded
    /// wrapper is never replaced by its cached predecessor.
    [[nodiscard]] static auto
    same_links(const permissions::provider_chain &previous,
               const permissions::provider_chain &next) noexcept -> bool {
        const auto &lhs{previous.wrappers()};
        const auto &rhs{next.wrappers()};
        if (lhs.size() != rhs.size()) {
            return false;
        }
        for (std::size_t index{0}; index < lhs.size(); ++index) {
            if (lhs[index].id != rhs[index].id ||
                lhs[index].self != rhs[index].self ||
                lhs[index].vtable != rhs[index].vtable) {
                return false;
            }
        }
        return true;
    }

    /// Compose the chain from one registry snapshot. A link-set change gets a
    /// monotonic revision offset so the facade revision keeps increasing even
    /// when the raw sum drops (a retired wrapper carried revision too). An
    /// unchanged link set keeps the previously published chain object, so an
    /// epoch advance caused by an unrelated mod does not invalidate caches; the
    /// new handles still replace the old ones.
    [[nodiscard]] auto
    rebuild_chain(const refrain::kernel::service_snapshot &snapshot,
                  bool forced_instance) const
        -> std::shared_ptr<const chain_state> {
        auto links{compose_links(snapshot)};
        std::vector<permissions::provider_link> wrappers{};
        wrappers.reserve(links.size());
        for (const auto &link : links) {
            wrappers.push_back(link.link);
        }
        auto composed{permissions::provider_chain::create(std::move(wrappers),
                                                          base_link())};
        if (!composed) {
            log_permissions.warn(
                "failed to compose the permission chain, keeping the base "
                "resolver alone: {}",
                composed.error().message);
            return fallback_state_;
        }
        auto chain{std::make_shared<const permissions::provider_chain>(
            std::move(*composed))};
        const auto previous{chain_.load(std::memory_order_acquire)};
        if (!forced_instance && previous != nullptr &&
            same_links(*previous->chain, *chain)) {
            return std::make_shared<const chain_state>(chain_state{
                .epoch = snapshot.epoch,
                .revision_offset = previous->revision_offset,
                .chain = previous->chain,
                .links = std::move(links),
            });
        }
        // The raw revision needs the wrapper tables, so lease every new link
        // first: a concurrent commit could otherwise retire a wrapper between
        // resolving the handle and calling it. When a lease is refused, the
        // chain is being replaced at this instant and the offset is derived
        // from the last published revision instead of the tables.
        const auto previous_revision{
            last_revision_.load(std::memory_order_acquire)};
        const auto raw{with_leases(links, [&chain] {
                           return chain->revision();
                       }).value_or(previous_revision)};
        const auto offset{previous_revision >= raw ? previous_revision + 1 - raw
                                                   : std::uint64_t{0}};
        if (!forced_instance) {
            log_permissions.info(
                "composed the permission chain at composition epoch {} with {} "
                "before the base resolver",
                snapshot.epoch,
                count_noun(chain->wrappers().size(), "wrapper", "wrappers"));
        }
        return std::make_shared<const chain_state>(chain_state{
            .epoch = snapshot.epoch,
            .revision_offset = offset,
            .chain = std::move(chain),
            .links = std::move(links),
        });
    }

    /// The link set the provider answers through. The committed composition
    /// epoch is read from the thread-safe registry; a changed epoch rebuilds
    /// the links from one registry snapshot, so a retired generation is
    /// replaced instead of being called from a cache and a rebuild can never
    /// resurrect a predecessor. Before the first commit (the host is still
    /// loading) the base-only chain answers and is never cached, so an early
    /// query cannot pin a wrapper-less link set.
    [[nodiscard]] auto current_chain_state() const
        -> std::shared_ptr<const chain_state> {
        if (host_ == nullptr) {
            return fallback_state_;
        }
        // The test seam stands in for a composition epoch change: it forces a
        // fresh chain instance even when no provider slot changed, so the
        // snapshot lifetime and the monotonic revision can be exercised without
        // a host that can publish a link change. Production never sets it.
        const auto forced_instance{consume_forced_rebuild()};
        const auto epoch{host_->services().epoch()};
        if (!forced_instance) {
            if (epoch == 0) {
                return fallback_state_;
            }
            if (auto published = chain_.load(std::memory_order_acquire);
                published != nullptr && published->epoch == epoch) {
                return published;
            }
        }
        const std::lock_guard lock{chain_mutex_};
        if (!forced_instance) {
            if (auto published = chain_.load(std::memory_order_acquire);
                published != nullptr && published->epoch == epoch) {
                return published;
            }
        }
        auto rebuilt{rebuild_chain(host_->services().snapshot(service_id),
                                   forced_instance)};
        chain_.store(rebuilt, std::memory_order_release);
        return rebuilt;
    }

    /// One walk of the published chain: the epoch-consistent link set plus one
    /// registry lease per wrapper link, held until the guard is destroyed. A
    /// walk whose links cannot all be leased is `incomplete`; its caller
    /// answers from the base resolver for that one call, so a query racing a
    /// commit never calls a retired wrapper table.
    struct chain_walk {
        std::shared_ptr<const chain_state> state{};
        std::vector<refrain::kernel::provider_lease> leases{};
        bool complete{true};
    };

    /// Pin the current link set for one walk. A refused lease means the link
    /// set moved at a composition epoch while it was being read; the state is
    /// reloaded once from the new epoch before giving up on the walk.
    [[nodiscard]] auto begin_walk() const -> chain_walk {
        for (int attempt{0}; attempt < 2; ++attempt) {
            auto state{current_chain_state()};
            std::vector<refrain::kernel::provider_lease> leases{};
            leases.reserve(state->links.size());
            auto complete{true};
            for (const auto &link : state->links) {
                auto lease{host_->services().acquire(link.handle)};
                if (!lease) {
                    complete = false;
                    break;
                }
                leases.push_back(std::move(*lease));
            }
            if (complete) {
                return chain_walk{.state = std::move(state),
                                  .leases = std::move(leases),
                                  .complete = true};
            }
        }
        return chain_walk{.complete = false};
    }

    /// Consume one forced chain rebuild requested by a test. The
    /// compare-exchange makes the decrement exact when query threads race the
    /// test driver for the same request.
    [[nodiscard]] static auto consume_forced_rebuild() noexcept -> bool {
        auto &counter{permissions::test_seam::forced_chain_rebuilds};
        auto expected{counter.load(std::memory_order_relaxed)};
        while (expected != 0U) {
            if (counter.compare_exchange_weak(expected, expected - 1U,
                                              std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    // -- C ABI slots of the base resolver ----------------------------------

    static auto
    base_abi_query(void *self,
                   const refrain_services_permissions_permission_query *query)
        -> refrain_services_permissions_permission_result {
        const auto *provider{static_cast<const provider_impl *>(self)};
        if (provider == nullptr) {
            return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_DENIED;
        }
        return static_cast<refrain_services_permissions_permission_result>(
            provider->query_base(query));
    }

    static auto
    base_abi_has(void *self,
                 const refrain_services_permissions_permission_query *query)
        -> std::uint8_t {
        return base_abi_query(self, query) !=
                       REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_DENIED
                   ? 1U
                   : 0U;
    }

    static auto base_abi_revision(void *self) -> std::uint64_t {
        const auto *provider{static_cast<const provider_impl *>(self)};
        return provider == nullptr ? 0U : provider->base_revision();
    }

    static auto base_abi_reload(void *self, std::uint64_t *out_value)
        -> refrain_status {
        auto *provider{static_cast<provider_impl *>(self)};
        if (provider == nullptr || out_value == nullptr) {
            return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_ERROR_INVALID_ARGUMENT;
        }
        auto reloaded{provider->reload_base()};
        if (!reloaded) {
            log_permissions.warn(
                "failed to reload the base permission policy from '{}': {}; "
                "keeping revision {}",
                provider->config_path(), reloaded.error().message,
                provider->base_revision());
            return reload_status(reloaded.error());
        }
        *out_value = reloaded->revision;
        return REFRAIN_STATUS_OK;
    }

  public:
    /// The registered provider surface: chain query, base-only fallback and
    /// composed revision/reload.
    [[nodiscard]] static auto provider_vtable()
        -> const refrain_services_permissions_vtable & {
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

  private:
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
        const auto *provider{static_cast<const provider_impl *>(self)};
        if (provider == nullptr) {
            return REFRAIN_SERVICES_PERMISSIONS_PERMISSION_RESULT_DENIED;
        }
        return static_cast<refrain_services_permissions_permission_result>(
            provider->default_result(query));
    }

    static auto
    abi_has(void *self,
            const refrain_services_permissions_permission_query *query)
        -> std::uint8_t {
        const auto *provider{static_cast<const provider_impl *>(self)};
        return provider != nullptr && provider->has(query) ? 1U : 0U;
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
        auto reloaded{provider->reload_collect()};
        if (!reloaded) {
            log_permissions.warn(
                "failed to reload the permission chain: {}; keeping revision "
                "{}",
                reloaded.error().message, provider->revision());
            return reload_status(reloaded.error());
        }
        *out_value = reloaded->revision;
        return REFRAIN_STATUS_OK;
    }

    // -- policy editing ----------------------------------------------------

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
                .revision = revision(),
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
                path_, revision(), written.error().message);
            return std::unexpected{make_error(
                written.error().code,
                "failed to write the permission configuration file")};
        }
        publish(policy_state{.policy = std::move(*next_policy),
                             .source_text = std::move(*edited)});
        const auto next_revision{revision()};
        log_permissions.info(
            "{} '{}' for {} in the permission policy, revision {}",
            add ? "granted" : "revoked", canonical_rule, subject,
            next_revision);
        return permissions::policy_change{
            .changed = true,
            .revision = next_revision,
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
        if (query->subject.kind >
            static_cast<std::uint32_t>(subject_kind::plugin)) {
            return false;
        }
        if (query->context.extra.len > permissions::max_context_entries) {
            return false;
        }
        return query->context.extra.len == 0 ||
               query->context.extra.data != nullptr;
    }

    /// Convert the ABI context into a policy view. Extra entries borrow the
    /// caller's strings, so `storage` must outlive the returned view. Returns
    /// `nullopt` for a context the policy cannot match.
    [[nodiscard]] static auto
    context_view_of(const refrain_services_permissions_permission_query &query,
                    std::vector<permissions::context_value_view> &storage)
        -> std::optional<permissions::context_view> {
        storage.reserve(static_cast<std::size_t>(query.context.extra.len));
        for (std::uint64_t index{0}; index < query.context.extra.len; ++index) {
            const auto &entry{query.context.extra.data[index]};
            if (entry.struct_size <
                sizeof(refrain_services_permissions_permission_context_entry)) {
                return std::nullopt;
            }
            storage.push_back(permissions::context_value_view{
                .key = abi::to_view(entry.key),
                .value = abi::to_view(entry.value)});
        }
        const permissions::context_view view{
            .world = abi::to_view(query.context.world),
            .gamemode = abi::to_view(query.context.gamemode),
            .extra = storage};
        if (!permissions::valid_context(view)) {
            return std::nullopt;
        }
        return view;
    }

    std::string path_{};
    const mod::host *host_{nullptr};
    mutable std::mutex reload_mutex_{};
    mutable std::mutex chain_mutex_{};
    std::atomic<std::shared_ptr<const policy_state>> policy_{
        std::make_shared<const policy_state>()};
    std::atomic<std::uint64_t> revision_{0};
    std::shared_ptr<const permissions::provider_chain> fallback_chain_{};
    mutable std::shared_ptr<const chain_state> fallback_state_{};
    mutable std::atomic<std::shared_ptr<const chain_state>> chain_{};
    /// Highest composed revision published so far; the facade revision never
    /// drops, including while a composition epoch replaces a chain link.
    mutable std::atomic<std::uint64_t> last_revision_{0};
};

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
        .vtable = &provider_impl::provider_vtable(),
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
