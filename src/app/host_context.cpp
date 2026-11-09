// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/app/host_context.hpp>

#include <refrain/core/log.hpp>
#include <refrain/core/memory.hpp>
#include <refrain/core/time.hpp>
#include <refrain/mod/host.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <span>
#include <string_view>
#include <utility>

namespace refrain::app {
namespace {

constexpr std::uint64_t max_alignment{4096};

struct allocation_header {
    void *base;
    std::uint64_t bytes;
    std::uint64_t alignment;
};

constexpr std::uint64_t min_alignment{alignof(allocation_header)};

[[nodiscard]] auto is_valid_alignment(std::uint64_t alignment) noexcept
    -> bool {
    return alignment >= min_alignment && std::has_single_bit(alignment) &&
           alignment <= max_alignment;
}

} // namespace

host_context::host_context(refrain::mod::host &owner,
                           std::function<std::int64_t()> tick,
                           const refrain::core::config::store *config)
    : owner_{owner}, tick_{std::move(tick)}, config_{config} {}

namespace {
thread_local std::string config_string_buffer;
} // namespace

void host_context::log(std::uint32_t level, std::string_view message) noexcept {
    auto mapped{refrain::log::level::info};
    if (level <= static_cast<std::uint32_t>(refrain::log::level::critical)) {
        mapped = static_cast<refrain::log::level>(level);
    }
    refrain::log::write(mapped, message);
}

auto host_context::now_ns() const noexcept -> std::int64_t {
    return refrain::core::now_ns();
}

auto host_context::tick_now() const noexcept -> std::int64_t {
    return tick_ ? tick_() : -1;
}

void host_context::version(
    refrain::host::core::host_version &out) const noexcept {
    out.major = REFRAIN_HOST_CORE_ABI_MAJOR;
    out.minor = REFRAIN_HOST_CORE_ABI_MINOR;
    out.patch = REFRAIN_HOST_CORE_ABI_PATCH;
}

auto host_context::config_i64(std::string_view ns, std::string_view key)
    -> refrain::result<std::int64_t> {
    if (config_ == nullptr) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::unsupported, "host config store is disabled")};
    }
    const auto view{config_->module(ns)};
    if (!view.valid() || !view.contains(key)) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::not_found,
            "config key is missing in namespace '" + std::string{ns} + "'")};
    }
    if (view.kind(key) != refrain::core::config::value_kind::integer) {
        return std::unexpected{
            refrain::make_error(refrain::error_code::invalid_argument,
                                "config key is not an integer")};
    }
    return view.get_int64(key, 0);
}

auto host_context::config_bool(std::string_view ns, std::string_view key)
    -> refrain::result<bool> {
    if (config_ == nullptr) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::unsupported, "host config store is disabled")};
    }
    const auto view{config_->module(ns)};
    if (!view.valid() || !view.contains(key)) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::not_found,
            "config key is missing in namespace '" + std::string{ns} + "'")};
    }
    if (view.kind(key) != refrain::core::config::value_kind::boolean) {
        return std::unexpected{
            refrain::make_error(refrain::error_code::invalid_argument,
                                "config key is not a boolean")};
    }
    return view.get_bool(key, false);
}

auto host_context::config_string(std::string_view ns, std::string_view key)
    -> refrain::result<std::string_view> {
    if (config_ == nullptr) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::unsupported, "host config store is disabled")};
    }
    const auto view{config_->module(ns)};
    if (!view.valid() || !view.contains(key)) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::not_found,
            "config key is missing in namespace '" + std::string{ns} + "'")};
    }
    if (view.kind(key) != refrain::core::config::value_kind::string) {
        return std::unexpected{
            refrain::make_error(refrain::error_code::invalid_argument,
                                "config key is not a string")};
    }
    try {
        config_string_buffer = view.get_string(key, {});
    } catch (const std::bad_alloc &) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::out_of_memory, "config string copy failed")};
    }
    return std::string_view{config_string_buffer};
}

auto host_context::query_service(std::string_view service,
                                 std::string_view instance)
    -> refrain::result<refrain::host::core::provider_ref> {
    auto active{owner_.services().active_provider(service, instance)};
    if (!active) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::not_found,
            "service is not active in the frozen registry")};
    }
    const auto *api{
        static_cast<const refrain::mod::provider_api *>(active->instance)};
    if (api == nullptr || api->vtable == nullptr || api->self == nullptr) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::unknown, "active provider has no ABI table")};
    }
    refrain::host::core::provider_ref ref{};
    ref.vtable = const_cast<void *>(api->vtable);
    ref.self = api->self;
    ref.abi_major = api->abi_major;
    return ref;
}

auto host_context::register_provider(
    std::string_view service, std::string_view instance,
    const refrain::host::core::provider_ref &ref) -> refrain::status {
    if (ref.vtable == nullptr || ref.self == nullptr || ref.abi_major == 0) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::invalid_argument,
            "register_provider requires vtable, self and abi_major")};
    }
    const refrain::mod::provider_api api{
        .vtable = ref.vtable, .self = ref.self, .abi_major = ref.abi_major};
    return owner_.register_provider(service, instance, api);
}

auto host_context::alloc(std::uint64_t size, std::uint64_t alignment)
    -> refrain::result<std::span<std::uint8_t>> {
    if (size == 0) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::invalid_argument, "alloc size is zero")};
    }
    if (!is_valid_alignment(alignment)) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::invalid_argument,
            "alloc alignment is not a power of two in [8, 4096]")};
    }
    constexpr std::uint64_t header_size{sizeof(allocation_header)};
    if (size >
        std::numeric_limits<std::uint64_t>::max() - alignment - header_size) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::out_of_memory, "alloc size overflow")};
    }
    const auto total{size + alignment + header_size};
    auto *resource{refrain::core::memory::default_resource()};
    void *base{nullptr};
    try {
        base = resource->allocate(static_cast<std::size_t>(total),
                                  static_cast<std::size_t>(alignment));
    } catch (const std::bad_alloc &) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::out_of_memory, "alloc failed")};
    }
    auto *cursor{static_cast<std::byte *>(base) + header_size};
    auto space{static_cast<std::size_t>(total - header_size)};
    void *aligned{cursor};
    if (std::align(static_cast<std::size_t>(alignment),
                   static_cast<std::size_t>(size), aligned, space) == nullptr) {
        resource->deallocate(base, static_cast<std::size_t>(total),
                             static_cast<std::size_t>(alignment));
        return std::unexpected{refrain::make_error(
            refrain::error_code::out_of_memory, "alloc alignment failed")};
    }
    auto *header{reinterpret_cast<allocation_header *>(
        static_cast<std::byte *>(aligned) - header_size)};
    header->base = base;
    header->bytes = total;
    header->alignment = alignment;
    return std::span<std::uint8_t>{static_cast<std::uint8_t *>(aligned),
                                   static_cast<std::size_t>(size)};
}

void host_context::free(std::span<std::uint8_t> block) noexcept {
    if (block.data() == nullptr || block.empty()) {
        return;
    }
    constexpr std::uint64_t header_size{sizeof(allocation_header)};
    auto *header{
        reinterpret_cast<allocation_header *>(block.data() - header_size)};
    refrain::core::memory::default_resource()->deallocate(
        header->base, static_cast<std::size_t>(header->bytes),
        static_cast<std::size_t>(header->alignment));
}

} // namespace refrain::app
