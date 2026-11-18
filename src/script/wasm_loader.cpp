// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/script/loaders.hpp>

#include <refrain/contracts/services/mod_loader.hpp>
#include <refrain/contracts/services/script_host.hpp>
#include <refrain/core/log.hpp>
#include <refrain/mod/entry.h>
#include <refrain/mod/host.hpp>
#include <refrain/mod/loader.hpp>
#include <refrain/mod/package.hpp>
#include <refrain/script/wasm.hpp>

#include <algorithm>
#include <array>
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
namespace script_abi = refrain::services::script_host;
namespace contracts = refrain::contracts;

using contract_handle = refrain_services_mod_loader_handle_mod_instance;
using contract_reason = refrain_services_mod_loader_mod_unload_reason;
using contract_capabilities =
    refrain_services_mod_loader_mod_loader_capabilities;
using contract_entry = refrain_services_mod_loader_mod_entry_manifest;
using contract_request = refrain_services_mod_loader_mod_load_request;
using contract_script_request = script_abi::script_call_request;
using contract_script_result = script_abi::script_call_result;
using contract_script_error = script_abi::script_error;

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

/// One failed guest call: the script contract error plus the detail the host
/// appends through the loader diagnostic channel.
struct script_failure {
    contract_script_error code{contract_script_error::internal};
    std::string message{};
};

template <typename T> using script_result = std::expected<T, script_failure>;

/// Longest accepted callback export name; the same bound the script host
/// contract documents.
inline constexpr std::size_t max_callback_name_bytes{64};

/// A guest callback name is a bounded identifier: letters, digits, '_', '.'
/// and '-', starting with a letter, digit or '_'. The bound keeps arbitrary
/// export names (and control characters in diagnostics) out of the call path.
[[nodiscard]] auto is_valid_callback_name(std::string_view name) noexcept
    -> bool {
    if (name.empty() || name.size() > max_callback_name_bytes) {
        return false;
    }
    const auto allowed = [](char value) {
        return (value >= 'a' && value <= 'z') ||
               (value >= 'A' && value <= 'Z') ||
               (value >= '0' && value <= '9') || value == '_' || value == '.' ||
               value == '-';
    };
    if (!allowed(name.front()) || name.front() == '.' || name.front() == '-') {
        return false;
    }
    return std::ranges::all_of(name, allowed);
}

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

/// Print the log lines a guest collected through its `log` host import. The
/// guest never writes the host log directly: the loader tags the line with the
/// mod id and the host logger applies its own level filter.
void drain_guest_logs(runtime &guest, std::string_view mod_id) {
    static constexpr std::array<refrain::log::level, 5> levels{
        refrain::log::level::trace, refrain::log::level::debug,
        refrain::log::level::info, refrain::log::level::warn,
        refrain::log::level::error};
    for (auto &line : guest.take_guest_logs()) {
        const auto index{std::min<std::size_t>(line.level, levels.size() - 1)};
        log_loader.log(levels[index], "guest {} logged: {}", mod_id,
                       line.message);
    }
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
    /// A trapped or over-budget call quarantines the instance: it keeps its
    /// store and registration but runs no further callbacks until unloaded.
    bool quarantined{false};
};

/// Provider state: budgets, the read-only host state guests see, plus every
/// guest the provider owns. The host keeps a pointer to this object for the
/// process lifetime when the provider is the default one, so it is never
/// moved.
struct loader_state {
    loader_budgets budgets{};
    host::grant_set grants{};
    host::environment environment{};
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
    // A refused host import is a sandbox-policy refusal: the runtime names the
    // import and the active grant set in the message before instantiation.
    if (problem.code == error_code::permission_denied) {
        return failure{loader_abi::mod_loader_error::sandbox_violation,
                       fmt::format("mod '{}': wasm entry '{}' failed during "
                                   "{}: {}",
                                   mod_name, member, phase, problem.message)};
    }
    if (guest.fuel_remaining() == 0) {
        return failure{loader_abi::mod_loader_error::sandbox_violation,
                       fmt::format("mod '{}': wasm entry '{}' exhausted its "
                                   "{}-instruction budget during {}",
                                   mod_name, member, budgets.load_fuel, phase)};
    }
    const auto text{problem.message};
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

    auto guest{runtime::create(
        engine_config{
            .consume_fuel = true,
            .fuel = state.budgets.load_fuel,
            .max_memory_bytes = state.budgets.max_memory_bytes,
            .epoch_interruption = true,
        },
        host_imports{
            .grants = state.grants,
            .environment = &state.environment,
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
    drain_guest_logs(*guest, mod_name);
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
        drain_guest_logs(*record.guest, record.mod_id);
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
    if (record.quarantined) {
        state.stats.quarantined_instances -= 1;
    }
    log_loader.info("unloaded wasm mod {} guest instance {} ({})",
                    record.mod_id, handle, unload_reason_name(reason));
    state.instances.erase(found);
    return loader_result<void>{};
}

// ---------------------------------------------------------------------------
// Call dispatch
// ---------------------------------------------------------------------------

/// Quarantine `record` after a failed call. The store stays allocated so the
/// declared clean unload can release it in order, but no further guest code
/// runs on the instance.
void quarantine_instance(loader_state &state, instance_record &record,
                         std::string_view reason) {
    if (record.quarantined) {
        return;
    }
    record.quarantined = true;
    state.stats.quarantined_instances += 1;
    log_loader.warn("quarantined wasm mod {} after {}; it runs no further "
                    "callbacks until unloaded",
                    record.mod_id, reason);
}

/// Invoke one exported guest callback under the per-call budgets. A caller
/// mistake (unknown callback, wrong ABI, oversized arguments) fails without
/// quarantining; a guest fault (budget, deadline, trap) quarantines.
[[nodiscard]] auto call_guest(loader_state &state, std::uint64_t handle,
                              std::string_view callback,
                              std::span<const std::uint8_t> arguments)
    -> script_result<std::int32_t> {
#if REFRAIN_WITH_WASM
    const auto found{state.instances.find(handle)};
    if (found == state.instances.end()) {
        return std::unexpected{script_failure{
            contract_script_error::not_found,
            fmt::format("wasm loader owns no instance {}", handle)}};
    }
    auto &record{found->second};
    if (record.quarantined) {
        return std::unexpected{script_failure{
            contract_script_error::quarantined,
            fmt::format("wasm mod {} instance {} is quarantined after an "
                        "earlier failed call; unload it to release the guest",
                        record.mod_id, handle)}};
    }
    if (!is_valid_callback_name(callback)) {
        return std::unexpected{script_failure{
            contract_script_error::invalid_argument,
            fmt::format("callback name '{}' is not a valid export name: 1 to "
                        "{} characters of [A-Za-z0-9_.-] starting with a "
                        "letter, digit or underscore",
                        callback, max_callback_name_bytes)}};
    }
    if (arguments.size() > host::max_call_argument_bytes) {
        return std::unexpected{script_failure{
            contract_script_error::invalid_argument,
            fmt::format("callback '{}' arguments are {} bytes, over the "
                        "{}-byte call argument budget",
                        callback, arguments.size(),
                        host::max_call_argument_bytes)}};
    }

    state.stats.total_calls += 1;
    if (!arguments.empty()) {
        const auto bytes{std::span<const std::byte>{
            reinterpret_cast<const std::byte *>(arguments.data()),
            arguments.size()}};
        auto written{record.guest->write_guest_memory(
            host::call_argument_address, bytes)};
        if (!written) {
            state.stats.failed_calls += 1;
            return std::unexpected{script_failure{
                contract_script_error::invalid_argument,
                fmt::format("wasm mod {} cannot receive callback '{}' "
                            "arguments: {}",
                            record.mod_id, callback, written.error().message)}};
        }
    }

    constexpr std::array<std::int32_t, host::callback_parameter_count>
        parameters{static_cast<std::int32_t>(host::call_argument_address), 0};
    auto call_parameters{parameters};
    call_parameters[1] = static_cast<std::int32_t>(arguments.size());
    auto called{record.guest->call_instance_i32(
        callback, call_parameters,
        call_options{.fuel = state.budgets.call_fuel,
                     .timeout_ms = state.budgets.call_timeout_ms})};
    drain_guest_logs(*record.guest, record.mod_id);
    if (called) {
        return *called;
    }

    state.stats.failed_calls += 1;
    const auto failed_at{called.error().code};
    if (failed_at == error_code::not_found) {
        return std::unexpected{script_failure{
            contract_script_error::not_found,
            fmt::format("wasm mod {} does not export callback '{}'",
                        record.mod_id, callback)}};
    }
    if (failed_at == error_code::invalid_argument) {
        return std::unexpected{script_failure{
            contract_script_error::invalid_argument,
            fmt::format("wasm mod {} callback '{}' does not implement the "
                        "call ABI: {}",
                        record.mod_id, callback, called.error().message)}};
    }
    quarantine_instance(state, record, "a call trapped");
    if (failed_at == error_code::timeout) {
        return std::unexpected{script_failure{
            contract_script_error::deadline_exceeded,
            fmt::format(
                "wasm mod {} callback '{}' exceeded its {}ms wall-clock "
                "deadline",
                record.mod_id, callback, state.budgets.call_timeout_ms)}};
    }
    if (record.guest->fuel_remaining() == 0) {
        return std::unexpected{script_failure{
            contract_script_error::budget_exceeded,
            fmt::format("wasm mod {} callback '{}' exhausted its per-call "
                        "instruction budget of {} instructions",
                        record.mod_id, callback, state.budgets.call_fuel)}};
    }
    return std::unexpected{script_failure{
        contract_script_error::guest_trapped,
        fmt::format("wasm mod {} callback '{}' trapped: {}", record.mod_id,
                    callback, called.error().message)}};
#else
    static_cast<void>(state);
    static_cast<void>(handle);
    static_cast<void>(callback);
    static_cast<void>(arguments);
    return std::unexpected{script_failure{
        contract_script_error::unsupported,
        "the wasm loader is not available in this build "
        "(REFRAIN_WITH_WASM=0), rebuild with a WebAssembly runtime to "
        "dispatch script callbacks"}};
#endif
}

// ---------------------------------------------------------------------------
// Script host ABI
// ---------------------------------------------------------------------------

auto bridge_script_call(void *self, contract_handle instance,
                        const contract_script_request *request,
                        contract_script_result *out) -> refrain_status {
    if (self == nullptr || request == nullptr || out == nullptr) {
        refrain::mod::set_loader_diagnostic(
            "wasm script host call received no provider, request or result "
            "slot");
        return static_cast<refrain_status>(
            contract_script_error::invalid_argument);
    }
    if (instance == REFRAIN_SERVICES_MOD_LOADER_HANDLE_MOD_INSTANCE_INVALID) {
        refrain::mod::set_loader_diagnostic(
            "wasm script host call received an invalid instance handle");
        return static_cast<refrain_status>(
            contract_script_error::invalid_argument);
    }
    if (request->struct_size <
        REFRAIN_SERVICES_SCRIPT_HOST_SCRIPT_CALL_REQUEST_SIZE) {
        refrain::mod::set_loader_diagnostic(fmt::format(
            "wasm script host call request struct_size {} is smaller than the "
            "contract's {}",
            request->struct_size,
            REFRAIN_SERVICES_SCRIPT_HOST_SCRIPT_CALL_REQUEST_SIZE));
        return static_cast<refrain_status>(
            contract_script_error::invalid_argument);
    }
    auto &state{*static_cast<loader_state *>(self)};
    auto called{call_guest(state, static_cast<std::uint64_t>(instance),
                           view(request->callback),
                           contracts::as_bytes(request->arguments))};
    if (!called) {
        refrain::mod::set_loader_diagnostic(called.error().message);
        return static_cast<refrain_status>(called.error().code);
    }
    out->value = *called;
    return REFRAIN_STATUS_OK;
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

/// The one vtable of the sealed `refrain:script.host` instance this loader
/// provides. The script host is the same provider state as the mod loader: its
/// instance handles are the loader's handles.
[[nodiscard]] auto script_host_vtable() noexcept
    -> const refrain_services_script_host_vtable & {
    static const refrain_services_script_host_vtable table{
        .struct_size = REFRAIN_SERVICES_SCRIPT_HOST_VTABLE_SIZE,
        .abi_major = REFRAIN_SERVICES_SCRIPT_HOST_ABI_MAJOR,
        .abi_minor = REFRAIN_SERVICES_SCRIPT_HOST_ABI_MINOR,
        .abi_patch = REFRAIN_SERVICES_SCRIPT_HOST_ABI_PATCH,
        .call = &bridge_script_call,
    };
    return table;
}

/// The provider shared by the mod loader and the script host of the default
/// server composition. Both provider triples must resolve to the same state, or
/// an instance handle returned by the loader would not be dispatchable.
[[nodiscard]] auto default_loader() -> loader & {
    static loader instance{};
    return instance;
}

} // namespace

loader::loader(loader_budgets budgets, loader_host_api host_api)
    : state_{std::make_unique<detail::loader_state>()} {
    state_->budgets = budgets;
    state_->grants = host_api.grants;
    state_->environment = std::move(host_api.environment);
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

auto loader::script_host_provider() const noexcept
    -> refrain::script::script_host_provider {
    return ::refrain::script::script_host_provider{
        .vtable = &script_host_vtable(),
        .self = state_.get(),
        .abi_major = REFRAIN_SERVICES_SCRIPT_HOST_ABI_MAJOR,
    };
}

void loader::set_tick_index(std::int64_t tick_index) noexcept {
    state_->environment.tick_index = tick_index;
}

auto loader::stats() const noexcept -> const loader_stats & {
    return state_->stats;
}

auto default_mod_loader_provider() noexcept -> mod_loader_provider {
    // The server registers this provider once; its budget accounting lives for
    // the process lifetime and is released by process teardown.
    return default_loader().provider();
}

auto default_script_host_provider() noexcept -> script_host_provider {
    return default_loader().script_host_provider();
}

} // namespace refrain::script::wasm

namespace refrain::script {

namespace {

namespace script_host_abi = refrain::services::script_host;
namespace contracts = refrain::contracts;

/// Map a script host failure onto the host's error vocabulary. The loader
/// diagnostic carries the detail the provider published.
[[nodiscard]] auto map_script_failure(script_host_abi::script_error code,
                                      std::string detail) -> error {
    auto mapped{error_code::mod_error};
    switch (code) {
    case script_host_abi::script_error::invalid_argument:
        mapped = error_code::invalid_argument;
        break;
    case script_host_abi::script_error::not_found:
        mapped = error_code::not_found;
        break;
    case script_host_abi::script_error::permission_denied:
    case script_host_abi::script_error::quarantined:
        mapped = error_code::permission_denied;
        break;
    case script_host_abi::script_error::unsupported:
        mapped = error_code::unsupported;
        break;
    case script_host_abi::script_error::budget_exceeded:
        mapped = error_code::permission_denied;
        break;
    case script_host_abi::script_error::deadline_exceeded:
        mapped = error_code::timeout;
        break;
    case script_host_abi::script_error::guest_trapped:
        mapped = error_code::mod_error;
        break;
    case script_host_abi::script_error::internal:
        mapped = error_code::unknown;
        break;
    }
    if (detail.empty()) {
        detail = "refrain:script.host call failed";
    }
    return make_error(mapped, std::move(detail));
}

} // namespace

auto register_script_host(mod::host &host, const script_host_provider &provider)
    -> status {
    if (host.is_sealed_service(script_host_abi::service_id)) {
        return status_ok();
    }
    if (provider.vtable == nullptr || provider.self == nullptr) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "script host registration needs a vtable and a self "
                       "pointer")};
    }
    return host.register_sealed_provider(
        script_host_abi::service_id,
        mod::provider_api{.vtable = provider.vtable,
                          .self = provider.self,
                          .abi_major = provider.abi_major});
}

auto dispatch_guest_call(const mod::host &host, std::uint64_t instance,
                         std::string_view callback,
                         std::span<const std::byte> arguments)
    -> result<guest_call_result> {
    const auto active{
        host.services().active_provider(script_host_abi::service_id)};
    if (!active || active->instance == nullptr) {
        return std::unexpected{make_error(
            error_code::unsupported,
            "no active refrain:script.host provider: the host bootstrap "
            "registers the script host with its script loaders")};
    }
    const auto *api{static_cast<const mod::provider_api *>(active->instance)};
    if (api->vtable == nullptr || api->self == nullptr) {
        return std::unexpected{make_error(
            error_code::unknown,
            "the active refrain:script.host provider has no ABI table")};
    }
    if (api->abi_major != script_host_abi::abi_major) {
        return std::unexpected{make_error(
            error_code::version_mismatch,
            fmt::format("refrain:script.host provider was built for ABI {}, "
                        "this host implements {}",
                        api->abi_major, script_host_abi::abi_major))};
    }
    const auto &table{
        *static_cast<const refrain_services_script_host_vtable *>(api->vtable)};
    if (table.struct_size < REFRAIN_SERVICES_SCRIPT_HOST_VTABLE_SIZE) {
        return std::unexpected{make_error(
            error_code::version_mismatch,
            "refrain:script.host provider table is shorter than the contract "
            "vtable")};
    }
    if (table.call == nullptr) {
        return std::unexpected{make_error(
            error_code::unsupported,
            "the active refrain:script.host provider does not implement call "
            "dispatch")};
    }

    auto request{script_host_abi::make_script_call_request()};
    request.callback = contracts::to_borrowed(callback);
    request.arguments = contracts::to_borrowed(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t *>(arguments.data()),
        arguments.size()});
    refrain::mod::clear_loader_diagnostic();
    const script_host_abi::provider provider{table, api->self};
    const auto called{provider.call(
        refrain::services::mod_loader::mod_instance{instance}, request)};
    if (!called) {
        auto detail{refrain::mod::take_loader_diagnostic()};
        return std::unexpected{
            map_script_failure(called.error(), std::move(detail))};
    }
    return guest_call_result{.value = called->value};
}

} // namespace refrain::script
