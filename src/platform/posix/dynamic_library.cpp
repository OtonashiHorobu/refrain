// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/platform/dynamic_library.hpp>

#if !defined(_WIN32)

#include <dlfcn.h>

#include <string>
#include <utility>

namespace refrain::platform {

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
    const std::string native_path{path};
    ::dlerror(); // clear any stale error before the call
    void *handle = ::dlopen(native_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char *reason = ::dlerror();
        return std::unexpected{make_error(error_code::mod_error,
                                          reason != nullptr
                                              ? std::string{reason}
                                              : std::string{"dlopen failed"})};
    }
    return dynamic_library{native_path, handle};
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
    ::dlerror();
    void *resolved = ::dlsym(handle_, native_name.c_str());
    if (resolved == nullptr) {
        const char *reason = ::dlerror();
        return std::unexpected{
            make_error(error_code::not_found,
                       reason != nullptr ? std::string{reason}
                                         : "symbol not found: " + native_name)};
    }
    return resolved;
}

auto dynamic_library::path() const noexcept -> std::string_view {
    return path_;
}

auto dynamic_library::valid() const noexcept -> bool {
    return handle_ != nullptr;
}

void dynamic_library::unload() noexcept {
    if (handle_ != nullptr) {
        (void)::dlclose(handle_);
        handle_ = nullptr;
    }
}

} // namespace refrain::platform

#else

// Windows implementation: see src/platform/windows/dynamic_library.cpp.

#endif // !_WIN32
