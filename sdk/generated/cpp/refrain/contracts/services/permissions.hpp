// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// GENERATED FILE - DO NOT EDIT.
// Source of truth: RSC/RIDL schemas (schema/, sdk/idl/).
// Regenerate with: refrain-tools schema gen
// Schema format: v1
// Schema hash: 29871d2bb99d3b05

#ifndef REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_PERMISSIONS_HPP
#define REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_PERMISSIONS_HPP

#include <refrain/contracts/services/permissions_abi.h>
#include <refrain/contracts/abi.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <refrain/core/error.hpp>
#include <refrain/core/schedule.hpp>

namespace refrain::services::permissions {

/// Kinds a query subject can have. `console` is the server console,
/// `player` a connected player, `entity` a non-player entity and `plugin` a
/// mod calling on behalf of itself.
enum class subject_kind : std::uint32_t { // enum 'subject_kind'
    console = 0,
    player = 1,
    entity = 2,
    plugin = 3,
};

/// Answer of one query. `denied` always wins over a grant; `not_handled` means
/// the provider holds no rule for the node and the chain must continue.
enum class permission_result : std::uint32_t { // enum 'permission_result'
    allowed = 0,
    denied = 1,
    not_handled = 2,
};

/// Errors of the resolver operations that can fail. Zero is success, so the
/// enum starts at one.
enum class permission_error : std::uint32_t { // enum 'permission_error'
    invalid_argument = 1,
    parse_error = 2,
    not_initialized = 3,
    unavailable = 4,
};

/// One key/value pair of the extensible query context.
using permission_context_entry = ::refrain_services_permissions_permission_context_entry;
/// Zero-initialized `permission_context_entry` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_permission_context_entry() noexcept -> permission_context_entry {
    permission_context_entry value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
/// Context a rule may match: the world, the game mode and provider-defined
/// extra entries. The built-in resolver matches its configured context
/// predicates against this value and a wrapper provider may match it too. A
/// consumer that matches context rejects duplicate extra keys.
using permission_context = ::refrain_services_permissions_permission_context;
/// Zero-initialized `permission_context` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_permission_context() noexcept -> permission_context {
    permission_context value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
/// Who a query is about. `console` leaves both strings empty. A `player`
/// carries its canonical profile uuid in `id` and its profile name in
/// `profile_name`; an `entity` carries its uuid in `id`; a `plugin` carries
/// its mod id in `id`.
using subject_ref = ::refrain_services_permissions_subject_ref;
/// Zero-initialized `subject_ref` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_subject_ref() noexcept -> subject_ref {
    subject_ref value{};
    ::refrain::contracts::init_struct(value);
    return value;
}
/// One permission question.
using permission_query = ::refrain_services_permissions_permission_query;
/// Zero-initialized `permission_query` with the mandatory `struct_size` set; fill the fields before passing it across the ABI.
[[nodiscard]] inline auto make_permission_query() noexcept -> permission_query {
    permission_query value{};
    ::refrain::contracts::init_struct(value);
    return value;
}

static_assert(sizeof(permission_context_entry) == 40);
static_assert(alignof(permission_context_entry) == 8);
static_assert(offsetof(permission_context_entry, struct_size) == 0);
static_assert(offsetof(permission_context_entry, key) == 8);
static_assert(offsetof(permission_context_entry, value) == 24);
static_assert(sizeof(permission_context) == 56);
static_assert(alignof(permission_context) == 8);
static_assert(offsetof(permission_context, struct_size) == 0);
static_assert(offsetof(permission_context, world) == 8);
static_assert(offsetof(permission_context, gamemode) == 24);
static_assert(offsetof(permission_context, extra) == 40);
static_assert(sizeof(subject_ref) == 40);
static_assert(alignof(subject_ref) == 8);
static_assert(offsetof(subject_ref, struct_size) == 0);
static_assert(offsetof(subject_ref, kind) == 4);
static_assert(offsetof(subject_ref, id) == 8);
static_assert(offsetof(subject_ref, profile_name) == 24);
static_assert(sizeof(permission_query) == 128);
static_assert(alignof(permission_query) == 8);
static_assert(offsetof(permission_query, struct_size) == 0);
static_assert(offsetof(permission_query, subject) == 8);
static_assert(offsetof(permission_query, node) == 48);
static_assert(offsetof(permission_query, context) == 64);
static_assert(offsetof(permission_query, op_level) == 120);

inline constexpr std::string_view service_id = "refrain:permissions";
inline constexpr std::uint32_t abi_major = 1;
inline constexpr std::uint32_t abi_minor = 0;
inline constexpr std::uint32_t abi_patch = 0;
inline constexpr ::refrain::contracts::side_kind side = ::refrain::contracts::side_kind::server;
inline constexpr ::refrain::contracts::thread_kind thread = ::refrain::contracts::thread_kind::any;
inline constexpr ::refrain::contracts::composition_kind composition = ::refrain::contracts::composition_kind::chain;
inline constexpr std::string_view capabilities = "default-resolver,config-policy,revision";
inline constexpr std::string_view required_services = "refrain:core>=1.0.0";
inline constexpr std::string_view service_ids[] = {"refrain:permissions"};

/// Type-safe wrapper around `refrain_services_permissions_vtable`. The provider (host) owns the table and `self`; this class is a borrowed call surface.
class provider final {
  public:
    provider(const ::refrain_services_permissions_vtable &table, void *self) noexcept
        : table_{&table}, self_{self} {}

    provider(const provider &) = delete;
    auto operator=(const provider &) -> provider & = delete;

    [[nodiscard]] auto table() const noexcept
        -> const ::refrain_services_permissions_vtable & {
        return *table_;
    }

    [[nodiscard]] auto self() const noexcept -> void * {
        return self_;
    }

    /// Answer one query from the provider's current policy snapshot. Read-only
    /// and safe to call from any thread. A malformed query (wrong struct size,
    /// unknown subject kind, invalid node) is answered `denied`: the resolver
    /// fails closed instead of granting on input it cannot evaluate.
    [[nodiscard]] auto query(const ::refrain::services::permissions::permission_query &query) const -> ::refrain::services::permissions::permission_result {
        const auto abi_query = ::refrain::contracts::with_struct_size(query);
        return static_cast<::refrain::services::permissions::permission_result>(table_->query(self_, &abi_query));
    }

    /// Answer the way the base resolver would on its own, ignoring wrapper
    /// grants. The chain falls back to this answer when every link answers
    /// `not_handled`; the base provider answers both calls from the same
    /// policy.
    [[nodiscard]] auto default_result(const ::refrain::services::permissions::permission_query &query) const -> ::refrain::services::permissions::permission_result {
        const auto abi_query = ::refrain::contracts::with_struct_size(query);
        return static_cast<::refrain::services::permissions::permission_result>(table_->default_result(self_, &abi_query));
    }

    /// Convenience predicate: true unless the query is denied, so `allowed`
    /// and `not_handled` both read as "maybe" for a caller that layers its own
    /// fallback.
    [[nodiscard]] auto has(const ::refrain::services::permissions::permission_query &query) const -> bool {
        const auto abi_query = ::refrain::contracts::with_struct_size(query);
        return (table_->has(self_, &abi_query) != 0);
    }

    /// Monotonic change counter of the published policy snapshot. It advances
    /// when the configuration is reloaded successfully; a consumer compares it
    /// against the value its cache was built with.
    [[nodiscard]] auto revision() const -> std::uint64_t {
        return static_cast<std::uint64_t>(table_->revision(self_));
    }

    /// Re-read the provider configuration, publish it and return the new
    /// revision. A provider updates its own snapshot atomically: on failure
    /// its snapshot and revision stay in effect and the error names the
    /// offending file or key. A chain reloads each link in turn, base first;
    /// when a later wrapper fails, the links that already reloaded keep their
    /// new snapshot because a provider has no rollback, and the error names
    /// the failing link.
    [[nodiscard]] auto reload() const -> std::expected<std::uint64_t, ::refrain::services::permissions::permission_error> {
        ::uint64_t value{};
        if (const auto code = table_->reload(self_, &value); code != REFRAIN_STATUS_OK) {
            return std::unexpected(static_cast<::refrain::services::permissions::permission_error>(code));
        }
        return static_cast<std::uint64_t>(value);
    }

  private:
    const ::refrain_services_permissions_vtable *table_;
    void *self_;
};

inline constexpr std::array<::refrain::core::schedule::task_descriptor, 0>
    schedule_tasks{};

} // namespace refrain::services::permissions

#endif // REFRAIN_GENERATED_CPP_REFRAIN_CONTRACTS_SERVICES_PERMISSIONS_HPP
