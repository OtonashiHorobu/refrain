// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace refrain::mod {

// Mod loader identities and the lifecycle guarantees a loader declares.
//
// A loader is the execution environment a mod package is loaded through. The
// host does not branch on manifest fields: it resolves the loader id an
// external manifest names in `[entry].loader` (or the implicit `builtin`
// loader of a package without an entry section) to an active
// `refrain:mod.loader` provider instance and dispatches the package through
// that provider's `validate` and `load`.

/// Contract id of the loader service lives with the generated contract
/// (`refrain::services::mod_loader::service_id`), so the id cannot drift from
/// `sdk/idl/refrain/services/mod_loader.ridl`.

/// Loader ids the host and the first-party platform own. They are reserved:
/// a third-party loader must use a namespaced id.
inline constexpr std::string_view builtin_loader_id{"builtin"};
inline constexpr std::string_view native_loader_id{"native"};
inline constexpr std::string_view wasm_loader_id{"wasm"};
inline constexpr std::string_view lua_loader_id{"lua"};
inline constexpr std::string_view bukkit_loader_id{"bukkit"};

/// Removal guarantee a loader declares for every instance it owns. The host
/// records the declaration; a loader never gets a stronger guarantee than it
/// asked for.
enum class mod_unload_semantics : std::uint8_t {
    /// Guest state and host registrations are released; reloading the mod in
    /// place is possible.
    clean = 0,
    /// No new calls are dispatched, but resources stay allocated until the
    /// process exits.
    retire = 1,
    /// The process must restart; nothing is released while it runs.
    restart_required = 2,
};

[[nodiscard]] auto to_string(mod_unload_semantics semantics) noexcept
    -> std::string_view;

/// Parse `clean` | `retire` | `restart_required`.
[[nodiscard]] auto parse_mod_unload_semantics(std::string_view text) noexcept
    -> std::optional<mod_unload_semantics>;

/// Loader ids are 1..128 characters of lowercase ASCII letters, digits and
/// `._-`, start with a letter or digit and contain no empty, `.` or `..` dot
/// segments. An unqualified id is only valid for a reserved first-party
/// loader; a third-party loader id carries a namespace such as
/// `com.example.loader`.
[[nodiscard]] auto is_valid_loader_id(std::string_view id) noexcept -> bool;

/// True for the unqualified ids reserved to first-party loaders: `builtin`,
/// `native`, `wasm`, `lua` and `bukkit`.
[[nodiscard]] auto is_reserved_loader_id(std::string_view id) noexcept -> bool;

/// True for ids only first-party code may implement: the reserved unqualified
/// ids and the `refrain.` namespace.
[[nodiscard]] auto is_first_party_loader_id(std::string_view id) noexcept
    -> bool;

/// A loader's self-declared capabilities. `abi_major` is the loader-ABI major
/// the provider implements and must equal the contract ABI the host was built
/// against; `unload` is the removal guarantee the host records for the
/// lifecycle layer; `sandboxed` forbids the loader's guests from registering
/// providers.
struct mod_loader_capabilities {
    std::string loader_id{};
    std::uint32_t abi_major{0};
    mod_unload_semantics unload{mod_unload_semantics::restart_required};
    bool sandboxed{false};
    bool supports_hot_install{false};
};

/// Detail message a loader provider attaches to its most recent failed
/// `validate`, `load`, `reload` or `unload` call.
///
/// The loader ABI carries an error code only, so a provider that returns only
/// the code leaves the host reporting the loader id and mod id without the
/// field or package member that actually failed. A provider sets this
/// diagnostic before it returns the error code; the host drains it on the
/// failure path and appends it to the reported error. The channel is
/// per-thread, which matches the contract's main-thread dispatch, and the host
/// clears it before every provider call so a stale message is never attributed
/// to a later failure.
void set_loader_diagnostic(std::string message);
void clear_loader_diagnostic() noexcept;

/// Return and clear the current thread's loader diagnostic.
[[nodiscard]] auto take_loader_diagnostic() -> std::string;

} // namespace refrain::mod
