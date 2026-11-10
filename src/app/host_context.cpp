// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/app/host_context.hpp>

#include <refrain/core/io.hpp>
#include <refrain/core/log.hpp>
#include <refrain/core/memory.hpp>
#include <refrain/core/time.hpp>
#include <refrain/kernel/crash.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/platform/mmap.hpp>
#include <refrain/platform/random_access_file.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace refrain::app {
namespace {

namespace host_platform = refrain::host::platform;
namespace host_kernel = refrain::host::kernel;
namespace file_api = refrain::platform::file;

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
    if (!view || !view->valid()) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::not_found,
            "config namespace '" + std::string{ns} + "' is not mounted")};
    }
    if (!view->contains(key)) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::not_found,
            "config key is missing in namespace '" + std::string{ns} + "'")};
    }
    if (view->kind(key) != refrain::core::config::value_kind::integer) {
        return std::unexpected{
            refrain::make_error(refrain::error_code::invalid_argument,
                                "config key is not an integer")};
    }
    return view->get_int64(key, 0);
}

auto host_context::config_bool(std::string_view ns, std::string_view key)
    -> refrain::result<bool> {
    if (config_ == nullptr) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::unsupported, "host config store is disabled")};
    }
    const auto view{config_->module(ns)};
    if (!view || !view->valid()) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::not_found,
            "config namespace '" + std::string{ns} + "' is not mounted")};
    }
    if (!view->contains(key)) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::not_found,
            "config key is missing in namespace '" + std::string{ns} + "'")};
    }
    if (view->kind(key) != refrain::core::config::value_kind::boolean) {
        return std::unexpected{
            refrain::make_error(refrain::error_code::invalid_argument,
                                "config key is not a boolean")};
    }
    return view->get_bool(key, false);
}

auto host_context::config_string(std::string_view ns, std::string_view key)
    -> refrain::result<std::string_view> {
    if (config_ == nullptr) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::unsupported, "host config store is disabled")};
    }
    const auto view{config_->module(ns)};
    if (!view || !view->valid()) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::not_found,
            "config namespace '" + std::string{ns} + "' is not mounted")};
    }
    if (!view->contains(key)) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::not_found,
            "config key is missing in namespace '" + std::string{ns} + "'")};
    }
    if (view->kind(key) != refrain::core::config::value_kind::string) {
        return std::unexpected{
            refrain::make_error(refrain::error_code::invalid_argument,
                                "config key is not a string")};
    }
    try {
        config_string_buffer = view->get_string(key, {});
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

auto host_context::file_open(std::string_view path,
                             host_platform::file_open_flags flags)
    -> refrain::result<host_platform::file> {
    const auto bits{static_cast<std::uint32_t>(flags)};
    const auto read_only{
        (bits & REFRAIN_HOST_PLATFORM_FILE_OPEN_FLAGS_READ_ONLY) != 0};
    const auto read_write{
        (bits & REFRAIN_HOST_PLATFORM_FILE_OPEN_FLAGS_READ_WRITE) != 0};
    if (read_only == read_write) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::invalid_argument,
            "file_open requires exactly one of read_only/read_write")};
    }
    const file_api::open_options options{
        .create = (bits & REFRAIN_HOST_PLATFORM_FILE_OPEN_FLAGS_CREATE) != 0,
        .exclusive =
            (bits & REFRAIN_HOST_PLATFORM_FILE_OPEN_FLAGS_EXCLUSIVE) != 0,
        .truncate =
            (bits & REFRAIN_HOST_PLATFORM_FILE_OPEN_FLAGS_TRUNCATE) != 0,
        .no_follow =
            (bits & REFRAIN_HOST_PLATFORM_FILE_OPEN_FLAGS_NO_FOLLOW) != 0,
    };
    auto file{file_api::random_access_file::open(
        path,
        read_only ? file_api::open_mode::read_only
                  : file_api::open_mode::read_write,
        options)};
    if (!file) {
        return std::unexpected{std::move(file.error())};
    }
    std::scoped_lock lock{platform_mutex_};
    const auto handle{next_handle_++};
    files_.emplace(handle, file_entry{std::move(*file), read_only});
    return host_platform::file{handle};
}

auto host_context::file_close(host_platform::file handle) -> refrain::status {
    std::scoped_lock lock{platform_mutex_};
    const auto erased{files_.erase(handle.raw())};
    if (erased == 0) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::not_found, "file handle is not open")};
    }
    return refrain::status_ok();
}

auto host_context::file_read(host_platform::file handle, std::uint64_t offset,
                             std::span<std::uint8_t> buffer)
    -> refrain::result<std::uint64_t> {
    std::scoped_lock lock{platform_mutex_};
    const auto found{files_.find(handle.raw())};
    if (found == files_.end()) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::not_found, "file handle is not open")};
    }
    auto read{
        found->second.file.read_at(offset, std::as_writable_bytes(buffer))};
    if (!read) {
        return std::unexpected{std::move(read.error())};
    }
    return static_cast<std::uint64_t>(buffer.size());
}

auto host_context::file_write(host_platform::file handle, std::uint64_t offset,
                              std::span<const std::uint8_t> data)
    -> refrain::result<std::uint64_t> {
    std::scoped_lock lock{platform_mutex_};
    const auto found{files_.find(handle.raw())};
    if (found == files_.end()) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::not_found, "file handle is not open")};
    }
    auto written{found->second.file.write_at(offset, std::as_bytes(data))};
    if (!written) {
        return std::unexpected{std::move(written.error())};
    }
    return static_cast<std::uint64_t>(data.size());
}

auto host_context::file_size(host_platform::file handle)
    -> refrain::result<std::uint64_t> {
    std::scoped_lock lock{platform_mutex_};
    const auto found{files_.find(handle.raw())};
    if (found == files_.end()) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::not_found, "file handle is not open")};
    }
    return found->second.file.size();
}

auto host_context::file_sync(host_platform::file handle) -> refrain::status {
    std::scoped_lock lock{platform_mutex_};
    const auto found{files_.find(handle.raw())};
    if (found == files_.end()) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::not_found, "file handle is not open")};
    }
    return found->second.file.sync();
}

auto host_context::file_exists(std::string_view path) -> bool {
    return refrain::core::io::exists(path);
}

auto host_context::mmap_open(std::string_view path)
    -> refrain::result<host_platform::map> {
    auto mapping{refrain::platform::mapped_file::open(path)};
    if (!mapping) {
        return std::unexpected{std::move(mapping.error())};
    }
    std::scoped_lock lock{platform_mutex_};
    const auto handle{next_handle_++};
    maps_.emplace(handle, std::move(*mapping));
    return host_platform::map{handle};
}

auto host_context::mmap_data(host_platform::map handle)
    -> refrain::result<std::span<const std::uint8_t>> {
    std::scoped_lock lock{platform_mutex_};
    const auto found{maps_.find(handle.raw())};
    if (found == maps_.end()) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::not_found, "mapping handle is not open")};
    }
    const auto data{found->second.data()};
    return std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t *>(data.data()), data.size()};
}

auto host_context::mmap_close(host_platform::map handle) -> refrain::status {
    std::scoped_lock lock{platform_mutex_};
    const auto erased{maps_.erase(handle.raw())};
    if (erased == 0) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::not_found, "mapping handle is not open")};
    }
    return refrain::status_ok();
}

void host_context::crash_annotation(std::string_view key,
                                    std::string_view value) noexcept {
    refrain::kernel::crash_reporter::set_annotation(key, value);
}

auto host_context::temp_directory() noexcept -> std::string_view {
    static thread_local std::string path;
    path = refrain::core::io::temp_directory();
    return path;
}

auto host_context::job_submit(std::uint32_t priority,
                              host_kernel::job_callback callback,
                              void *userdata)
    -> refrain::result<host_kernel::job> {
    if (callback == nullptr || priority > 3) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::invalid_argument,
            "job_submit requires a callback and priority in 0..3")};
    }
    std::scoped_lock lock{kernel_mutex_};
    if (!jobs_) {
        try {
            jobs_ = std::make_unique<refrain::core::jobs::executor>(1);
        } catch (...) {
            return std::unexpected{
                refrain::make_error(refrain::error_code::out_of_memory,
                                    "cannot start the host job worker")};
        }
    }
    auto state{std::make_shared<job_state>()};
    const auto handle{next_job_++};
    job_states_.emplace(handle, state);
    try {
        static_cast<void>(jobs_->submit(
            static_cast<refrain::core::jobs::priority>(priority),
            [state, callback, userdata] {
                try {
                    if (!state->cancelled.load(std::memory_order_acquire)) {
                        callback(userdata);
                    }
                } catch (...) {
                    // A native callback must not take down the worker.
                }
                state->promise.set_value();
            }));
    } catch (...) {
        job_states_.erase(handle);
        return std::unexpected{refrain::make_error(
            refrain::error_code::out_of_memory, "cannot submit host job")};
    }
    return host_kernel::job{handle};
}

auto host_context::job_cancel(host_kernel::job handle) -> refrain::status {
    std::scoped_lock lock{kernel_mutex_};
    const auto found{job_states_.find(handle.raw())};
    if (found == job_states_.end()) {
        return std::unexpected{refrain::make_error(
            refrain::error_code::not_found, "job handle is not active")};
    }
    found->second->cancelled.store(true, std::memory_order_release);
    return refrain::status_ok();
}

auto host_context::job_wait(host_kernel::job handle) -> refrain::status {
    std::shared_ptr<job_state> state{};
    {
        std::scoped_lock lock{kernel_mutex_};
        const auto found{job_states_.find(handle.raw())};
        if (found == job_states_.end()) {
            return std::unexpected{refrain::make_error(
                refrain::error_code::not_found, "job handle is not active")};
        }
        state = found->second;
    }
    state->future.wait();
    std::scoped_lock lock{kernel_mutex_};
    job_states_.erase(handle.raw());
    return refrain::status_ok();
}

auto host_context::event_subscribe(std::uint64_t event_type,
                                   host_kernel::event_callback callback,
                                   void *userdata)
    -> refrain::result<host_kernel::subscription> {
    if (callback == nullptr) {
        return std::unexpected{
            refrain::make_error(refrain::error_code::invalid_argument,
                                "event_subscribe requires a callback")};
    }
    std::scoped_lock lock{kernel_mutex_};
    const auto handle{next_subscription_++};
    events_.emplace(handle, event_state{.type = event_type,
                                        .callback = callback,
                                        .userdata = userdata});
    return host_kernel::subscription{handle};
}

auto host_context::event_unsubscribe(host_kernel::subscription handle)
    -> refrain::status {
    std::scoped_lock lock{kernel_mutex_};
    if (events_.erase(handle.raw()) == 0) {
        return std::unexpected{
            refrain::make_error(refrain::error_code::not_found,
                                "subscription handle is not active")};
    }
    return refrain::status_ok();
}

void host_context::event_publish(std::uint64_t event_type, void *payload,
                                 std::uint64_t size) {
    std::vector<event_state> callbacks{};
    {
        std::scoped_lock lock{kernel_mutex_};
        for (const auto &[handle, state] : events_) {
            static_cast<void>(handle);
            if (state.type == event_type) {
                callbacks.push_back(state);
            }
        }
    }
    for (const auto &state : callbacks) {
        try {
            state.callback(state.userdata, payload, size);
        } catch (...) {
            // A native callback must not take down the publisher.
        }
    }
}

} // namespace refrain::app
