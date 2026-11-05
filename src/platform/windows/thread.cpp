// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/platform/thread.hpp>

#if defined(_WIN32)

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace refrain::platform {
namespace {

/// `SetThreadDescription` is a Windows 10 1607 API. Resolve it dynamically so
/// the build does not depend on a particular `_WIN32_WINNT` value and older
/// systems degrade to "no OS-visible name" instead of failing to link.
using set_thread_description_fn = HRESULT(WINAPI *)(HANDLE, PCWSTR);

[[nodiscard]] auto set_thread_description() noexcept
    -> set_thread_description_fn {
    static const auto function = []() -> set_thread_description_fn {
        const HMODULE kernel{::GetModuleHandleW(L"kernel32.dll")};
        if (kernel == nullptr) {
            return nullptr;
        }
        // GetProcAddress returns FARPROC (a function pointer with a generic
        // signature); going through uintptr_t avoids the
        // -Werror=cast-function-type diagnostic on MinGW without changing the
        // resolved address.
        const auto address{reinterpret_cast<std::uintptr_t>(
            ::GetProcAddress(kernel, "SetThreadDescription"))};
        return reinterpret_cast<set_thread_description_fn>(address);
    }();
    return function;
}

/// Guard against absurd input before narrowing to `int` for Win32.
constexpr std::size_t max_thread_name_bytes = 1024;

} // namespace

auto set_current_thread_name(std::string_view name) noexcept -> bool {
    if (name.empty() || name.size() > max_thread_name_bytes) {
        return false;
    }
    const auto utf8_length{static_cast<int>(name.size())};
    const int wide_length{::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), utf8_length, nullptr, 0)};
    if (wide_length <= 0) {
        return false;
    }
    std::wstring wide(static_cast<std::size_t>(wide_length), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(),
                              utf8_length, wide.data(),
                              wide_length) != wide_length) {
        return false;
    }
    const auto function{set_thread_description()};
    if (function == nullptr) {
        return false;
    }
    return SUCCEEDED(function(::GetCurrentThread(), wide.c_str()));
}

auto set_current_thread_priority(thread_priority priority) noexcept -> bool {
    int native{THREAD_PRIORITY_NORMAL};
    switch (priority) {
    case thread_priority::background:
        native = THREAD_PRIORITY_BELOW_NORMAL;
        break;
    case thread_priority::normal:
        native = THREAD_PRIORITY_NORMAL;
        break;
    case thread_priority::high:
        native = THREAD_PRIORITY_ABOVE_NORMAL;
        break;
    }
    return ::SetThreadPriority(::GetCurrentThread(), native) != 0;
}

auto set_current_thread_affinity(std::span<const std::uint32_t> cpus) noexcept
    -> bool {
    if (cpus.empty() || cpus.size() > 64U) {
        return false;
    }
    DWORD_PTR mask{0};
    for (const auto cpu : cpus) {
        if (cpu >= 64U) {
            return false;
        }
        mask |= static_cast<DWORD_PTR>(1) << cpu;
    }
    return ::SetThreadAffinityMask(::GetCurrentThread(), mask) != 0;
}

auto current_thread_id() noexcept -> std::uint64_t {
    // Same value spdlog prints for `%t` on Windows.
    return static_cast<std::uint64_t>(::GetCurrentThreadId());
}

} // namespace refrain::platform

#endif // defined(_WIN32)
