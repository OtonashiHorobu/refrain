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
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/loader.hpp>
#include <refrain/mod/package.hpp>
#include <refrain/mod/static_mod.hpp>
#include <refrain/platform/dynamic_library.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <span>
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
    case refrain::host::core::host_error::stale:
        return error_code::stale;
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

} // namespace

auto chain_wrapper_instance(std::size_t position) -> std::string {
    // The plan verification refuses a chain that declares more wrappers than
    // the bound, so a position outside it never reaches a published slot.
    assert(position < max_chain_wrappers);
    return fmt::format("{}{}", chain_wrapper_instance_prefix, position);
}

auto chain_wrapper_position(std::string_view instance) noexcept
    -> std::optional<std::size_t> {
    if (!instance.starts_with(chain_wrapper_instance_prefix)) {
        return std::nullopt;
    }
    auto text{instance.substr(chain_wrapper_instance_prefix.size())};
    // `max_chain_wrappers` has seven decimal digits, so a longer suffix cannot
    // encode a valid position and must not overflow the accumulator below.
    if (text.empty() || text.size() > 7U) {
        return std::nullopt;
    }
    std::size_t position{0};
    for (const auto digit : text) {
        if (digit < '0' || digit > '9') {
            return std::nullopt;
        }
        position = position * 10U + static_cast<std::size_t>(digit - '0');
    }
    if (position >= max_chain_wrappers) {
        return std::nullopt;
    }
    return position;
}

namespace {

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

/// Validate the vtable ABI header claimed by `api.abi_major`. The declared
/// major is mandatory: a provider must say which contract ABI its table was
/// built against, so a mismatched or missing declaration fails before the
/// table is ever dispatched.
///
/// Safety: `provider_api::vtable` is a host-owned static table by contract
/// ("must stay valid for the process lifetime") and every generated vtable
/// starts with this 16-byte header, so reading exactly `sizeof(vtable_header)`
/// bytes is always in bounds. `std::memcpy` avoids alignment and
/// strict-aliasing assumptions about the opaque pointer.
[[nodiscard]] auto check_abi_header(const provider_api &api) -> status {
    if (api.abi_major == 0) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "provider does not declare its contract ABI major; a "
                 "provider table must state which ABI it was built against")};
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
// Custom contract conformance
// ---------------------------------------------------------------------------

/// One SemVer AND group as an interval. Every operator the range parser
/// supports constrains a single interval (`=`, `>`, `>=`, `<`, `<=`, `^`,
/// `~`), so a conjunction is an interval and a compatibility check is interval
/// containment without consulting the generated contract headers.
struct version_interval {
    bool has_lower{false};
    core::semver lower{};
    bool lower_inclusive{false};
    bool has_upper{false};
    core::semver upper{};
    bool upper_inclusive{false};
    bool empty{false};
};

void tighten_lower(version_interval &interval, const core::semver &bound,
                   bool inclusive) {
    if (!interval.has_lower) {
        interval.has_lower = true;
        interval.lower = bound;
        interval.lower_inclusive = inclusive;
        return;
    }
    const auto order{core::compare(bound, interval.lower)};
    if (order == std::strong_ordering::greater) {
        interval.lower = bound;
        interval.lower_inclusive = inclusive;
    } else if (order == std::strong_ordering::equal) {
        interval.lower_inclusive = interval.lower_inclusive && inclusive;
    }
}

void tighten_upper(version_interval &interval, const core::semver &bound,
                   bool inclusive) {
    if (!interval.has_upper) {
        interval.has_upper = true;
        interval.upper = bound;
        interval.upper_inclusive = inclusive;
        return;
    }
    const auto order{core::compare(bound, interval.upper)};
    if (order == std::strong_ordering::less) {
        interval.upper = bound;
        interval.upper_inclusive = inclusive;
    } else if (order == std::strong_ordering::equal) {
        interval.upper_inclusive = interval.upper_inclusive && inclusive;
    }
}

[[nodiscard]] auto
to_interval(const std::vector<core::version_comparator> &group)
    -> version_interval {
    version_interval interval{};
    for (const auto &comparator : group) {
        switch (comparator.kind) {
        case core::range_operator::equal:
            tighten_lower(interval, comparator.bound, true);
            tighten_upper(interval, comparator.bound, true);
            break;
        case core::range_operator::greater:
            tighten_lower(interval, comparator.bound, false);
            break;
        case core::range_operator::greater_equal:
            tighten_lower(interval, comparator.bound, true);
            break;
        case core::range_operator::less:
            tighten_upper(interval, comparator.bound, false);
            break;
        case core::range_operator::less_equal:
            tighten_upper(interval, comparator.bound, true);
            break;
        case core::range_operator::caret: {
            tighten_lower(interval, comparator.bound, true);
            if (core::semver_detail::caret_has_upper(comparator.bound)) {
                tighten_upper(
                    interval,
                    core::semver_detail::caret_upper(comparator.bound), false);
            }
            break;
        }
        case core::range_operator::tilde: {
            tighten_lower(interval, comparator.bound, true);
            if (core::semver_detail::tilde_has_upper(comparator.bound)) {
                tighten_upper(
                    interval,
                    core::semver_detail::tilde_upper(comparator.bound), false);
            }
            break;
        }
        }
    }
    if (interval.has_lower && interval.has_upper) {
        const auto order{core::compare(interval.lower, interval.upper)};
        interval.empty =
            order == std::strong_ordering::greater ||
            (order == std::strong_ordering::equal &&
             !(interval.lower_inclusive && interval.upper_inclusive));
    }
    return interval;
}

/// True when every version of `inner` lies inside `outer`. An unbounded inner
/// side needs an unbounded outer side (otherwise the outer bound excludes
/// versions the inner range still accepts), while an unbounded outer side
/// covers every bound on that side.
[[nodiscard]] auto interval_within(const version_interval &inner,
                                   const version_interval &outer) -> bool {
    if (inner.empty) {
        return true;
    }
    if (outer.empty) {
        return false;
    }
    if (inner.has_lower) {
        if (outer.has_lower) {
            const auto order{core::compare(outer.lower, inner.lower)};
            if (order == std::strong_ordering::greater) {
                return false;
            }
            if (order == std::strong_ordering::equal && inner.lower_inclusive &&
                !outer.lower_inclusive) {
                return false;
            }
        }
    } else if (outer.has_lower) {
        return false;
    }
    if (inner.has_upper) {
        if (outer.has_upper) {
            const auto order{core::compare(outer.upper, inner.upper)};
            if (order == std::strong_ordering::less) {
                return false;
            }
            if (order == std::strong_ordering::equal && inner.upper_inclusive &&
                !outer.upper_inclusive) {
                return false;
            }
        }
    } else if (outer.has_upper) {
        return false;
    }
    return true;
}

/// True when `inner` denotes a subset of `outer`. Used as the conformance
/// check between a consumer's required range and the contract owner's
/// declared compatibility band, so the host never has to read the generated
/// headers of a custom contract.
[[nodiscard]] auto range_within(const core::semver_range &inner,
                                const core::semver_range &outer) -> bool {
    std::vector<version_interval> outer_intervals{};
    outer_intervals.reserve(outer.alternatives.size());
    for (const auto &group : outer.alternatives) {
        outer_intervals.push_back(to_interval(group));
    }
    for (const auto &group : inner.alternatives) {
        const auto candidate{to_interval(group)};
        bool contained{false};
        for (const auto &container : outer_intervals) {
            if (interval_within(candidate, container)) {
                contained = true;
                break;
            }
        }
        if (!contained) {
            return false;
        }
    }
    return true;
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
    case error_code::stale:
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

/// First contract layout of `mod_loader_capabilities` (v1.1): the struct ended
/// after `supports_hot_install`. A provider built against it reports that
/// struct_size and has no `provider` field; the host keeps accepting it so an
/// older host-owned or embedder loader table stays usable.
constexpr std::uint32_t loader_capabilities_v1_1_size =
    offsetof(loader_abi::mod_loader_capabilities, provider);

/// Loader capabilities as the C++ host records them, decoded from the contract
/// struct. The struct size and the unload semantics are validated here so an
/// undersized or future-unknown declaration never reaches the dispatch.
[[nodiscard]] auto
capabilities_from_abi(const loader_abi::mod_loader_capabilities &abi)
    -> result<mod_loader_capabilities> {
    if (abi.struct_size < loader_capabilities_v1_1_size) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 fmt::format("capability struct_size {} is smaller than the "
                             "contract's {}",
                             abi.struct_size, loader_capabilities_v1_1_size))};
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
    std::string provider{};
    if (abi.struct_size >=
        REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_CAPABILITIES_SIZE) {
        provider =
            std::string{refrain::contracts::as_string_view(abi.provider)};
    }
    return mod_loader_capabilities{
        .loader_id =
            std::string{refrain::contracts::as_string_view(abi.loader_id)},
        .abi_major = abi.abi_major,
        .unload = unload,
        .sandboxed = abi.sandboxed != 0,
        .supports_hot_install = abi.supports_hot_install != 0,
        .provider = std::move(provider),
    };
}

/// Cross-check a loader's declared capabilities against the id it is active
/// under, the mod that registered it and the loader ABI this host implements.
/// A host-owned loader reports no provider; a package-provided loader must name
/// the mod that registers the instance.
[[nodiscard]] auto
validate_loader_capabilities(std::string_view expected_id,
                             std::string_view expected_provider,
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
    if (caps.provider != expected_provider) {
        if (expected_provider.empty()) {
            return std::unexpected{fail(
                error_code::version_mismatch,
                fmt::format("mod loader '{}' reports provider mod '{}' but is "
                            "registered by the host bootstrap",
                            expected_id, caps.provider))};
        }
        if (caps.provider.empty()) {
            return std::unexpected{fail(
                error_code::version_mismatch,
                fmt::format("mod loader '{}' is registered by mod '{}' but "
                            "its capability struct does not report a provider; "
                            "the loader contract v1.2 requires a "
                            "package-provided loader to name the mod that "
                            "provides it",
                            expected_id, expected_provider))};
        }
        return std::unexpected{fail(
            error_code::version_mismatch,
            fmt::format("mod loader '{}' reports provider mod '{}' but was "
                        "registered by mod '{}'",
                        expected_id, caps.provider, expected_provider))};
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

/// `unload_reason` to the loader contract's reason enum.
[[nodiscard]] auto to_abi_unload_reason(unload_reason reason) noexcept
    -> loader_abi::mod_unload_reason {
    switch (reason) {
    case unload_reason::shutdown:
        return loader_abi::mod_unload_reason::shutdown;
    case unload_reason::reload:
        return loader_abi::mod_unload_reason::reload;
    case unload_reason::uninstall:
        return loader_abi::mod_unload_reason::uninstall;
    }
    return loader_abi::mod_unload_reason::uninstall;
}

/// Decode the loader contract's reason enum. An unknown value is treated as an
/// administrative removal.
[[nodiscard]] auto
from_abi_unload_reason(loader_abi::mod_unload_reason reason) noexcept
    -> unload_reason {
    switch (reason) {
    case loader_abi::mod_unload_reason::shutdown:
        return unload_reason::shutdown;
    case loader_abi::mod_unload_reason::reload:
        return unload_reason::reload;
    case loader_abi::mod_unload_reason::explicit_request:
    case loader_abi::mod_unload_reason::uninstall:
        return unload_reason::uninstall;
    }
    return unload_reason::uninstall;
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
    // The host bootstrap owns these instances; a package-provided loader
    // reports its provider mod instead.
    out->provider = refrain_str{};
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
    if (self == nullptr ||
        instance == REFRAIN_SERVICES_MOD_LOADER_HANDLE_MOD_INSTANCE_INVALID) {
        return static_cast<refrain_status>(
            loader_error_from(error_code::invalid_argument));
    }
    const auto *state{static_cast<const host::loader_provider_state *>(self)};
    if (state->owner == nullptr) {
        return static_cast<refrain_status>(
            loader_error_from(error_code::unknown));
    }
    auto &owner{*state->owner};
    const auto requested{from_abi_unload_reason(
        static_cast<loader_abi::mod_unload_reason>(reason))};
    if (state->loader_id == builtin_loader_id) {
        // Static code cannot stop executing inside the process. The lifecycle
        // layer gates this earlier through the declared semantics; refuse here
        // rather than pretend the factory-provided objects were released.
        owner.set_loader_dispatch_error(fmt::format(
            "builtin loader cannot unload instance {}: statically linked code "
            "requires a process restart",
            instance));
        return static_cast<refrain_status>(
            loader_abi::mod_loader_error::unsupported);
    }
    auto released{owner.release_loader_instance(instance, requested)};
    if (!released) {
        return fail_with(owner, released.error());
    }
    return REFRAIN_STATUS_OK;
}

auto bridge::reload(void *self,
                    refrain_services_mod_loader_handle_mod_instance instance,
                    const loader_abi::mod_load_request *request,
                    refrain_services_mod_loader_handle_mod_instance *out)
    -> refrain_status {
    if (self == nullptr || request == nullptr || out == nullptr ||
        instance == REFRAIN_SERVICES_MOD_LOADER_HANDLE_MOD_INSTANCE_INVALID) {
        return static_cast<refrain_status>(
            loader_error_from(error_code::invalid_argument));
    }
    *out = REFRAIN_SERVICES_MOD_LOADER_HANDLE_MOD_INSTANCE_INVALID;
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
            "loader provider for instance '{}' was asked to reload loader "
            "'{}'",
            state->loader_id, std::string{loader}));
        return static_cast<refrain_status>(
            loader_abi::mod_loader_error::version_mismatch);
    }
    if (request->manifest.activate_code == 0) {
        // Data-only reload: no code generation exists to rebuild.
        *out = instance;
        return REFRAIN_STATUS_OK;
    }
    if (state->loader_id == builtin_loader_id) {
        owner.set_loader_dispatch_error(fmt::format(
            "builtin loader cannot rebuild instance {}: statically linked code "
            "requires a process restart",
            instance));
        return static_cast<refrain_status>(
            loader_abi::mod_loader_error::unsupported);
    }
    std::uint64_t replacement{0};
    auto rebuilt{owner.native_reload_instance(instance, replacement)};
    if (!rebuilt) {
        return fail_with(owner, rebuilt.error());
    }
    *out = replacement;
    return REFRAIN_STATUS_OK;
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

    // Custom contract descriptors are data: record them before the loader
    // runs so the provider registration below (and any provider code the
    // factory calls into later) sees the declared identity. A conflicting
    // declaration by another loaded mod fails closed here, naming both mods.
    if (auto recorded{record_contract_definitions(*manifest)}; !recorded) {
        return std::unexpected{
            annotate(std::move(recorded.error()), entry.path)};
    }
    current_contract_records_.clear();
    current_contract_records_.reserve(manifest->contracts.size());
    for (const auto &definition : manifest->contracts) {
        current_contract_records_.push_back(contract_record{
            .owner = manifest->common.id, .definition = definition});
    }

    // Context for `register_provider()`: the mod may only claim services its
    // manifest declares and the plan selected it for. External mods receive
    // the same context through the sealed host core table.
    current_mod_provides_ = manifest->provides;
    current_mod_planned_slots_.clear();
    current_mod_planned_instances_.clear();
    current_mod_chain_slots_.clear();
    current_mod_default_slots_.clear();
    if (const auto found{planned.slots.find(manifest->common.id)};
        found != planned.slots.end()) {
        current_mod_planned_slots_ = found->second;
    }
    if (const auto found{planned.instances.find(manifest->common.id)};
        found != planned.instances.end()) {
        current_mod_planned_instances_ = found->second;
    }
    if (const auto found{planned.chain_slots.find(manifest->common.id)};
        found != planned.chain_slots.end()) {
        current_mod_chain_slots_ = found->second;
    }
    if (const auto found{planned.default_slots.find(manifest->common.id)};
        found != planned.default_slots.end()) {
        current_mod_default_slots_ = found->second;
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
    current_mod_registered_slots_.clear();
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
            owner.current_contract_records_.clear();
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
    const auto can_reload_code{current_replaced_handle_.has_value() &&
                               activate_code &&
                               current_loader_id_ == current_replaced_loader_};
    if (can_reload_code) {
        // The diff decided this id is a new generation of an already active
        // mod: ask the loader to build the replacement next to the running
        // instance. The old instance is unloaded after the commit, never
        // before, because the committed registry still hands it out until the
        // epoch switches.
        const auto replaced{*current_replaced_handle_};
        if (auto rebuilt{reload_with_loader(*loader, *manifest, activate_code,
                                            replaced, instance_handle)};
            !rebuilt) {
            return std::unexpected{
                annotate(std::move(rebuilt.error()), entry.path)};
        }
    } else if (auto loaded_with{load_with_loader(
                   *loader, *manifest, activate_code, instance_handle)};
               !loaded_with) {
        return std::unexpected{
            annotate(std::move(loaded_with.error()), entry.path)};
    }

    const auto generation{current_replaced_generation_ != 0
                              ? current_replaced_generation_ + 1
                              : std::uint64_t{1}};
    return loaded_mod{.id = manifest->common.id,
                      .version = manifest->common.version,
                      .abi = manifest->common.abi,
                      .external = manifest->is_external(),
                      .has_code = current_load_has_code_,
                      .provides_summary = provides_summary(*manifest),
                      .contracts = manifest->contracts,
                      .contract_requires = manifest->contract_requires,
                      .manifest_hash_hex = manifest_hash_hex,
                      .required = manifest->required,
                      .optional = manifest->optional,
                      .loader = manifest->loader,
                      .loader_unload = loader->record->unload,
                      .loader_sandboxed = loader->record->sandboxed,
                      .instance_handle = instance_handle,
                      .state = staging_ ? mod_lifecycle_state::staged
                                        : mod_lifecycle_state::active,
                      .generation = generation,
                      .provided_slots = current_mod_registered_slots_};
}

host::~host() {
    unload_external_mods();
    remove_materialized_entries();
}

void host::unload_external_mods() noexcept {
    for (auto entry{external_mods_.rbegin()}; entry != external_mods_.rend();
         ++entry) {
        if (entry->released) {
            continue;
        }
        try {
            if (entry->on_unload_ex != nullptr) {
                static_cast<void>(entry->on_unload_ex(
                    entry->user,
                    static_cast<std::uint32_t>(unload_reason::shutdown)));
            } else if (entry->on_unload != nullptr) {
                entry->on_unload(entry->user);
            }
        } catch (...) {
            // A mod must not terminate the host during shutdown.
        }
        entry->released = true;
    }
    // Process/host teardown: the mappings are only unmapped here, never while
    // the server is running.
    external_mods_.clear();
    retired_native_libraries_.clear();
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
    // Embedders/tests may register their own loader before the host bootstrap
    // runs; the engine-owned loaders are still part of every host, so only the
    // missing ones are registered.
    if (find_loader(builtin_loader_id) != nullptr &&
        find_loader(native_loader_id) != nullptr) {
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
    // The bootstrap path owns its loader ids: a reserved unqualified id or a
    // namespaced embedder/test loader. A package-provided loader goes through
    // `register_provider("refrain:mod.loader", ...)` so it can never claim a
    // host-owned id.
    if (!is_valid_loader_id(loader_id)) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 fmt::format("mod loader id '{}' is not a valid loader id",
                             loader_id))};
    }
    if (!is_reserved_loader_id(loader_id) &&
        loader_id.find('.') == std::string_view::npos) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 fmt::format("mod loader id '{}' is not namespaced; a loader "
                             "provider uses an id such as com.example.loader",
                             loader_id))};
    }
    return add_loader_instance(loader_id, api, version, {}, true);
}

auto host::add_loader_instance(std::string_view loader_id,
                               const provider_api &api,
                               std::string_view version,
                               std::string_view provider, bool host_owned,
                               std::uint64_t *out_serial) -> status {
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
    if (auto checked{check_abi_header(copy)}; !checked) {
        return std::unexpected{
            annotate(std::move(checked.error()),
                     fmt::format("mod loader '{}'", loader_id))};
    }

    // Query the loader's declared capabilities now: a loader that cannot
    // answer, names another id, implements another ABI or misattributes its
    // provider is not usable, and the host records the unload/sandbox contract
    // before dispatching to it.
    mod_loader_capabilities capabilities{};
    try {
        const loader_abi::provider provider_table{
            *static_cast<const loader_vtable *>(copy.vtable), copy.self};
        loader_abi::mod_loader_capabilities abi_capabilities{};
        provider_table.capabilities(abi_capabilities);
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
    if (auto checked{
            validate_loader_capabilities(loader_id, provider, capabilities)};
        !checked) {
        return checked;
    }

    const auto effective_version{version.empty() ? std::string{"1.0.0"}
                                                 : std::string{version}};
    // The registry candidate key is (service, provider id, version), so the
    // candidate identity is the loader instance id: it is unique per process
    // and lets one provider mod register several loader ids. The providing mod
    // is recorded on the loader record and in the candidate source instead.
    const std::string provider_id{loader_id};
    provider_apis_.push_back(std::make_unique<provider_api>(copy));
    provider_info info{};
    info.service = std::string{loader_abi::service_id};
    info.id = provider_id;
    info.version = effective_version;
    info.instance = provider_apis_.back().get();
    info.source = host_owned ? std::string{"host"} : std::string{provider};
    auto serial{services_.add_provider_serial(std::move(info))};
    if (!serial) {
        return std::unexpected{std::move(serial.error())};
    }
    auto selected{services_.select_active(loader_abi::service_id, provider_id,
                                          effective_version, loader_id)};
    if (!selected) {
        static_cast<void>(services_.remove_provider(*serial));
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
        .provider = std::string{provider},
        .host_owned = host_owned,
    });
    if (out_serial != nullptr) {
        *out_serial = *serial;
    }
    if (host_owned) {
        log_loader.info("registered bootstrap mod loader {} version {}",
                        loader_id, effective_version);
    } else {
        log_loader.info("registered mod loader {} version {} from mod {}",
                        loader_id, effective_version, provider);
    }
    return status_ok();
}

auto host::register_mod_loader_from_mod(std::string_view loader_id,
                                        const provider_api &api,
                                        std::string_view version) -> status {
    if (staging_) {
        return std::unexpected{fail(
            error_code::unsupported,
            fmt::format("mod '{}' cannot register loader '{}' while a "
                        "composition epoch is staged; a loader provider is "
                        "part of the bootstrap composition and changing it "
                        "needs a process restart",
                        current_mod_, loader_id))};
    }
    if (current_mod_.empty() || current_plan_ == nullptr) {
        return std::unexpected{
            fail(error_code::not_initialized,
                 "register_provider() for the mod loader service is only legal "
                 "from a mod factory during load()")};
    }
    // A loader provider is host-level code. A guest a sandboxed loader
    // executes can never become the environment other guests run in.
    if (current_loader_sandboxed_) {
        return std::unexpected{fail(
            error_code::permission_denied,
            fmt::format("mod '{}' cannot provide loader '{}': it is loaded "
                        "through the sandboxed loader '{}', and a sandboxed "
                        "guest can never become a loader",
                        current_mod_, loader_id, current_loader_id_))};
    }
    if (!is_host_loader_id(current_loader_id_)) {
        return std::unexpected{fail(
            error_code::permission_denied,
            fmt::format("mod '{}' cannot provide loader '{}': it is loaded "
                        "through loader '{}'; a loader-providing mod must run "
                        "through the host-owned builtin or native loader",
                        current_mod_, loader_id, current_loader_id_))};
    }
    if (auto allowed{check_mod_loader_id(loader_id)}; !allowed) {
        return std::unexpected{fail(
            allowed.error().code,
            fmt::format("mod '{}' cannot provide loader '{}': {}", current_mod_,
                        loader_id, allowed.error().message))};
    }
    // The gamepack plan is the trust selection: the loader id must be
    // declared exactly once and with this mod as its provider.
    const resolved_loader *declared{current_plan_->loader(loader_id)};
    if (declared == nullptr) {
        std::vector<std::string> declared_ids;
        declared_ids.reserve(current_plan_->loaders.size());
        for (const auto &entry : current_plan_->loaders) {
            declared_ids.push_back(entry.id);
        }
        return std::unexpected{fail(
            error_code::permission_denied,
            fmt::format("mod '{}' cannot provide loader '{}': the gamepack "
                        "plan declares no loader '{}'; declared loaders: {}",
                        current_mod_, loader_id, loader_id,
                        join_instance_names(declared_ids)))};
    }
    const auto declared_provider{
        std::string{split_provider_ref(declared->provider).id}};
    if (declared_provider != current_mod_) {
        return std::unexpected{fail(
            error_code::permission_denied,
            fmt::format("mod '{}' cannot provide loader '{}': the gamepack "
                        "plan declares mod '{}' as its provider",
                        current_mod_, loader_id, declared_provider))};
    }
    // The package must be honest about the service it provides.
    if (!declares_service(current_mod_provides_,
                          std::string{loader_abi::service_id})) {
        return std::unexpected{fail(
            error_code::permission_denied,
            fmt::format("mod '{}' cannot provide loader '{}': the manifest "
                        "[provides] list does not declare '{}'",
                        current_mod_, loader_id,
                        std::string{loader_abi::service_id}))};
    }
    // The install/signature policy decides what is trusted before the host
    // sees a package. A loader provider must come from the built-in image or
    // an installed, receipt-checked package; a directory tree is not an
    // install and is refused as a runtime source everywhere else too.
    if (current_package_ == nullptr ||
        (current_package_->kind() != package_kind::builtin &&
         current_package_->kind() != package_kind::rfmod)) {
        return std::unexpected{
            fail(error_code::permission_denied,
                 fmt::format("mod '{}' cannot provide loader '{}': a loader "
                             "provider must come from the built-in image or an "
                             "installed package that passed the install policy",
                             current_mod_, loader_id))};
    }
    std::uint64_t serial{0};
    const auto effective_version{version.empty()
                                     ? std::string{current_mod_version_}
                                     : std::string{version}};
    if (auto added{add_loader_instance(loader_id, api, effective_version,
                                       current_mod_, false, &serial)};
        !added) {
        return added;
    }
    current_mod_registered_slots_.push_back(provided_slot{
        .service = std::string{loader_abi::service_id},
        .instance = std::string{loader_id},
        .serial = serial,
    });
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

/// Package root handed to a loader contract call: the installed package's
/// receipt directory, or empty for a manifest read from the built-in image.
/// The contract documents the installed case as the directory that holds the
/// receipt, so `<root>/<version>.json` names the install record a loader reads
/// its guest members through.
auto host::loader_package_root() const -> std::string {
    if (current_candidate_ == nullptr ||
        current_candidate_->kind == package_kind::builtin) {
        return {};
    }
    const auto parent{
        std::filesystem::path{current_candidate_->path}.parent_path()};
    return parent.empty() ? std::string{} : parent.string();
}

auto host::resolve_loader(std::string_view loader_id) const
    -> result<loader_binding> {
    const auto active{
        services_.active_provider(loader_abi::service_id, loader_id)};
    if (!active) {
        const auto available{join_instance_names(
            services_.active_instances(loader_abi::service_id))};
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
        return std::unexpected{
            fail(error_code::not_found,
                 fmt::format("mod loader '{}' is active but has no recorded "
                             "loader registration; active mod loaders: {}",
                             loader_id, registered_loader_ids()))};
    }
    // Every loader instance is registered under its loader id; a mismatch
    // means the registry slot was replaced behind the loader record.
    if (active->id != record->id) {
        return std::unexpected{fail(
            error_code::version_mismatch,
            fmt::format("mod loader '{}' is active but its provider slot names "
                        "'{}' instead of '{}'",
                        loader_id, active->id, record->id))};
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
    if (auto checked{validate_loader_capabilities(
            binding.record->id, binding.record->provider, capabilities)};
        !checked) {
        return std::unexpected{std::move(checked.error())};
    }
    if (capabilities.abi_major != binding.record->abi_major ||
        capabilities.unload != binding.record->unload ||
        capabilities.sandboxed != binding.record->sandboxed ||
        capabilities.supports_hot_install !=
            binding.record->supports_hot_install ||
        capabilities.provider != binding.record->provider) {
        return std::unexpected{fail(
            error_code::version_mismatch,
            binding.record->provider.empty()
                ? fmt::format("mod loader '{}' changed its declared "
                              "capabilities while running",
                              binding.record->id)
                : fmt::format("mod loader '{}' provided by mod '{}' changed "
                              "its declared capabilities while running",
                              binding.record->id, binding.record->provider))};
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
    // The contract borrows the package root for the duration of the call, so
    // it must stay alive across `provider.validate()`.
    const auto package_root{loader_package_root()};
    const auto entry{make_loader_entry(manifest, activate_code, package_root)};
    clear_loader_diagnostic();
    try {
        const loader_abi::provider provider{
            *static_cast<const loader_vtable *>(binding.api.vtable),
            binding.api.self};
        const auto validated{provider.validate(entry)};
        if (!validated) {
            auto detail{take_loader_dispatch_error()};
            if (detail.empty()) {
                detail = take_loader_diagnostic();
            }
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
    const auto package_root{loader_package_root()};
    loader_abi::mod_load_request request{};
    request.struct_size = REFRAIN_SERVICES_MOD_LOADER_MOD_LOAD_REQUEST_SIZE;
    request.manifest = make_loader_entry(manifest, activate_code, package_root);
    clear_loader_diagnostic();
    try {
        const loader_abi::provider provider{
            *static_cast<const loader_vtable *>(binding.api.vtable),
            binding.api.self};
        const auto loaded{provider.load(request)};
        if (!loaded) {
            auto detail{take_loader_dispatch_error()};
            if (detail.empty()) {
                detail = take_loader_diagnostic();
            }
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
        // A loader that returned an instance handle executed mod code; the
        // lifecycle layer uses this to decide whether an unload hook must run.
        if (activate_code && handle != 0) {
            current_load_has_code_ = true;
        }
    } catch (...) {
        return std::unexpected{
            fail(error_code::unknown,
                 fmt::format("mod loader '{}' load threw an exception",
                             binding.record->id))};
    }
    return status_ok();
}

auto host::reload_with_loader(const loader_binding &binding,
                              const mod_manifest &manifest, bool activate_code,
                              std::uint64_t old_handle,
                              std::uint64_t &new_handle) -> status {
    new_handle = 0;
    if (current_candidate_ == nullptr) {
        return std::unexpected{
            fail(error_code::unknown,
                 "mod loader dispatch has no candidate context")};
    }
    const auto package_root{loader_package_root()};
    loader_abi::mod_load_request request{};
    request.struct_size = REFRAIN_SERVICES_MOD_LOADER_MOD_LOAD_REQUEST_SIZE;
    request.manifest = make_loader_entry(manifest, activate_code, package_root);
    clear_loader_diagnostic();
    try {
        const loader_abi::provider provider{
            *static_cast<const loader_vtable *>(binding.api.vtable),
            binding.api.self};
        const auto rebuilt{
            provider.reload(loader_abi::mod_instance{old_handle}, request)};
        if (!rebuilt) {
            auto detail{take_loader_dispatch_error()};
            if (detail.empty()) {
                detail = take_loader_diagnostic();
            }
            return std::unexpected{fail(
                loader_error_to(rebuilt.error()),
                detail.empty()
                    ? fmt::format("mod loader '{}' failed to reload mod '{}'",
                                  binding.record->id, manifest.common.id)
                    : fmt::format("mod loader '{}' failed to reload mod '{}': "
                                  "{}",
                                  binding.record->id, manifest.common.id,
                                  detail))};
        }
        new_handle = rebuilt->raw();
        if (activate_code && new_handle != 0) {
            current_load_has_code_ = true;
        }
    } catch (...) {
        return std::unexpected{
            fail(error_code::unknown,
                 fmt::format("mod loader '{}' reload threw an exception",
                             binding.record->id))};
    }
    return status_ok();
}

auto host::dispatch_loader_unload(const loaded_mod &mod, unload_reason reason)
    -> status {
    if (!mod.has_code || mod.instance_handle == 0) {
        // A data-only mod runs no code; removing its composition record is the
        // unload.
        return status_ok();
    }
    auto binding{resolve_loader(mod.loader)};
    if (!binding) {
        return std::unexpected{
            annotate(std::move(binding.error()),
                     fmt::format("mod '{}' unload", mod.id))};
    }
    clear_loader_diagnostic();
    try {
        const loader_abi::provider provider{
            *static_cast<const loader_vtable *>(binding->api.vtable),
            binding->api.self};
        const auto unloaded{
            provider.unload(loader_abi::mod_instance{mod.instance_handle},
                            to_abi_unload_reason(reason))};
        if (!unloaded) {
            auto detail{take_loader_dispatch_error()};
            if (detail.empty()) {
                detail = take_loader_diagnostic();
            }
            return std::unexpected{fail(
                loader_error_to(unloaded.error()),
                detail.empty()
                    ? fmt::format("mod loader '{}' failed to unload mod '{}'",
                                  binding->record->id, mod.id)
                    : fmt::format("mod loader '{}' failed to unload mod '{}': "
                                  "{}",
                                  binding->record->id, mod.id, detail))};
        }
    } catch (...) {
        return std::unexpected{
            fail(error_code::unknown,
                 fmt::format("mod loader '{}' unload threw an exception",
                             binding->record->id))};
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

auto host::find_external_mod(std::uint64_t handle) -> external_mod_state * {
    for (auto &state : external_mods_) {
        if (state.handle == handle) {
            return &state;
        }
    }
    return nullptr;
}

auto host::open_native_instance(std::uint64_t &handle) -> status {
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
    // Name the copy after its content and a per-load sequence: a reload writes
    // a new file even when the package bytes are identical, so the dynamic
    // loader can never resolve the new generation to an earlier mapping by
    // path identity.
    const auto digest_hex{data::to_hex(data::blake3(*library_bytes))};
    // Keep the packaged member's extension; the dynamic loader opens the file
    // by path and infers nothing from it.
    const auto extension{std::filesystem::path{member}.extension().string()};
    const auto library_path{data::join_path(
        materialized_dir_, fmt::format("{}-{}{}", digest_hex,
                                       ++materialized_sequence_, extension))};
    if (auto written{io::write_file(library_path, *library_bytes)}; !written) {
        return std::unexpected{
            annotate(std::move(written.error()),
                     fmt::format("mod '{}' native entry copy {}",
                                 manifest.common.id, library_path))};
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
    // The unload hook was added after the first descriptors were written: a
    // smaller struct is a valid v1 descriptor and is accepted with the legacy
    // hook, a larger one must cover exactly the fields this host knows.
    if (desc->struct_size < REFRAIN_MOD_DESC_V1_SIZE) {
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

    const auto new_handle{next_instance_handle_++};
    external_mods_.push_back(external_mod_state{
        .library = std::move(library),
        .on_unload_ex =
            desc->struct_size >= offsetof(refrain_mod_desc, on_unload_ex) +
                                     sizeof(refrain_mod_on_unload_ex)
                ? desc->on_unload_ex
                : nullptr,
        .on_unload = desc->on_unload,
        .user = user,
        .handle = new_handle,
        .id = manifest.common.id,
        .version = manifest.common.version,
    });
    handle = new_handle;
    current_load_has_code_ = true;
    return status_ok();
}

auto host::native_load(std::uint64_t &handle) -> status {
    return open_native_instance(handle);
}

auto host::native_reload_instance(std::uint64_t instance,
                                  std::uint64_t &out_new) -> status {
    out_new = 0;
    const auto *old{find_external_mod(instance)};
    if (old == nullptr || old->released) {
        return std::unexpected{
            fail(error_code::not_found,
                 fmt::format("the native loader has no live instance {}",
                             instance))};
    }
    if (current_manifest_ == nullptr) {
        return std::unexpected{
            fail(error_code::unknown,
                 "native loader reload has no manifest context")};
    }
    const auto &id{current_manifest_->common.id};
    if (old->id != id) {
        return std::unexpected{fail(
            error_code::version_mismatch,
            fmt::format("native reload instance {} belongs to mod '{}' and "
                        "cannot be replaced by '{}'",
                        instance, old->id, id))};
    }
    const auto retired{native_retired_generations_[id]};
    if (retired >= native_retire_limit_) {
        return std::unexpected{
            fail(error_code::unsupported,
                 fmt::format(
                     "mod '{}' already keeps {} retired native generation(s) "
                     "mapped and the limit is {}; a reload would leak "
                     "another shared library mapping, so the server must be "
                     "restarted",
                     id, retired, native_retire_limit_))};
    }
    return open_native_instance(out_new);
}

auto host::release_loader_instance(std::uint64_t instance, unload_reason reason)
    -> status {
    auto *state{find_external_mod(instance)};
    if (state == nullptr) {
        return std::unexpected{
            fail(error_code::not_found,
                 fmt::format("the native loader does not own instance {}",
                             instance))};
    }
    if (state->released) {
        return status_ok();
    }
    refrain_status callback_status{REFRAIN_STATUS_OK};
    try {
        if (state->on_unload_ex != nullptr) {
            callback_status = state->on_unload_ex(
                state->user, static_cast<std::uint32_t>(reason));
        } else if (state->on_unload != nullptr) {
            state->on_unload(state->user);
        }
    } catch (...) {
        // A mod callback must never unwind into the host; an exception is a
        // failed unload and quarantines the instance like a non-zero status.
        callback_status = static_cast<refrain_status>(REFRAIN_HANDLE_INVALID);
    }
    state->released = true;
    if (callback_status != REFRAIN_STATUS_OK) {
        state->quarantined = true;
        ++native_retired_generations_[state->id];
        record_quarantine(
            state->id,
            fmt::format("on_unload failed for {}@{} with reason '{}' (status "
                        "{}); the instance is quarantined and never called "
                        "again",
                        state->id, state->version, to_string(reason),
                        callback_status));
        return std::unexpected{
            fail(error_code::mod_error,
                 fmt::format("mod '{}' on_unload failed with status {}; the "
                             "instance is quarantined and the server must be "
                             "restarted to load it again",
                             state->id, callback_status))};
    }
    if (state->library != nullptr) {
        // Retire, never dlclose: a call that started before the commit may
        // still be inside this mapping.
        retired_native_libraries_.push_back(std::move(state->library));
        ++native_retired_generations_[state->id];
    }
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
        // activate provider code and which loaders are part of it, so project
        // it once before any factory runs. The registration path reads the
        // declared loader plan from `current_plan_` until it is published.
        planned_providers planned{plan_providers(plan)};
        current_plan_ = &plan;

        // The bootstrap set is derived from the plan and the manifests: every
        // declared loader provider plus the transitive hard dependencies it
        // needs. The providers themselves are identified in plan order, so the
        // set and the order are deterministic.
        auto bootstrap{bootstrap_mod_ids(plan)};
        if (!bootstrap) {
            return std::unexpected{std::move(bootstrap.error())};
        }

        std::vector<loaded_mod> mods{};
        mods.reserve(plan.load_order.size());
        const auto load_entry = [&](const candidate &entry) -> status {
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
            return status_ok();
        };

        // Bootstrap phase: loader-providing mods run before the rest of the
        // composition, through a host-owned loader. Their factories register
        // the namespaced loader instances the other packages dispatch through.
        for (const auto &entry : plan.load_order) {
            if (!bootstrap->contains(entry.id)) {
                continue;
            }
            if (auto loaded{load_entry(entry)}; !loaded) {
                return loaded;
            }
        }
        if (auto verified{verify_declared_loaders(plan)}; !verified) {
            return verified;
        }
        if (!plan.loaders.empty()) {
            std::string ids;
            for (const auto &declared : plan.loaders) {
                if (!ids.empty()) {
                    ids += ", ";
                }
                ids += declared.id;
            }
            log_loader.info("bootstrapped {} package-provided loader(s): {}",
                            plan.loaders.size(), ids);
        }

        // Main phase: the rest of the composition in dependency order. Every
        // package naming a package-provided loader is ordered after its
        // provider by the resolver, so the loader is active when it is needed.
        for (const auto &entry : plan.load_order) {
            if (bootstrap->contains(entry.id)) {
                continue;
            }
            if (auto loaded{load_entry(entry)}; !loaded) {
                return loaded;
            }
        }
        current_plan_ = nullptr;
        current_mod_.clear();
        current_mod_version_.clear();
        current_mod_provides_.clear();
        current_mod_planned_slots_.clear();
        current_mod_planned_instances_.clear();
        current_mod_chain_slots_.clear();
        current_mod_default_slots_.clear();
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

        // A composition that would publish an ambiguous or incompatible
        // custom contract set fails before the registry is frozen.
        if (auto contracts{verify_contract_composition()}; !contracts) {
            return contracts;
        }

        // Fail closed: every provider the gamepack selected must be active
        // before the registry is frozen, so a factory that silently skips (or
        // the wrong mod winning) fails startup instead of shipping a
        // composition that does not match the plan.
        if (auto verified{verify_plan_activation(plan)}; !verified) {
            return verified;
        }

        loaded_flag_ = true;
        // Publish the boot composition as epoch 1: every immediate
        // `select_active()` above becomes generation 1 of its slot in one
        // commit. From here on the registry stays open for hot epochs while the
        // host-owned surfaces are sealed permanently.
        if (auto committed{services_.commit_epoch()}; !committed) {
            return std::unexpected{std::move(committed.error())};
        }
        plan_ = plan;
        epoch_ = 1;
        for (const auto &mod : mods_) {
            mod_generations_[mod.id] = mod.generation;
        }
        for (const auto &service : sealed_services_) {
            if (auto sealed{services_.seal(service)}; !sealed) {
                return std::unexpected{std::move(sealed.error())};
            }
        }
        if (auto sealed{services_.seal(loader_abi::service_id)}; !sealed) {
            return std::unexpected{std::move(sealed.error())};
        }
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

auto host::plan_providers(const composition &plan) -> planned_providers {
    planned_providers planned{};
    for (const auto &resolved : plan.providers) {
        // `role` selects which slot table records the reference: the reserved
        // default instance (exclusive provider or chain base), an `instanced`
        // instance name, or one chain wrapper position.
        enum class role { default_instance, instance, chain_wrapper };
        const auto add_provider_ref{[&planned, &resolved](std::string_view ref,
                                                          std::string_view slot,
                                                          role kind) {
            const auto id{std::string{split_provider_ref(ref).id}};
            if (id.empty()) {
                return;
            }
            planned.mods.insert(id);
            if (resolved.service.empty()) {
                return;
            }
            planned.slots[id].insert(resolved.service);
            switch (kind) {
            case role::default_instance:
                planned.default_slots[id].insert(resolved.service);
                break;
            case role::instance:
                planned.instances[id][resolved.service].insert(
                    std::string{slot});
                break;
            case role::chain_wrapper:
                planned.chain_slots[id][resolved.service].insert(
                    std::string{slot});
                break;
            }
        }};
        add_provider_ref(resolved.provider, {}, role::default_instance);
        for (const auto &instance : resolved.instances) {
            add_provider_ref(instance.provider, instance.name, role::instance);
        }
        // A chain wrapper occupies its own slot, one instance per declared
        // wrapper position; the base keeps the reserved default instance.
        for (std::size_t position{0}; position < resolved.wrappers.size();
             ++position) {
            add_provider_ref(resolved.wrappers[position],
                             chain_wrapper_instance(position),
                             role::chain_wrapper);
        }
    }
    // Package-provided loaders project onto the loader service the same way a
    // provider binding does: the providing mod is selected for
    // `refrain:mod.loader` on the instance named after the loader id, so the
    // generic registration permission checks and the plan verification see the
    // declared loader plan.
    for (const auto &declared : plan.loaders) {
        const auto provider{
            std::string{split_provider_ref(declared.provider).id}};
        if (provider.empty()) {
            continue;
        }
        planned.mods.insert(provider);
        planned.slots[provider].insert(std::string{loader_abi::service_id});
        planned.instances[provider][std::string{loader_abi::service_id}].insert(
            declared.id);
    }
    return planned;
}

auto host::registered_loader_ids() const -> std::string {
    std::vector<std::string> ids;
    ids.reserve(loaders_.size());
    for (const auto &loader : loaders_) {
        ids.push_back(loader.id);
    }
    return join_instance_names(ids);
}

auto host::bootstrap_mod_ids(const composition &plan) const
    -> result<std::set<std::string>> {
    std::map<std::string, const candidate *> by_id;
    for (const auto &entry : plan.load_order) {
        by_id.emplace(entry.id, &entry);
    }
    std::set<std::string> bootstrap;
    std::vector<const candidate *> pending;
    pending.reserve(plan.loaders.size());
    for (const auto &declared : plan.loaders) {
        const auto provider{
            std::string{split_provider_ref(declared.provider).id}};
        const auto found{by_id.find(provider)};
        if (found == by_id.end()) {
            return std::unexpected{fail(
                error_code::not_found,
                fmt::format("gamepack declares loader '{}' provided by mod "
                            "'{}' but that mod is not part of the composition",
                            declared.id, provider))};
        }
        pending.push_back(found->second);
    }
    // Loader providers are loaded first together with the hard dependencies
    // they need, so every dependency of a bootstrap mod is available before it
    // and a package naming a package-provided loader is only dispatched after
    // that loader's provider registered the instance.
    while (!pending.empty()) {
        const auto *mod{pending.back()};
        pending.pop_back();
        if (!bootstrap.insert(mod->id).second) {
            continue;
        }
        for (const auto &requirement : mod->required) {
            const auto found{by_id.find(requirement.id)};
            if (found != by_id.end()) {
                pending.push_back(found->second);
            }
        }
    }
    return bootstrap;
}

auto host::verify_declared_loaders(const composition &plan) const -> status {
    for (const auto &declared : plan.loaders) {
        // The plan is normally produced by `mod::resolve()`, which rejects
        // host-owned ids; a hand-built plan must fail the same way instead of
        // letting a package shadow `builtin`, `native` or a script loader.
        if (auto allowed{check_mod_loader_id(declared.id)}; !allowed) {
            return std::unexpected{
                fail(allowed.error().code,
                     fmt::format("gamepack declares loader '{}': {}",
                                 declared.id, allowed.error().message))};
        }
        const auto provider{
            std::string{split_provider_ref(declared.provider).id}};
        const auto *record{find_loader(declared.id)};
        if (record == nullptr) {
            return std::unexpected{fail(
                error_code::not_found,
                fmt::format("mod '{}' was declared as the provider of loader "
                            "'{}' but did not register it during the bootstrap "
                            "phase; active mod loaders: {}",
                            provider, declared.id, registered_loader_ids()))};
        }
        if (record->host_owned) {
            return std::unexpected{fail(
                error_code::permission_denied,
                fmt::format("loader '{}' is registered by the host bootstrap "
                            "but the gamepack declares mod '{}' as its "
                            "provider",
                            declared.id, provider))};
        }
        if (record->provider != provider) {
            return std::unexpected{fail(
                error_code::version_mismatch,
                fmt::format("loader '{}' is declared with provider mod '{}' "
                            "but was registered by mod '{}'",
                            declared.id, provider, record->provider))};
        }
    }
    return status_ok();
}

auto host::verify_plan_activation(const composition &plan, bool staged) const
    -> status {
    const auto find_mod{[this,
                         staged](std::string_view id) -> const loaded_mod * {
        const auto match{
            std::find_if(mods_.begin(), mods_.end(),
                         [id](const loaded_mod &mod) { return mod.id == id; })};
        if (match != mods_.end()) {
            return &*match;
        }
        if (!staged) {
            return nullptr;
        }
        const auto staged_match{
            std::find_if(staged_mods_.begin(), staged_mods_.end(),
                         [id](const loaded_mod &mod) { return mod.id == id; })};
        return staged_match == staged_mods_.end() ? nullptr : &*staged_match;
    }};
    const auto active_view{
        [this,
         staged](std::string_view service,
                 std::string_view instance) -> std::optional<provider_info> {
            if (staged) {
                return services_.staged_active_provider(service, instance);
            }
            return services_.active_provider(service, instance);
        }};
    const auto verify_active{[this, &find_mod, &active_view](
                                 std::string_view slot, std::string_view ref,
                                 std::string_view instance,
                                 bool allow_data_only) -> status {
        const auto [id, version] = split_provider_ref(ref);
        if (id.empty()) {
            return status_ok();
        }
        const auto *mod{find_mod(id)};
        if (mod == nullptr) {
            return std::unexpected{
                fail(error_code::not_found,
                     fmt::format("provider activation for service '{}': "
                                 "planned mod '{}' was not loaded",
                                 slot, id))};
        }
        if (mod->provides_summary.empty()) {
            if (allow_data_only) {
                // Data-only binding (`content = ...`): the mod declares no
                // contract, so there is no provider instance to activate.
                return status_ok();
            }
            // A chain wrapper is a provider by definition: a data-only package
            // that cannot fill the slot would silently drop its decisions from
            // the composed chain.
            return std::unexpected{fail(
                error_code::not_found,
                fmt::format("provider activation for service '{}' instance "
                            "'{}': planned mod '{}' declares no provider "
                            "contract, so it cannot occupy this provider slot",
                            slot, instance, id))};
        }
        for (const auto &service : services_.services()) {
            if (!matches_slot(service, slot)) {
                continue;
            }
            const auto active{active_view(service, instance)};
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

    // A chain wrapper is an active provider on its own `chain:<position>`
    // instance; the base keeps the reserved default instance. Verifying the
    // active slot (instead of the candidate table) is what makes a staged
    // wrapper reload truthful: the staged activation of the wrapper does not
    // shadow the base, and a wrapper that never activated still fails closed.
    for (const auto &resolved : plan.providers) {
        if (!resolved.provider.empty()) {
            if (auto checked{verify_active(resolved.service, resolved.provider,
                                           {}, true)};
                !checked) {
                return checked;
            }
        }
        for (const auto &instance : resolved.instances) {
            if (auto checked{verify_active(resolved.service, instance.provider,
                                           instance.name, true)};
                !checked) {
                return checked;
            }
        }
        if (resolved.wrappers.size() > max_chain_wrappers) {
            return std::unexpected{fail(
                error_code::unsupported,
                fmt::format("chain binding for service '{}' declares {} "
                            "wrappers, more than the supported bound of {}",
                            resolved.service, resolved.wrappers.size(),
                            max_chain_wrappers))};
        }
        for (std::size_t position{0}; position < resolved.wrappers.size();
             ++position) {
            if (auto checked{
                    verify_active(resolved.service, resolved.wrappers[position],
                                  chain_wrapper_instance(position), false)};
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
    current_mod_chain_slots_.clear();
    current_mod_default_slots_.clear();
    current_mod_selected_provider_ = false;
    current_manifest_ = nullptr;
    current_plan_ = nullptr;
    current_candidate_ = nullptr;
    current_package_ = nullptr;
    current_static_descriptor_ = nullptr;
    current_loader_id_.clear();
    current_loader_sandboxed_ = false;
    current_load_has_code_ = false;
    current_mod_registered_slots_.clear();
    current_replaced_handle_.reset();
    current_replaced_generation_ = 0;
    current_replaced_loader_.clear();
    loader_dispatch_error_.clear();
    plan_ = composition{};
    epoch_ = 0;
    mod_generations_.clear();
    staged_change_.reset();
    staged_next_composition_.reset();
    staged_mods_.clear();
    staged_retire_mods_.clear();
    staged_retire_reasons_.clear();
    staged_retired_chain_slots_.clear();
    contract_definitions_.clear();
    staged_contract_definitions_.clear();
    current_contract_records_.clear();

    staging_ = false;
    quarantined_mods_.clear();
    loaded_flag_ = false;
    failed_ = true;
}

// ---------------------------------------------------------------------------
// hot lifecycle: staged composition epochs
// ---------------------------------------------------------------------------

auto host::restart_required_reason_for(const loaded_mod &mod) const
    -> std::string {
    for (const auto &quarantined : quarantined_mods_) {
        if (quarantined == mod.id) {
            return fmt::format(
                "mod '{}' is quarantined and needs a process restart before it "
                "can be installed again",
                mod.id);
        }
    }
    for (const auto &declared : plan_.loaders) {
        if (split_provider_ref(declared.provider).id != mod.id) {
            continue;
        }
        return fmt::format(
            "mod '{}' provides the mod loader '{}' that other packages are "
            "dispatched through; a loader provider is part of the bootstrap "
            "composition and needs a process restart to be unloaded or "
            "replaced",
            mod.id, declared.id);
    }
    if (mod.has_code &&
        mod.loader_unload == mod_unload_semantics::restart_required) {
        return fmt::format(
            "mod '{}' was loaded by '{}' with restart-required unload "
            "semantics; statically linked code cannot be removed while the "
            "server runs",
            mod.id, mod.loader);
    }
    for (const auto &slot : mod.provided_slots) {
        if (is_restart_required_service(slot.service)) {
            return fmt::format("mod '{}' provides {}; the server must be "
                               "restarted to replace it",
                               mod.id, restart_required_reason(slot.service));
        }
    }
    return {};
}

void host::record_quarantine(std::string_view id, std::string_view reason) {
    if (std::find(quarantined_mods_.begin(), quarantined_mods_.end(), id) ==
        quarantined_mods_.end()) {
        quarantined_mods_.emplace_back(id);
    }
    log_mod.error("quarantined mod {}: {}", id, reason);
}

auto host::lifecycle_records() const -> std::vector<mod_lifecycle_record> {
    std::vector<mod_lifecycle_record> records;
    records.reserve(mods_.size() + quarantined_mods_.size());
    for (const auto &mod : mods_) {
        records.push_back(mod_lifecycle_record{
            .id = mod.id,
            .version = mod.version,
            .loader = mod.loader,
            .state = mod.state,
            .generation = mod.generation,
            .unload_semantics = mod.loader_unload,
            .quarantined =
                std::find(quarantined_mods_.begin(), quarantined_mods_.end(),
                          mod.id) != quarantined_mods_.end(),
        });
    }
    for (const auto &id : quarantined_mods_) {
        if (std::any_of(records.begin(), records.end(),
                        [&id](const mod_lifecycle_record &record) {
                            return record.id == id;
                        })) {
            continue;
        }
        records.push_back(mod_lifecycle_record{
            .id = id,
            .state = mod_lifecycle_state::retired,
            .quarantined = true,
        });
    }
    std::sort(records.begin(), records.end(),
              [](const mod_lifecycle_record &lhs,
                 const mod_lifecycle_record &rhs) { return lhs.id < rhs.id; });
    return records;
}

auto host::lifecycle_record(std::string_view id) const
    -> std::optional<mod_lifecycle_record> {
    for (const auto &record : lifecycle_records()) {
        if (record.id == id) {
            return record;
        }
    }
    return std::nullopt;
}

auto host::retired_native_generations(std::string_view id) const
    -> std::size_t {
    const auto found{native_retired_generations_.find(std::string{id})};
    return found == native_retired_generations_.end() ? 0 : found->second;
}

void host::set_native_retire_limit(std::size_t limit) noexcept {
    native_retire_limit_ = limit;
}

auto host::composition_digest_text() const -> std::string {
    std::string text{
        fmt::format("refrain-mod-composition/1\nepoch {}\ngamepack "
                    "{} {}\n",
                    epoch_, plan_.spec.id, plan_.spec.version)};
    for (const auto &declared : plan_.loaders) {
        text += fmt::format("loader {} provider {}\n", declared.id,
                            declared.provider);
    }
    std::vector<const loaded_mod *> ordered;
    ordered.reserve(mods_.size());
    for (const auto &mod : mods_) {
        ordered.push_back(&mod);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const loaded_mod *lhs, const loaded_mod *rhs) {
                  return lhs->id < rhs->id;
              });
    for (const auto *mod : ordered) {
        text += fmt::format("mod {} {} {} {} generation {}\n", mod->id,
                            mod->version, mod->loader, to_string(mod->state),
                            mod->generation);
    }
    for (const auto &[service, record] : contract_definitions_) {
        text += fmt::format(
            "contract {} owner {} version {} vtable {} "
            "compatible {}\n",
            service, record.owner, record.definition.version.text(),
            record.definition.vtable_size, record.definition.compatible);
    }
    for (const auto &service : services_.active_services()) {
        for (const auto &instance : services_.active_instances(service)) {
            const auto active{services_.active_provider(service, instance)};
            if (!active) {
                continue;
            }
            text += fmt::format("service {} {} {} {} generation {}\n", service,
                                instance, active->id, active->version,
                                active->generation);
        }
    }
    return text;
}

auto host::composition_digest() const -> std::string {
    const auto text{composition_digest_text()};
    return data::to_hex(data::blake3(std::as_bytes(std::span{text})));
}

namespace {

/// True when `ids` contains `id`.
[[nodiscard]] auto contains_id(const std::vector<std::string> &ids,
                               std::string_view id) -> bool {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

} // namespace

auto host::stage_composition(const composition &next,
                             const package_resolver &resolve,
                             const stage_options &options)
    -> result<staged_composition_change> {
    const std::lock_guard lifecycle_lock{lifecycle_mutex_};
    return stage_composition_locked(next, resolve, options);
}

auto host::stage_composition_locked(const composition &next,
                                    const package_resolver &resolve,
                                    const stage_options &options)
    -> result<staged_composition_change> {
    if (failed_) {
        return std::unexpected{
            fail(error_code::not_initialized,
                 "mod host failed a previous load and cannot stage changes; "
                 "recreate the host")};
    }
    if (!loaded_flag_) {
        return std::unexpected{
            fail(error_code::not_initialized,
                 "mod host is not loaded; stage changes after a successful "
                 "load()")};
    }
    if (staged_change_.has_value()) {
        return std::unexpected{
            fail(error_code::already_exists,
                 "a composition change is already staged; commit or abort it "
                 "before staging another one")};
    }
    if (next.spec.id != plan_.spec.id ||
        next.spec.version != plan_.spec.version ||
        next.spec.abi != plan_.spec.abi) {
        return std::unexpected{fail(
            error_code::version_mismatch,
            fmt::format("staged composition is gamepack {} {} (abi {}) but the "
                        "running composition is {} {} (abi {})",
                        next.spec.id, next.spec.version, next.spec.abi,
                        plan_.spec.id, plan_.spec.version, plan_.spec.abi))};
    }
    // Package-provided loaders are part of the bootstrap composition: the
    // loader service is sealed once the first composition is published, so a
    // hot epoch can neither add, remove nor repoint a declared loader. The
    // guarantee is a refusal, not a quarantine: nothing of the running epoch
    // changes.
    if (next.loaders != plan_.loaders) {
        auto added{std::string{}};
        auto removed{std::string{}};
        for (const auto &declared : next.loaders) {
            if (plan_.loader(declared.id) == nullptr) {
                if (!added.empty()) {
                    added += ", ";
                }
                added += declared.id;
            }
        }
        for (const auto &declared : plan_.loaders) {
            if (next.loader(declared.id) == nullptr) {
                if (!removed.empty()) {
                    removed += ", ";
                }
                removed += declared.id;
            }
        }
        for (const auto &declared : plan_.loaders) {
            const auto *now{next.loader(declared.id)};
            if (now != nullptr && now->provider != declared.provider) {
                if (!removed.empty()) {
                    removed += ", ";
                }
                removed += declared.id;
            }
        }
        return std::unexpected{fail(
            error_code::unsupported,
            fmt::format("the staged composition changes the loader plan "
                        "(added: {}, removed or repointed: {}); a "
                        "package-provided loader is registered during the "
                        "bootstrap phase and the loader service is sealed, so "
                        "changing it needs a process restart",
                        added.empty() ? std::string{"none"} : added,
                        removed.empty() ? std::string{"none"} : removed))};
    }
    for (const auto &declared : next.loaders) {
        const auto provider{
            std::string{split_provider_ref(declared.provider).id}};
        if (next.find(provider) == nullptr) {
            return std::unexpected{fail(
                error_code::unsupported,
                fmt::format("cannot remove mod '{}': it provides the mod "
                            "loader '{}' that other packages are dispatched "
                            "through, so the loader provider needs a process "
                            "restart",
                            provider, declared.id))};
        }
    }

    const auto planned{plan_providers(next)};
    const auto is_reload{[&options](std::string_view id) {
        return contains_id(options.reload, id);
    }};

    std::vector<const candidate *> additions{};
    std::vector<loaded_mod> retiring{};
    std::vector<std::pair<std::string, unload_reason>> retire_reasons{};
    for (const auto &entry : next.load_order) {
        const auto *current{find(entry.id)};
        if (current == nullptr) {
            additions.push_back(&entry);
            continue;
        }
        const auto changed{current->version != entry.version ||
                           current->manifest_hash_hex !=
                               data::to_hex(entry.manifest_hash)};
        if (!changed && !is_reload(entry.id)) {
            continue;
        }
        retiring.push_back(*current);
        retire_reasons.emplace_back(entry.id, unload_reason::reload);
        additions.push_back(&entry);
    }
    for (const auto &mod : mods_) {
        if (next.find(mod.id) != nullptr) {
            continue;
        }
        retiring.push_back(mod);
        retire_reasons.emplace_back(mod.id, unload_reason::uninstall);
    }

    // Restart-required and quarantine gates. A refusal happens before any
    // package is opened or any slot is quiesced.
    for (const auto &mod : retiring) {
        if (auto reason{restart_required_reason_for(mod)}; !reason.empty()) {
            return std::unexpected{
                fail(error_code::unsupported, std::move(reason))};
        }
    }
    for (const auto *entry : additions) {
        if (contains_id(quarantined_mods_, entry->id)) {
            return std::unexpected{
                fail(error_code::unsupported,
                     fmt::format("mod '{}' is quarantined and needs a process "
                                 "restart before it can be installed again",
                                 entry->id))};
        }
        const auto slots{planned.slots.find(entry->id)};
        if (slots == planned.slots.end()) {
            continue;
        }
        for (const auto &slot : slots->second) {
            if (is_restart_required_service(slot)) {
                return std::unexpected{fail(
                    error_code::unsupported,
                    fmt::format("hot install of '{}' targets {}; the server "
                                "must be restarted to replace it",
                                entry->id, restart_required_reason(slot)))};
            }
        }
    }
    // A mod that stays active must keep every dependency; removing one would
    // leave the running composition referencing a package that is gone.
    for (const auto &mod : mods_) {
        if (std::any_of(retiring.begin(), retiring.end(),
                        [&mod](const loaded_mod &retired) {
                            return retired.id == mod.id;
                        })) {
            continue;
        }
        for (const auto &requirement : mod.required) {
            if (next.find(requirement.id) == nullptr) {
                return std::unexpected{
                    fail(error_code::unsupported,
                         fmt::format("cannot uninstall '{}': mod '{}' still "
                                     "requires it",
                                     requirement.id, mod.id))};
            }
        }
    }

    // Build the new generations. The loads are outside the tick (this call is
    // the command/IO half of the epoch), the publish below is the tick half.
    staging_ = true;
    staged_mods_.clear();
    for (const auto *entry : additions) {
        auto package{resolve(*entry)};
        if (!package) {
            abort_staged_internals();
            return std::unexpected{
                annotate(std::move(package.error()),
                         fmt::format("mod '{}' package", entry->id))};
        }
        current_mod_ = entry->id;
        current_mod_version_ = entry->version;
        current_replaced_handle_.reset();
        current_replaced_generation_ = 0;
        current_replaced_loader_.clear();
        if (const auto *current{find(entry->id)}; current != nullptr) {
            current_replaced_generation_ = current->generation;
            current_replaced_loader_ = current->loader;
            if (current->has_code && current->instance_handle != 0) {
                current_replaced_handle_ = current->instance_handle;
            }
        }
        auto loaded{load_one(next, **package, *entry, planned)};
        if (!loaded) {
            abort_staged_internals();
            return std::unexpected{std::move(loaded.error())};
        }
        staged_mods_.push_back(std::move(*loaded));
    }
    current_mod_.clear();
    current_mod_version_.clear();
    current_mod_provides_.clear();
    current_mod_planned_slots_.clear();
    current_mod_planned_instances_.clear();
    current_mod_chain_slots_.clear();
    current_mod_default_slots_.clear();
    current_mod_selected_provider_ = false;
    current_replaced_handle_.reset();
    current_replaced_generation_ = 0;
    current_replaced_loader_.clear();

    // Retire the slots of a replaced/removed generation that the new
    // generation does not re-activate: the staged activation is what replaces
    // a slot, anything else must disappear from the committed registry.
    for (const auto &mod : retiring) {
        for (const auto &slot : mod.provided_slots) {
            if (services_.staged_decision(slot.service, slot.instance)
                    .value_or(false)) {
                continue;
            }
            if (auto staged{
                    services_.stage_retire(slot.service, slot.instance)};
                !staged && staged.error().code != error_code::not_found) {
                abort_staged_internals();
                return std::unexpected{std::move(staged.error())};
            }
        }
    }
    // A wrapper the staged composition no longer declares leaves its slot even
    // when its package stays loaded: the chain is resolved from the registry,
    // not from the plan, so a stale slot would keep answering after the commit
    // that removed the declaration.
    const auto wrapper_is_planned{[&planned](std::string_view id,
                                             std::string_view service,
                                             std::string_view instance) {
        const auto found{planned.chain_slots.find(std::string{id})};
        if (found == planned.chain_slots.end()) {
            return false;
        }
        for (const auto &[slot, instances] : found->second) {
            if (matches_slot(service, slot) &&
                instances.contains(std::string{instance})) {
                return true;
            }
        }
        return false;
    }};
    for (const auto &mod : mods_) {
        if (std::any_of(retiring.begin(), retiring.end(),
                        [&mod](const loaded_mod &retired) {
                            return retired.id == mod.id;
                        })) {
            continue;
        }
        for (const auto &slot : mod.provided_slots) {
            if (!chain_wrapper_position(slot.instance).has_value() ||
                wrapper_is_planned(mod.id, slot.service, slot.instance) ||
                services_.staged_decision(slot.service, slot.instance)
                    .has_value()) {
                continue;
            }
            if (auto staged{
                    services_.stage_retire(slot.service, slot.instance)};
                !staged && staged.error().code != error_code::not_found) {
                abort_staged_internals();
                return std::unexpected{std::move(staged.error())};
            }
            staged_retired_chain_slots_.push_back(slot);
        }
    }

    // The contract check needs the complete staged view: publish the retiring
    // set first, so a reloaded declaration owner is replaced in the effective
    // mod set instead of appearing twice, and an uninstalled owner is absent.
    staged_retire_mods_ = std::move(retiring);
    staged_retire_reasons_ = std::move(retire_reasons);
    staged_drain_timeout_ = options.drain_timeout;

    if (auto contracts{verify_contract_composition()}; !contracts) {
        const auto failure{std::move(contracts.error())};
        abort_staged_internals();
        return std::unexpected{failure};
    }

    if (auto verified{verify_plan_activation(next, true)}; !verified) {
        const auto failure{std::move(verified.error())};
        abort_staged_internals();
        return std::unexpected{failure};
    }

    staged_next_composition_ = next;
    staging_ = false;

    staged_change_ = staged_composition_change{
        .base_epoch = epoch_,
        .added = {},
        .reloaded = {},
        .removed = {},
        .retired_native_generations = retired_native_libraries_.size(),
    };
    for (const auto &mod : staged_mods_) {
        const auto replacing{std::any_of(staged_retire_mods_.begin(),
                                         staged_retire_mods_.end(),
                                         [&mod](const loaded_mod &retired) {
                                             return retired.id == mod.id;
                                         })};
        if (replacing) {
            staged_change_->reloaded.push_back(mod.id);
        } else {
            staged_change_->added.push_back(mod.id);
        }
    }
    for (const auto &[id, reason] : staged_retire_reasons_) {
        if (!std::any_of(staged_mods_.begin(), staged_mods_.end(),
                         [&id = id](const loaded_mod &staged) {
                             return staged.id == id;
                         })) {
            staged_change_->removed.push_back(id);
        }
    }
    std::sort(staged_change_->added.begin(), staged_change_->added.end());
    std::sort(staged_change_->reloaded.begin(), staged_change_->reloaded.end());
    std::sort(staged_change_->removed.begin(), staged_change_->removed.end());
    return *staged_change_;
}

auto host::stage_uninstall(std::string_view id, const stage_options &options)
    -> result<staged_composition_change> {
    const std::lock_guard lifecycle_lock{lifecycle_mutex_};
    if (!loaded_flag_) {
        return std::unexpected{
            fail(error_code::not_initialized,
                 "mod host is not loaded; there is no composition to remove "
                 "a mod from")};
    }
    const auto *mod{find(id)};
    if (mod == nullptr) {
        return std::unexpected{
            fail(error_code::not_found,
                 fmt::format("mod '{}' is not part of the running composition",
                             id))};
    }
    composition next{plan_};
    std::erase_if(next.load_order,
                  [id](const candidate &entry) { return entry.id == id; });
    std::erase_if(next.mods,
                  [id](const locked_mod &entry) { return entry.id == id; });
    for (auto &resolved : next.providers) {
        if (split_provider_ref(resolved.provider).id == id) {
            resolved.provider.clear();
        }
        std::erase_if(resolved.instances, [id](const resolved_instance &entry) {
            return split_provider_ref(entry.provider).id == id;
        });
        std::erase_if(resolved.wrappers, [id](const std::string &wrapper) {
            return split_provider_ref(wrapper).id == id;
        });
    }
    return stage_composition_locked(next, {}, options);
}

void host::abort_staged_internals() noexcept {
    try {
        for (auto &mod : staged_mods_) {
            if (mod.has_code && mod.instance_handle != 0) {
                auto unloaded{
                    dispatch_loader_unload(mod, unload_reason::shutdown)};
                if (!unloaded) {
                    record_quarantine(mod.id, unloaded.error().message);
                }
            }
            for (const auto &slot : mod.provided_slots) {
                if (slot.serial != 0) {
                    static_cast<void>(services_.remove_provider(slot.serial));
                }
            }
        }
        staged_mods_.clear();
        staged_retire_mods_.clear();
        staged_retire_reasons_.clear();
        staged_retired_chain_slots_.clear();
        staged_contract_definitions_.clear();
        staged_drain_timeout_ = default_drain_timeout;
        staged_next_composition_.reset();
        staged_change_.reset();
        staging_ = false;
        current_replaced_handle_.reset();
        current_replaced_generation_ = 0;
        current_replaced_loader_.clear();
        current_mod_.clear();
        current_mod_version_.clear();
        current_mod_provides_.clear();
        current_mod_planned_slots_.clear();
        current_mod_planned_instances_.clear();
        current_mod_chain_slots_.clear();
        current_mod_default_slots_.clear();
        current_mod_selected_provider_ = false;
        current_loader_id_.clear();
        current_loader_sandboxed_ = false;
        current_load_has_code_ = false;
        current_mod_registered_slots_.clear();
        services_.abort_staging();
    } catch (...) {
        // A rollback path must not throw out of a noexcept function; the
        // registry staging is dropped below even if a log/alloc failed.
        services_.abort_staging();
    }
}

void host::abort_staged() noexcept {
    try {
        const std::lock_guard lifecycle_lock{lifecycle_mutex_};
        if (!staging_ && !staged_change_.has_value()) {
            return;
        }
        abort_staged_internals();
    } catch (...) {
        // Aborting must not throw out of the lifecycle layer.
    }
}

auto host::commit_epoch() -> result<composition_commit> {
    // The tick never waits for a package installation: if the command thread
    // is still staging, this tick keeps the committed composition and the next
    // tick boundary publishes the change.
    std::unique_lock lifecycle_lock{lifecycle_mutex_, std::try_to_lock};
    if (!lifecycle_lock.owns_lock()) {
        return composition_commit{.epoch = epoch_,
                                  .digest = composition_digest()};
    }
    if (!loaded_flag_) {
        return std::unexpected{
            fail(error_code::not_initialized, "mod host is not loaded")};
    }
    if (!staged_change_.has_value()) {
        return composition_commit{.epoch = epoch_,
                                  .digest = composition_digest()};
    }

    // Quiesce the slots of every retiring generation and wait a bounded time
    // for calls that are already inside them. This happens at the tick
    // boundary, immediately before the atomic switch, so the staging window
    // (which can be seconds of package IO) never refuses a running tick's
    // calls. A timeout is not a reason to call a retired implementation again:
    // the mod is quarantined and its unload hook is skipped.
    std::map<std::string, std::string> forced_quarantine{};
    std::vector<std::pair<std::string, std::string>> quiesced{};
    for (const auto &mod : staged_retire_mods_) {
        for (const auto &slot : mod.provided_slots) {
            auto quiesced_slot{services_.quiesce(slot.service, slot.instance)};
            if (!quiesced_slot &&
                quiesced_slot.error().code != error_code::not_found) {
                for (const auto &[service, instance] : quiesced) {
                    static_cast<void>(services_.resume(service, instance));
                }
                abort_staged_internals();
                return std::unexpected{std::move(quiesced_slot.error())};
            }
            if (!quiesced_slot) {
                continue;
            }
            quiesced.emplace_back(slot.service, slot.instance);
            auto drained{services_.drain(slot.service, slot.instance,
                                         staged_drain_timeout_)};
            if (!drained) {
                forced_quarantine[mod.id] = drained.error().message;
            }
        }
    }
    // A chain slot the staged plan stops declaring is retired at the same
    // commitment point, so a query racing the switch starts from the base
    // resolver instead of the table leaving the committed chain. The package
    // stays loaded, so a timed-out drain is not a quarantine: the table and its
    // code remain valid after the switch.
    for (const auto &slot : staged_retired_chain_slots_) {
        auto quiesced_slot{services_.quiesce(slot.service, slot.instance)};
        if (!quiesced_slot &&
            quiesced_slot.error().code != error_code::not_found) {
            for (const auto &[service, instance] : quiesced) {
                static_cast<void>(services_.resume(service, instance));
            }
            abort_staged_internals();
            return std::unexpected{std::move(quiesced_slot.error())};
        }
        if (!quiesced_slot) {
            continue;
        }
        quiesced.emplace_back(slot.service, slot.instance);
        static_cast<void>(services_.drain(slot.service, slot.instance,
                                          staged_drain_timeout_));
    }

    // The registry commit is the atomic switch: every staged slot changes at
    // once and the old generations become stale before their code is unloaded.
    if (auto committed{services_.commit_epoch()}; !committed) {
        for (const auto &[service, instance] : quiesced) {
            static_cast<void>(services_.resume(service, instance));
        }
        abort_staged_internals();
        return std::unexpected{std::move(committed.error())};
    }

    const auto change{*staged_change_};
    composition_commit committed{.epoch = epoch_ + 1, .digest = {}};
    // Retire the old generations: the loader's unload hook runs with the
    // reason, native mappings move to the retired list (never dlclose).
    for (const auto &mod : staged_retire_mods_) {
        const auto forced{forced_quarantine.find(mod.id)};
        unload_reason reason{unload_reason::uninstall};
        for (const auto &[retire_id, retire_reason] : staged_retire_reasons_) {
            if (retire_id == mod.id) {
                reason = retire_reason;
            }
        }
        if (forced != forced_quarantine.end()) {
            record_quarantine(mod.id, forced->second);
            committed.quarantined.push_back(mod.id);
        } else {
            auto unloaded{dispatch_loader_unload(mod, reason)};
            if (!unloaded) {
                record_quarantine(mod.id, unloaded.error().message);
                committed.quarantined.push_back(mod.id);
            }
        }
        // The retired candidates are no longer referenced by any committed
        // slot; dropping them keeps the candidate table from growing with
        // every reload.
        for (const auto &slot : mod.provided_slots) {
            if (slot.serial != 0) {
                static_cast<void>(services_.remove_provider(slot.serial));
            }
        }
    }
    // Publish the new mod records, then drop the retired ones.
    for (auto &mod : staged_mods_) {
        mod.state = mod_lifecycle_state::active;
        mod_generations_[mod.id] = mod.generation;
        const auto replacing{std::any_of(staged_retire_mods_.begin(),
                                         staged_retire_mods_.end(),
                                         [&mod](const loaded_mod &retired) {
                                             return retired.id == mod.id;
                                         })};
        if (replacing) {
            committed.reloaded.push_back(mod.id);
        } else {
            committed.added.push_back(mod.id);
        }
    }
    // Drop the retired generations before publishing the replacements: the
    // same mod id appears in both lists and must survive as the new record.
    std::set<std::string> retired_ids{};
    for (const auto &mod : staged_retire_mods_) {
        retired_ids.insert(mod.id);
    }
    std::erase_if(mods_, [&retired_ids](const loaded_mod &mod) {
        return retired_ids.contains(mod.id);
    });
    mods_.insert(mods_.end(), staged_mods_.begin(), staged_mods_.end());
    // The published mod list is the authority on contract declarations too:
    // a definition is retired with the generation that owned it.
    contract_definitions_.clear();
    for (const auto &mod : mods_) {
        for (const auto &definition : mod.contracts) {
            contract_definitions_.insert_or_assign(
                definition.service,
                contract_record{.owner = mod.id, .definition = definition});
        }
    }
    committed.removed = change.removed;
    committed.quarantined.erase(
        std::unique(committed.quarantined.begin(), committed.quarantined.end()),
        committed.quarantined.end());
    std::sort(committed.added.begin(), committed.added.end());
    std::sort(committed.reloaded.begin(), committed.reloaded.end());
    std::sort(committed.quarantined.begin(), committed.quarantined.end());
    committed.retired_native_generations = retired_native_libraries_.size();

    staged_mods_.clear();
    staged_retire_mods_.clear();
    staged_retire_reasons_.clear();
    staged_retired_chain_slots_.clear();
    staged_contract_definitions_.clear();
    staged_drain_timeout_ = default_drain_timeout;
    staged_change_.reset();
    staging_ = false;
    services_.abort_staging();

    epoch_ = committed.epoch;
    if (staged_next_composition_.has_value()) {
        plan_ = std::move(*staged_next_composition_);
    }
    staged_next_composition_.reset();

    committed.digest = composition_digest();
    log_mod.info(
        "committed composition epoch {} with {} added, {} reloaded and {} "
        "removed mod(s), digest {}",
        committed.epoch, committed.added.size(), committed.reloaded.size(),
        committed.removed.size(), committed.digest);
    for (const auto &id : committed.quarantined) {
        log_mod.error(
            "mod {} is quarantined after the composition commit; the server "
            "must be restarted to load it again",
            id);
    }
    return committed;
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

auto host::record_contract_definitions(const mod_manifest &manifest) -> status {
    auto &target{staging_ ? staged_contract_definitions_
                          : contract_definitions_};
    for (const auto &definition : manifest.contracts) {
        const contract_record *existing{nullptr};
        if (const auto staged{
                staged_contract_definitions_.find(definition.service)};
            staged != staged_contract_definitions_.end()) {
            existing = &staged->second;
        } else if (const auto committed{
                       contract_definitions_.find(definition.service)};
                   committed != contract_definitions_.end()) {
            existing = &committed->second;
        }
        if (existing != nullptr && existing->owner != manifest.common.id) {
            if (existing->definition.version != definition.version ||
                existing->definition.vtable_size != definition.vtable_size) {
                return std::unexpected{fail(
                    error_code::version_mismatch,
                    fmt::format("custom contract '{}' is declared by mod '{}' "
                                "as version {} (vtable {} bytes) but mod '{}' "
                                "declares version {} (vtable {} bytes)",
                                definition.service, existing->owner,
                                existing->definition.version.text(),
                                existing->definition.vtable_size,
                                manifest.common.id, definition.version.text(),
                                definition.vtable_size))};
            }
            return std::unexpected{fail(
                error_code::already_exists,
                fmt::format("custom contract '{}' is declared by both mod '{}' "
                            "and mod '{}'; a service id has exactly one "
                            "declaration owner",
                            definition.service, existing->owner,
                            manifest.common.id))};
        }
        target.insert_or_assign(definition.service,
                                contract_record{.owner = manifest.common.id,
                                                .definition = definition});
    }
    return status_ok();
}

auto host::find_contract_definition(std::string_view service) const
    -> const contract_record * {
    for (const auto &record : current_contract_records_) {
        if (record.definition.service == service) {
            return &record;
        }
    }
    const std::string key{service};
    if (const auto staged{staged_contract_definitions_.find(key)};
        staged != staged_contract_definitions_.end()) {
        return &staged->second;
    }
    const auto committed{contract_definitions_.find(key)};
    return committed == contract_definitions_.end() ? nullptr
                                                    : &committed->second;
}

auto host::check_contract_provider(const contract_record &record,
                                   const provider_api &api) const -> status {
    const auto &definition{record.definition};
    vtable_header header{};
    std::memcpy(&header, api.vtable, sizeof(header));
    if (header.struct_size != definition.vtable_size) {
        return std::unexpected{fail(
            error_code::version_mismatch,
            fmt::format(
                "mod '{}' registers a provider for custom contract '{}': the "
                "vtable declares version {}.{}.{} with struct_size {} but the "
                "contract descriptor declares version {} with struct_size {}",
                current_mod_, definition.service, header.abi_major,
                header.abi_minor, header.abi_patch, header.struct_size,
                definition.version.text(), definition.vtable_size))};
    }
    const auto expected_major{definition.version.major};
    const auto expected_minor{definition.version.minor};
    const auto expected_patch{definition.version.patch};
    if (header.abi_major != expected_major ||
        header.abi_minor != expected_minor ||
        header.abi_patch != expected_patch) {
        return std::unexpected{
            fail(error_code::version_mismatch,
                 fmt::format(
                     "mod '{}' registers a provider for custom contract '{}': "
                     "the vtable declares version {}.{}.{} with struct_size {} "
                     "but the contract descriptor declares version {} with "
                     "struct_size {}",
                     current_mod_, definition.service, header.abi_major,
                     header.abi_minor, header.abi_patch, header.struct_size,
                     definition.version.text(), definition.vtable_size))};
    }
    return status_ok();
}

auto host::verify_contract_composition() const -> status {
    // Effective mod set of the composition that would be published: during a
    // hot epoch the staged generations replace their committed ids, otherwise
    // the committed list is the whole composition.
    std::vector<const loaded_mod *> effective{};
    effective.reserve(mods_.size() + staged_mods_.size());
    for (const auto &mod : mods_) {
        const auto retired{staging_ &&
                           std::any_of(staged_retire_mods_.begin(),
                                       staged_retire_mods_.end(),
                                       [&mod](const loaded_mod &candidate) {
                                           return candidate.id == mod.id;
                                       })};
        if (!retired) {
            effective.push_back(&mod);
        }
    }
    if (staging_) {
        for (const auto &mod : staged_mods_) {
            effective.push_back(&mod);
        }
    }

    struct declared_contract {
        const loaded_mod *owner{nullptr};
        const contract_definition *definition{nullptr};
    };
    std::map<std::string, declared_contract> declared{};
    for (const auto *mod : effective) {
        for (const auto &definition : mod->contracts) {
            const auto [found, inserted] = declared.emplace(
                definition.service,
                declared_contract{.owner = mod, .definition = &definition});
            if (inserted) {
                continue;
            }
            const auto &other{found->second};
            if (other.definition->version != definition.version ||
                other.definition->vtable_size != definition.vtable_size) {
                return std::unexpected{fail(
                    error_code::version_mismatch,
                    fmt::format("custom contract '{}' is declared by mod '{}' "
                                "as version {} (vtable {} bytes) but mod '{}' "
                                "declares version {} (vtable {} bytes)",
                                definition.service, other.owner->id,
                                other.definition->version.text(),
                                other.definition->vtable_size, mod->id,
                                definition.version.text(),
                                definition.vtable_size))};
            }
            return std::unexpected{fail(
                error_code::already_exists,
                fmt::format("custom contract '{}' is declared by both mod '{}' "
                            "and mod '{}'; a service id has exactly one "
                            "declaration owner",
                            definition.service, other.owner->id, mod->id))};
        }
    }

    for (const auto *mod : effective) {
        for (const auto &requirement : mod->contract_requires) {
            const auto found{declared.find(requirement.service)};
            if (found == declared.end()) {
                return std::unexpected{fail(
                    error_code::not_found,
                    fmt::format("mod '{}' requires custom contract '{}' range "
                                "'{}' but no mod in the composition declares "
                                "it; the declaring provider must load in the "
                                "same composition",
                                mod->id, requirement.service,
                                requirement.version))};
            }
            const auto &owner{*found->second.owner};
            const auto &definition{*found->second.definition};
            const auto required{core::parse_range(requirement.version)};
            const auto compatible{core::parse_range(definition.compatible)};
            if (!required.has_value() || !compatible.has_value()) {
                // The manifest parser validated both ranges; reaching this
                // means the recorded descriptor is corrupt.
                return std::unexpected{
                    fail(error_code::invalid_argument,
                         fmt::format("custom contract '{}' has an unparsable "
                                     "compatibility declaration",
                                     requirement.service))};
            }
            if (!core::satisfies(definition.version, *required) ||
                !range_within(*required, *compatible)) {
                return std::unexpected{fail(
                    error_code::version_mismatch,
                    fmt::format("mod '{}' requires custom contract '{}' range "
                                "'{}' but mod '{}' declares contract version "
                                "{} with compatible range '{}'; the declared "
                                "version must satisfy the required range and "
                                "the range must lie inside the declared band",
                                mod->id, requirement.service,
                                requirement.version, owner.id,
                                definition.version.text(),
                                definition.compatible))};
            }
        }
    }

    // An active provider table must still match the declaration that is about
    // to be published. A hot epoch can reload the declaring mod without
    // touching the trusted provider that serves the service, so the table
    // registered against the previous descriptor is re-checked here.
    const auto active_view{
        [this](std::string_view service,
               std::string_view instance) -> std::optional<provider_info> {
            if (staging_) {
                return services_.staged_active_provider(service, instance);
            }
            return services_.active_provider(service, instance);
        }};
    for (const auto *mod : effective) {
        for (const auto &slot : mod->provided_slots) {
            const auto declared_it{declared.find(slot.service)};
            if (declared_it == declared.end()) {
                continue;
            }
            const auto active{active_view(slot.service, slot.instance)};
            if (!active.has_value() || active->id != mod->id) {
                continue;
            }
            const auto *api{
                static_cast<const provider_api *>(active->instance)};
            if (api == nullptr || api->vtable == nullptr) {
                continue;
            }
            const auto &definition{*declared_it->second.definition};
            vtable_header header{};
            std::memcpy(&header, api->vtable, sizeof(header));
            if (header.struct_size == definition.vtable_size &&
                header.abi_major == definition.version.major &&
                header.abi_minor == definition.version.minor &&
                header.abi_patch == definition.version.patch) {
                continue;
            }
            return std::unexpected{fail(
                error_code::version_mismatch,
                fmt::format("mod '{}' has an active provider table for custom "
                            "contract '{}' that does not match the published "
                            "declaration: the table declares version {}.{}.{} "
                            "with struct_size {} but the contract descriptor "
                            "declares version {} with struct_size {}",
                            mod->id, slot.service, header.abi_major,
                            header.abi_minor, header.abi_patch,
                            header.struct_size, definition.version.text(),
                            definition.vtable_size))};
        }
    }
    return status_ok();
}

auto host::register_provider(std::string_view service,
                             std::string_view instance, const provider_api &api,
                             std::string_view version,
                             kernel::capability_bits capabilities) -> status {
    if (failed_ || current_mod_.empty() || (loaded_flag_ && !staging_)) {
        return std::unexpected{fail(
            error_code::not_initialized,
            "register_provider() is only legal from a mod factory while the "
            "host is loading a composition")};
    }
    if (service == loader_abi::service_id) {
        // A loader provider is host-level code: the registration path has its
        // own gates (planned loader declaration, first-party loader of the
        // providing mod, package source) and records the loader contract
        // instead of an ordinary service slot.
        return register_mod_loader_from_mod(instance, api, version);
    }
    if (current_loader_sandboxed_) {
        return std::unexpected{fail(
            error_code::permission_denied,
            fmt::format("sandboxed loader '{}' cannot register service '{}' "
                        "for mod '{}'; a sandboxed guest only reaches the host "
                        "through its imports",
                        current_loader_id_, service, current_mod_))};
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
    const auto chain_binding{current_mod_chain_slots_.find(*planned_slot)};
    const bool chain_wrapper{chain_binding != current_mod_chain_slots_.end() &&
                             !chain_binding->second.empty()};
    const bool chain_base{current_mod_default_slots_.contains(*planned_slot)};
    if (chain_wrapper && chain_base) {
        return std::unexpected{fail(
            error_code::permission_denied,
            fmt::format("mod '{}' cannot provide service '{}': the gamepack "
                        "plan binds it as both the chain base and a chain "
                        "wrapper, and one provider table cannot occupy two "
                        "composition slots",
                        current_mod_, service))};
    }
    if (chain_wrapper && instanced) {
        return std::unexpected{fail(
            error_code::permission_denied,
            fmt::format("mod '{}' cannot provide service '{}': the gamepack "
                        "plan binds it as both a chain wrapper and an "
                        "instanced provider",
                        current_mod_, service))};
    }
    // The registry instance the registration targets: the plan's instanced
    // name, the mod's own chain wrapper slot, or the reserved default instance.
    std::string resolved_instance{};
    if (chain_wrapper) {
        if (chain_binding->second.size() > 1U) {
            return std::unexpected{fail(
                error_code::permission_denied,
                fmt::format("mod '{}' cannot provide service '{}': the "
                            "gamepack plan declares it at more than one chain "
                            "wrapper position, and one provider table can only "
                            "occupy one slot",
                            current_mod_, service))};
        }
        resolved_instance = *chain_binding->second.begin();
        if (!instance.empty() && instance != resolved_instance &&
            instance != kernel::service_registry::default_instance) {
            return std::unexpected{fail(
                error_code::permission_denied,
                fmt::format("mod '{}' cannot activate service '{}' on instance "
                            "'{}': the plan binds it as the chain wrapper on "
                            "slot '{}'",
                            current_mod_, service, instance,
                            resolved_instance))};
        }
        if (instance == kernel::service_registry::default_instance) {
            return std::unexpected{fail(
                error_code::permission_denied,
                fmt::format("mod '{}' cannot activate service '{}' on the "
                            "default instance: the plan binds it as the chain "
                            "wrapper on slot '{}', and the default instance "
                            "belongs to the chain base",
                            current_mod_, service, resolved_instance))};
        }
    } else if (instanced) {
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
        resolved_instance = std::string{instance};
    } else if (!instance.empty() &&
               instance != kernel::service_registry::default_instance) {
        return std::unexpected{fail(
            error_code::permission_denied,
            fmt::format("mod '{}' cannot activate service '{}' on instance "
                        "'{}': an exclusive/chain binding only has the "
                        "default instance",
                        current_mod_, service, instance))};
    }

    // A custom service id may only be provided against the contract a loaded
    // mod declares: the declaring owner (or a mod the owner lists as trusted)
    // registers a provider table whose vtable header matches the descriptor.
    if (is_custom_service_id(service)) {
        const auto *record{find_contract_definition(service)};
        if (record == nullptr) {
            return std::unexpected{fail(
                error_code::permission_denied,
                fmt::format("mod '{}' cannot provide custom service '{}': no "
                            "loaded mod declares it in [contracts]; a custom "
                            "contract must be declared by a mod package before "
                            "a provider can register for it",
                            current_mod_, service))};
        }
        const auto trusted{
            std::find(record->definition.trusted_providers.begin(),
                      record->definition.trusted_providers.end(),
                      current_mod_) !=
            record->definition.trusted_providers.end()};
        if (record->owner != current_mod_ && !trusted) {
            return std::unexpected{fail(
                error_code::permission_denied,
                fmt::format("mod '{}' cannot provide custom service '{}': mod "
                            "'{}' declares the contract and does not list '{}' "
                            "as a trusted provider",
                            current_mod_, service, record->owner,
                            current_mod_))};
        }
        if (auto checked{check_contract_provider(*record, api)}; !checked) {
            return checked;
        }
    }

    const auto effective_version{version.empty() ? current_mod_version_
                                                 : version};
    if (resolved_instance.empty()) {
        // The default instance normalizes to its reserved name so the slot a
        // mod records is the same name a later retire/quiesce addresses.
        resolved_instance =
            std::string{kernel::service_registry::default_instance};
    }
    provider_apis_.push_back(std::make_unique<provider_api>(api));
    provider_info info{};
    info.service = std::string{service};
    info.id = current_mod_;
    info.version = std::string{effective_version};
    info.capabilities = capabilities;
    info.instance = provider_apis_.back().get();
    info.source = "builtin";
    if (staging_) {
        // A staged generation coexists with the running one: the candidate may
        // share its id/version text with the committed candidate, and the
        // activation is not visible until the tick-boundary commit.
        auto serial{services_.add_staged_provider(std::move(info))};
        if (!serial) {
            return std::unexpected{std::move(serial.error())};
        }
        if (auto staged{
                services_.stage_active(*serial, service, resolved_instance)};
            !staged) {
            static_cast<void>(services_.remove_provider(*serial));
            return std::unexpected{std::move(staged.error())};
        }
        current_mod_registered_slots_.push_back(
            provided_slot{.service = std::string{service},
                          .instance = resolved_instance,
                          .serial = *serial});
        return status_ok();
    }
    auto serial{services_.add_provider_serial(std::move(info))};
    if (!serial) {
        return std::unexpected{std::move(serial.error())};
    }
    if (auto selected{services_.select_active(
            service, current_mod_, effective_version, resolved_instance)};
        !selected) {
        static_cast<void>(services_.remove_provider(*serial));
        return std::unexpected{std::move(selected.error())};
    }
    current_mod_registered_slots_.push_back(
        provided_slot{.service = std::string{service},
                      .instance = resolved_instance,
                      .serial = *serial});
    return status_ok();
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
    auto out{fmt::format(
        "mod host: {} mod(s) loaded at composition epoch {} (digest {}), "
        "{} retired native mapping(s), built-in image BLAKE3 {}\n",
        mods_.size(), epoch_, composition_digest(),
        retired_native_libraries_.size(), builtin_image_digest_hex_)};
    out += "  loaders:";
    if (loaders_.empty()) {
        out += " none";
    }
    for (const auto &loader : loaders_) {
        out +=
            fmt::format(" {} abi {} unload {}{}{}{};", loader_identity(loader),
                        loader.abi_major, to_string(loader.unload),
                        loader.sandboxed ? " sandboxed" : "",
                        loader.supports_hot_install ? " hot-install" : "",
                        loader.provider.empty()
                            ? std::string{}
                            : fmt::format(" from mod {}", loader.provider));
    }
    out += '\n';
    for (const auto &mod : mods_) {
        // Order the fields as the audit text always had them (`abi`, code
        // state, provides) and append the loader record, so log greps keep
        // matching while the loader stays visible.
        out += fmt::format(
            "  {}@{} (abi {}, {}{}, loader {}{}, unload {}, generation {}, "
            "state {})",
            mod.id, mod.version, mod.abi, mod.has_code ? "code" : "data-only",
            mod.provides_summary.empty() ? ""
                                         : ", provides " + mod.provides_summary,
            mod.loader, mod.loader_sandboxed ? " sandboxed" : "",
            to_string(mod.loader_unload), mod.generation, to_string(mod.state));
        out += '\n';
    }
    return out;
}

} // namespace refrain::mod
