// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/script/loaders.hpp>

#include <refrain/contracts/services/mod_loader.hpp>
#include <refrain/core/log.hpp>
#include <refrain/mod/entry.h>
#include <refrain/mod/loader.hpp>
#include <refrain/mod/package.hpp>
#include <refrain/script/wasm.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace refrain::script::wasm {
namespace {

namespace loader_abi = refrain::services::mod_loader;
namespace contracts = refrain::contracts;

using contract_handle = refrain_services_mod_loader_handle_mod_instance;
using contract_reason = refrain_services_mod_loader_mod_unload_reason;
using contract_capabilities =
    refrain_services_mod_loader_mod_loader_capabilities;
using contract_entry = refrain_services_mod_loader_mod_entry_manifest;
using contract_request = refrain_services_mod_loader_mod_load_request;

constexpr refrain::log::logger log_loader{"mod-loader-wasm"};

static_assert(guest_abi_major == REFRAIN_MOD_ABI_MAJOR,
              "the wasm guest ABI major follows the host runtime ABI major");

/// One loader failure: contract error code plus the detail the host appends to
/// its own message. The ABI carries the code only, so the message travels
/// through the per-thread loader diagnostic.
struct failure {
    loader_abi::mod_loader_error code{loader_abi::mod_loader_error::internal};
    std::string message{};
};

template <typename T> using loader_result = std::expected<T, failure>;

template <typename... Args>
[[nodiscard]] auto fail(loader_abi::mod_loader_error code,
                        fmt::format_string<Args...> format, Args &&...args)
    -> std::unexpected<failure> {
    return std::unexpected{
        failure{code, fmt::format(format, std::forward<Args>(args)...)}};
}

/// Publish `failure` on the loader diagnostic channel and return its code as a
/// contract status.
[[nodiscard]] auto report(const failure &problem) -> refrain_status {
    refrain::mod::set_loader_diagnostic(problem.message);
    return static_cast<refrain_status>(problem.code);
}

[[nodiscard]] auto view(const refrain_str &text) noexcept -> std::string_view {
    return contracts::as_string_view(text);
}

/// A package member path is '/'-separated and may not escape the package root
/// through an empty, `.` or `..` segment, an absolute path or a backslash. The
/// manifest parser enforces the same rule before the loader is dispatched; the
/// loader repeats it because a provider can be called directly.
[[nodiscard]] auto is_safe_member_path(std::string_view path) noexcept -> bool {
    if (path.empty() || path.size() > 1024 || path.front() == '/' ||
        path.back() == '/' || path.find('\\') != std::string_view::npos) {
        return false;
    }
    if (path.size() >= 2 && path[1] == ':') {
        return false;
    }
    std::size_t start{0};
    while (start <= path.size()) {
        const auto end{path.find('/', start)};
        const auto segment{path.substr(start, end == std::string_view::npos
                                                  ? std::string_view::npos
                                                  : end - start)};
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        for (const char character : segment) {
            const auto byte{static_cast<unsigned char>(character)};
            if (byte < 0x20 || byte == 0x7F) {
                return false;
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return true;
}

#if REFRAIN_WITH_WASM

/// A receipt file is `<package_root>/<version>.json`; the version is a SemVer
/// string from the manifest, but the loader still refuses separators so a
/// hand-built contract cannot point the read at another file.
[[nodiscard]] auto is_safe_version_component(std::string_view version) noexcept
    -> bool {
    if (version.empty() || version == "." || version == "..") {
        return false;
    }
    return version.find('/') == std::string_view::npos &&
           version.find('\\') == std::string_view::npos &&
           version.find('\0') == std::string_view::npos;
}

/// Map a package/receipt error onto the loader contract error that best tells
/// the operator whether the package is damaged or missing.
[[nodiscard]] auto package_error_code(error_code code) noexcept
    -> loader_abi::mod_loader_error {
    switch (code) {
    case error_code::not_found:
        return loader_abi::mod_loader_error::not_found;
    case error_code::already_exists:
    case error_code::invalid_argument:
    case error_code::parse_error:
        return loader_abi::mod_loader_error::invalid_argument;
    case error_code::permission_denied:
        return loader_abi::mod_loader_error::permission_denied;
    case error_code::version_mismatch:
        return loader_abi::mod_loader_error::version_mismatch;
    case error_code::unsupported:
        return loader_abi::mod_loader_error::unsupported;
    case error_code::success:
    case error_code::unknown:
    case error_code::io_error:
    case error_code::not_initialized:
    case error_code::timeout:
    case error_code::cancelled:
    case error_code::out_of_memory:
    case error_code::mod_error:
    case error_code::stale:
        return loader_abi::mod_loader_error::io_error;
    }
    return loader_abi::mod_loader_error::internal;
}

#endif // REFRAIN_WITH_WASM

/// Validate the manifest entry fields the wasm loader owns. Returns the first
/// refusal, naming the field and the offending value. Host runtime API and
/// capability requirements (`host_api`, `host_capabilities`) are gated by the
/// host before the loader is dispatched; the manifest subset the loader
/// contract carries has no capability list.
[[nodiscard]] auto check_entry(const contract_entry &entry)
    -> std::optional<failure> {
    const auto mod_id{view(entry.mod_id)};
    if (entry.struct_size <
        REFRAIN_SERVICES_MOD_LOADER_MOD_ENTRY_MANIFEST_SIZE) {
        return failure{
            loader_abi::mod_loader_error::invalid_argument,
            fmt::format("mod '{}': entry struct_size {} is smaller "
                        "than the contract's {}",
                        mod_id, entry.struct_size,
                        REFRAIN_SERVICES_MOD_LOADER_MOD_ENTRY_MANIFEST_SIZE)};
    }
    const auto loader{view(entry.loader)};
    if (loader != "wasm") {
        return failure{
            loader_abi::mod_loader_error::version_mismatch,
            fmt::format("loader provider for instance 'wasm' was asked to "
                        "handle loader '{}'",
                        loader)};
    }
    const auto mod_name{mod_id.empty() ? std::string{"?"}
                                       : std::string{mod_id}};
    if (!view(entry.native_path).empty()) {
        return failure{
            loader_abi::mod_loader_error::invalid_argument,
            fmt::format("mod '{}': [entry].native is not part of the wasm "
                        "entry; the native loader owns shared libraries",
                        mod_name)};
    }
    if (!view(entry.symbol).empty()) {
        return failure{
            loader_abi::mod_loader_error::invalid_argument,
            fmt::format("mod '{}': [entry].symbol is native-only", mod_name)};
    }
    const auto member{view(entry.wasm_path)};
    if (member.empty()) {
        return failure{
            loader_abi::mod_loader_error::invalid_argument,
            fmt::format("mod '{}': the wasm loader requires [entry].wasm with "
                        "the module member path",
                        mod_name)};
    }
    if (!is_safe_member_path(member)) {
        return failure{loader_abi::mod_loader_error::invalid_argument,
                       fmt::format("mod '{}': [entry].wasm '{}' is not a safe "
                                   "package-relative path",
                                   mod_name, member)};
    }
    if (entry.abi != guest_abi_major) {
        return failure{
            loader_abi::mod_loader_error::version_mismatch,
            fmt::format("mod '{}': wasm entry '{}' was built for guest ABI {}, "
                        "this loader implements wasm guest ABI {}",
                        mod_name, member, entry.abi, guest_abi_major)};
    }
    return std::nullopt;
}

#if REFRAIN_WITH_WASM

struct module_source {
    std::vector<std::byte> bytes{};
    std::string member{};
};

/// Read the module member out of the installed package named by the manifest
/// entry: the host passes the package root of an installed `.rfmod`, so the
/// receipt next to it is the source of truth and every member read is checked
/// against the receipt's content-store digest. A package whose bytes are not
/// the installed bytes fails here, before compilation.
[[nodiscard]] auto read_module(const contract_entry &entry,
                               const loader_budgets &budgets)
    -> loader_result<module_source> {
    const auto mod_name{view(entry.mod_id).empty()
                            ? std::string{"?"}
                            : std::string{view(entry.mod_id)}};
    const auto member{std::string{view(entry.wasm_path)}};
    const auto version{view(entry.version)};
    if (!is_safe_version_component(version)) {
        return fail(loader_abi::mod_loader_error::invalid_argument,
                    "mod '{}': manifest version '{}' is not a safe receipt "
                    "file name",
                    mod_name, version);
    }
    const auto package_root{view(entry.package_root)};
    if (package_root.empty()) {
        return fail(loader_abi::mod_loader_error::not_found,
                    "mod '{}': wasm entry '{}' has no package root; a wasm mod "
                    "is loaded from an installed package",
                    mod_name, member);
    }
    const auto receipt{std::filesystem::path{std::string{package_root}} /
                       fmt::format("{}.json", version)};
    std::error_code stat_error;
    if (!std::filesystem::is_regular_file(receipt, stat_error)) {
        return fail(loader_abi::mod_loader_error::not_found,
                    "mod '{}': wasm entry '{}' has no install receipt at '{}'; "
                    "a wasm mod is loaded from an installed package",
                    mod_name, member, receipt.string());
    }
    auto source{refrain::mod::make_receipt_package_source(receipt.string())};
    if (!source) {
        return fail(package_error_code(source.error().code),
                    "mod '{}': cannot open install receipt '{}': {}", mod_name,
                    receipt.string(), source.error().message);
    }
    auto bytes{(*source)->read(member)};
    if (!bytes) {
        return fail(package_error_code(bytes.error().code),
                    "mod '{}': wasm entry '{}' is not readable from the "
                    "installed package: {}",
                    mod_name, member, bytes.error().message);
    }
    if (bytes->empty()) {
        return fail(loader_abi::mod_loader_error::invalid_argument,
                    "mod '{}': wasm entry '{}' is empty", mod_name, member);
    }
    if (bytes->size() > budgets.max_module_bytes) {
        return fail(
            loader_abi::mod_loader_error::sandbox_violation,
            "mod '{}': wasm entry '{}' is {} bytes, over the {}-byte module "
            "budget",
            mod_name, member, bytes->size(), budgets.max_module_bytes);
    }
    return module_source{.bytes = std::move(*bytes),
                         .member = std::move(member)};
}

#endif // REFRAIN_WITH_WASM

} // namespace

namespace detail {

/// One loaded guest. The runtime owns the engine, store, compiled module and
/// instantiated guest: destroying the record releases every byte the guest
/// allocated.
struct instance_record {
    std::string mod_id{};
    std::uint64_t module_bytes{0};
    std::uint64_t memory_budget{0};
    std::unique_ptr<runtime> guest{};
    // Fuel left when the load path finished; unload subtracts exactly this
    // value from the provider accounting, independent of what the guest's
    // own on_unload hook consumes.
    std::uint64_t fuel_remaining{0};
    bool has_on_unload{false};
};

/// Provider state: budgets plus every guest the provider owns. The host keeps a
/// pointer to this object for the process lifetime when the provider is the
/// default one, so it is never moved.
struct loader_state {
    loader_budgets budgets{};
    std::map<std::uint64_t, instance_record> instances{};
    std::uint64_t next_handle{1};
    loader_stats stats{};
};

} // namespace detail

namespace {

using detail::instance_record;
using detail::loader_state;

[[nodiscard]] auto unload_reason_name(contract_reason reason) noexcept
    -> std::string_view {
    switch (reason) {
    case REFRAIN_SERVICES_MOD_LOADER_MOD_UNLOAD_REASON_SHUTDOWN:
        return "shutdown";
    case REFRAIN_SERVICES_MOD_LOADER_MOD_UNLOAD_REASON_EXPLICIT_REQUEST:
        return "explicit request";
    case REFRAIN_SERVICES_MOD_LOADER_MOD_UNLOAD_REASON_RELOAD:
        return "reload";
    case REFRAIN_SERVICES_MOD_LOADER_MOD_UNLOAD_REASON_UNINSTALL:
        return "uninstall";
    }
    return "unknown";
}

#if REFRAIN_WITH_WASM

/// Classify a guest trap: budget exhaustion and memory-limit failures are
/// sandbox refusals, everything else during the load path is a package defect.
[[nodiscard]] auto guest_failure(const runtime &guest,
                                 const contract_entry &entry,
                                 std::string_view phase, const error &problem,
                                 const loader_budgets &budgets) -> failure {
    const auto mod_name{view(entry.mod_id).empty()
                            ? std::string{"?"}
                            : std::string{view(entry.mod_id)}};
    const auto member{std::string{view(entry.wasm_path)}};
    if (guest.fuel_remaining() == 0) {
        return failure{loader_abi::mod_loader_error::sandbox_violation,
                       fmt::format("mod '{}': wasm entry '{}' exhausted its "
                                   "{}-instruction budget during {}",
                                   mod_name, member, budgets.load_fuel, phase)};
    }
    const auto text{problem.message};
    // No host imports exist in the guest ABI of this slice, so a module that
    // declares one asked the sandbox for an interface it does not expose.
    if (text.find("unknown import") != std::string::npos ||
        text.find("incompatible import") != std::string::npos) {
        return failure{
            loader_abi::mod_loader_error::sandbox_violation,
            fmt::format("mod '{}': wasm entry '{}' requested a host import "
                        "during {}; the guest sandbox exposes no host imports: "
                        "{}",
                        mod_name, member, phase, text)};
    }
    if (text.find("memory") != std::string::npos &&
        (text.find("limit") != std::string::npos ||
         text.find("maximum") != std::string::npos ||
         text.find("minimum") != std::string::npos)) {
        return failure{
            loader_abi::mod_loader_error::sandbox_violation,
            fmt::format("mod '{}': wasm entry '{}' exceeds the {}-byte memory "
                        "budget during {}: {}",
                        mod_name, member, budgets.max_memory_bytes, phase,
                        text)};
    }
    return failure{loader_abi::mod_loader_error::invalid_argument,
                   fmt::format("mod '{}': wasm entry '{}' failed during {}: {}",
                               mod_name, member, phase, text)};
}

#endif // REFRAIN_WITH_WASM

[[nodiscard]] auto load_guest(loader_state &state, const contract_entry &entry)
    -> loader_result<std::uint64_t> {
    if (auto checked{check_entry(entry)}) {
        return std::unexpected{*checked};
    }
    if (entry.activate_code == 0) {
        // The plan kept the package data-only: nothing to execute and no
        // instance to release later.
        return std::uint64_t{0};
    }
#if REFRAIN_WITH_WASM
    const auto mod_name{view(entry.mod_id).empty()
                            ? std::string{"?"}
                            : std::string{view(entry.mod_id)}};
    const auto clock{[] { return std::chrono::steady_clock::now(); }};
    const auto started{clock()};
    const auto deadline{
        started + std::chrono::milliseconds{state.budgets.load_timeout_ms}};
    const auto check_deadline{
        [&](std::string_view phase) -> std::optional<failure> {
            if (clock() <= deadline) {
                return std::nullopt;
            }
            return failure{
                loader_abi::mod_loader_error::sandbox_violation,
                fmt::format("mod '{}': wasm load exceeded its {}ms deadline "
                            "during {}",
                            mod_name, state.budgets.load_timeout_ms, phase)};
        }};

    auto module{read_module(entry, state.budgets)};
    if (!module) {
        return std::unexpected{module.error()};
    }
    if (auto expired{check_deadline("module read")}) {
        return std::unexpected{*expired};
    }

    auto guest{runtime::create(engine_config{
        .consume_fuel = true,
        .fuel = state.budgets.load_fuel,
        .max_memory_bytes = state.budgets.max_memory_bytes,
    })};
    if (!guest) {
        return fail(loader_abi::mod_loader_error::internal,
                    "mod '{}': cannot create the wasm guest runtime: {}",
                    mod_name, guest.error().message);
    }
    auto compiled{guest->compile_bytes(module->bytes)};
    if (!compiled) {
        return fail(loader_abi::mod_loader_error::invalid_argument,
                    "mod '{}': wasm entry '{}' is not a valid module: {}",
                    mod_name, module->member, compiled.error().message);
    }
    if (auto expired{check_deadline("module compilation")}) {
        return std::unexpected{*expired};
    }
    if (auto instantiated{guest->instantiate(*compiled)}; !instantiated) {
        return std::unexpected{guest_failure(*guest, entry, "instantiation",
                                             instantiated.error(),
                                             state.budgets)};
    }
    if (!guest->has_export("on_load")) {
        return fail(loader_abi::mod_loader_error::invalid_argument,
                    "mod '{}': wasm entry '{}' does not export "
                    "'on_load: () -> i32'",
                    mod_name, module->member);
    }
    auto loaded{guest->call_instance_i32("on_load", {})};
    if (!loaded) {
        return std::unexpected{guest_failure(*guest, entry, "on_load",
                                             loaded.error(), state.budgets)};
    }
    if (*loaded != 0) {
        return fail(loader_abi::mod_loader_error::invalid_argument,
                    "mod '{}': guest on_load reported failure code {}",
                    mod_name, *loaded);
    }
    if (auto expired{check_deadline("guest on_load")}) {
        return std::unexpected{*expired};
    }

    // TODO: guest execution after load has no dispatch path yet. When the
    // script host API lands, reset the store fuel per call and enforce a
    // wall-clock deadline through wasmtime epoch interruption instead of the
    // phase-boundary check above.
    const auto remaining_fuel{guest->fuel_remaining()};
    const auto handle{state.next_handle++};
    instance_record record{
        .mod_id = mod_name,
        .module_bytes = module->bytes.size(),
        .memory_budget = state.budgets.max_memory_bytes,
        .guest = std::make_unique<runtime>(std::move(*guest)),
        .fuel_remaining = remaining_fuel,
        .has_on_unload = false,
    };
    record.has_on_unload = record.guest->has_export("on_unload");
    state.stats.live_instances += 1;
    state.stats.live_module_bytes += record.module_bytes;
    state.stats.live_memory_bytes += record.memory_budget;
    state.stats.live_fuel += remaining_fuel;
    state.stats.total_loaded += 1;
    state.instances.emplace(handle, std::move(record));
    log_loader.info("loaded wasm mod {} guest instance {}, module {} bytes, "
                    "{} fuel left",
                    mod_name, handle, module->bytes.size(), remaining_fuel);
    return handle;
#else
    static_cast<void>(state);
    return fail(loader_abi::mod_loader_error::unsupported,
                "mod '{}': the wasm loader is not available in this build "
                "(REFRAIN_WITH_WASM=0), rebuild with a WebAssembly runtime to "
                "load script mods",
                view(entry.mod_id).empty() ? std::string{"?"}
                                           : std::string{view(entry.mod_id)});
#endif
}

[[nodiscard]] auto unload_guest(loader_state &state, std::uint64_t handle,
                                contract_reason reason) -> loader_result<void> {
    const auto found{state.instances.find(handle)};
    if (found == state.instances.end()) {
        return fail(loader_abi::mod_loader_error::not_found,
                    "wasm loader owns no instance {}", handle);
    }
    auto &record{found->second};
    // A clean loader releases the guest even when the guest's own hook fails:
    // the hook is guest code, the store is host-owned state. Report the hook
    // problem in the log and release the budget either way.
    if (record.has_on_unload) {
        auto called{record.guest->call_instance_i32("on_unload", {})};
        if (!called) {
            log_loader.warn("guest {} on_unload failed during {}: {}; "
                            "releasing the instance anyway",
                            record.mod_id, unload_reason_name(reason),
                            called.error().message);
        } else if (*called != 0) {
            log_loader.warn("guest {} on_unload reported failure code {} "
                            "during {}; releasing the instance anyway",
                            record.mod_id, *called, unload_reason_name(reason));
        }
    }
    state.stats.live_instances -= 1;
    state.stats.live_module_bytes -= record.module_bytes;
    state.stats.live_memory_bytes -= record.memory_budget;
    state.stats.live_fuel -= record.fuel_remaining;
    state.stats.total_unloaded += 1;
    log_loader.info("unloaded wasm mod {} guest instance {} ({})",
                    record.mod_id, handle, unload_reason_name(reason));
    state.instances.erase(found);
    return loader_result<void>{};
}

// ---------------------------------------------------------------------------
// ABI bridge
// ---------------------------------------------------------------------------

void bridge_capabilities(void *self, contract_capabilities *out) {
    static_cast<void>(self);
    if (out == nullptr) {
        return;
    }
    *out = contract_capabilities{};
    out->struct_size = REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_CAPABILITIES_SIZE;
    out->loader_id = contracts::to_borrowed(std::string_view{"wasm"});
    out->abi_major = REFRAIN_SERVICES_MOD_LOADER_ABI_MAJOR;
    out->unload = REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_UNLOAD_CLEAN;
    out->sandboxed = 1;
#if REFRAIN_WITH_WASM
    out->supports_hot_install = 1;
#else
    // A build without the runtime can never build a replacement generation.
    out->supports_hot_install = 0;
#endif
}

auto bridge_validate(void *self, const contract_entry *entry)
    -> refrain_status {
    if (self == nullptr || entry == nullptr) {
        return report(failure{loader_abi::mod_loader_error::invalid_argument,
                              "wasm loader validate called without a provider "
                              "or an entry"});
    }
    if (auto checked{check_entry(*entry)}) {
        return report(*checked);
    }
    if (entry->activate_code == 0) {
        return REFRAIN_STATUS_OK;
    }
#if REFRAIN_WITH_WASM
    const auto &state{*static_cast<loader_state *>(self)};
    auto module{read_module(*entry, state.budgets)};
    if (!module) {
        return report(module.error());
    }
    return REFRAIN_STATUS_OK;
#else
    static_cast<void>(self);
    const auto mod_id{view(entry->mod_id)};
    return report(failure{
        loader_abi::mod_loader_error::unsupported,
        fmt::format("mod '{}': the wasm loader is not available in this build "
                    "(REFRAIN_WITH_WASM=0), rebuild with a WebAssembly "
                    "runtime to load script mods",
                    mod_id.empty() ? std::string{"?"} : std::string{mod_id})});
#endif
}

auto bridge_load(void *self, const contract_request *request,
                 contract_handle *out) -> refrain_status {
    if (self == nullptr || request == nullptr || out == nullptr) {
        return report(failure{loader_abi::mod_loader_error::invalid_argument,
                              "wasm loader load called without a provider, a "
                              "request or a result slot"});
    }
    *out = REFRAIN_SERVICES_MOD_LOADER_HANDLE_MOD_INSTANCE_INVALID;
    auto &state{*static_cast<loader_state *>(self)};
    auto loaded{load_guest(state, request->manifest)};
    if (!loaded) {
        return report(loaded.error());
    }
    *out = static_cast<contract_handle>(*loaded);
    return REFRAIN_STATUS_OK;
}

auto bridge_unload(void *self, contract_handle instance, contract_reason reason)
    -> refrain_status {
    if (self == nullptr ||
        instance == REFRAIN_SERVICES_MOD_LOADER_HANDLE_MOD_INSTANCE_INVALID) {
        return report(failure{loader_abi::mod_loader_error::invalid_argument,
                              "wasm loader unload called without a provider or "
                              "an instance handle"});
    }
    auto &state{*static_cast<loader_state *>(self)};
    auto unloaded{
        unload_guest(state, static_cast<std::uint64_t>(instance), reason)};
    if (!unloaded) {
        return report(unloaded.error());
    }
    return REFRAIN_STATUS_OK;
}

auto bridge_reload(void *self, contract_handle instance,
                   const contract_request *request, contract_handle *out)
    -> refrain_status {
    if (self == nullptr || request == nullptr || out == nullptr ||
        instance == REFRAIN_SERVICES_MOD_LOADER_HANDLE_MOD_INSTANCE_INVALID) {
        return report(
            failure{loader_abi::mod_loader_error::invalid_argument,
                    "wasm loader reload called without a provider, an "
                    "instance, a request or a result slot"});
    }
    *out = REFRAIN_SERVICES_MOD_LOADER_HANDLE_MOD_INSTANCE_INVALID;
    auto &state{*static_cast<loader_state *>(self)};
    const auto handle{static_cast<std::uint64_t>(instance)};
    if (state.instances.find(handle) == state.instances.end()) {
        return report(
            failure{loader_abi::mod_loader_error::not_found,
                    fmt::format("wasm loader owns no instance {}", handle)});
    }
    if (request->manifest.activate_code == 0) {
        // Nothing to rebuild: a data-only package keeps the same (absent)
        // instance handle.
        *out = instance;
        return REFRAIN_STATUS_OK;
    }
    // Build the replacement next to the running guest; the host unloads the
    // old instance after the composition epoch commits.
    auto replacement{load_guest(state, request->manifest)};
    if (!replacement) {
        return report(replacement.error());
    }
    *out = static_cast<contract_handle>(*replacement);
    return REFRAIN_STATUS_OK;
}

[[nodiscard]] auto loader_vtable() noexcept
    -> const refrain_services_mod_loader_vtable & {
    static const refrain_services_mod_loader_vtable table{
        .struct_size = REFRAIN_SERVICES_MOD_LOADER_VTABLE_SIZE,
        .abi_major = REFRAIN_SERVICES_MOD_LOADER_ABI_MAJOR,
        .abi_minor = REFRAIN_SERVICES_MOD_LOADER_ABI_MINOR,
        .abi_patch = REFRAIN_SERVICES_MOD_LOADER_ABI_PATCH,
        .capabilities = &bridge_capabilities,
        .validate = &bridge_validate,
        .load = &bridge_load,
        .unload = &bridge_unload,
        .reload = &bridge_reload,
    };
    return table;
}

} // namespace

loader::loader(loader_budgets budgets)
    : state_{std::make_unique<detail::loader_state>()} {
    state_->budgets = budgets;
}

loader::loader(loader &&other) noexcept = default;

auto loader::operator=(loader &&other) noexcept -> loader & = default;

loader::~loader() = default;

auto loader::provider() const noexcept -> mod_loader_provider {
    return mod_loader_provider{
        .vtable = &loader_vtable(),
        .self = state_.get(),
        .abi_major = REFRAIN_SERVICES_MOD_LOADER_ABI_MAJOR,
    };
}

auto loader::stats() const noexcept -> const loader_stats & {
    return state_->stats;
}

auto default_mod_loader_provider() noexcept -> mod_loader_provider {
    // The server registers this provider once; its budget accounting lives for
    // the process lifetime and is released by process teardown.
    static loader instance{};
    return instance.provider();
}

} // namespace refrain::script::wasm
