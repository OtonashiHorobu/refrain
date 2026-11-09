// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// `refrain:host.kernel` C ABI adapter. Job callbacks are plain C function
// pointers; the concrete kernel context owns the worker pool and event table.

#include <refrain/host/kernel_api.hpp>

#include <refrain/contracts/abi.hpp>

#include <cstdint>

namespace refrain::host {
namespace {

namespace host_kernel = refrain::host::kernel;

[[nodiscard]] auto to_kernel_error(error_code code) noexcept
    -> host_kernel::kernel_error {
    switch (code) {
    case error_code::success:
    case error_code::unknown:
        return host_kernel::kernel_error::internal;
    case error_code::invalid_argument:
        return host_kernel::kernel_error::invalid_argument;
    case error_code::not_found:
        return host_kernel::kernel_error::not_found;
    case error_code::already_exists:
        return host_kernel::kernel_error::invalid_argument;
    case error_code::permission_denied:
        return host_kernel::kernel_error::permission_denied;
    case error_code::io_error:
        return host_kernel::kernel_error::internal;
    case error_code::parse_error:
        return host_kernel::kernel_error::internal;
    case error_code::version_mismatch:
    case error_code::unsupported:
        return host_kernel::kernel_error::unsupported;
    case error_code::not_initialized:
        return host_kernel::kernel_error::internal;
    case error_code::timeout:
        return host_kernel::kernel_error::timeout;
    case error_code::cancelled:
        return host_kernel::kernel_error::cancelled;
    case error_code::out_of_memory:
        return host_kernel::kernel_error::out_of_memory;
    case error_code::mod_error:
        return host_kernel::kernel_error::internal;
    }
    return host_kernel::kernel_error::internal;
}

[[nodiscard]] auto context_of(void *self) noexcept -> kernel_context * {
    return static_cast<kernel_context *>(self);
}

auto host_job_submit(void *self, std::uint32_t priority,
                     refrain_host_kernel_job_callback callback, void *userdata,
                     refrain_host_kernel_handle_job *out) -> refrain_status {
    if (self == nullptr || callback == nullptr || out == nullptr) {
        return static_cast<refrain_status>(
            host_kernel::kernel_error::invalid_argument);
    }
    auto handle{context_of(self)->job_submit(
        priority, static_cast<host_kernel::job_callback>(callback), userdata)};
    if (!handle) {
        return static_cast<refrain_status>(
            to_kernel_error(handle.error().code));
    }
    *out = handle->raw();
    return REFRAIN_STATUS_OK;
}

auto host_job_cancel(void *self, refrain_host_kernel_handle_job handle)
    -> refrain_status {
    if (self == nullptr) {
        return static_cast<refrain_status>(
            host_kernel::kernel_error::invalid_argument);
    }
    auto cancelled{context_of(self)->job_cancel(host_kernel::job{handle})};
    return cancelled ? REFRAIN_STATUS_OK
                     : static_cast<refrain_status>(
                           to_kernel_error(cancelled.error().code));
}

auto host_job_wait(void *self, refrain_host_kernel_handle_job handle)
    -> refrain_status {
    if (self == nullptr) {
        return static_cast<refrain_status>(
            host_kernel::kernel_error::invalid_argument);
    }
    auto waited{context_of(self)->job_wait(host_kernel::job{handle})};
    return waited ? REFRAIN_STATUS_OK
                  : static_cast<refrain_status>(
                        to_kernel_error(waited.error().code));
}

auto host_event_subscribe(void *self, std::uint64_t event_type,
                          refrain_host_kernel_event_callback callback,
                          void *userdata,
                          refrain_host_kernel_handle_subscription *out)
    -> refrain_status {
    if (self == nullptr || callback == nullptr || out == nullptr) {
        return static_cast<refrain_status>(
            host_kernel::kernel_error::invalid_argument);
    }
    auto handle{context_of(self)->event_subscribe(
        event_type, static_cast<host_kernel::event_callback>(callback),
        userdata)};
    if (!handle) {
        return static_cast<refrain_status>(
            to_kernel_error(handle.error().code));
    }
    *out = handle->raw();
    return REFRAIN_STATUS_OK;
}

auto host_event_unsubscribe(void *self,
                            refrain_host_kernel_handle_subscription handle)
    -> refrain_status {
    if (self == nullptr) {
        return static_cast<refrain_status>(
            host_kernel::kernel_error::invalid_argument);
    }
    auto removed{
        context_of(self)->event_unsubscribe(host_kernel::subscription{handle})};
    return removed ? REFRAIN_STATUS_OK
                   : static_cast<refrain_status>(
                         to_kernel_error(removed.error().code));
}

void host_event_publish(void *self, std::uint64_t event_type, void *payload,
                        std::uint64_t size) {
    if (self == nullptr) {
        return;
    }
    context_of(self)->event_publish(event_type, payload, size);
}

} // namespace

auto kernel_vtable() noexcept -> const refrain_host_kernel_vtable & {
    static const refrain_host_kernel_vtable table{
        .struct_size = REFRAIN_HOST_KERNEL_VTABLE_SIZE,
        .abi_major = REFRAIN_HOST_KERNEL_ABI_MAJOR,
        .abi_minor = REFRAIN_HOST_KERNEL_ABI_MINOR,
        .abi_patch = REFRAIN_HOST_KERNEL_ABI_PATCH,
        .job_submit = &host_job_submit,
        .job_cancel = &host_job_cancel,
        .job_wait = &host_job_wait,
        .event_subscribe = &host_event_subscribe,
        .event_unsubscribe = &host_event_unsubscribe,
        .event_publish = &host_event_publish,
    };
    return table;
}

} // namespace refrain::host
