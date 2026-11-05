// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/platform/mmap.hpp>

#if !defined(_WIN32)

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace refrain::platform {
namespace {

/// Open with EINTR retry: a signal in the middle of open() must not turn into a
/// spurious io_error. O_NONBLOCK keeps a FIFO/device from blocking the open
/// before fstat() can reject it as non-regular; it is a no-op for regular
/// files.
[[nodiscard]] auto open_retry(const char *path) noexcept -> int {
    for (;;) {
        const int descriptor = ::open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (descriptor >= 0 || errno != EINTR) {
            return descriptor;
        }
    }
}

[[nodiscard]] auto io_failure(std::string_view what, std::string_view path)
    -> error {
    std::string message{what};
    if (!path.empty()) {
        message += " '";
        message += path;
        message += "'";
    }
    const std::error_code code{errno, std::generic_category()};
    if (code) {
        message += ": ";
        message += code.message();
    }
    return make_error(error_code::io_error, std::move(message));
}

} // namespace

struct mapped_file::impl {
    void *address{nullptr};
    std::size_t size{0};

    ~impl() {
        if (address != nullptr && size != 0) {
            (void)::munmap(address, size);
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
    const std::string location{path};
    const int descriptor = open_retry(location.c_str());
    if (descriptor < 0) {
        return std::unexpected{
            io_failure("failed to open mapped file", location)};
    }

    struct stat status{};
    if (::fstat(descriptor, &status) != 0) {
        const auto failure = io_failure("failed to stat mapped file", location);
        (void)::close(descriptor);
        return std::unexpected{failure};
    }
    if (!S_ISREG(status.st_mode)) {
        (void)::close(descriptor);
        return std::unexpected{
            make_error(error_code::io_error,
                       "mapped file '" + location + "' is not a regular file")};
    }
    const auto size{static_cast<std::uint64_t>(status.st_size)};
    if (size > std::numeric_limits<std::size_t>::max()) {
        (void)::close(descriptor);
        return std::unexpected{
            make_error(error_code::io_error,
                       "mapped file '" + location +
                           "' is larger than the host address space")};
    }

    auto state{std::make_unique<impl>()};
    state->size = static_cast<std::size_t>(size);
    if (state->size != 0) {
        void *address{::mmap(nullptr, state->size, PROT_READ, MAP_PRIVATE,
                             descriptor, 0)};
        if (address == MAP_FAILED) {
            const auto failure = io_failure("failed to map file", location);
            (void)::close(descriptor);
            return std::unexpected{failure};
        }
        state->address = address;
    }
    (void)::close(descriptor);
    return mapped_file{std::move(state)};
}

auto mapped_file::valid() const noexcept -> bool { return impl_ != nullptr; }

auto mapped_file::data() const noexcept -> std::span<const std::byte> {
    if (impl_ == nullptr || impl_->size == 0) {
        return {};
    }
    return {static_cast<const std::byte *>(impl_->address), impl_->size};
}

auto mapped_file::size() const noexcept -> std::uint64_t {
    return impl_ != nullptr ? static_cast<std::uint64_t>(impl_->size) : 0U;
}

} // namespace refrain::platform

#else

// Windows implementation: see src/platform/windows/mmap.cpp.

#endif
