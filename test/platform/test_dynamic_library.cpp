// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/platform/dynamic_library.hpp>

#include <boost/ut.hpp>

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

namespace {
using namespace boost::ut;

namespace fs = std::filesystem;

/// Returns a system shared library path expected to exist on the test host, or
/// an empty string. Positive load coverage is best-effort so CI does not depend
/// on distro-specific paths.
[[nodiscard]] auto find_system_library() -> std::string {
    constexpr const char *candidates[] = {
#if defined(__linux__)
        "/lib/x86_64-linux-gnu/libc.so.6",
        "/lib64/libc.so.6",
        "/usr/lib/x86_64-linux-gnu/libc.so.6",
        "/lib/aarch64-linux-gnu/libc.so.6",
        "/lib/x86_64-linux-gnu/libm.so.6",
#elif defined(__APPLE__)
        "/usr/lib/libSystem.B.dylib",
#else
        "",
#endif
    };

    for (const char *candidate : candidates) {
        if (candidate[0] == '\0') {
            continue;
        }
        std::error_code error;
        if (fs::is_regular_file(fs::path{candidate}, error) && !error) {
            return std::string{candidate};
        }
    }
    return {};
}

suite<"platform/dynamic_library"> dynamic_library_tests = [] {
    "platform/dynamic_library: missing library is mod_error"_test = [] {
        const auto loaded = refrain::platform::dynamic_library::load(
            "/nonexistent/refrain-mod.so");
        expect(!loaded.has_value());
        if (!loaded.has_value()) {
            expect(static_cast<int>(loaded.error().code) ==
                   static_cast<int>(refrain::error_code::mod_error));
            expect(!loaded.error().message.empty());
        }
    };

    "platform/dynamic_library: empty path is invalid_argument"_test = [] {
        const auto loaded = refrain::platform::dynamic_library::load("");
        expect(!loaded.has_value());
        if (!loaded.has_value()) {
            expect(static_cast<int>(loaded.error().code) ==
                   static_cast<int>(refrain::error_code::invalid_argument));
        }
    };

    "platform/dynamic_library: loads, resolves, moves and unloads"_test = [] {
        const std::string library_path = find_system_library();
        if (library_path.empty()) {
            expect(true) << "SKIPPED: no system shared library found for the "
                            "dynamic_library smoke test (coverage gap)";
            return;
        }

        auto loaded = refrain::platform::dynamic_library::load(library_path);
        expect(loaded.has_value());
        if (!loaded.has_value()) {
            return;
        }

        expect(loaded->valid());
        expect(loaded->path() == library_path);

        const auto symbol = loaded->symbol("malloc");
        expect(symbol.has_value());
        expect(loaded->symbol_as<void *(*)()>("malloc").has_value());

        const auto missing =
            loaded->symbol("refrain_dynamic_library_missing_symbol_zz");
        expect(!missing.has_value());
        if (!missing.has_value()) {
            expect(static_cast<int>(missing.error().code) ==
                   static_cast<int>(refrain::error_code::not_found));
        }

        refrain::platform::dynamic_library moved{std::move(*loaded)};
        expect(moved.valid());
        expect(moved.path() == library_path);
        expect(!loaded->valid());
        expect(!loaded->symbol("malloc").has_value());

        moved.unload();
        expect(!moved.valid());
        expect(!moved.symbol("malloc").has_value());

        moved.unload(); // idempotent
        expect(!moved.valid());
    };
};

} // namespace
