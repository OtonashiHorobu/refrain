// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// `refrain:host.platform` C ABI adapter. Handles are opaque indices; the
// concrete platform context owns the table and the file/mmap objects.

#include <refrain/host/platform_api.hpp>

#include <refrain/contracts/abi.hpp>
#include <refrain/host/abi_bridge.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace refrain::host {
namespace {

namespace abi = refrain::host::abi;
namespace host_platform = refrain::host::platform;

[[nodiscard]] auto to_platform_error(error_code code) noexcept
    -> host_platform::platform_error {
    switch (code) {
    case error_code::success:
    case error_code::unknown:
        return host_platform::platform_error::internal;
    case error_code::invalid_argument:
        return host_platform::platform_error::invalid_argument;
    case error_code::not_found:
        return host_platform::platform_error::not_found;
    case error_code::already_exists:
        return host_platform::platform_error::already_exists;
    case error_code::permission_denied:
        return host_platform::platform_error::permission_denied;
    case error_code::io_error:
        return host_platform::platform_error::io_error;
    case error_code::parse_error:
        return host_platform::platform_error::internal;
    case error_code::version_mismatch:
    case error_code::unsupported:
        return host_platform::platform_error::unsupported;
    case error_code::not_initialized:
        return host_platform::platform_error::internal;
    case error_code::timeout:
    case error_code::cancelled:
        return host_platform::platform_error::internal;
    case error_code::out_of_memory:
        return host_platform::platform_error::out_of_memory;
    case error_code::mod_error:
        return host_platform::platform_error::internal;
    }
    return host_platform::platform_error::internal;
}

[[nodiscard]] auto context_of(void *self) noexcept -> platform_context * {
    return static_cast<platform_context *>(self);
}

auto host_file_open(void *self, refrain_str path,
                    refrain_host_platform_file_open_flags flags,
                    refrain_host_platform_handle_file *out) -> refrain_status {
    if (self == nullptr || out == nullptr) {
        return static_cast<refrain_status>(
            host_platform::platform_error::invalid_argument);
    }
    auto handle{context_of(self)->file_open(
        abi::to_view(path),
        static_cast<host_platform::file_open_flags>(flags))};
    if (!handle) {
        return static_cast<refrain_status>(
            to_platform_error(handle.error().code));
    }
    *out = handle->raw();
    return REFRAIN_STATUS_OK;
}

auto host_file_close(void *self, refrain_host_platform_handle_file handle)
    -> refrain_status {
    if (self == nullptr) {
        return static_cast<refrain_status>(
            host_platform::platform_error::invalid_argument);
    }
    auto closed{context_of(self)->file_close(host_platform::file{handle})};
    return closed ? REFRAIN_STATUS_OK
                  : static_cast<refrain_status>(
                        to_platform_error(closed.error().code));
}

auto host_file_read(void *self, refrain_host_platform_handle_file handle,
                    std::uint64_t offset, refrain_mut_bytes buffer,
                    std::uint64_t *out) -> refrain_status {
    if (self == nullptr || out == nullptr) {
        return static_cast<refrain_status>(
            host_platform::platform_error::invalid_argument);
    }
    auto read{context_of(self)->file_read(
        host_platform::file{handle}, offset,
        std::span<std::uint8_t>{buffer.data,
                                static_cast<std::size_t>(buffer.len)})};
    if (!read) {
        return static_cast<refrain_status>(
            to_platform_error(read.error().code));
    }
    *out = *read;
    return REFRAIN_STATUS_OK;
}

auto host_file_write(void *self, refrain_host_platform_handle_file handle,
                     std::uint64_t offset, refrain_bytes data,
                     std::uint64_t *out) -> refrain_status {
    if (self == nullptr || out == nullptr) {
        return static_cast<refrain_status>(
            host_platform::platform_error::invalid_argument);
    }
    auto written{context_of(self)->file_write(
        host_platform::file{handle}, offset,
        std::span<const std::uint8_t>{data.data,
                                      static_cast<std::size_t>(data.len)})};
    if (!written) {
        return static_cast<refrain_status>(
            to_platform_error(written.error().code));
    }
    *out = *written;
    return REFRAIN_STATUS_OK;
}

auto host_file_size(void *self, refrain_host_platform_handle_file handle,
                    std::uint64_t *out) -> refrain_status {
    if (self == nullptr || out == nullptr) {
        return static_cast<refrain_status>(
            host_platform::platform_error::invalid_argument);
    }
    auto size{context_of(self)->file_size(host_platform::file{handle})};
    if (!size) {
        return static_cast<refrain_status>(
            to_platform_error(size.error().code));
    }
    *out = *size;
    return REFRAIN_STATUS_OK;
}

auto host_file_sync(void *self, refrain_host_platform_handle_file handle)
    -> refrain_status {
    if (self == nullptr) {
        return static_cast<refrain_status>(
            host_platform::platform_error::invalid_argument);
    }
    auto synced{context_of(self)->file_sync(host_platform::file{handle})};
    return synced ? REFRAIN_STATUS_OK
                  : static_cast<refrain_status>(
                        to_platform_error(synced.error().code));
}

auto host_file_exists(void *self, refrain_str path) -> std::uint8_t {
    return self != nullptr && context_of(self)->file_exists(abi::to_view(path))
               ? 1U
               : 0U;
}

auto host_mmap_open(void *self, refrain_str path,
                    refrain_host_platform_handle_map *out) -> refrain_status {
    if (self == nullptr || out == nullptr) {
        return static_cast<refrain_status>(
            host_platform::platform_error::invalid_argument);
    }
    auto handle{context_of(self)->mmap_open(abi::to_view(path))};
    if (!handle) {
        return static_cast<refrain_status>(
            to_platform_error(handle.error().code));
    }
    *out = handle->raw();
    return REFRAIN_STATUS_OK;
}

auto host_mmap_data(void *self, refrain_host_platform_handle_map handle,
                    refrain_bytes *out) -> refrain_status {
    if (self == nullptr || out == nullptr) {
        return static_cast<refrain_status>(
            host_platform::platform_error::invalid_argument);
    }
    auto data{context_of(self)->mmap_data(host_platform::map{handle})};
    if (!data) {
        return static_cast<refrain_status>(
            to_platform_error(data.error().code));
    }
    *out =
        refrain_bytes{data->data(), static_cast<std::uint64_t>(data->size())};
    return REFRAIN_STATUS_OK;
}

auto host_mmap_close(void *self, refrain_host_platform_handle_map handle)
    -> refrain_status {
    if (self == nullptr) {
        return static_cast<refrain_status>(
            host_platform::platform_error::invalid_argument);
    }
    auto closed{context_of(self)->mmap_close(host_platform::map{handle})};
    return closed ? REFRAIN_STATUS_OK
                  : static_cast<refrain_status>(
                        to_platform_error(closed.error().code));
}

void host_crash_annotation(void *self, refrain_str key, refrain_str value) {
    if (self == nullptr) {
        return;
    }
    context_of(self)->crash_annotation(abi::to_view(key), abi::to_view(value));
}

auto host_temp_directory(void *self) -> refrain_str {
    if (self == nullptr) {
        return {};
    }
    return abi::to_borrowed(context_of(self)->temp_directory());
}

} // namespace

auto platform_vtable() noexcept -> const refrain_host_platform_vtable & {
    static const refrain_host_platform_vtable table{
        .struct_size = REFRAIN_HOST_PLATFORM_VTABLE_SIZE,
        .abi_major = REFRAIN_HOST_PLATFORM_ABI_MAJOR,
        .abi_minor = REFRAIN_HOST_PLATFORM_ABI_MINOR,
        .abi_patch = REFRAIN_HOST_PLATFORM_ABI_PATCH,
        .file_open = &host_file_open,
        .file_close = &host_file_close,
        .file_read = &host_file_read,
        .file_write = &host_file_write,
        .file_size = &host_file_size,
        .file_sync = &host_file_sync,
        .file_exists = &host_file_exists,
        .mmap_open = &host_mmap_open,
        .mmap_data = &host_mmap_data,
        .mmap_close = &host_mmap_close,
        .crash_annotation = &host_crash_annotation,
        .temp_directory = &host_temp_directory,
    };
    return table;
}

} // namespace refrain::host
