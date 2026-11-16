// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/mod/host.hpp>

#include <algorithm>
#include <new>

#include <refrain/contracts/host/core.hpp>
#include <refrain/contracts/host/kernel.hpp>
#include <refrain/contracts/host/metadata.hpp>
#include <refrain/contracts/host/platform.hpp>
#include <refrain/contracts/services/mod_loader.hpp>
#include <refrain/core/config.hpp>
#include <refrain/core/io.hpp>
#include <refrain/core/log.hpp>
#include <refrain/core/semver.hpp>
#include <refrain/data/builtin_image.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/path.hpp>
#include <refrain/mod/loader.hpp>
#include <refrain/mod/package.hpp>
#include <refrain/mod/static_mod.hpp>
#include <refrain/platform/dynamic_library.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace {

constexpr refrain::log::logger log_mod{"mod"};
constexpr refrain::log::logger log_loader{"mod-loader"};

} // namespace

namespace refrain::mod {

namespace loader_abi = refrain::services::mod_loader;

/// Generated C ABI vtable of the loader service.
using loader_vtable = refrain_services_mod_loader_vtable;
/// Generated C ABI unload-reason value (plain uint32_t in C).
using loader_unload_reason = refrain_services_mod_loader_mod_unload_reason;

namespace {

void *host_alloc(void *, std::uint64_t size, std::uint64_t alignment) noexcept {
    // A non-power-of-two request is refused before it is raised to the natural
    // alignment, otherwise every value would look valid.
    if (alignment != 0 && (alignment & (alignment - 1)) != 0) {
        return nullptr;
    }
    const auto requested{
        std::max<std::uint64_t>(alignment, alignof(std::max_align_t))};
    // Over-allocate and align by hand instead of calling aligned_alloc: that
    // function is absent from the MSVC standard library, and a platform split
    // for one call is not worth it. The unaligned base is stored in the word
    // before the result, which `free` reads back without needing the alignment.
    constexpr std::size_t header{sizeof(void *)};
    // Room for the header, for rounding up to `requested`, and for the caller's
    // bytes. Kept in one named slack so the request is `size + slack` rather
    // than an addition that reads like a buffer size plus a header by accident.
    const auto slack{header + (requested - 1)};
    auto *base{
        static_cast<std::byte *>(::operator new(size + slack, std::nothrow))};
    if (base == nullptr) {
        return nullptr;
    }
    auto *aligned{base + header};
    const auto misalignment{reinterpret_cast<std::uintptr_t>(aligned) %
                            requested};
    if (misalignment != 0) {
        aligned += requested - misalignment;
    }
    reinterpret_cast<void **>(aligned)[-1] = base;
    return aligned;
}

void host_free(void *, void *ptr) noexcept {
    if (ptr == nullptr) {
        return;
    }
    auto *aligned{static_cast<std::byte *>(ptr)};
    ::operator delete(reinterpret_cast<void **>(aligned)[-1]);
}

} // namespace

auto host_allocator() noexcept -> refrain_allocator {
    return refrain_allocator{
        .self = nullptr,
        .alloc = &host_alloc,
        .free = &host_free,
    };
}

namespace {

// A mod reports a contract status; the loader records the engine error code.
// Unknown values stay unknown instead of being reinterpreted as a different
// error taxonomy.
[[nodiscard]] auto from_mod_status(refrain_status status) noexcept
    -> error_code {
    switch (static_cast<refrain::host::core::host_error>(status)) {
    case refrain::host::core::host_error::invalid_argument:
        return error_code::invalid_argument;
    case refrain::host::core::host_error::not_found:
        return error_code::not_found;
    case refrain::host::core::host_error::permission_denied:
        return error_code::permission_denied;
    case refrain::host::core::host_error::version_mismatch:
        return error_code::version_mismatch;
    case refrain::host::core::host_error::unsupported:
        return error_code::unsupported;
    case refrain::host::core::host_error::already_exists:
        return error_code::already_exists;
    case refrain::host::core::host_error::io_error:
        return error_code::io_error;
    case refrain::host::core::host_error::not_initialized:
    case refrain::host::core::host_error::out_of_memory:
    case refrain::host::core::host_error::limit_exceeded:
    case refrain::host::core::host_error::thread_violation:
    case refrain::host::core::host_error::internal:
        return error_code::unknown;
    }
    return error_code::unknown;
}

namespace config = core::config;
namespace io = core::io;
namespace kernel = refrain::kernel;
namespace data = refrain::data;
namespace log = refrain::log;

using kernel::capability_bits;
using kernel::provider_info;
using kernel::service_registry;
using mod::manifest_side_matches;
using mod::mod_manifest;
using mod::parse_mod_manifest;
using mod::static_mod_descriptor;
using mod::static_mod_registry;

[[nodiscard]] auto fail(error_code code, std::string message) -> error {
    return make_error(code, std::move(message));
}

[[nodiscard]] auto to_refrain_str(std::string_view text) noexcept
    -> refrain_str {
    return refrain_str{reinterpret_cast<const std::uint8_t *>(text.data()),
                       static_cast<std::uint64_t>(text.size())};
}

/// Capabilities advertised by the sealed `refrain:host.core` contract.
[[nodiscard]] auto host_capability_set() -> const std::set<std::string_view> & {
    static const auto capabilities = [] {
        std::set<std::string_view> out;
        const auto add = [&out](std::string_view text) {
            while (!text.empty()) {
                const auto comma{text.find(',')};
                const auto token{text.substr(0, comma)};
                if (!token.empty()) {
                    out.insert(token);
                }
                if (comma == std::string_view::npos) {
                    break;
                }
                text.remove_prefix(comma + 1);
            }
        };
        add(REFRAIN_HOST_CORE_CAPABILITIES);
        add(REFRAIN_HOST_PLATFORM_CAPABILITIES);
        add(REFRAIN_HOST_KERNEL_CAPABILITIES);
        add(REFRAIN_HOST_METADATA_CAPABILITIES);
        return out;
    }();
    return capabilities;
}

/// Manifest-declared host runtime requirements. The loader compares the
/// requested host API major/minor and each capability against the sealed
/// contract the binary was built with.
[[nodiscard]] auto check_host_requirements(const mod_manifest &manifest)
    -> status {
    if (!manifest.host_api.empty()) {
        const auto requested{core::semver::parse(manifest.host_api)};
        if (!requested.has_value()) {
            return std::unexpected{
                fail(error_code::version_mismatch,
                     fmt::format("host_api '{}' is not a valid SemVer",
                                 manifest.host_api))};
        }
        if (requested->major != REFRAIN_HOST_CORE_ABI_MAJOR) {
            return std::unexpected{fail(
                error_code::version_mismatch,
                fmt::format("host_api {} requires major {}, this host is {}",
                            manifest.host_api, requested->major,
                            REFRAIN_HOST_CORE_ABI_MAJOR))};
        }
        if (requested->minor > REFRAIN_HOST_CORE_ABI_MINOR) {
            return std::unexpected{fail(
                error_code::version_mismatch,
                fmt::format("host_api {} requires a newer host minor {} > {}",
                            manifest.host_api, requested->minor,
                            REFRAIN_HOST_CORE_ABI_MINOR))};
        }
    }
    for (const auto &capability : manifest.host_capabilities) {
        if (!host_capability_set().contains(std::string_view{capability})) {
            return std::unexpected{fail(
                error_code::unsupported,
                fmt::format("host capability '{}' is not provided by this host",
                            capability))};
        }
    }
    return status_ok();
}

[[nodiscard]] auto annotate(error original, std::string_view context) -> error {
    if (original.message.find(context) != std::string::npos) {
        return original;
    }
    original.message = std::string{context} + ": " + original.message;
    return original;
}

[[nodiscard]] auto provides_summary(const mod_manifest &manifest)
    -> std::string {
    auto summary{std::string{}};
    for (const auto &service : manifest.provides) {
        if (!summary.empty()) {
            summary += ',';
        }
        summary += service;
    }
    return summary;
}

// ---------------------------------------------------------------------------
// Short-slot mapping (gamepack `[providers]` keys vs. contract service ids)
// ---------------------------------------------------------------------------

/// Is `service` exactly `<prefix><slot>`?
[[nodiscard]] auto equals_slot(std::string_view service,
                               std::string_view prefix, std::string_view slot)
    -> bool {
    return service.size() == prefix.size() + slot.size() &&
           service.starts_with(prefix) && service.substr(prefix.size()) == slot;
}

/// Is `service` a member of the `<prefix><slot>.` namespace?
[[nodiscard]] auto in_slot_namespace(std::string_view service,
                                     std::string_view prefix,
                                     std::string_view slot) -> bool {
    return service.size() > prefix.size() + slot.size() &&
           service.starts_with(prefix) &&
           service.substr(prefix.size(), slot.size()) == slot &&
           service[prefix.size() + slot.size()] == '.';
}

/// Compare `refrain:<slot with dots>` against an underscore slot, so
/// "refrain:session.status" activates the gamepack slot "session_status".
[[nodiscard]] auto equals_dotted_slot(std::string_view service,
                                      std::string_view slot) -> bool {
    constexpr std::string_view prefix{"refrain:"};
    if (!service.starts_with(prefix)) {
        return false;
    }
    const auto rest{service.substr(prefix.size())};
    if (rest.size() != slot.size()) {
        return false;
    }
    for (std::size_t index{0}; index < rest.size(); ++index) {
        const auto character{rest[index] == '.' ? '_' : rest[index]};
        if (character != slot[index]) {
            return false;
        }
    }
    return true;
}

/// A gamepack names a service by its `[providers]` key ("data", "world",
/// "protocol"), while a factory passes the full contract id
/// ("refrain:data.packs", "refrain:net.protocol"). A service matches a slot
/// when it is the slot itself, `refrain:<slot>`, `refrain:net.<slot>`, or a
/// member of the `refrain:<slot>.` namespace; everything else is a different
/// service.
[[nodiscard]] auto matches_slot(std::string_view service, std::string_view slot)
    -> bool {
    if (service.empty() || slot.empty()) {
        return false;
    }
    constexpr std::string_view namespace_prefix{"refrain:"};
    constexpr std::string_view net_prefix{"refrain:net."};
    return service == slot || equals_slot(service, namespace_prefix, slot) ||
           equals_slot(service, net_prefix, slot) ||
           in_slot_namespace(service, namespace_prefix, slot) ||
           equals_dotted_slot(service, slot);
}

/// Does the manifest `[provides]` list cover `service`? Exact id match, or the
/// short-slot rule applied to the declared entry (a mod may declare "data"
/// instead of "refrain:data.packs").
[[nodiscard]] auto declares_service(const std::vector<std::string> &provides,
                                    std::string_view service) -> bool {
    return std::any_of(provides.begin(), provides.end(),
                       [service](const std::string &declared) {
                           return declared == service ||
                                  matches_slot(service, declared);
                       });
}

// ---------------------------------------------------------------------------
// startup_plan provider refs
// ---------------------------------------------------------------------------

struct provider_ref {
    std::string_view id{};
    std::string_view version{};
};

/// Split the canonical `id@version` text used by `startup_plan.providers`.
[[nodiscard]] auto split_provider_ref(std::string_view text) -> provider_ref {
    const auto at{text.find('@')};
    if (at == std::string_view::npos) {
        return provider_ref{.id = text};
    }
    return provider_ref{.id = text.substr(0, at),
                        .version = text.substr(at + 1)};
}

// ---------------------------------------------------------------------------
// Generated vtable ABI header
// ---------------------------------------------------------------------------

/// Leading header of every generated service vtable: exactly four `uint32_t`,
/// field order and size identical to `refrain_services_<name>_vtable`. The
/// slots after it are contract specific and are never inspected here.
struct vtable_header {
    std::uint32_t struct_size{0};
    std::uint32_t abi_major{0};
    std::uint32_t abi_minor{0};
    std::uint32_t abi_patch{0};
};

static_assert(sizeof(vtable_header) == 16U);
static_assert(alignof(vtable_header) == alignof(std::uint32_t));

/// Validate the vtable ABI header claimed by `api.abi_major` (0 = skip, for
/// factories written before the field existed).
///
/// Safety: `provider_api::vtable` is a host-owned static table by contract
/// ("must stay valid for the process lifetime") and every generated vtable
/// starts with this 16-byte header, so reading exactly `sizeof(vtable_header)`
/// bytes is always in bounds. `std::memcpy` avoids alignment and
/// strict-aliasing assumptions about the opaque pointer.
[[nodiscard]] auto check_abi_header(const provider_api &api) -> status {
    if (api.abi_major == 0) {
        return status_ok();
    }
    if (api.vtable == nullptr) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "provider vtable is null; cannot verify its ABI header")};
    }
    vtable_header header{};
    std::memcpy(&header, api.vtable, sizeof(vtable_header));
    if (header.struct_size < static_cast<std::uint32_t>(sizeof(header))) {
        return std::unexpected{
            fail(error_code::version_mismatch,
                 fmt::format("provider vtable declares struct_size {} but the "
                             "generated ABI header needs at least {}",
                             header.struct_size, sizeof(vtable_header)))};
    }
    if (header.abi_major != api.abi_major) {
        return std::unexpected{
            fail(error_code::version_mismatch,
                 fmt::format("provider vtable abi major {} does not match the "
                             "declared abi major {}",
                             header.abi_major, api.abi_major))};
    }
    return status_ok();
}

// ---------------------------------------------------------------------------
// `refrain:mod.loader` adapter
// ---------------------------------------------------------------------------

/// Engine error -> loader contract error. The loader contract carries its own
/// enum so a loader can report a sandbox violation the engine has no code for;
/// everything without a counterpart collapses to `internal`.
[[nodiscard]] auto loader_error_from(error_code code) noexcept
    -> loader_abi::mod_loader_error {
    switch (code) {
    case error_code::invalid_argument:
        return loader_abi::mod_loader_error::invalid_argument;
    case error_code::not_found:
        return loader_abi::mod_loader_error::not_found;
    case error_code::already_exists:
        return loader_abi::mod_loader_error::already_exists;
    case error_code::permission_denied:
        return loader_abi::mod_loader_error::permission_denied;
    case error_code::io_error:
        return loader_abi::mod_loader_error::io_error;
    case error_code::unsupported:
        return loader_abi::mod_loader_error::unsupported;
    case error_code::version_mismatch:
        return loader_abi::mod_loader_error::version_mismatch;
    case error_code::success:
    case error_code::unknown:
    case error_code::parse_error:
    case error_code::not_initialized:
    case error_code::timeout:
    case error_code::cancelled:
    case error_code::out_of_memory:
    case error_code::mod_error:
        return loader_abi::mod_loader_error::internal;
    }
    return loader_abi::mod_loader_error::internal;
}

/// Loader contract error -> engine error.
[[nodiscard]] auto loader_error_to(loader_abi::mod_loader_error error) noexcept
    -> error_code {
    switch (error) {
    case loader_abi::mod_loader_error::invalid_argument:
        return error_code::invalid_argument;
    case loader_abi::mod_loader_error::not_found:
        return error_code::not_found;
    case loader_abi::mod_loader_error::already_exists:
        return error_code::already_exists;
    case loader_abi::mod_loader_error::permission_denied:
    case loader_abi::mod_loader_error::sandbox_violation:
        return error_code::permission_denied;
    case loader_abi::mod_loader_error::io_error:
        return error_code::io_error;
    case loader_abi::mod_loader_error::unsupported:
        return error_code::unsupported;
    case loader_abi::mod_loader_error::version_mismatch:
        return error_code::version_mismatch;
    case loader_abi::mod_loader_error::internal:
        return error_code::unknown;
    }
    return error_code::unknown;
}

/// Manifest side -> the contract's `REFRAIN_SIDE_*` value.
[[nodiscard]] auto abi_side(manifest_side side) noexcept -> std::uint32_t {
    switch (side) {
    case manifest_side::server:
        return REFRAIN_SIDE_SERVER;
    case manifest_side::client:
        return REFRAIN_SIDE_CLIENT;
    case manifest_side::both:
        return REFRAIN_SIDE_BOTH;
    }
    return REFRAIN_SIDE_BOTH;
}

/// Build the contract view of one manifest for `validate`/`load`. Every string
/// is borrowed from `manifest`, `entry` or `package_root`, all of which outlive
/// the contract call.
[[nodiscard]] auto make_loader_entry(const mod_manifest &manifest,
                                     bool activate_code,
                                     std::string_view package_root)
    -> loader_abi::mod_entry_manifest {
    loader_abi::mod_entry_manifest out{};
    out.struct_size = REFRAIN_SERVICES_MOD_LOADER_MOD_ENTRY_MANIFEST_SIZE;
    out.mod_id = refrain::contracts::to_borrowed(manifest.common.id);
    out.version = refrain::contracts::to_borrowed(manifest.common.version);
    out.abi = manifest.common.abi;
    out.side = abi_side(manifest.common.side);
    out.loader = refrain::contracts::to_borrowed(manifest.loader);
    out.package_root = refrain::contracts::to_borrowed(package_root);
    if (manifest.entry.has_value()) {
        out.native_path =
            refrain::contracts::to_borrowed(manifest.entry->native);
        out.wasm_path = refrain::contracts::to_borrowed(manifest.entry->wasm);
        out.symbol = refrain::contracts::to_borrowed(manifest.entry->symbol);
    }
    out.activate_code = activate_code ? 1 : 0;
    return out;
}

/// Loader capabilities as the C++ host records them, decoded from the contract
/// struct. The struct size and the unload semantics are validated here so an
/// undersized or future-unknown declaration never reaches the dispatch.
[[nodiscard]] auto
capabilities_from_abi(const loader_abi::mod_loader_capabilities &abi)
    -> result<mod_loader_capabilities> {
    if (abi.struct_size <
        REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_CAPABILITIES_SIZE) {
        return std::unexpected{fail(
            error_code::invalid_argument,
            fmt::format(
                "capability struct_size {} is smaller than the "
                "contract's {}",
                abi.struct_size,
                REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_CAPABILITIES_SIZE))};
    }
    mod_unload_semantics unload{};
    switch (abi.unload) {
    case REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_UNLOAD_CLEAN:
        unload = mod_unload_semantics::clean;
        break;
    case REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_UNLOAD_RETIRE:
        unload = mod_unload_semantics::retire;
        break;
    case REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_UNLOAD_RESTART_REQUIRED:
        unload = mod_unload_semantics::restart_required;
        break;
    default:
        return std::unexpected{
            fail(error_code::version_mismatch,
                 fmt::format("unknown unload semantics value {}",
                             static_cast<unsigned>(abi.unload)))};
    }
    return mod_loader_capabilities{
        .loader_id =
            std::string{refrain::contracts::as_string_view(abi.loader_id)},
        .abi_major = abi.abi_major,
        .unload = unload,
        .sandboxed = abi.sandboxed != 0,
        .supports_hot_install = abi.supports_hot_install != 0,
    };
}

/// Cross-check a loader's declared capabilities against the id it is active
/// under and the loader ABI this host implements.
[[nodiscard]] auto
validate_loader_capabilities(std::string_view expected_id,
                             const mod_loader_capabilities &caps) -> status {
    if (caps.loader_id != expected_id) {
        return std::unexpected{fail(
            error_code::version_mismatch,
            fmt::format("mod loader provider on instance '{}' reports loader "
                        "id '{}'",
                        expected_id, caps.loader_id))};
    }
    if (caps.abi_major != REFRAIN_SERVICES_MOD_LOADER_ABI_MAJOR) {
        return std::unexpected{fail(
            error_code::version_mismatch,
            fmt::format("mod loader '{}' implements loader ABI {}, this host "
                        "provides {}",
                        expected_id, caps.abi_major,
                        REFRAIN_SERVICES_MOD_LOADER_ABI_MAJOR))};
    }
    return status_ok();
}

/// `a, b, c` for diagnostics; `none` for an empty list.
[[nodiscard]] auto join_instance_names(const std::vector<std::string> &names)
    -> std::string {
    auto out{std::string{}};
    for (const auto &name : names) {
        if (!out.empty()) {
            out += ", ";
        }
        out += name;
    }
    return out.empty() ? std::string{"none"} : out;
}

/// One private directory per host that materialises installed native entries
/// into regular files a dynamic loader can open. Removed with the host.
[[nodiscard]] auto create_loader_scratch_root() -> result<std::string> {
    const auto base{io::temp_directory()};
    if (base.empty()) {
        return std::unexpected{
            fail(error_code::io_error,
                 "mod loader: the temporary directory is unknown")};
    }
    static std::atomic<std::uint64_t> sequence{0};
    const auto stamp{static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count())};
    for (int attempt{0}; attempt < 16; ++attempt) {
        const auto index{sequence.fetch_add(1, std::memory_order_relaxed)};
        auto path{data::join_path(
            base, fmt::format("refrain-mod-loader-{}-{}", stamp, index))};
        std::error_code error;
        if (std::filesystem::create_directories(path, error)) {
            return path;
        }
        if (error && error != std::errc::file_exists) {
            return std::unexpected{
                fail(error_code::io_error,
                     fmt::format("mod loader: cannot create the temporary "
                                 "directory '{}'",
                                 path))};
        }
    }
    return std::unexpected{
        fail(error_code::io_error,
             "mod loader: cannot create a unique temporary directory")};
}

/// `name@version` identity for loader audit lines.
[[nodiscard]] auto loader_identity(const registered_loader &loader)
    -> std::string {
    return fmt::format("{}@{}", loader.id,
                       loader.version.empty() ? std::string{"1.0.0"}
                                              : loader.version);
}

} // namespace

namespace loader_detail {

/// ABI bridge of the host-owned `builtin` and `native` loaders. Both providers
/// share one generated vtable; the bridge branches on the state object, then
/// calls the host implementation that actually owns the execution mechanism.
struct bridge {
    static auto capabilities(void *self,
                             loader_abi::mod_loader_capabilities *out) -> void;
    static auto validate(void *self,
                         const loader_abi::mod_entry_manifest *entry)
        -> refrain_status;
    static auto load(void *self, const loader_abi::mod_load_request *request,
                     refrain_services_mod_loader_handle_mod_instance *out)
        -> refrain_status;
    static auto unload(void *self,
                       refrain_services_mod_loader_handle_mod_instance instance,
                       loader_unload_reason reason) -> refrain_status;
    static auto reload(void *self,
                       refrain_services_mod_loader_handle_mod_instance instance,
                       const loader_abi::mod_load_request *request,
                       refrain_services_mod_loader_handle_mod_instance *out)
        -> refrain_status;

  private:
    /// Capture a host failure: the contract carries a code only, so the detail
    /// message stays on the host for the next diagnostic.
    static auto fail_with(host &owner, const refrain::error &failure)
        -> refrain_status {
        owner.set_loader_dispatch_error(failure.message);
        return static_cast<refrain_status>(loader_error_from(failure.code));
    }
};

auto bridge::capabilities(void *self, loader_abi::mod_loader_capabilities *out)
    -> void {
    if (self == nullptr || out == nullptr) {
        return;
    }
    const auto *state{static_cast<const host::loader_provider_state *>(self)};
    out->struct_size = REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_CAPABILITIES_SIZE;
    out->loader_id = refrain::contracts::to_borrowed(state->loader_id);
    out->abi_major = REFRAIN_SERVICES_MOD_LOADER_ABI_MAJOR;
    out->unload = static_cast<std::uint8_t>(state->unload);
    out->sandboxed = state->sandboxed ? 1 : 0;
    out->supports_hot_install = state->supports_hot_install ? 1 : 0;
}

auto bridge::validate(void *self, const loader_abi::mod_entry_manifest *entry)
    -> refrain_status {
    if (self == nullptr || entry == nullptr) {
        return static_cast<refrain_status>(
            loader_error_from(error_code::invalid_argument));
    }
    const auto *state{static_cast<const host::loader_provider_state *>(self)};
    if (state->owner == nullptr) {
        return static_cast<refrain_status>(
            loader_error_from(error_code::unknown));
    }
    auto &owner{*state->owner};
    const auto loader{refrain::contracts::as_string_view(entry->loader)};
    if (loader != state->loader_id) {
        owner.set_loader_dispatch_error(fmt::format(
            "loader provider for instance '{}' was asked to validate loader "
            "'{}'",
            state->loader_id, std::string{loader}));
        return static_cast<refrain_status>(
            loader_abi::mod_loader_error::version_mismatch);
    }
    const auto verified{
        loader == builtin_loader_id
            ? owner.builtin_validate()
            : owner.native_validate(
                  refrain::contracts::as_string_view(entry->native_path),
                  refrain::contracts::as_string_view(entry->wasm_path),
                  entry->abi, entry->activate_code != 0)};
    return verified ? REFRAIN_STATUS_OK : fail_with(owner, verified.error());
}

auto bridge::load(void *self, const loader_abi::mod_load_request *request,
                  refrain_services_mod_loader_handle_mod_instance *out)
    -> refrain_status {
    if (self == nullptr || request == nullptr || out == nullptr) {
        return static_cast<refrain_status>(
            loader_error_from(error_code::invalid_argument));
    }
    *out = REFRAIN_HANDLE_INVALID;
    const auto *state{static_cast<const host::loader_provider_state *>(self)};
    if (state->owner == nullptr) {
        return static_cast<refrain_status>(
            loader_error_from(error_code::unknown));
    }
    auto &owner{*state->owner};
    const auto loader{
        refrain::contracts::as_string_view(request->manifest.loader)};
    if (loader != state->loader_id) {
        owner.set_loader_dispatch_error(fmt::format(
            "loader provider for instance '{}' was asked to load loader '{}'",
            state->loader_id, std::string{loader}));
        return static_cast<refrain_status>(
            loader_abi::mod_loader_error::version_mismatch);
    }
    if (request->manifest.activate_code == 0) {
        // The plan kept the package data-only: nothing to execute. The
        // loader-specific drift checks already ran in `validate`.
        return REFRAIN_STATUS_OK;
    }
    std::uint64_t handle{0};
    const auto loaded{loader == builtin_loader_id ? owner.builtin_load(handle)
                                                  : owner.native_load(handle)};
    if (!loaded) {
        return fail_with(owner, loaded.error());
    }
    *out = handle;
    return REFRAIN_STATUS_OK;
}

auto bridge::unload(void *self,
                    refrain_services_mod_loader_handle_mod_instance instance,
                    loader_unload_reason reason) -> refrain_status {
    static_cast<void>(reason);
    if (self == nullptr || instance == REFRAIN_HANDLE_INVALID) {
        return static_cast<refrain_status>(
            loader_error_from(error_code::invalid_argument));
    }
    const auto *state{static_cast<const host::loader_provider_state *>(self)};
    if (state->owner == nullptr) {
        return static_cast<refrain_status>(
            loader_error_from(error_code::unknown));
    }
    // TODO: dispatch instance unload to the lifecycle layer once it exists.
    // The host records the declared semantics only, so a loader must not
    // pretend it released an instance while its resources are still live.
    state->owner->set_loader_dispatch_error(fmt::format(
        "loader '{}' cannot unload instance {} before process shutdown; its "
        "declared removal semantics are '{}'",
        state->loader_id, instance, to_string(state->unload)));
    return static_cast<refrain_status>(
        loader_abi::mod_loader_error::unsupported);
}

auto bridge::reload(void *self,
                    refrain_services_mod_loader_handle_mod_instance instance,
                    const loader_abi::mod_load_request *request,
                    refrain_services_mod_loader_handle_mod_instance *out)
    -> refrain_status {
    static_cast<void>(self);
    static_cast<void>(instance);
    static_cast<void>(request);
    static_cast<void>(out);
    // TODO: implement reload through the lifecycle layer. Until then the host
    // must not replace an instance it cannot release.
    return static_cast<refrain_status>(
        loader_abi::mod_loader_error::unsupported);
}

} // namespace loader_detail

namespace {

/// The one vtable of the host-owned loaders: `builtin` and `native` are both
/// implementations of the same contract and differ only in their state object.
[[nodiscard]] auto internal_loader_vtable() noexcept
    -> const refrain_services_mod_loader_vtable & {
    static const refrain_services_mod_loader_vtable table{
        .struct_size = REFRAIN_SERVICES_MOD_LOADER_VTABLE_SIZE,
        .abi_major = REFRAIN_SERVICES_MOD_LOADER_ABI_MAJOR,
        .abi_minor = REFRAIN_SERVICES_MOD_LOADER_ABI_MINOR,
        .abi_patch = REFRAIN_SERVICES_MOD_LOADER_ABI_PATCH,
        .capabilities = &loader_detail::bridge::capabilities,
        .validate = &loader_detail::bridge::validate,
        .load = &loader_detail::bridge::load,
        .unload = &loader_detail::bridge::unload,
        .reload = &loader_detail::bridge::reload,
    };
    return table;
}

} // namespace

auto host::load_one(const composition &plan, const package_source &package,
                    const candidate &entry, const planned_providers &planned)
    -> result<loaded_mod> {
    constexpr std::string_view manifest_name{"manifest.toml"};
    auto bytes{package.read(manifest_name)};
    if (!bytes) {
        auto failure{std::move(bytes.error())};
        failure.message =
            fmt::format("mod '{}' ({}) has no readable manifest: {}", entry.id,
                        mod::to_string(package.kind()), failure.message);
        return std::unexpected{std::move(failure)};
    }
    const std::string_view text{reinterpret_cast<const char *>(bytes->data()),
                                bytes->size()};
    // The raw manifest bytes are the identity discovery/lock
    // verification compares against, so hash them before parsing.
    const auto manifest_hash_hex{data::to_hex(data::blake3(*bytes))};
    // The candidate's digest was captured during discovery. Loading re-reads a
    // directory package from disk; if the bytes changed in between, the
    // mods.lock trust anchor no longer describes what is about to be loaded.
    // A zero digest means the caller only previewed the composition (tests /
    // pre-scan), so there is nothing to compare against.
    const auto has_expected_digest{
        std::any_of(entry.manifest_hash.begin(), entry.manifest_hash.end(),
                    [](std::byte value) { return value != std::byte{0}; })};
    if (has_expected_digest &&
        manifest_hash_hex != data::to_hex(entry.manifest_hash)) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 fmt::format("{}: manifest changed after discovery (expected "
                             "manifest hash {}, got {})",
                             entry.path, data::to_hex(entry.manifest_hash),
                             manifest_hash_hex))};
    }

    auto doc{config::document::parse_text(text, entry.path)};
    if (!doc) {
        return std::unexpected{annotate(std::move(doc.error()), entry.path)};
    }
    // An installed package carries the packaging tables (`[files]`,
    // `[packaging]`, `[signature]`) in the same manifest.toml; the installer
    // validated their contents, so the loader only admits their shape. A
    // directory/source manifest must not carry them.
    auto manifest{parse_mod_manifest(
        *doc,
        manifest_parse_options{.allow_package_tables =
                                   package.kind() == package_kind::rfmod})};
    if (!manifest) {
        return std::unexpected{
            annotate(std::move(manifest.error()), entry.path)};
    }

    if (manifest->common.id != entry.id) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 fmt::format(
                     "{}: gamepack loads '{}' but the manifest declares '{}'",
                     entry.path, entry.id, manifest->common.id))};
    }
    if (manifest->common.version != entry.version) {
        return std::unexpected{
            fail(error_code::version_mismatch,
                 fmt::format(
                     "{}: gamepack pins version {} but the manifest declares "
                     "{}",
                     entry.path, entry.version, manifest->common.version))};
    }
    if (manifest->common.abi != plan.spec.abi) {
        return std::unexpected{
            fail(error_code::version_mismatch,
                 fmt::format(
                     "{}: manifest abi {} does not match the gamepack abi {}",
                     entry.path, manifest->common.abi, plan.spec.abi))};
    }
    if (!manifest_side_matches(manifest->common.side, plan.spec.side)) {
        return std::unexpected{
            fail(error_code::version_mismatch,
                 fmt::format("{}: side '{}' cannot run in a '{}' gamepack",
                             entry.path, to_string(manifest->common.side),
                             plan.spec.side))};
    }
    if (auto host_requirements{check_host_requirements(*manifest)};
        !host_requirements) {
        return std::unexpected{
            annotate(std::move(host_requirements.error()), entry.path)};
    }
    // A sealed service is owned by the host bootstrap; a mod that declares it
    // is rejected before any mod code runs, not merely at register_provider().
    for (const auto &provided : manifest->provides) {
        if (is_sealed_service(provided)) {
            return std::unexpected{fail(
                error_code::permission_denied,
                fmt::format("mod '{}' declares sealed service '{}'; the host "
                            "owns that service",
                            manifest->common.id, provided))};
        }
    }

    // Context for `register_provider()`: the mod may only claim services its
    // manifest declares and the plan selected it for. External mods receive
    // the same context through the sealed host core table.
    current_mod_provides_ = manifest->provides;
    current_mod_planned_slots_.clear();
    current_mod_planned_instances_.clear();
    if (const auto found{planned.slots.find(manifest->common.id)};
        found != planned.slots.end()) {
        current_mod_planned_slots_ = found->second;
    }
    if (const auto found{planned.instances.find(manifest->common.id)};
        found != planned.instances.end()) {
        current_mod_planned_instances_ = found->second;
    }

    const auto declares_provides{!manifest->provides.empty()};
    const auto selected_provider{planned.mods.contains(manifest->common.id)};
    if (declares_provides && !selected_provider) {
        log_mod.info("mod {} provides {} but the gamepack does not select it "
                     "as a provider, so it loads as data only",
                     manifest->common.id, provides_summary(*manifest));
    }

    // Every package, internal or external, is loaded through an active
    // `refrain:mod.loader` provider: the host no longer branches on the entry
    // shape. The builtin loader wraps the static registry, the native loader
    // wraps the dynamic-library entry ABI, and a future loader plugs in
    // without touching this function.
    current_manifest_ = &*manifest;
    current_candidate_ = &entry;
    current_package_ = &package;
    current_static_descriptor_ = nullptr;
    current_mod_selected_provider_ = selected_provider;
    current_load_has_code_ = false;
    clear_loader_dispatch_error();

    struct dispatch_scope {
        host &owner;
        ~dispatch_scope() {
            owner.current_manifest_ = nullptr;
            owner.current_candidate_ = nullptr;
            owner.current_package_ = nullptr;
            owner.current_static_descriptor_ = nullptr;
            owner.current_mod_selected_provider_ = false;
            owner.current_load_has_code_ = false;
            owner.current_loader_id_.clear();
            owner.current_loader_sandboxed_ = false;
            owner.clear_loader_dispatch_error();
        }
    } const scope{*this};

    auto loader{resolve_loader(manifest->loader)};
    if (!loader) {
        return std::unexpected{annotate(std::move(loader.error()), entry.path)};
    }
    current_loader_id_ = loader->record->id;
    current_loader_sandboxed_ = loader->record->sandboxed;

    if (auto capabilities{read_loader_capabilities(*loader)}; !capabilities) {
        return std::unexpected{
            annotate(std::move(capabilities.error()), entry.path)};
    }

    const auto activate_code{!declares_provides || selected_provider};
    if (auto validated{validate_with_loader(*loader, *manifest, activate_code)};
        !validated) {
        return std::unexpected{
            annotate(std::move(validated.error()), entry.path)};
    }

    std::uint64_t instance_handle{0};
    if (auto loaded_with{load_with_loader(*loader, *manifest, activate_code,
                                          instance_handle)};
        !loaded_with) {
        return std::unexpected{
            annotate(std::move(loaded_with.error()), entry.path)};
    }

    return loaded_mod{.id = manifest->common.id,
                      .version = manifest->common.version,
                      .abi = manifest->common.abi,
                      .external = manifest->is_external(),
                      .has_code = current_load_has_code_,
                      .provides_summary = provides_summary(*manifest),
                      .manifest_hash_hex = manifest_hash_hex,
                      .required = manifest->required,
                      .optional = manifest->optional,
                      .loader = manifest->loader,
                      .loader_inferred = manifest->loader_inferred,
                      .loader_unload = loader->record->unload,
                      .loader_sandboxed = loader->record->sandboxed,
                      .instance_handle = instance_handle};
}

host::~host() {
    unload_external_mods();
    remove_materialized_entries();
}

void host::unload_external_mods() noexcept {
    for (auto entry{external_mods_.rbegin()}; entry != external_mods_.rend();
         ++entry) {
        if (entry->on_unload != nullptr) {
            try {
                entry->on_unload(entry->user);
            } catch (...) {
                // A mod must not terminate the host during shutdown.
            }
        }
    }
    external_mods_.clear();
}

void host::remove_materialized_entries() noexcept {
    if (materialized_dir_.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::remove_all(materialized_dir_, error);
    materialized_dir_.clear();
}

void host::set_loader_dispatch_error(std::string message) {
    loader_dispatch_error_ = std::move(message);
}

void host::clear_loader_dispatch_error() { loader_dispatch_error_.clear(); }

auto host::take_loader_dispatch_error() -> std::string {
    auto message{std::move(loader_dispatch_error_)};
    loader_dispatch_error_.clear();
    return message;
}

auto host::register_bootstrap_loaders() -> status {
    if (!loaders_.empty()) {
        return status_ok();
    }
    builtin_loader_state_ = std::make_unique<loader_provider_state>();
    builtin_loader_state_->owner = this;
    builtin_loader_state_->loader_id = builtin_loader_id;
    builtin_loader_state_->unload = mod_unload_semantics::restart_required;

    native_loader_state_ = std::make_unique<loader_provider_state>();
    native_loader_state_->owner = this;
    native_loader_state_->loader_id = native_loader_id;
    native_loader_state_->unload = mod_unload_semantics::retire;

    const auto &vtable{internal_loader_vtable()};
    const provider_api builtin_api{
        .vtable = &vtable,
        .self = builtin_loader_state_.get(),
        .abi_major = REFRAIN_SERVICES_MOD_LOADER_ABI_MAJOR,
    };
    if (auto registered{register_mod_loader(builtin_loader_id, builtin_api)};
        !registered) {
        return std::unexpected{
            annotate(std::move(registered.error()), "builtin loader")};
    }
    const provider_api native_api{
        .vtable = &vtable,
        .self = native_loader_state_.get(),
        .abi_major = REFRAIN_SERVICES_MOD_LOADER_ABI_MAJOR,
    };
    if (auto registered{register_mod_loader(native_loader_id, native_api)};
        !registered) {
        return std::unexpected{
            annotate(std::move(registered.error()), "native loader")};
    }
    log_loader.info("registered bootstrap mod loaders builtin and native");
    return status_ok();
}

auto host::register_mod_loader(std::string_view loader_id,
                               const provider_api &api,
                               std::string_view version) -> status {
    if (failed_ || loaded_flag_) {
        return std::unexpected{
            fail(error_code::not_initialized,
                 "register_mod_loader() is only legal before load()")};
    }
    if (!current_mod_.empty()) {
        return std::unexpected{fail(
            error_code::permission_denied,
            "register_mod_loader() is only legal from the host bootstrap, not "
            "from a mod factory")};
    }
    if (!is_valid_loader_id(loader_id)) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 fmt::format("mod loader id '{}' is not a valid loader id",
                             loader_id))};
    }
    if (!is_reserved_loader_id(loader_id) &&
        loader_id.find('.') == std::string_view::npos) {
        return std::unexpected{fail(
            error_code::invalid_argument,
            fmt::format("mod loader id '{}' is not namespaced; a third-party "
                        "loader uses an id such as com.example.loader",
                        loader_id))};
    }
    if (find_loader(loader_id) != nullptr) {
        return std::unexpected{fail(
            error_code::already_exists,
            fmt::format("mod loader '{}' is already registered", loader_id))};
    }
    if (api.vtable == nullptr || api.self == nullptr) {
        return std::unexpected{fail(
            error_code::invalid_argument,
            fmt::format("mod loader '{}' requires both a vtable and a self "
                        "pointer",
                        loader_id))};
    }
    provider_api copy{api};
    if (copy.abi_major == 0) {
        copy.abi_major = REFRAIN_SERVICES_MOD_LOADER_ABI_MAJOR;
    }
    if (auto checked{check_abi_header(copy)}; !checked) {
        return std::unexpected{
            annotate(std::move(checked.error()),
                     fmt::format("mod loader '{}'", loader_id))};
    }

    // Query the loader's declared capabilities now: a loader that cannot
    // answer, names another id or implements another ABI is not usable, and
    // the host records the unload/sandbox contract before dispatching to it.
    mod_loader_capabilities capabilities{};
    try {
        const loader_abi::provider provider{
            *static_cast<const loader_vtable *>(copy.vtable), copy.self};
        loader_abi::mod_loader_capabilities abi_capabilities{};
        provider.capabilities(abi_capabilities);
        auto decoded{capabilities_from_abi(abi_capabilities)};
        if (!decoded) {
            return std::unexpected{
                annotate(std::move(decoded.error()),
                         fmt::format("mod loader '{}'", loader_id))};
        }
        capabilities = std::move(*decoded);
    } catch (...) {
        return std::unexpected{
            fail(error_code::unknown,
                 fmt::format("mod loader '{}' capabilities threw an exception",
                             loader_id))};
    }
    if (auto checked{validate_loader_capabilities(loader_id, capabilities)};
        !checked) {
        return checked;
    }

    const auto effective_version{version.empty() ? std::string{"1.0.0"}
                                                 : std::string{version}};
    provider_apis_.push_back(std::make_unique<provider_api>(copy));
    provider_info info{};
    info.service = std::string{mod_loader_service_id};
    info.id = std::string{loader_id};
    info.version = effective_version;
    info.instance = provider_apis_.back().get();
    info.source = "host";
    auto added{services_.add_provider(std::move(info))};
    if (!added) {
        return added;
    }
    auto selected{services_.select_active(mod_loader_service_id, loader_id,
                                          effective_version, loader_id)};
    if (!selected) {
        return selected;
    }
    loaders_.push_back(registered_loader{
        .id = std::string{loader_id},
        .version = effective_version,
        .api = copy,
        .abi_major = capabilities.abi_major,
        .unload = capabilities.unload,
        .sandboxed = capabilities.sandboxed,
        .supports_hot_install = capabilities.supports_hot_install,
    });
    log_loader.info("registered mod loader {} version {}", loader_id,
                    effective_version);
    return status_ok();
}

auto host::find_loader(std::string_view loader_id) const noexcept
    -> const registered_loader * {
    const auto match{std::find_if(loaders_.begin(), loaders_.end(),
                                  [loader_id](const registered_loader &loader) {
                                      return loader.id == loader_id;
                                  })};
    return match == loaders_.end() ? nullptr : &*match;
}

auto host::resolve_loader(std::string_view loader_id) const
    -> result<loader_binding> {
    const auto active{
        services_.active_provider(mod_loader_service_id, loader_id)};
    if (!active) {
        const auto available{join_instance_names(
            services_.active_instances(mod_loader_service_id))};
        if (is_first_party_loader_id(loader_id)) {
            return std::unexpected{
                fail(error_code::unsupported,
                     fmt::format("mod loader '{}' is not built into this host; "
                                 "active mod loaders: {}",
                                 loader_id, available))};
        }
        return std::unexpected{
            fail(error_code::not_found,
                 fmt::format("no active mod loader named '{}'; active mod "
                             "loaders: {}",
                             loader_id, available))};
    }
    const auto *record{find_loader(loader_id)};
    if (record == nullptr) {
        return std::unexpected{fail(
            error_code::not_found,
            fmt::format("mod loader '{}' is active but was not registered by "
                        "the host bootstrap",
                        loader_id))};
    }
    const auto *api{static_cast<const provider_api *>(active->instance)};
    if (api == nullptr || api->vtable == nullptr || api->self == nullptr) {
        return std::unexpected{
            fail(error_code::unknown,
                 fmt::format("mod loader '{}' provider has no ABI table",
                             loader_id))};
    }
    return loader_binding{.record = record, .api = *api};
}

auto host::read_loader_capabilities(const loader_binding &binding) const
    -> result<mod_loader_capabilities> {
    if (binding.record == nullptr || binding.api.vtable == nullptr ||
        binding.api.self == nullptr) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "mod loader dispatch has no provider table")};
    }
    if (auto checked{check_abi_header(binding.api)}; !checked) {
        return std::unexpected{
            annotate(std::move(checked.error()),
                     fmt::format("mod loader '{}'", binding.record->id))};
    }
    mod_loader_capabilities capabilities{};
    try {
        const loader_abi::provider provider{
            *static_cast<const loader_vtable *>(binding.api.vtable),
            binding.api.self};
        loader_abi::mod_loader_capabilities abi_capabilities{};
        provider.capabilities(abi_capabilities);
        auto decoded{capabilities_from_abi(abi_capabilities)};
        if (!decoded) {
            return std::unexpected{
                annotate(std::move(decoded.error()),
                         fmt::format("mod loader '{}'", binding.record->id))};
        }
        capabilities = std::move(*decoded);
    } catch (...) {
        return std::unexpected{
            fail(error_code::unknown,
                 fmt::format("mod loader '{}' capabilities threw an exception",
                             binding.record->id))};
    }
    if (auto checked{
            validate_loader_capabilities(binding.record->id, capabilities)};
        !checked) {
        return std::unexpected{std::move(checked.error())};
    }
    if (capabilities.abi_major != binding.record->abi_major ||
        capabilities.unload != binding.record->unload ||
        capabilities.sandboxed != binding.record->sandboxed ||
        capabilities.supports_hot_install !=
            binding.record->supports_hot_install) {
        return std::unexpected{fail(
            error_code::version_mismatch,
            fmt::format("mod loader '{}' changed its declared capabilities "
                        "while running",
                        binding.record->id))};
    }
    return capabilities;
}

auto host::validate_with_loader(const loader_binding &binding,
                                const mod_manifest &manifest,
                                bool activate_code) -> status {
    if (current_candidate_ == nullptr) {
        return std::unexpected{
            fail(error_code::unknown,
                 "mod loader dispatch has no candidate context")};
    }
    auto package_root{std::string{}};
    if (const auto parent{
            std::filesystem::path{current_candidate_->path}.parent_path()};
        !parent.empty()) {
        package_root = parent.string();
    }
    const auto entry{make_loader_entry(manifest, activate_code, package_root)};
    try {
        const loader_abi::provider provider{
            *static_cast<const loader_vtable *>(binding.api.vtable),
            binding.api.self};
        const auto validated{provider.validate(entry)};
        if (!validated) {
            const auto detail{take_loader_dispatch_error()};
            return std::unexpected{
                fail(loader_error_to(validated.error()),
                     detail.empty()
                         ? fmt::format("mod loader '{}' rejected mod '{}'",
                                       binding.record->id, manifest.common.id)
                         : fmt::format("mod loader '{}' rejected mod '{}': {}",
                                       binding.record->id, manifest.common.id,
                                       detail))};
        }
    } catch (...) {
        return std::unexpected{
            fail(error_code::unknown,
                 fmt::format("mod loader '{}' validate threw an exception",
                             binding.record->id))};
    }
    return status_ok();
}

auto host::load_with_loader(const loader_binding &binding,
                            const mod_manifest &manifest, bool activate_code,
                            std::uint64_t &handle) -> status {
    handle = 0;
    if (current_candidate_ == nullptr) {
        return std::unexpected{
            fail(error_code::unknown,
                 "mod loader dispatch has no candidate context")};
    }
    auto package_root{std::string{}};
    if (const auto parent{
            std::filesystem::path{current_candidate_->path}.parent_path()};
        !parent.empty()) {
        package_root = parent.string();
    }
    loader_abi::mod_load_request request{};
    request.struct_size = REFRAIN_SERVICES_MOD_LOADER_MOD_LOAD_REQUEST_SIZE;
    request.manifest = make_loader_entry(manifest, activate_code, package_root);
    try {
        const loader_abi::provider provider{
            *static_cast<const loader_vtable *>(binding.api.vtable),
            binding.api.self};
        const auto loaded{provider.load(request)};
        if (!loaded) {
            const auto detail{take_loader_dispatch_error()};
            const auto code{loader_error_to(loaded.error())};
            // A sandbox refusal is the loader's policy, not a package defect:
            // say so instead of leaving the operator with a bare permission
            // error.
            const auto sandbox_reason{
                binding.record->sandboxed &&
                        code == error_code::permission_denied
                    ? std::string{" because the loader is sandboxed"}
                    : std::string{}};
            return std::unexpected{fail(
                code,
                detail.empty()
                    ? fmt::format("mod loader '{}' failed to load mod '{}'{}",
                                  binding.record->id, manifest.common.id,
                                  sandbox_reason)
                    : fmt::format("mod loader '{}' failed to load mod '{}'{}: "
                                  "{}",
                                  binding.record->id, manifest.common.id,
                                  sandbox_reason, detail))};
        }
        handle = loaded->raw();
    } catch (...) {
        return std::unexpected{
            fail(error_code::unknown,
                 fmt::format("mod loader '{}' load threw an exception",
                             binding.record->id))};
    }
    return status_ok();
}

auto host::builtin_validate() -> status {
    if (current_manifest_ == nullptr) {
        return std::unexpected{fail(error_code::unknown,
                                    "builtin loader has no manifest context")};
    }
    const auto &manifest{*current_manifest_};
    auto matched{registry_.match(manifest)};
    if (!matched) {
        return std::unexpected{std::move(matched.error())};
    }
    current_static_descriptor_ = *matched;

    // Only a mod the plan selected may run provider code. A mod that declares
    // `[provides]` but is not named by `composition.providers` keeps loading
    // as a data/content mod; a selected provider without linked factory code
    // fails closed instead of shipping a composition with a dead service.
    const auto declares_provides{!manifest.provides.empty()};
    if (declares_provides && current_mod_selected_provider_ &&
        (current_static_descriptor_ == nullptr ||
         current_static_descriptor_->factory == nullptr)) {
        return std::unexpected{fail(
            error_code::unsupported,
            fmt::format("gamepack selects internal mod '{}' as provider but "
                        "it declares [provides] ({}) without a "
                        "REFR_MOD_STATIC factory",
                        manifest.common.id, provides_summary(manifest)))};
    }
    return status_ok();
}

auto host::builtin_load(std::uint64_t &handle) -> status {
    handle = 0;
    if (current_manifest_ == nullptr) {
        return std::unexpected{fail(error_code::unknown,
                                    "builtin loader has no manifest context")};
    }
    const auto descriptor{current_static_descriptor_};
    if (descriptor == nullptr || descriptor->factory == nullptr) {
        return status_ok(); // data/content mod: no code to run
    }
    const auto &manifest{*current_manifest_};
    // Host→mod boundary: a static mod factory is mod code, so an escaping
    // exception must be contained here (the mod loader has no exception
    // channel) instead of unwinding through the host. A hard OOM reports a
    // short SSO message so the failure path does not allocate again.
    status factory_status{};
    try {
        factory_status = descriptor->factory(*this);
    } catch (const std::bad_alloc &) {
        return std::unexpected{
            make_error(error_code::out_of_memory, "out of memory")};
    } catch (...) {
        return std::unexpected{annotate(
            make_error(error_code::mod_error,
                       fmt::format("mod '{}' factory threw an exception",
                                   manifest.common.id)),
            "provider activation")};
    }
    if (!factory_status) {
        return std::unexpected{
            annotate(std::move(factory_status.error()),
                     fmt::format("mod '{}' factory", manifest.common.id))};
    }
    current_load_has_code_ = true;
    handle = next_instance_handle_++;
    return status_ok();
}

auto host::native_validate(std::string_view native_path,
                           std::string_view wasm_path, std::uint32_t abi,
                           bool activate_code) -> status {
    if (!wasm_path.empty()) {
        return std::unexpected{fail(
            error_code::invalid_argument,
            "the native loader does not accept [entry].wasm; the wasm loader "
            "owns script modules")};
    }
    if (native_path.empty()) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "the native loader requires [entry].native with the shared "
                 "library member path")};
    }
    if (!activate_code) {
        // Provider declared but not selected: the package stays data-only and
        // no library is opened. The structural checks above still ran.
        return status_ok();
    }
    // The manifest declares the entry ABI the package was built against. A
    // mismatch is rejected before the library is opened, so no mod code runs
    // against a host it cannot understand.
    if (abi != REFRAIN_MOD_ABI_MAJOR) {
        const auto mod_id{current_manifest_ == nullptr
                              ? std::string{"?"}
                              : current_manifest_->common.id};
        return std::unexpected{fail(
            error_code::version_mismatch,
            fmt::format("external mod '{}' was built for entry ABI {}, this "
                        "host provides {}",
                        mod_id, abi, REFRAIN_MOD_ABI_MAJOR))};
    }
    return status_ok();
}

auto host::native_load(std::uint64_t &handle) -> status {
    handle = 0;
    if (current_manifest_ == nullptr || current_candidate_ == nullptr ||
        current_package_ == nullptr) {
        return std::unexpected{
            fail(error_code::unknown, "native loader has no package context")};
    }
    const auto &manifest{*current_manifest_};
    const auto &entry{*current_candidate_};
    if (!manifest.entry.has_value() || manifest.entry->native.empty()) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "native loader has no [entry].native member path")};
    }
    const auto &member{manifest.entry->native};

    // An installed package keeps its members in the content store: the
    // manifest path is a package member, not a filesystem path. Read it
    // through the package source (which verifies the bytes against the install
    // receipt) and write one private copy a dynamic loader can open.
    auto library_bytes{current_package_->read(member)};
    if (!library_bytes) {
        return std::unexpected{
            annotate(std::move(library_bytes.error()),
                     fmt::format("mod '{}' native entry '{}'",
                                 manifest.common.id, member))};
    }
    if (materialized_dir_.empty()) {
        auto created{create_loader_scratch_root()};
        if (!created) {
            return std::unexpected{std::move(created.error())};
        }
        materialized_dir_ = std::move(*created);
    }
    // Name the copy after its content: two packages that ship the same library
    // share one file, and a retry never opens a half-written entry.
    const auto digest_hex{data::to_hex(data::blake3(*library_bytes))};
    // Keep the packaged member's extension; the dynamic loader opens the file
    // by path and infers nothing from it.
    const auto extension{std::filesystem::path{member}.extension().string()};
    const auto library_path{
        data::join_path(materialized_dir_, digest_hex + extension)};
    if (!io::exists(library_path)) {
        if (auto written{io::write_file(library_path, *library_bytes)};
            !written) {
            return std::unexpected{
                annotate(std::move(written.error()),
                         fmt::format("mod '{}' native entry copy {}",
                                     manifest.common.id, library_path))};
        }
    }

    auto loaded_library{platform::dynamic_library::load(library_path)};
    if (!loaded_library) {
        return std::unexpected{annotate(
            fail(error_code::io_error,
                 fmt::format("cannot load native entry '{}'", library_path)),
            entry.path)};
    }
    auto library{std::make_unique<platform::dynamic_library>(
        std::move(*loaded_library))};
    const auto symbol_name{manifest.entry->symbol.empty()
                               ? std::string{"refrain_mod_main"}
                               : manifest.entry->symbol};
    auto symbol{library->symbol(symbol_name)};
    if (!symbol) {
        return std::unexpected{
            annotate(fail(error_code::not_found,
                          fmt::format("native entry '{}' does not export '{}'",
                                      library_path, symbol_name)),
                     entry.path)};
    }
    auto main_fn{reinterpret_cast<refrain_mod_main_fn>(*symbol)};

    const auto host_core{
        services_.active_provider(REFRAIN_HOST_CORE_SERVICE_ID)};
    if (!host_core) {
        return std::unexpected{
            fail(error_code::unsupported,
                 "refrain:host.core is not registered; external mods need the "
                 "sealed host runtime provider")};
    }
    const auto *api{static_cast<const provider_api *>(host_core->instance)};
    if (api == nullptr || api->vtable == nullptr || api->self == nullptr) {
        return std::unexpected{fail(error_code::unknown,
                                    "refrain:host.core provider has no ABI "
                                    "table")};
    }

    refrain_mod_init init{};
    init.struct_size = static_cast<std::uint32_t>(sizeof(init));
    init.abi_major = REFRAIN_MOD_ABI_MAJOR;
    init.mod_id = to_refrain_str(manifest.common.id);
    init.mod_version = to_refrain_str(manifest.common.version);
    init.host_core = static_cast<const refrain_host_core_vtable *>(api->vtable);
    init.host_core_self = api->self;
    init.allocator = host_allocator();

    const refrain_mod_desc *desc{nullptr};
    try {
        desc = main_fn(&init);
    } catch (...) {
        return std::unexpected{
            annotate(fail(error_code::unknown,
                          fmt::format("'{}' entry point threw", symbol_name)),
                     entry.path)};
    }
    if (desc == nullptr) {
        return std::unexpected{
            fail(error_code::parse_error,
                 fmt::format("'{}' returned a null descriptor", symbol_name))};
    }
    if (desc->struct_size < sizeof(refrain_mod_desc)) {
        return std::unexpected{
            fail(error_code::version_mismatch,
                 fmt::format("'{}' descriptor struct_size {} is too small",
                             symbol_name, desc->struct_size))};
    }
    if (desc->abi_major != REFRAIN_MOD_ABI_MAJOR) {
        return std::unexpected{fail(
            error_code::version_mismatch,
            fmt::format("'{}' entry ABI major {} does not match {}",
                        symbol_name, desc->abi_major, REFRAIN_MOD_ABI_MAJOR))};
    }
    if (desc->on_load == nullptr) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 fmt::format("'{}' descriptor has no on_load callback",
                             symbol_name))};
    }

    refrain_mod_context context{};
    context.struct_size = static_cast<std::uint32_t>(sizeof(context));
    context.mod_id = to_refrain_str(manifest.common.id);
    context.mod_version = to_refrain_str(manifest.common.version);
    context.host_core = init.host_core;
    context.host_core_self = init.host_core_self;
    void *user{nullptr};
    refrain_status on_load_status{REFRAIN_STATUS_OK};
    try {
        on_load_status = desc->on_load(&context, &user);
    } catch (...) {
        return std::unexpected{
            annotate(fail(error_code::unknown,
                          fmt::format("external mod '{}' on_load threw",
                                      manifest.common.id)),
                     entry.path)};
    }
    if (on_load_status != REFRAIN_STATUS_OK) {
        return std::unexpected{
            annotate(fail(from_mod_status(on_load_status),
                          fmt::format("external mod '{}' on_load failed "
                                      "with status {}",
                                      manifest.common.id, on_load_status)),
                     entry.path)};
    }

    handle = next_instance_handle_++;
    current_load_has_code_ = true;
    external_mods_.push_back(external_mod_state{
        .library = std::move(library),
        .on_unload = desc->on_unload,
        .user = user,
        .handle = handle,
    });
    return status_ok();
}

auto host::load(const composition &plan, std::span<const std::byte> image)
    -> status {
    auto resolver{make_package_resolver(image)};
    if (!resolver) {
        // A resolver that cannot even open the builtin image is a failed load
        // attempt: keep the single-use contract (the next call asks the caller
        // to recreate the host) instead of allowing a retry on half state.
        enter_failed_state();
        return std::unexpected{std::move(resolver.error())};
    }
    return load(plan, *resolver);
}

auto host::load(const composition &plan, const package_resolver &resolve)
    -> status {
    if (failed_) {
        return std::unexpected{
            fail(error_code::unsupported,
                 "mod host failed a previous load and cannot be reused; "
                 "recreate the host")};
    }
    if (loaded_flag_) {
        return std::unexpected{
            fail(error_code::already_exists, "mod host is already loaded")};
    }
    if (!resolve) {
        return std::unexpected{
            fail(error_code::invalid_argument, "package resolver is empty")};
    }

    const auto attempt{[&]() -> status {
        // Static registrars run during static initialization; freeze now so
        // duplicate/invalid descriptors surface as a startup error instead of
        // being irrelevantly present or half-registered.
        if (auto frozen{registry_.freeze()}; !frozen) {
            return std::unexpected{std::move(frozen.error())};
        }
        // The engine-owned loaders are part of the host bootstrap, not of a
        // gamepack: a mod loader cannot be selected or replaced by a
        // composition. Registration queries their capabilities and records the
        // unload/sandbox contract before any mod is dispatched.
        if (auto registered{register_bootstrap_loaders()}; !registered) {
            return registered;
        }

        // The resolved plan is the only authority on which mods may
        // activate provider code, so project it once before any factory runs.
        // Every exclusive provider, chain base/wrapper and instanced provider
        // contributes its mod id and the `[providers]` short slots it serves.
        planned_providers planned{};
        for (const auto &resolved : plan.providers) {
            const auto add_provider_ref{
                [&planned, &resolved](std::string_view ref,
                                      std::string_view instance) {
                    const auto id{std::string{split_provider_ref(ref).id}};
                    if (id.empty()) {
                        return;
                    }
                    planned.mods.insert(id);
                    if (resolved.service.empty()) {
                        return;
                    }
                    planned.slots[id].insert(resolved.service);
                    if (!instance.empty()) {
                        planned.instances[id][resolved.service].insert(
                            std::string{instance});
                    }
                }};
            add_provider_ref(resolved.provider, {});
            for (const auto &instance : resolved.instances) {
                add_provider_ref(instance.provider, instance.name);
            }
            for (const auto &wrapper : resolved.wrappers) {
                add_provider_ref(wrapper, {});
            }
        }

        std::vector<loaded_mod> mods{};
        mods.reserve(plan.load_order.size());
        for (const auto &entry : plan.load_order) {
            auto package{resolve(entry)};
            if (!package) {
                return std::unexpected{
                    annotate(std::move(package.error()),
                             fmt::format("mod '{}' package", entry.id))};
            }
            // Host-owned copy: providers such as `refrain.data.vanilla` mount
            // the builtin image as their lowest-priority pack and may keep the
            // bytes for the host lifetime.
            if (builtin_image_bytes_.empty()) {
                const auto image_bytes{(*package)->image_bytes()};
                if (!image_bytes.empty()) {
                    builtin_image_bytes_.assign(image_bytes.begin(),
                                                image_bytes.end());
                    auto opened{
                        data::builtin_image::open(builtin_image_bytes_)};
                    if (!opened) {
                        return std::unexpected{std::move(opened.error())};
                    }
                    builtin_image_digest_hex_ = data::to_hex(opened->digest());
                }
            }
            current_mod_ = entry.id;
            current_mod_version_ = entry.version;
            auto loaded{load_one(plan, **package, entry, planned)};
            if (!loaded) {
                return std::unexpected{std::move(loaded.error())};
            }
            mods.push_back(std::move(*loaded));
        }
        current_mod_.clear();
        current_mod_version_.clear();
        current_mod_provides_.clear();
        current_mod_planned_slots_.clear();
        current_mod_planned_instances_.clear();
        current_mod_selected_provider_ = false;
        current_manifest_ = nullptr;
        current_candidate_ = nullptr;
        current_package_ = nullptr;
        current_static_descriptor_ = nullptr;

        // `verify_plan_activation()` resolves planned provider ids through
        // `find()`, which reads `mods_`; publish the list before verifying so
        // the check sees the mods that were just loaded. A failure clears it
        // again through `enter_failed_state()`.
        mods_ = std::move(mods);

        // Fail closed: every provider the gamepack selected must be active
        // before the registry is frozen, so a factory that silently skips (or
        // the wrong mod winning) fails startup instead of shipping a
        // composition that does not match the plan.
        if (auto verified{verify_plan_activation(plan)}; !verified) {
            return verified;
        }

        loaded_flag_ = true;
        // Composition is complete: providers can no longer be added.
        services_.freeze();
        return status_ok();
    }};

    auto result{attempt()};
    if (!result) {
        // No partial activation is exposed: drop the providers, API copies,
        // image and mod list assembled so far and make the host unusable.
        enter_failed_state();
    }
    return result;
}

auto host::verify_plan_activation(const composition &plan) const -> status {
    const auto verify_active{[this](std::string_view slot, std::string_view ref,
                                    std::string_view instance) -> status {
        const auto [id, version] = split_provider_ref(ref);
        if (id.empty()) {
            return status_ok();
        }
        const auto *mod{find(id)};
        if (mod == nullptr) {
            return std::unexpected{
                fail(error_code::not_found,
                     fmt::format("provider activation for service '{}': "
                                 "planned mod '{}' was not loaded",
                                 slot, id))};
        }
        if (mod->provides_summary.empty()) {
            // Data-only binding (`content = ...`): the mod declares no
            // contract, so there is no provider instance to activate.
            return status_ok();
        }
        for (const auto &service : services_.active_services()) {
            if (!matches_slot(service, slot)) {
                continue;
            }
            const auto active{services_.active_provider(service, instance)};
            if (!active || active->id != id) {
                continue;
            }
            if (!version.empty() && active->version != version) {
                return std::unexpected{fail(
                    error_code::version_mismatch,
                    fmt::format("provider activation for service '{}' "
                                "instance '{}': planned mod '{}' "
                                "registered version '{}' but the plan "
                                "pins {}",
                                slot, instance, id, active->version, version))};
            }
            return status_ok();
        }
        return std::unexpected{fail(
            error_code::not_found,
            fmt::format("provider activation for service '{}' instance '{}': "
                        "planned mod '{}' has no active provider",
                        slot, instance, id))};
    }};

    // Chain wrappers wrap the base provider; the upper layer has no runtime
    // chain composition yet, so a wrapper only has to be registered as a
    // candidate (the base is the active default instance).
    const auto verify_registered{[this](std::string_view slot,
                                        std::string_view ref) -> status {
        const auto [id, version] = split_provider_ref(ref);
        if (id.empty()) {
            return status_ok();
        }
        for (const auto &service : services_.services()) {
            if (!matches_slot(service, slot)) {
                continue;
            }
            const auto candidate{services_.find(service, id, version)};
            if (candidate.has_value()) {
                return status_ok();
            }
        }
        return std::unexpected{fail(
            error_code::not_found,
            fmt::format("provider activation for service '{}': planned chain "
                        "wrapper '{}' was not registered",
                        slot, ref))};
    }};

    for (const auto &resolved : plan.providers) {
        if (!resolved.provider.empty()) {
            if (auto checked{
                    verify_active(resolved.service, resolved.provider, {})};
                !checked) {
                return checked;
            }
        }
        for (const auto &instance : resolved.instances) {
            if (auto checked{verify_active(resolved.service, instance.provider,
                                           instance.name)};
                !checked) {
                return checked;
            }
        }
        for (const auto &wrapper : resolved.wrappers) {
            if (auto checked{verify_registered(resolved.service, wrapper)};
                !checked) {
                return checked;
            }
        }
    }
    return status_ok();
}

/// Not `noexcept`: `service_registry::clear()` locks its mutex
/// (`std::system_error` on failure) and `make_error` in the frozen path
/// allocates. Reached only from the `load()` error paths, which return
/// `status`.
void host::enter_failed_state() {
    unload_external_mods();
    remove_materialized_entries();
    // `freeze()` only runs at the very end of a successful load, so the
    // registry is still mutable here and `clear()` cannot fail.
    static_cast<void>(services_.clear());
    provider_apis_.clear();
    sealed_services_.clear();
    loaders_.clear();
    builtin_loader_state_.reset();
    native_loader_state_.reset();
    builtin_image_bytes_.clear();
    builtin_image_digest_hex_.clear();
    mods_.clear();
    current_mod_.clear();
    current_mod_version_.clear();
    current_mod_provides_.clear();
    current_mod_planned_slots_.clear();
    current_mod_planned_instances_.clear();
    current_mod_selected_provider_ = false;
    current_manifest_ = nullptr;
    current_candidate_ = nullptr;
    current_package_ = nullptr;
    current_static_descriptor_ = nullptr;
    current_loader_id_.clear();
    current_loader_sandboxed_ = false;
    current_load_has_code_ = false;
    loader_dispatch_error_.clear();
    loaded_flag_ = false;
    failed_ = true;
}

auto host::register_provider(std::string_view service, const provider_api &api,
                             std::string_view version,
                             kernel::capability_bits capabilities) -> status {
    return register_provider(service, {}, api, version, capabilities);
}

auto host::register_sealed_provider(std::string_view service,
                                    const provider_api &api,
                                    std::string_view version,
                                    kernel::capability_bits capabilities)
    -> status {
    if (failed_ || loaded_flag_) {
        return std::unexpected{
            fail(error_code::not_initialized,
                 "register_sealed_provider() is only legal before load()")};
    }
    if (!current_mod_.empty()) {
        return std::unexpected{fail(
            error_code::permission_denied,
            "register_sealed_provider() is only legal from the host bootstrap, "
            "not from a mod factory")};
    }
    if (service.empty()) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "register_sealed_provider() requires a non-empty service id")};
    }
    if (api.vtable == nullptr || api.self == nullptr) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "register_sealed_provider() requires both a vtable and a self "
                 "pointer")};
    }
    if (auto checked{check_abi_header(api)}; !checked) {
        return checked;
    }

    const std::string effective_version{version.empty() ? std::string{"1.0.0"}
                                                        : std::string{version}};
    provider_apis_.push_back(std::make_unique<provider_api>(api));
    provider_info info{};
    info.service = std::string{service};
    info.id = "host";
    info.version = effective_version;
    info.capabilities = capabilities;
    info.instance = provider_apis_.back().get();
    info.source = "host";
    auto added{services_.add_provider(std::move(info))};
    if (!added) {
        return added;
    }
    auto selected{services_.select_active(service, "host", effective_version)};
    if (!selected) {
        return selected;
    }
    sealed_services_.insert(std::string{service});
    return status_ok();
}

auto host::is_sealed_service(std::string_view service) const noexcept -> bool {
    for (const auto &sealed : sealed_services_) {
        if (sealed.compare(service) == 0) {
            return true;
        }
    }
    return false;
}

auto host::register_provider(std::string_view service,
                             std::string_view instance, const provider_api &api,
                             std::string_view version,
                             kernel::capability_bits capabilities) -> status {
    if (failed_ || loaded_flag_ || current_mod_.empty()) {
        return std::unexpected{fail(
            error_code::not_initialized,
            "register_provider() is only legal from a static mod factory while "
            "the host is loading")};
    }
    if (current_loader_sandboxed_) {
        return std::unexpected{fail(
            error_code::permission_denied,
            fmt::format("sandboxed loader '{}' cannot register service '{}' "
                        "for mod '{}'; a sandboxed guest only reaches the host "
                        "through its imports",
                        current_loader_id_, service, current_mod_))};
    }
    if (service == mod_loader_service_id) {
        return std::unexpected{fail(
            error_code::permission_denied,
            "the mod loader service is registered by the host bootstrap, not "
            "by a mod")};
    }
    if (is_sealed_service(service)) {
        return std::unexpected{fail(
            error_code::permission_denied,
            fmt::format("service '{}' is sealed and is owned by the host; a "
                        "mod cannot provide it",
                        service))};
    }
    if (api.vtable == nullptr || api.self == nullptr) {
        return std::unexpected{fail(
            error_code::invalid_argument,
            "register_provider() requires both a vtable and a self pointer")};
    }
    if (auto checked{check_abi_header(api)}; !checked) {
        return checked;
    }

    // A factory may only claim a service its manifest declares *and* the
    // gamepack plan selected this mod for. Anything else is a composition bug
    // and fails closed instead of silently replacing the planned provider.
    if (!declares_service(current_mod_provides_, service)) {
        return std::unexpected{fail(
            error_code::permission_denied,
            fmt::format("mod '{}' cannot provide service '{}': the manifest "
                        "[provides] list does not declare it",
                        current_mod_, service))};
    }
    const std::string *planned_slot{nullptr};
    for (const auto &slot : current_mod_planned_slots_) {
        if (matches_slot(service, slot)) {
            planned_slot = &slot;
            break;
        }
    }
    if (planned_slot == nullptr) {
        return std::unexpected{fail(
            error_code::permission_denied,
            fmt::format("mod '{}' cannot activate service '{}': the gamepack "
                        "plan does not select it as that provider",
                        current_mod_, service))};
    }
    const auto binding{current_mod_planned_instances_.find(*planned_slot)};
    const bool instanced{binding != current_mod_planned_instances_.end() &&
                         !binding->second.empty()};
    if (instanced) {
        if (instance.empty() ||
            binding->second.count(std::string{instance}) == 0) {
            std::string planned;
            for (const auto &name : binding->second) {
                if (!planned.empty()) {
                    planned += ", ";
                }
                planned += name;
            }
            return std::unexpected{
                fail(error_code::permission_denied,
                     fmt::format("mod '{}' cannot activate service '{}' on "
                                 "instance '{}': the plan binds [{}]",
                                 current_mod_, service, instance, planned))};
        }
    } else if (!instance.empty() &&
               instance != kernel::service_registry::default_instance) {
        return std::unexpected{fail(
            error_code::permission_denied,
            fmt::format("mod '{}' cannot activate service '{}' on instance "
                        "'{}': an exclusive/chain binding only has the "
                        "default instance",
                        current_mod_, service, instance))};
    }

    const auto effective_version{version.empty() ? current_mod_version_
                                                 : version};
    provider_apis_.push_back(std::make_unique<provider_api>(api));
    provider_info info{};
    info.service = std::string{service};
    info.id = current_mod_;
    info.version = std::string{effective_version};
    info.capabilities = capabilities;
    info.instance = provider_apis_.back().get();
    info.source = "builtin";
    auto added{services_.add_provider(std::move(info))};
    if (!added) {
        return added;
    }
    return services_.select_active(service, current_mod_, effective_version,
                                   instance);
}

auto host::find(std::string_view id) const noexcept -> const loaded_mod * {
    const auto match{
        std::find_if(mods_.begin(), mods_.end(),
                     [id](const loaded_mod &mod) { return mod.id == id; })};
    return match == mods_.end() ? nullptr : &*match;
}

auto host::format() const -> std::string {
    if (!loaded_flag_) {
        return "mod host: not loaded\n";
    }
    auto out{fmt::format("mod host: {} mod(s) loaded, built-in image BLAKE3 "
                         "{}\n",
                         mods_.size(), builtin_image_digest_hex_)};
    out += "  loaders:";
    if (loaders_.empty()) {
        out += " none";
    }
    for (const auto &loader : loaders_) {
        out += fmt::format(" {} abi {} unload {}{}{};", loader_identity(loader),
                           loader.abi_major, to_string(loader.unload),
                           loader.sandboxed ? " sandboxed" : "",
                           loader.supports_hot_install ? " hot-install" : "");
    }
    out += '\n';
    for (const auto &mod : mods_) {
        // Order the fields as the audit text always had them (`abi`, code
        // state, provides) and append the loader record, so log greps keep
        // matching while the loader stays visible.
        out += fmt::format(
            "  {}@{} (abi {}, {}{}, loader {}{}{}, unload {})", mod.id,
            mod.version, mod.abi, mod.has_code ? "code" : "data-only",
            mod.provides_summary.empty() ? ""
                                         : ", provides " + mod.provides_summary,
            mod.loader, mod.loader_inferred ? " inferred" : "",
            mod.loader_sandboxed ? " sandboxed" : "",
            to_string(mod.loader_unload));
        out += '\n';
    }
    return out;
}

} // namespace refrain::mod
