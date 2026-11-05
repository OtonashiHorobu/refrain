// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/app/services.hpp>

#include <refrain/core/log.hpp>
#include <refrain/version.hpp>

#include <iostream>
#include <string>

namespace refrain::app {

[[nodiscard]] auto build_features() -> std::string {
    std::string features{};
#if REFRAIN_WITH_CRASHPAD
    features += "crashpad ";
#endif
#if REFRAIN_WITH_WASM
    features += "wasm ";
#endif
#if REFRAIN_USE_REFLECTION
    features += "reflection ";
#endif
#if REFRAIN_WITH_MIMALLOC
    features += "mimalloc ";
#endif
    if (!features.empty()) {
        features.pop_back();
    }
    return features.empty() ? "none" : features;
}

void configure_logging(bool verbose) {
    log::config cfg{};
    cfg.console_level = verbose ? log::level::debug : log::level::info;
    // A failed log init must not make every later diagnostic disappear.
    // Keep running and fall back to stderr (report_error() checks whether the
    // error level is actually enabled).
    if (!log::init(cfg)) {
        std::cerr << "warning: logging initialization failed; diagnostics use "
                     "stderr\n";
    }
    // CLI tools are single-threaded; the name keeps their logs uniform with
    // the server (`[main/info]`) and names the main thread in `top -H`.
    log::set_thread_name("main");
}

[[nodiscard]] auto report_error(const refrain::error &error) -> int {
    log::error("{} ({})", error.message, refrain::to_string(error.code));
    // When logging is unavailable (init failed, or every sink filters the
    // level out) print to stderr so a tool failure is never silent.
    if (!log::enabled(log::level::error)) {
        std::cerr << "error: " << error.message << " ("
                  << refrain::to_string(error.code) << ")\n";
    }
    return 1;
}

} // namespace refrain::app
