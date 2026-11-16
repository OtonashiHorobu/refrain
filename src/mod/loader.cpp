// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/mod/loader.hpp>

#include <string>
#include <utility>

namespace refrain::mod {
namespace {

/// Per-thread because the loader contract dispatches on the main thread and a
/// process-global string would race with a provider that dispatches from
/// another thread despite the contract.
thread_local std::string loader_diagnostic{};

} // namespace

void set_loader_diagnostic(std::string message) {
    loader_diagnostic = std::move(message);
}

void clear_loader_diagnostic() noexcept { loader_diagnostic.clear(); }

auto take_loader_diagnostic() -> std::string {
    auto message{std::move(loader_diagnostic)};
    loader_diagnostic.clear();
    return message;
}

} // namespace refrain::mod
