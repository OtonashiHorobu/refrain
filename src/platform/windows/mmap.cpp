// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/platform/mmap.hpp>

#if defined(_WIN32)

#include <refrain/platform/file.hpp>

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace refrain::platform {
namespace {

[[nodiscard]] auto io_failure(std::string_view what, std::string_view path,
                              DWORD native_error) -> error {
    std::string message{what};
    if (!path.empty()) {
        message += " '";
        message += path;
        message += "'";
    }
    message += ": ";
    message += std::system_category().message(static_cast<int>(native_error));
    return make_error(error_code::io_error, std::move(message));
}

} // namespace

struct mapped_file::impl {
    void *view{nullptr};
    std::size_t size{0};

    ~impl() {
        if (view != nullptr) {
            (void)::UnmapViewOfFile(view);
        }
    }
};

mapped_file::mapped_file() noexcept = default;

mapped_file::mapped_file(std::unique_ptr<impl> state) noexcept
    : impl_{std::move(state)} {}

mapped_file::mapped_file(mapped_file &&other) noexcept = default;

auto mapped_file::operator=(mapped_file &&other) noexcept
    -> mapped_file & = default;

mapped_file::~mapped_file() = default;

auto mapped_file::open(std::string_view path) -> result<mapped_file> {
    if (path.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument, "empty mapped-file path")};
    }
    const auto location = file::native_path(path);
    // FILE_SHARE_DELETE lets a concurrent GC/compaction rename or delete the
    // file while a reader holds it; the view itself is released before the
    // read call returns.
    const HANDLE handle =
        ::CreateFileW(location.c_str(), GENERIC_READ,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::unexpected{
            io_failure("failed to open mapped file", path, ::GetLastError())};
    }
    if (::GetFileType(handle) != FILE_TYPE_DISK) {
        (void)::CloseHandle(handle);
        return std::unexpected{make_error(error_code::io_error,
                                          "mapped file '" + std::string{path} +
                                              "' is not a regular file")};
    }

    LARGE_INTEGER file_size{};
    if (::GetFileSizeEx(handle, &file_size) == 0) {
        const auto failure =
            io_failure("failed to stat mapped file", path, ::GetLastError());
        (void)::CloseHandle(handle);
        return std::unexpected{failure};
    }
    if (file_size.QuadPart < 0 ||
        static_cast<std::uint64_t>(file_size.QuadPart) >
            std::numeric_limits<std::size_t>::max()) {
        (void)::CloseHandle(handle);
        return std::unexpected{
            make_error(error_code::io_error,
                       "mapped file '" + std::string{path} +
                           "' is larger than the host address space")};
    }
    const auto size{static_cast<std::size_t>(file_size.QuadPart)};

    auto state{std::make_unique<impl>()};
    state->size = size;
    if (size != 0) {
        const HANDLE mapping{::CreateFileMappingW(
            handle, nullptr, PAGE_READONLY, 0, 0, nullptr)};
        if (mapping == nullptr) {
            const auto failure = io_failure("failed to create a file mapping",
                                            path, ::GetLastError());
            (void)::CloseHandle(handle);
            return std::unexpected{failure};
        }
        void *view{::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0)};
        if (view == nullptr) {
            const auto failure =
                io_failure("failed to map file", path, ::GetLastError());
            (void)::CloseHandle(mapping);
            (void)::CloseHandle(handle);
            return std::unexpected{failure};
        }
        (void)::CloseHandle(mapping);
        state->view = view;
    }
    (void)::CloseHandle(handle);
    return mapped_file{std::move(state)};
}

auto mapped_file::valid() const noexcept -> bool { return impl_ != nullptr; }

auto mapped_file::data() const noexcept -> std::span<const std::byte> {
    if (impl_ == nullptr || impl_->size == 0) {
        return {};
    }
    return {static_cast<const std::byte *>(impl_->view), impl_->size};
}

auto mapped_file::size() const noexcept -> std::uint64_t {
    return impl_ != nullptr ? static_cast<std::uint64_t>(impl_->size) : 0U;
}

} // namespace refrain::platform

#else

// POSIX implementation: see src/platform/posix/mmap.cpp.

#endif
