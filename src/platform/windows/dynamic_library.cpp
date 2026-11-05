// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/platform/dynamic_library.hpp>

#if defined(_WIN32)

#include <refrain/core/error.hpp>

#include <windows.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

namespace refrain::platform {
namespace {

/// LoadLibraryW + std::filesystem keeps UTF-8 engine paths working on Windows.
[[nodiscard]] auto native_path(std::string_view utf8) -> std::filesystem::path {
    const auto *first = reinterpret_cast<const char8_t *>(utf8.data());
    return std::filesystem::path{std::u8string{first, utf8.size()}};
}

[[nodiscard]] auto last_error_message(std::string_view prefix) -> std::string {
    std::string message{prefix};
    message += ": ";
    message +=
        std::system_category().message(static_cast<int>(::GetLastError()));
    return message;
}

} // namespace

dynamic_library::dynamic_library(std::string path, void *handle) noexcept
    : path_{std::move(path)}, handle_{handle} {}

dynamic_library::dynamic_library(dynamic_library &&other) noexcept
    : path_{std::move(other.path_)}, handle_{other.handle_} {
    other.handle_ = nullptr;
}

auto dynamic_library::operator=(dynamic_library &&other) noexcept
    -> dynamic_library & {
    if (this != &other) {
        unload();
        path_ = std::move(other.path_);
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

dynamic_library::~dynamic_library() { unload(); }

auto dynamic_library::load(std::string_view path) -> result<dynamic_library> {
    if (path.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument, "empty library path")};
    }
    const std::filesystem::path native = native_path(path);
    ::SetLastError(ERROR_SUCCESS);
    HMODULE module = ::LoadLibraryW(native.c_str());
    if (module == nullptr) {
        return std::unexpected{make_error(
            error_code::mod_error, last_error_message("LoadLibrary failed"))};
    }
    return dynamic_library{std::string{path}, static_cast<void *>(module)};
}

auto dynamic_library::symbol(std::string_view name) const -> result<void *> {
    if (handle_ == nullptr) {
        return std::unexpected{
            make_error(error_code::not_initialized, "library is not loaded")};
    }
    if (name.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument, "empty symbol name")};
    }
    const std::string native_name{name};
    ::SetLastError(ERROR_SUCCESS);
    FARPROC address =
        ::GetProcAddress(static_cast<HMODULE>(handle_), native_name.c_str());
    if (address == nullptr) {
        return std::unexpected{make_error(
            error_code::not_found,
            last_error_message("GetProcAddress failed for " + native_name))};
    }
    return reinterpret_cast<void *>(address); // NOLINT
}

auto dynamic_library::path() const noexcept -> std::string_view {
    return path_;
}

auto dynamic_library::valid() const noexcept -> bool {
    return handle_ != nullptr;
}

void dynamic_library::unload() noexcept {
    if (handle_ != nullptr) {
        (void)::FreeLibrary(static_cast<HMODULE>(handle_));
        handle_ = nullptr;
    }
}

} // namespace refrain::platform

#else

// POSIX implementation: see src/platform/posix/dynamic_library.cpp.

#endif // _WIN32
