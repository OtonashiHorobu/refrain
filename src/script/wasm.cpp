// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/script/wasm.hpp>

#if !defined(REFRAIN_WITH_WASM)
#define REFRAIN_WITH_WASM 0
#endif

#if REFRAIN_WITH_WASM

#include <wasmtime.h>

#include <wasmtime/wat.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refrain::script::wasm {

namespace {

[[nodiscard]] auto fail(error_code code, std::string message) -> error {
    return make_error(code, std::move(message));
}

/// Take ownership of a Wasmtime error and flatten it into `refrain::error`.
[[nodiscard]] auto take_error(wasmtime_error_t *wasm_error) -> error {
    wasm_name_t message = WASM_EMPTY_VEC;
    wasmtime_error_message(wasm_error, &message);
    auto text{
        std::string{message.data != nullptr ? message.data : "", message.size}};
    wasm_name_delete(&message);
    wasmtime_error_delete(wasm_error);
    return fail(error_code::mod_error, "wasmtime: " + text);
}

/// Take ownership of a Wasmtime trap and flatten it into `refrain::error`.
[[nodiscard]] auto take_trap(wasm_trap_t *trap) -> error {
    wasm_message_t message = WASM_EMPTY_VEC;
    wasm_trap_message(trap, &message);
    auto text{
        std::string{message.data != nullptr ? message.data : "", message.size}};
    wasm_name_delete(&message);
    wasm_trap_delete(trap);
    return fail(error_code::mod_error, "wasm trap: " + text);
}

/// Resolve `module`'s imports through `linker`, instantiate it in `context` and
/// run its start function. The linker is empty in this slice, so a module that
/// imports anything fails here instead of reaching host code. Wasmtime writes
/// `instance`; the caller keeps the value alive.
[[nodiscard]] auto instantiate_module(wasmtime_linker_t *linker,
                                      wasmtime_context_t *context,
                                      const wasmtime_module_t *module,
                                      wasmtime_instance_t &instance) -> status {
    wasm_trap_t *trap{nullptr};
    if (auto *error{wasmtime_linker_instantiate(linker, context, module,
                                                &instance, &trap)}) {
        return std::unexpected{take_error(error)};
    }
    if (trap != nullptr) {
        return std::unexpected{take_trap(trap)};
    }
    return status_ok();
}

/// Call an exported `(i32, ...) -> i32` function on an already instantiated
/// guest.
[[nodiscard]] auto
call_export(wasmtime_context_t *context, const wasmtime_instance_t &instance,
            std::string_view export_name, std::span<const std::int32_t> args)
    -> result<std::int32_t> {
    wasmtime_extern_t item{};
    if (!wasmtime_instance_export_get(context, &instance, export_name.data(),
                                      export_name.size(), &item)) {
        return std::unexpected(fail(
            error_code::not_found,
            "wasmtime: export '" + std::string{export_name} + "' not found"));
    }
    if (item.kind != WASMTIME_EXTERN_FUNC) {
        return std::unexpected{
            fail(error_code::invalid_argument, "wasmtime: export '" +
                                                   std::string{export_name} +
                                                   "' is not a function")};
    }

    auto wasm_args{std::vector<wasmtime_val_t>(args.size())};
    for (std::size_t i = 0; i < args.size(); ++i) {
        wasm_args[i].kind = WASMTIME_I32;
        wasm_args[i].of.i32 = args[i];
    }
    wasmtime_val_t result{};
    wasm_trap_t *trap{nullptr};
    if (auto *error{wasmtime_func_call(context, &item.of.func, wasm_args.data(),
                                       wasm_args.size(), &result, 1, &trap)}) {
        return std::unexpected{take_error(error)};
    }
    if (trap != nullptr) {
        return std::unexpected{take_trap(trap)};
    }
    if (result.kind != WASMTIME_I32) {
        return std::unexpected{
            fail(error_code::mod_error,
                 "wasmtime: exported function does not return an i32 value")};
    }
    return result.of.i32;
}

} // namespace

struct runtime::impl {
    wasm_engine_t *engine{nullptr};
    wasmtime_store_t *store{nullptr};
    wasmtime_context_t *context{nullptr};
    wasmtime_linker_t *linker{nullptr};
    std::vector<wasmtime_module_t *> modules{};
    /// The single persistent guest instance, when `instantiate` ran.
    wasmtime_instance_t instance{};
    bool instance_live{false};
    engine_config config{};

    ~impl() {
        for (auto *module : modules) {
            wasmtime_module_delete(module);
        }
        if (linker != nullptr) {
            wasmtime_linker_delete(linker);
        }
        if (store != nullptr) {
            wasmtime_store_delete(store);
        }
        if (engine != nullptr) {
            wasm_engine_delete(engine);
        }
    }
};

runtime::runtime(std::unique_ptr<impl> state) : impl_{std::move(state)} {}

runtime::runtime(runtime &&other) noexcept = default;

auto runtime::operator=(runtime &&other) noexcept -> runtime & = default;

runtime::~runtime() = default;

auto assemble_wat(std::string_view wat) -> result<std::vector<std::byte>> {
    wasm_byte_vec_t wasm = WASM_EMPTY_VEC;
    if (auto *error{wasmtime_wat2wasm(wat.data(), wat.size(), &wasm)}) {
        return std::unexpected{take_error(error)};
    }
    auto bytes{std::vector<std::byte>(wasm.size)};
    if (wasm.size != 0) {
        std::memcpy(bytes.data(), wasm.data, wasm.size);
    }
    wasm_byte_vec_delete(&wasm);
    return bytes;
}

auto runtime::create(engine_config config) -> result<runtime> {
    auto state{std::make_unique<impl>()};
    state->config = config;

    wasm_config_t *wasm_config{wasm_config_new()};
    if (wasm_config == nullptr) {
        return std::unexpected{fail(error_code::out_of_memory,
                                    "wasmtime: cannot allocate config")};
    }
    if (config.consume_fuel) {
        wasmtime_config_consume_fuel_set(wasm_config, true);
    }
    if (config.wasm_threads) {
        wasmtime_config_wasm_threads_set(wasm_config, true);
    }
    if (config.memory64) {
        wasmtime_config_wasm_memory64_set(wasm_config, true);
    }

    // wasm_engine_new_with_config takes ownership of the config, including on
    // failure.
    state->engine = wasm_engine_new_with_config(wasm_config);
    if (state->engine == nullptr) {
        return std::unexpected{
            fail(error_code::mod_error, "wasmtime: cannot create engine")};
    }

    state->store = wasmtime_store_new(state->engine, nullptr, nullptr);
    if (state->store == nullptr) {
        return std::unexpected{
            fail(error_code::out_of_memory, "wasmtime: cannot create store")};
    }
    state->context = wasmtime_store_context(state->store);
    if (config.max_memory_bytes != 0) {
        const auto maximum{
            std::min(config.max_memory_bytes,
                     static_cast<std::uint64_t>(
                         std::numeric_limits<std::int64_t>::max()))};
        // -1 leaves the other store limits (tables, instances, memories) at
        // their Wasmtime defaults; the engine uses them only as a coarse
        // backstop, the loader's own budgets are the contract.
        wasmtime_store_limiter(state->store, static_cast<std::int64_t>(maximum),
                               -1, -1, -1, -1);
    }
    state->linker = wasmtime_linker_new(state->engine);
    if (state->linker == nullptr) {
        return std::unexpected{
            fail(error_code::out_of_memory, "wasmtime: cannot create linker")};
    }

    if (config.consume_fuel) {
        if (auto *error{
                wasmtime_context_set_fuel(state->context, config.fuel)}) {
            return std::unexpected{take_error(error)};
        }
    }
    return runtime{std::move(state)};
}

auto runtime::compile_bytes(std::span<const std::byte> wasm)
    -> result<module_handle> {
    if (impl_ == nullptr || impl_->engine == nullptr) {
        return std::unexpected{
            fail(error_code::not_initialized, "wasmtime: runtime is not open")};
    }
    if (wasm.empty()) {
        return std::unexpected{
            fail(error_code::invalid_argument,
                 "wasmtime: cannot compile an empty module")};
    }

    wasmtime_module_t *module{nullptr};
    auto *error{wasmtime_module_new(
        impl_->engine, reinterpret_cast<const std::uint8_t *>(wasm.data()),
        wasm.size(), &module)};
    if (error != nullptr) {
        return std::unexpected{take_error(error)};
    }
    if (module == nullptr) {
        return std::unexpected{
            fail(error_code::mod_error, "wasmtime: module is null")};
    }
    impl_->modules.push_back(module);
    return static_cast<module_handle>(impl_->modules.size() - 1);
}

auto runtime::compile_wat(std::string_view wat) -> result<module_handle> {
    auto bytes{assemble_wat(wat)};
    if (!bytes) {
        return std::unexpected{std::move(bytes.error())};
    }
    return compile_bytes(*bytes);
}

auto runtime::instantiate(module_handle module) -> status {
    if (impl_ == nullptr || impl_->engine == nullptr) {
        return std::unexpected{
            fail(error_code::not_initialized, "wasmtime: runtime is not open")};
    }
    if (module >= impl_->modules.size()) {
        return std::unexpected{
            fail(error_code::not_found, "wasmtime: unknown module handle")};
    }
    if (impl_->instance_live) {
        return std::unexpected{
            fail(error_code::already_exists,
                 "wasmtime: runtime already holds an instantiated guest")};
    }
    auto instantiated{instantiate_module(impl_->linker, impl_->context,
                                         impl_->modules[module],
                                         impl_->instance)};
    if (!instantiated) {
        return instantiated;
    }
    impl_->instance_live = true;
    return status_ok();
}

auto runtime::has_export(std::string_view export_name) const -> bool {
    if (impl_ == nullptr || !impl_->instance_live) {
        return false;
    }
    wasmtime_extern_t item{};
    return wasmtime_instance_export_get(impl_->context, &impl_->instance,
                                        export_name.data(), export_name.size(),
                                        &item);
}

auto runtime::call_i32(module_handle module, std::string_view export_name,
                       std::span<const std::int32_t> args)
    -> result<std::int32_t> {
    if (impl_ == nullptr || impl_->modules.empty()) {
        return std::unexpected{
            fail(error_code::not_initialized, "wasmtime: runtime is not open")};
    }
    if (module >= impl_->modules.size()) {
        return std::unexpected{
            fail(error_code::not_found, "wasmtime: unknown module handle")};
    }
    wasmtime_instance_t instance{};
    auto instantiated{instantiate_module(impl_->linker, impl_->context,
                                         impl_->modules[module], instance)};
    if (!instantiated) {
        return std::unexpected{std::move(instantiated.error())};
    }
    return call_export(impl_->context, instance, export_name, args);
}

auto runtime::call_instance_i32(std::string_view export_name,
                                std::span<const std::int32_t> args)
    -> result<std::int32_t> {
    if (impl_ == nullptr || !impl_->instance_live) {
        return std::unexpected{
            fail(error_code::not_initialized,
                 "wasmtime: runtime has no instantiated guest")};
    }
    return call_export(impl_->context, impl_->instance, export_name, args);
}

auto runtime::fuel_remaining() const noexcept -> std::uint64_t {
    if (impl_ == nullptr || impl_->context == nullptr ||
        !impl_->config.consume_fuel) {
        return 0;
    }
    std::uint64_t fuel{0};
    if (wasmtime_context_get_fuel(impl_->context, &fuel) != nullptr) {
        return 0;
    }
    return fuel;
}

auto runtime::memory_bytes() const noexcept -> std::uint64_t {
    if (impl_ == nullptr || !impl_->instance_live) {
        return 0;
    }
    wasmtime_extern_t item{};
    constexpr std::string_view export_name{"memory"};
    if (!wasmtime_instance_export_get(impl_->context, &impl_->instance,
                                      export_name.data(), export_name.size(),
                                      &item)) {
        return 0;
    }
    if (item.kind != WASMTIME_EXTERN_MEMORY) {
        return 0;
    }
    return static_cast<std::uint64_t>(
        wasmtime_memory_data_size(impl_->context, &item.of.memory));
}

auto runtime::module_count() const noexcept -> std::size_t {
    return impl_ != nullptr ? impl_->modules.size() : 0U;
}

} // namespace refrain::script::wasm

#else // REFRAIN_WITH_WASM

namespace refrain::script::wasm {

struct runtime::impl {};

runtime::runtime(std::unique_ptr<impl> state) : impl_{std::move(state)} {}

runtime::runtime(runtime &&other) noexcept = default;

auto runtime::operator=(runtime &&other) noexcept -> runtime & = default;

runtime::~runtime() = default;

auto assemble_wat(std::string_view /*wat*/) -> result<std::vector<std::byte>> {
    return std::unexpected{make_error(error_code::unsupported,
                                      "wasmtime is not enabled in this build "
                                      "(REFRAIN_WITH_WASM=0)")};
}

auto runtime::create(engine_config /*config*/) -> result<runtime> {
    return std::unexpected{make_error(error_code::unsupported,
                                      "wasmtime is not enabled in this build "
                                      "(REFRAIN_WITH_WASM=0)")};
}

auto runtime::compile_wat(std::string_view /*wat*/) -> result<module_handle> {
    return std::unexpected{make_error(error_code::unsupported,
                                      "wasmtime is not enabled in this build")};
}

auto runtime::compile_bytes(std::span<const std::byte> /*wasm*/)
    -> result<module_handle> {
    return std::unexpected{make_error(error_code::unsupported,
                                      "wasmtime is not enabled in this build")};
}

auto runtime::instantiate(module_handle /*module*/) -> status {
    return std::unexpected{make_error(error_code::unsupported,
                                      "wasmtime is not enabled in this build")};
}

auto runtime::has_export(std::string_view /*export_name*/) const -> bool {
    return false;
}

auto runtime::call_i32(module_handle /*module*/, std::string_view /*name*/,
                       std::span<const std::int32_t> /*args*/)
    -> result<std::int32_t> {
    return std::unexpected{make_error(error_code::unsupported,
                                      "wasmtime is not enabled in this build")};
}

auto runtime::call_instance_i32(std::string_view /*name*/,
                                std::span<const std::int32_t> /*args*/)
    -> result<std::int32_t> {
    return std::unexpected{make_error(error_code::unsupported,
                                      "wasmtime is not enabled in this build")};
}

auto runtime::fuel_remaining() const noexcept -> std::uint64_t { return 0; }

auto runtime::memory_bytes() const noexcept -> std::uint64_t { return 0; }

auto runtime::module_count() const noexcept -> std::size_t { return 0U; }

} // namespace refrain::script::wasm

#endif // REFRAIN_WITH_WASM
