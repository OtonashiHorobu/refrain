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
#include <refrain/core/config.hpp>
#include <refrain/core/log.hpp>
#include <refrain/core/semver.hpp>
#include <refrain/data/builtin_image.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/mod/package.hpp>
#include <refrain/mod/static_mod.hpp>
#include <refrain/platform/dynamic_library.hpp>

#include <algorithm>
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

} // namespace

namespace refrain::mod {

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
    auto manifest{parse_mod_manifest(*doc)};
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

    if (manifest->is_external()) {
        const auto activate_external{!declares_provides || selected_provider};
        if (declares_provides && !selected_provider) {
            log_mod.info("mod {} provides {} but the gamepack does not select "
                         "it as a provider, so it loads as data only",
                         manifest->common.id, provides_summary(*manifest));
        }
        loaded_mod loaded{.id = manifest->common.id,
                          .version = manifest->common.version,
                          .abi = manifest->common.abi,
                          .external = true,
                          .has_code = activate_external,
                          .provides_summary = provides_summary(*manifest),
                          .manifest_hash_hex = manifest_hash_hex,
                          .required = manifest->required,
                          .optional = manifest->optional};
        if (auto external{
                load_external_mod(*manifest, entry, activate_external, loaded)};
            !external) {
            return std::unexpected{
                annotate(std::move(external.error()), entry.path)};
        }
        return loaded;
    }

    auto matched{registry_.match(*manifest)};
    if (!matched) {
        return std::unexpected{
            annotate(std::move(matched.error()), entry.path)};
    }
    const static_mod_descriptor *descriptor{*matched};

    // Only a mod the plan selected may run provider code. A mod that
    // declares `[provides]` but is not named by `startup_plan.providers` keeps
    // loading as a data/content mod: its factory stays dormant so it cannot
    // win a service the gamepack did not select it for.
    const auto activate_code{descriptor != nullptr &&
                             descriptor->factory != nullptr &&
                             (!declares_provides || selected_provider)};
    if (declares_provides && selected_provider && !activate_code) {
        return std::unexpected{fail(
            error_code::unsupported,
            fmt::format("{}: gamepack selects internal mod '{}' as provider "
                        "but it declares [provides] ({}) without a "
                        "REFR_MOD_STATIC factory",
                        entry.path, manifest->common.id,
                        provides_summary(*manifest)))};
    }
    if (declares_provides && !selected_provider) {
        log_mod.info("mod {} provides {} but the gamepack does not select it "
                     "as a provider, so it loads as data only",
                     manifest->common.id, provides_summary(*manifest));
    }

    loaded_mod loaded{.id = manifest->common.id,
                      .version = manifest->common.version,
                      .abi = manifest->common.abi,
                      .external = manifest->is_external(),
                      .has_code = activate_code,
                      .provides_summary = provides_summary(*manifest),
                      .manifest_hash_hex = manifest_hash_hex,
                      .required = manifest->required,
                      .optional = manifest->optional};
    if (activate_code) {
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
                                       manifest->common.id)),
                "provider activation")};
        }
        if (!factory_status) {
            return std::unexpected{
                annotate(std::move(factory_status.error()),
                         fmt::format("mod '{}' factory", manifest->common.id))};
        }
    }
    return loaded;
}

host::~host() { unload_external_mods(); }

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

auto host::load_external_mod(const mod_manifest &manifest,
                             const candidate &entry, bool activate_code,
                             loaded_mod &out) -> status {
    if (!manifest.entry.has_value()) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "external mod is missing its [entry] table")};
    }
    if (!manifest.entry->wasm.empty()) {
        return std::unexpected{
            fail(error_code::unsupported,
                 "WASM external mods are not implemented; only native entry "
                 "points are supported")};
    }
    if (manifest.entry->native.empty()) {
        return std::unexpected{fail(error_code::invalid_argument,
                                    "external mod has no native entry point")};
    }
    if (!activate_code) {
        // Provider declared but not selected: keep the package as data only
        // and do not run external code.
        return status_ok();
    }
    // The manifest declares the entry ABI the package was built against. A
    // mismatch is rejected before the library is opened, so no mod code runs
    // against a host it cannot understand.
    if (manifest.common.abi != REFRAIN_MOD_ABI_MAJOR) {
        return std::unexpected{annotate(
            fail(error_code::version_mismatch,
                 fmt::format("external mod '{}' was built for entry ABI {}, "
                             "this host provides {}",
                             manifest.common.id, manifest.common.abi,
                             REFRAIN_MOD_ABI_MAJOR)),
            entry.path)};
    }

    const auto root{std::filesystem::path{entry.path}.parent_path()};
    const auto library_path{root / manifest.entry->native};
    auto loaded_library{platform::dynamic_library::load(library_path.string())};
    if (!loaded_library) {
        return std::unexpected{
            annotate(fail(error_code::io_error,
                          fmt::format("cannot load native entry '{}'",
                                      library_path.string())),
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
                                      library_path.string(), symbol_name)),
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

    external_mods_.push_back(external_mod_state{
        .library = std::move(library),
        .on_unload = desc->on_unload,
        .user = user,
    });
    out.external = true;
    out.has_code = true;
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
    // `freeze()` only runs at the very end of a successful load, so the
    // registry is still mutable here and `clear()` cannot fail.
    static_cast<void>(services_.clear());
    provider_apis_.clear();
    sealed_services_.clear();
    builtin_image_bytes_.clear();
    builtin_image_digest_hex_.clear();
    mods_.clear();
    current_mod_.clear();
    current_mod_version_.clear();
    current_mod_provides_.clear();
    current_mod_planned_slots_.clear();
    current_mod_planned_instances_.clear();
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
    for (const auto &mod : mods_) {
        out += fmt::format("  {}@{} (abi {}, {}{})", mod.id, mod.version,
                           mod.abi, mod.has_code ? "code" : "data-only",
                           mod.provides_summary.empty()
                               ? ""
                               : ", provides " + mod.provides_summary);
        out += '\n';
    }
    return out;
}

} // namespace refrain::mod
