// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/mod/host.hpp>

#include <refrain/core/config.hpp>
#include <refrain/core/log.hpp>
#include <refrain/data/builtin_image.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/mod/package.hpp>
#include <refrain/mod/static_mod.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace refrain::mod {

namespace {

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
           in_slot_namespace(service, namespace_prefix, slot);
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
    if (manifest->is_external()) {
        const auto &native{manifest->entry->native};
        const auto &wasm{manifest->entry->wasm};
        return std::unexpected{fail(
            error_code::unsupported,
            fmt::format("{}: external mod '{}' ({}); dynamic mod loading is "
                        "not implemented",
                        entry.path, manifest->common.id,
                        native.empty() ? fmt::format("wasm {}", wasm)
                                       : fmt::format("native {}", native)))};
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
    const auto declares_provides{!manifest->provides.empty()};
    const auto selected_provider{planned.mods.contains(manifest->common.id)};
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
        log::info("mod '{}': provider not selected; code not activated "
                  "(provides {})",
                  manifest->common.id, provides_summary(*manifest));
    }

    // Context for `register_provider()`: the factory may only claim services
    // this manifest declares and the plan selected this mod for.
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
    // `freeze()` only runs at the very end of a successful load, so the
    // registry is still mutable here and `clear()` cannot fail.
    static_cast<void>(services_.clear());
    provider_apis_.clear();
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
