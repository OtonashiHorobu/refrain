// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// `refrain:host.core` C ABI adapter. Every entry point validates its out
// pointers, maps `refrain::error` to `host_error`, records a thread-local
// diagnostic, and never lets an exception cross the ABI.

#include <refrain/host/core_api.hpp>

#include <refrain/contracts/abi.hpp>
#include <refrain/core/log.hpp>
#include <refrain/host/abi_bridge.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace refrain::host {
namespace {

namespace abi = refrain::host::abi;

thread_local std::string last_error;

void clear_error() noexcept {
    // A failed host call is a cold path; an allocation failure while clearing
    // the message only loses diagnostics.
    try {
        last_error.clear();
    } catch (...) {
        last_error = {};
    }
}

void set_error(const error &failure) noexcept {
    try {
        last_error = failure.message.empty()
                         ? std::string{to_string(failure.code)}
                         : failure.message;
    } catch (...) {
        last_error.clear();
    }
}

void set_error(std::string_view message) noexcept {
    try {
        last_error = message;
    } catch (...) {
        last_error.clear();
    }
}

[[nodiscard]] auto to_host_error(error_code code) noexcept -> core::host_error {
    switch (code) {
    case error_code::success:
    case error_code::unknown:
        return core::host_error::internal;
    case error_code::invalid_argument:
        return core::host_error::invalid_argument;
    case error_code::not_found:
        return core::host_error::not_found;
    case error_code::already_exists:
        return core::host_error::already_exists;
    case error_code::permission_denied:
        return core::host_error::permission_denied;
    case error_code::io_error:
        return core::host_error::io_error;
    case error_code::parse_error:
        return core::host_error::internal;
    case error_code::version_mismatch:
        return core::host_error::version_mismatch;
    case error_code::unsupported:
        return core::host_error::unsupported;
    case error_code::not_initialized:
        return core::host_error::not_initialized;
    case error_code::timeout:
    case error_code::cancelled:
        return core::host_error::internal;
    case error_code::out_of_memory:
        return core::host_error::out_of_memory;
    case error_code::mod_error:
        return core::host_error::internal;
    }
    return core::host_error::internal;
}

[[nodiscard]] auto log_level(std::uint32_t level) noexcept -> log::level {
    if (level <= static_cast<std::uint32_t>(log::level::critical)) {
        return static_cast<log::level>(level);
    }
    return log::level::info;
}

[[nodiscard]] auto context_of(void *self) noexcept -> context * {
    return static_cast<context *>(self);
}

void host_version(void *self, refrain_host_core_host_version *out) {
    if (self == nullptr || out == nullptr) {
        return;
    }
    out->struct_size = REFRAIN_HOST_CORE_HOST_VERSION_SIZE;
    try {
        context_of(self)->version(*out);
    } catch (...) {
        // The version table is informational; keep the struct_size field.
    }
}

void host_log(void *self, std::uint32_t level, refrain_str message) {
    if (self == nullptr) {
        return;
    }
    try {
        context_of(self)->log(static_cast<std::uint32_t>(log_level(level)),
                              abi::to_view(message));
    } catch (...) {
        // A log sink must never terminate the caller.
    }
}

auto host_now_ns(void *self) -> std::int64_t {
    if (self == nullptr) {
        return 0;
    }
    try {
        return context_of(self)->now_ns();
    } catch (...) {
        return 0;
    }
}

auto host_tick_now(void *self) -> std::int64_t {
    if (self == nullptr) {
        return 0;
    }
    try {
        return context_of(self)->tick_now();
    } catch (...) {
        return -1;
    }
}

auto host_config_i64(void *self, refrain_str ns, refrain_str key,
                     std::int64_t *out) -> refrain_status {
    if (self == nullptr || out == nullptr) {
        clear_error();
        set_error("config_get_i64: null argument");
        return static_cast<refrain_status>(core::host_error::invalid_argument);
    }
    clear_error();
    try {
        auto value{
            context_of(self)->config_i64(abi::to_view(ns), abi::to_view(key))};
        if (!value) {
            set_error(value.error());
            return static_cast<refrain_status>(
                to_host_error(value.error().code));
        }
        *out = *value;
    } catch (...) {
        set_error("config_get_i64: host threw");
        return static_cast<refrain_status>(core::host_error::internal);
    }
    return REFRAIN_STATUS_OK;
}

auto host_config_bool(void *self, refrain_str ns, refrain_str key,
                      std::uint8_t *out) -> refrain_status {
    if (self == nullptr || out == nullptr) {
        clear_error();
        set_error("config_get_bool: null argument");
        return static_cast<refrain_status>(core::host_error::invalid_argument);
    }
    clear_error();
    try {
        auto value{
            context_of(self)->config_bool(abi::to_view(ns), abi::to_view(key))};
        if (!value) {
            set_error(value.error());
            return static_cast<refrain_status>(
                to_host_error(value.error().code));
        }
        *out = *value ? 1U : 0U;
    } catch (...) {
        set_error("config_get_bool: host threw");
        return static_cast<refrain_status>(core::host_error::internal);
    }
    return REFRAIN_STATUS_OK;
}

auto host_config_string(void *self, refrain_str ns, refrain_str key,
                        refrain_str *out) -> refrain_status {
    if (self == nullptr || out == nullptr) {
        clear_error();
        set_error("config_get_string: null argument");
        return static_cast<refrain_status>(core::host_error::invalid_argument);
    }
    clear_error();
    try {
        auto value{context_of(self)->config_string(abi::to_view(ns),
                                                   abi::to_view(key))};
        if (!value) {
            set_error(value.error());
            return static_cast<refrain_status>(
                to_host_error(value.error().code));
        }
        *out = abi::to_borrowed(*value);
    } catch (...) {
        set_error("config_get_string: host threw");
        return static_cast<refrain_status>(core::host_error::internal);
    }
    return REFRAIN_STATUS_OK;
}

auto host_query_service(void *self, refrain_str service, refrain_str instance,
                        refrain_host_core_provider_ref *out) -> refrain_status {
    if (self == nullptr || out == nullptr) {
        clear_error();
        set_error("query_service: null argument");
        return static_cast<refrain_status>(core::host_error::invalid_argument);
    }
    clear_error();
    try {
        auto value{context_of(self)->query_service(abi::to_view(service),
                                                   abi::to_view(instance))};
        if (!value) {
            set_error(value.error());
            return static_cast<refrain_status>(
                to_host_error(value.error().code));
        }
        *out = *value;
        out->struct_size = REFRAIN_HOST_CORE_PROVIDER_REF_SIZE;
    } catch (...) {
        set_error("query_service: host threw");
        return static_cast<refrain_status>(core::host_error::internal);
    }
    return REFRAIN_STATUS_OK;
}

auto host_register_provider(void *self, refrain_str service,
                            refrain_str instance,
                            const refrain_host_core_provider_ref *ref)
    -> refrain_status {
    if (self == nullptr || ref == nullptr) {
        clear_error();
        set_error("register_provider: null argument");
        return static_cast<refrain_status>(core::host_error::invalid_argument);
    }
    if (ref->struct_size < REFRAIN_HOST_CORE_PROVIDER_REF_SIZE) {
        clear_error();
        set_error("register_provider: provider_ref struct_size is too small");
        return static_cast<refrain_status>(core::host_error::version_mismatch);
    }
    clear_error();
    try {
        auto registered{context_of(self)->register_provider(
            abi::to_view(service), abi::to_view(instance), *ref)};
        if (!registered) {
            set_error(registered.error());
            return static_cast<refrain_status>(
                to_host_error(registered.error().code));
        }
    } catch (...) {
        set_error("register_provider: host threw");
        return static_cast<refrain_status>(core::host_error::internal);
    }
    return REFRAIN_STATUS_OK;
}

auto host_alloc(void *self, std::uint64_t size, std::uint64_t alignment,
                refrain_mut_bytes *out) -> refrain_status {
    if (self == nullptr || out == nullptr) {
        clear_error();
        set_error("alloc: null argument");
        return static_cast<refrain_status>(core::host_error::invalid_argument);
    }
    clear_error();
    try {
        auto block{context_of(self)->alloc(size, alignment)};
        if (!block) {
            set_error(block.error());
            return static_cast<refrain_status>(
                to_host_error(block.error().code));
        }
        out->data = block->data();
        out->len = static_cast<std::uint64_t>(block->size());
    } catch (...) {
        set_error("alloc: host threw");
        return static_cast<refrain_status>(core::host_error::internal);
    }
    return REFRAIN_STATUS_OK;
}

void host_free(void *self, refrain_mut_bytes block) {
    if (self == nullptr || block.data == nullptr || block.len == 0) {
        return;
    }
    try {
        context_of(self)->free(std::span<std::uint8_t>{
            block.data, static_cast<std::size_t>(block.len)});
    } catch (...) {
        // Freeing must not throw across the ABI.
    }
}

auto host_last_error_message(void *self, refrain_str *out) -> refrain_status {
    if (self == nullptr || out == nullptr) {
        return static_cast<refrain_status>(core::host_error::invalid_argument);
    }
    if (last_error.empty()) {
        return static_cast<refrain_status>(core::host_error::not_found);
    }
    *out = abi::to_borrowed(last_error);
    return REFRAIN_STATUS_OK;
}

} // namespace

auto core_vtable() noexcept -> const refrain_host_core_vtable & {
    static const refrain_host_core_vtable table{
        .struct_size = REFRAIN_HOST_CORE_VTABLE_SIZE,
        .abi_major = REFRAIN_HOST_CORE_ABI_MAJOR,
        .abi_minor = REFRAIN_HOST_CORE_ABI_MINOR,
        .abi_patch = REFRAIN_HOST_CORE_ABI_PATCH,
        .version = &host_version,
        .log = &host_log,
        .now_ns = &host_now_ns,
        .tick_now = &host_tick_now,
        .config_get_i64 = &host_config_i64,
        .config_get_bool = &host_config_bool,
        .config_get_string = &host_config_string,
        .query_service = &host_query_service,
        .register_provider = &host_register_provider,
        .alloc = &host_alloc,
        .free = &host_free,
        .last_error_message = &host_last_error_message,
    };
    return table;
}

} // namespace refrain::host
