// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/script/wasm.hpp>

#include <refrain/data/path.hpp>

#if !defined(REFRAIN_WITH_WASM)
#define REFRAIN_WITH_WASM 0
#endif

namespace refrain::script::host {

auto capability_name(capability value) noexcept -> std::string_view {
    switch (value) {
    case capability::log:
        return "log";
    case capability::config:
        return "config";
    case capability::clock:
        return "clock";
    }
    return "unknown";
}

auto parse_capability(std::string_view name) noexcept
    -> std::optional<capability> {
    if (name == "log") {
        return capability::log;
    }
    if (name == "config") {
        return capability::config;
    }
    if (name == "clock") {
        return capability::clock;
    }
    return std::nullopt;
}

auto format_grants(grant_set grants) -> std::string {
    auto text{std::string{}};
    for (const auto value :
         {capability::clock, capability::config, capability::log}) {
        if (!grants.contains(value)) {
            continue;
        }
        if (!text.empty()) {
            text += ", ";
        }
        text += capability_name(value);
    }
    return text;
}

} // namespace refrain::script::host

#if REFRAIN_WITH_WASM

#include <wasmtime.h>

#include <wasmtime/wat.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
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

} // namespace

namespace detail {

/// State one runtime hands to its host import callbacks. The object lives in
/// the runtime's impl for the runtime's lifetime; the callbacks only read it.
struct host_import_env {
    host::grant_set grants{};
    const host::environment *environment{nullptr};
    /// Log lines this dispatch may still emit; reset before every call so a
    /// guest cannot turn its instruction budget into unbounded log volume.
    std::uint32_t log_lines_remaining{host::max_log_messages_per_call};
    /// Lines collected for the host to drain after the call.
    std::vector<guest_log_entry> logs{};
};

} // namespace detail

namespace {

using detail::host_import_env;

/// The guest's exported linear memory, resolved for one callback invocation.
struct caller_memory {
    std::uint8_t *data{nullptr};
    std::size_t size{0};
};

[[nodiscard]] auto resolve_caller_memory(wasmtime_caller_t *caller)
    -> std::optional<caller_memory> {
    wasmtime_extern_t item{};
    constexpr std::string_view export_name{"memory"};
    if (!wasmtime_caller_export_get(caller, export_name.data(),
                                    export_name.size(), &item)) {
        return std::nullopt;
    }
    if (item.kind != WASMTIME_EXTERN_MEMORY) {
        return std::nullopt;
    }
    auto *context{wasmtime_caller_context(caller)};
    return caller_memory{
        .data = wasmtime_memory_data(context, &item.of.memory),
        .size = wasmtime_memory_data_size(context, &item.of.memory),
    };
}

/// Validate a guest-supplied `(pointer, length)` view of the guest's own
/// linear memory: non-negative, within `limit` and inside the memory. Returns
/// the bound-checked offset, or nullopt when the view is rejected.
[[nodiscard]] auto
checked_guest_range(std::int32_t pointer, std::int32_t length,
                    std::size_t memory_size, std::uint32_t limit) noexcept
    -> std::optional<std::size_t> {
    if (pointer < 0 || length < 0) {
        return std::nullopt;
    }
    const auto offset{static_cast<std::uint64_t>(pointer)};
    const auto bytes{static_cast<std::uint64_t>(length)};
    if (bytes > limit || offset + bytes > memory_size) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(offset);
}

/// True when `bytes` is well-formed UTF-8 without control characters. A guest
/// log message that fails this is refused: a line break or escape sequence in
/// a host log line would forge operator-facing output. Well-formedness reuses
/// the repository's shared RFC 3629 predicate; the stricter output policy (no
/// C0/C1 controls, line separators or byte-order mark) is layered on top.
[[nodiscard]] auto
is_safe_log_text(std::span<const std::uint8_t> bytes) noexcept -> bool {
    const std::string_view text{reinterpret_cast<const char *>(bytes.data()),
                                bytes.size()};
    if (!refrain::data::is_valid_utf8(text)) {
        return false;
    }
    std::size_t index{0};
    while (index < text.size()) {
        const auto lead{static_cast<unsigned char>(text[index])};
        if (lead < 0x80U) {
            if (lead < 0x20U || lead == 0x7FU) {
                return false;
            }
            ++index;
            continue;
        }
        const auto length{(lead & 0xE0U) == 0xC0U   ? 2U
                          : (lead & 0xF0U) == 0xE0U ? 3U
                                                    : 4U};
        std::uint32_t codepoint{lead & (length == 2   ? 0x1FU
                                        : length == 3 ? 0x0FU
                                                      : 0x07U)};
        for (std::size_t offset = 1; offset < length; ++offset) {
            codepoint =
                (codepoint << 6U) |
                (static_cast<unsigned char>(text[index + offset]) & 0x3FU);
        }
        // C0 and C1 controls, the line separators and the byte-order mark are
        // refused: a reader must not be able to reinterpret a guest log line as
        // a terminal escape or as a second line.
        if (codepoint < 0xA0U || codepoint == 0x2028U || codepoint == 0x2029U ||
            codepoint == 0xFEFFU) {
            return false;
        }
        index += length;
    }
    return true;
}

/// True when `bytes` is a plausible configuration key: printable ASCII, not
/// empty and without whitespace or control characters.
[[nodiscard]] auto
is_safe_config_key(std::span<const std::uint8_t> bytes) noexcept -> bool {
    if (bytes.empty() || bytes.size() > host::max_config_key_bytes) {
        return false;
    }
    return std::ranges::all_of(
        bytes, [](std::uint8_t byte) { return byte > 0x20U && byte < 0x7FU; });
}

/// Write the i32 status of a host import and return no trap. A malformed call
/// is reported inside the guest (negative status), never as a host trap: the
/// guest decides whether a bad view is fatal, the host only refuses to touch
/// it.
auto set_status(wasmtime_val_t *results, std::size_t nresults,
                std::int32_t status) noexcept -> wasm_trap_t * {
    if (results != nullptr && nresults >= 1) {
        results[0].kind = WASMTIME_I32;
        results[0].of.i32 = status;
    }
    return nullptr;
}

auto import_log(void *env, wasmtime_caller_t *caller,
                const wasmtime_val_t *args, std::size_t nargs,
                wasmtime_val_t *results, std::size_t nresults)
    -> wasm_trap_t * {
    auto *state{static_cast<host_import_env *>(env)};
    if (state == nullptr || args == nullptr || nargs != 3) {
        return set_status(results, nresults, host::import_invalid_argument);
    }
    const auto level{args[0].of.i32};
    if (level < 0 || level > 4) {
        return set_status(results, nresults, host::import_invalid_argument);
    }
    if (state->log_lines_remaining == 0) {
        return set_status(results, nresults, host::import_quota_exceeded);
    }
    const auto memory{resolve_caller_memory(caller)};
    if (!memory) {
        return set_status(results, nresults, host::import_invalid_argument);
    }
    const auto offset{checked_guest_range(args[1].of.i32, args[2].of.i32,
                                          memory->size,
                                          host::max_log_message_bytes)};
    if (!offset) {
        return set_status(results, nresults, host::import_invalid_argument);
    }
    const auto byte_count{static_cast<std::size_t>(args[2].of.i32)};
    const auto message{
        std::span<const std::uint8_t>{memory->data + *offset, byte_count}};
    if (!is_safe_log_text(message)) {
        return set_status(results, nresults, host::import_invalid_argument);
    }
    --state->log_lines_remaining;
    state->logs.push_back(guest_log_entry{
        .level = static_cast<std::uint8_t>(level),
        .message = std::string{reinterpret_cast<const char *>(message.data()),
                               message.size()},
    });
    return set_status(results, nresults, host::import_ok);
}

auto import_config_get(void *env, wasmtime_caller_t *caller,
                       const wasmtime_val_t *args, std::size_t nargs,
                       wasmtime_val_t *results, std::size_t nresults)
    -> wasm_trap_t * {
    auto *state{static_cast<host_import_env *>(env)};
    if (state == nullptr || args == nullptr || nargs != 4 ||
        state->environment == nullptr || results == nullptr || nresults != 1) {
        return set_status(results, nresults, host::import_invalid_argument);
    }
    const auto memory{resolve_caller_memory(caller)};
    if (!memory) {
        return set_status(results, nresults, host::import_invalid_argument);
    }
    const auto key_offset{checked_guest_range(args[0].of.i32, args[1].of.i32,
                                              memory->size,
                                              host::max_config_key_bytes)};
    if (!key_offset) {
        return set_status(results, nresults, host::import_invalid_argument);
    }
    const auto key_bytes{static_cast<std::size_t>(args[1].of.i32)};
    const auto key_view{
        std::span<const std::uint8_t>{memory->data + *key_offset, key_bytes}};
    if (!is_safe_config_key(key_view)) {
        return set_status(results, nresults, host::import_invalid_argument);
    }
    const std::string_view key{reinterpret_cast<const char *>(key_view.data()),
                               key_view.size()};
    const auto found{state->environment->config.find(key)};
    if (found == state->environment->config.end()) {
        return set_status(results, nresults, host::import_not_found);
    }
    const auto &value{found->second};
    if (value.size() > host::max_config_value_bytes) {
        return set_status(results, nresults, host::import_invalid_argument);
    }
    if (args[3].of.i32 < 0 ||
        static_cast<std::uint64_t>(args[3].of.i32) < value.size()) {
        return set_status(results, nresults, host::import_buffer_too_small);
    }
    const auto value_offset{checked_guest_range(args[2].of.i32, args[3].of.i32,
                                                memory->size,
                                                host::max_config_value_bytes)};
    if (!value_offset) {
        return set_status(results, nresults, host::import_invalid_argument);
    }
    if (!value.empty()) {
        std::memcpy(memory->data + *value_offset, value.data(), value.size());
    }
    return set_status(results, nresults,
                      static_cast<std::int32_t>(value.size()));
}

auto import_tick(void *env, wasmtime_caller_t * /*caller*/,
                 const wasmtime_val_t * /*args*/, std::size_t /*nargs*/,
                 wasmtime_val_t *results, std::size_t nresults)
    -> wasm_trap_t * {
    auto *state{static_cast<host_import_env *>(env)};
    if (state == nullptr || state->environment == nullptr ||
        results == nullptr || nresults != 1) {
        return nullptr;
    }
    results[0].kind = WASMTIME_I64;
    results[0].of.i64 = state->environment->tick_index;
    return nullptr;
}

/// Callbacks registered in the Wasmtime linker must not let an exception
/// unwind across the C ABI. Each wrapper guards one import implementation and
/// reports a host-side failure as a negative import status instead.
[[nodiscard]] auto
guarded_import_log(void *env, wasmtime_caller_t *caller,
                   const wasmtime_val_t *args, std::size_t nargs,
                   wasmtime_val_t *results, std::size_t nresults) noexcept
    -> wasm_trap_t * {
    try {
        return import_log(env, caller, args, nargs, results, nresults);
    } catch (...) {
        return set_status(results, nresults, host::import_internal_failure);
    }
}

[[nodiscard]] auto
guarded_import_config_get(void *env, wasmtime_caller_t *caller,
                          const wasmtime_val_t *args, std::size_t nargs,
                          wasmtime_val_t *results,
                          std::size_t nresults) noexcept -> wasm_trap_t * {
    try {
        return import_config_get(env, caller, args, nargs, results, nresults);
    } catch (...) {
        return set_status(results, nresults, host::import_internal_failure);
    }
}

[[nodiscard]] auto
guarded_import_tick(void *env, wasmtime_caller_t *caller,
                    const wasmtime_val_t *args, std::size_t nargs,
                    wasmtime_val_t *results, std::size_t nresults) noexcept
    -> wasm_trap_t * {
    try {
        return import_tick(env, caller, args, nargs, results, nresults);
    } catch (...) {
        return set_status(results, nresults, host::import_internal_failure);
    }
}

/// One import of the guest ABI: its name, the capability gating it and the
/// function signature the runtime validates against a module's import section.
struct import_shape {
    std::string_view name;
    host::capability capability;
    std::array<wasm_valkind_t, 4> parameters{};
    std::size_t parameter_count{0};
    bool returns_value{false};
    wasm_valkind_t result_kind{WASM_I32};
    wasmtime_func_callback_t callback{nullptr};
};

constexpr std::array<import_shape, 3> import_shapes{{
    import_shape{.name = host::log_import,
                 .capability = host::capability::log,
                 .parameters = {WASM_I32, WASM_I32, WASM_I32},
                 .parameter_count = 3,
                 .returns_value = true,
                 .callback = &guarded_import_log},
    import_shape{.name = host::config_get_import,
                 .capability = host::capability::config,
                 .parameters = {WASM_I32, WASM_I32, WASM_I32, WASM_I32},
                 .parameter_count = 4,
                 .returns_value = true,
                 .callback = &guarded_import_config_get},
    import_shape{.name = host::tick_import,
                 .capability = host::capability::clock,
                 .parameters = {},
                 .parameter_count = 0,
                 .returns_value = true,
                 .result_kind = WASM_I64,
                 .callback = &guarded_import_tick},
}};

[[nodiscard]] auto find_import(std::string_view module,
                               std::string_view name) noexcept
    -> const import_shape * {
    if (module != host::import_module) {
        return nullptr;
    }
    const auto found{
        std::ranges::find_if(import_shapes, [name](const import_shape &shape) {
            return shape.name == name;
        })};
    return found == import_shapes.end() ? nullptr : &*found;
}

/// Names the runtime exposes, for an operator-facing refusal.
[[nodiscard]] auto format_exposed_imports() -> std::string {
    auto text{std::string{}};
    for (const auto &shape : import_shapes) {
        if (!text.empty()) {
            text += ", ";
        }
        text += host::import_module;
        text += '.';
        text += shape.name;
    }
    return text;
}

/// True when the import's wasm function type is exactly the ABI function type
/// of `shape`.
[[nodiscard]] auto import_type_matches(const wasm_functype_t *type,
                                       const import_shape &shape) noexcept
    -> bool {
    const auto *parameters{wasm_functype_params(type)};
    const auto *results{wasm_functype_results(type)};
    if (parameters == nullptr || results == nullptr ||
        parameters->size != shape.parameter_count) {
        return false;
    }
    for (std::size_t index = 0; index < shape.parameter_count; ++index) {
        if (wasm_valtype_kind(parameters->data[index]) !=
            shape.parameters[index]) {
            return false;
        }
    }
    const auto expected_results{shape.returns_value ? 1U : 0U};
    if (results->size != expected_results) {
        return false;
    }
    return expected_results == 0 ||
           wasm_valtype_kind(results->data[0]) == shape.result_kind;
}

/// Check the module's import section against the granted host imports. Refused
/// before instantiation with a message naming the offending import, the
/// exposed imports and the active grant set.
[[nodiscard]] auto validate_imports(const wasmtime_module_t *module,
                                    const host_import_env &env) -> status {
    wasm_importtype_vec_t imports = WASM_EMPTY_VEC;
    wasmtime_module_imports(module, &imports);
    auto checked{status_ok()};
    for (std::size_t index = 0; index < imports.size; ++index) {
        const auto *type{wasm_importtype_type(imports.data[index])};
        const auto *module_name{wasm_importtype_module(imports.data[index])};
        const auto *import_name{wasm_importtype_name(imports.data[index])};
        const std::string_view module_text{
            module_name != nullptr && module_name->data != nullptr
                ? std::string_view{module_name->data, module_name->size}
                : std::string_view{}};
        const std::string_view name_text{
            import_name != nullptr && import_name->data != nullptr
                ? std::string_view{import_name->data, import_name->size}
                : std::string_view{}};
        const auto qualified{host::quote_guest_name(module_text) + "." +
                             host::quote_guest_name(name_text)};
        const auto *shape{find_import(module_text, name_text)};
        if (shape == nullptr) {
            checked = std::unexpected{
                fail(error_code::permission_denied,
                     "sandboxed guest requested host import '" + qualified +
                         "', which the script host does not expose; exposed "
                         "imports are [" +
                         format_exposed_imports() +
                         "] and the active grant set is [" +
                         host::format_grants(env.grants) + "]")};
            break;
        }
        if (!env.grants.contains(shape->capability)) {
            checked = std::unexpected{
                fail(error_code::permission_denied,
                     "sandboxed guest requested host import '" + qualified +
                         "', which requires capability '" +
                         std::string{host::capability_name(shape->capability)} +
                         "'; the active grant set is [" +
                         host::format_grants(env.grants) + "]")};
            break;
        }
        if (shape->capability != host::capability::log &&
            env.environment == nullptr) {
            checked = std::unexpected{
                fail(error_code::not_initialized,
                     "host import '" + qualified +
                         "' was granted but no host state was provided to the "
                         "guest runtime")};
            break;
        }
        if (type == nullptr || wasm_externtype_kind(type) != WASM_EXTERN_FUNC) {
            checked = std::unexpected{fail(
                error_code::permission_denied,
                "sandboxed guest imported '" + qualified +
                    "' as a non-function extern; the script host exposes only "
                    "functions")};
            break;
        }
        if (!import_type_matches(wasm_externtype_as_functype_const(type),
                                 *shape)) {
            checked = std::unexpected{
                fail(error_code::permission_denied,
                     "sandboxed guest imported host function '" + qualified +
                         "' with an incompatible signature; the script host "
                         "defines its documented signature")};
            break;
        }
    }
    wasm_importtype_vec_delete(&imports);
    return checked;
}

/// Define one granted host import in the runtime's linker.
[[nodiscard]] auto define_import(wasmtime_linker_t *linker,
                                 const import_shape &shape,
                                 host_import_env *env) -> status {
    std::array<wasm_valtype_t *, 4> parameters{};
    parameters.fill(nullptr);
    for (std::size_t index = 0; index < shape.parameter_count; ++index) {
        parameters[index] = wasm_valtype_new(shape.parameters[index]);
    }
    wasm_valtype_t *results[1]{
        shape.returns_value ? wasm_valtype_new(shape.result_kind) : nullptr};
    wasm_valtype_vec_t parameter_vec = WASM_EMPTY_VEC;
    wasm_valtype_vec_new(&parameter_vec, shape.parameter_count,
                         parameters.data());
    wasm_valtype_vec_t result_vec = WASM_EMPTY_VEC;
    wasm_valtype_vec_new(&result_vec, shape.returns_value ? 1U : 0U, results);
    wasm_functype_t *type{wasm_functype_new(&parameter_vec, &result_vec)};
    auto *problem{wasmtime_linker_define_func(
        linker, host::import_module.data(), host::import_module.size(),
        shape.name.data(), shape.name.size(), type, shape.callback, env,
        nullptr)};
    wasm_functype_delete(type);
    if (problem != nullptr) {
        return std::unexpected{take_error(problem)};
    }
    return status_ok();
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
    detail::host_import_env imports{};

    // Wall-clock enforcement. One watchdog thread per runtime arms a single
    // deadline: call dispatch is serialised by the loader contract, so there is
    // never more than one deadline in flight. The thread increments the engine
    // epoch when the deadline passes, which traps the running guest; an
    // increment that races with a finished call is harmless.
    std::mutex deadline_mutex{};
    std::condition_variable deadline_ready{};
    std::thread deadline_thread{};
    bool deadline_running{false};
    bool deadline_stop{false};
    bool deadline_fired{false};
    std::optional<std::chrono::steady_clock::time_point> deadline_point{};

    ~impl() {
        stop_deadline_thread();
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

    void deadline_loop() {
        while (true) {
            std::unique_lock lock{deadline_mutex};
            deadline_ready.wait(lock, [this] {
                return deadline_stop || deadline_point.has_value();
            });
            if (deadline_stop) {
                return;
            }
            const auto target{*deadline_point};
            deadline_ready.wait_until(lock, target, [this, target] {
                return deadline_stop || !deadline_point.has_value() ||
                       *deadline_point != target;
            });
            if (deadline_stop) {
                return;
            }
            if (!deadline_point.has_value() || *deadline_point != target) {
                // The call finished in time: `disarm_deadline` consumed it.
                continue;
            }
            deadline_fired = true;
            deadline_point.reset();
            lock.unlock();
            wasmtime_engine_increment_epoch(engine);
        }
    }

    [[nodiscard]] auto arm_deadline(std::uint64_t timeout_ms) -> status {
        if (timeout_ms == 0) {
            // A call without a deadline must not inherit the expired flag from
            // an earlier timed-out call: without this reset a later call would
            // report its own trap as a deadline expiry.
            std::unique_lock lock{deadline_mutex};
            deadline_fired = false;
            deadline_point.reset();
            lock.unlock();
            deadline_ready.notify_all();
            return status_ok();
        }
        if (!config.epoch_interruption) {
            return std::unexpected{
                fail(error_code::unsupported,
                     "wasmtime: a call deadline needs a runtime created with "
                     "epoch interruption enabled")};
        }
        std::unique_lock lock{deadline_mutex};
        deadline_fired = false;
        deadline_point = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds{timeout_ms};
        if (!deadline_running) {
            try {
                deadline_thread = std::thread{[this] { deadline_loop(); }};
            } catch (const std::system_error &) {
                deadline_point.reset();
                return std::unexpected{
                    fail(error_code::out_of_memory,
                         "wasmtime: cannot start the call deadline thread")};
            }
            deadline_running = true;
        }
        lock.unlock();
        deadline_ready.notify_all();
        return status_ok();
    }

    /// Cancel the in-flight deadline and report whether it fired.
    [[nodiscard]] auto disarm_deadline() -> bool {
        std::unique_lock lock{deadline_mutex};
        deadline_point.reset();
        const bool fired{deadline_fired};
        lock.unlock();
        deadline_ready.notify_all();
        return fired;
    }

    void stop_deadline_thread() noexcept {
        std::unique_lock lock{deadline_mutex};
        if (!deadline_running) {
            return;
        }
        deadline_stop = true;
        deadline_point.reset();
        lock.unlock();
        deadline_ready.notify_all();
        if (deadline_thread.joinable()) {
            deadline_thread.join();
        }
        deadline_running = false;
    }

    /// Fuel left in the store, or nullopt when fuel accounting is disabled.
    [[nodiscard]] auto fuel_left() const -> std::optional<std::uint64_t> {
        if (!config.consume_fuel || context == nullptr) {
            return std::nullopt;
        }
        std::uint64_t fuel{0};
        if (wasmtime_context_get_fuel(context, &fuel) != nullptr) {
            return std::nullopt;
        }
        return fuel;
    }

    /// Resolve the guest's imports through the linker, instantiate it and run
    /// its start function. The import section is validated against the
    /// runtime's host imports before Wasmtime sees it, so a refused import
    /// names the import and the grant set instead of a linker string.
    [[nodiscard]] auto instantiate_module(const wasmtime_module_t *module,
                                          wasmtime_instance_t &instance_out)
        -> status {
        if (auto checked{validate_imports(module, imports)}; !checked) {
            return checked;
        }
        wasm_trap_t *trap{nullptr};
        if (auto *error{wasmtime_linker_instantiate(linker, context, module,
                                                    &instance_out, &trap)}) {
            return std::unexpected{take_error(error)};
        }
        if (trap != nullptr) {
            return std::unexpected{take_trap(trap)};
        }
        return status_ok();
    }

    /// Call an exported `(i32, ...) -> i32` function on an instantiated guest
    /// under the per-call budgets in `options`. Fuel and deadline are
    /// provisioned for this call only; a trapped call leaves the store dirty,
    /// which the caller treats as a quarantined instance.
    [[nodiscard]] auto call_export(const wasmtime_instance_t &guest_instance,
                                   std::string_view export_name,
                                   std::span<const std::int32_t> args,
                                   call_options options)
        -> result<std::int32_t> {
        wasmtime_extern_t item{};
        if (!wasmtime_instance_export_get(context, &guest_instance,
                                          export_name.data(),
                                          export_name.size(), &item)) {
            return std::unexpected{
                fail(error_code::not_found,
                     "wasmtime: export '" +
                         host::quote_guest_name(export_name) + "' not found")};
        }
        if (item.kind != WASMTIME_EXTERN_FUNC) {
            return std::unexpected{fail(
                error_code::invalid_argument,
                "wasmtime: export '" + host::quote_guest_name(export_name) +
                    "' is not a function")};
        }

        // Check the export's signature before calling it: a callback with the
        // wrong arity or a non-i32 result is a caller mistake, not guest
        // misbehaviour, and must be reported as `invalid_argument` before any
        // guest code runs instead of surfacing as a Wasmtime call error.
        wasm_functype_t *function_type{
            wasmtime_func_type(context, &item.of.func)};
        if (function_type == nullptr) {
            return std::unexpected{
                fail(error_code::mod_error,
                     "wasmtime: cannot read the signature of export '" +
                         host::quote_guest_name(export_name) + "'")};
        }
        const auto *parameters{wasm_functype_params(function_type)};
        const auto *results{wasm_functype_results(function_type)};
        auto signature_matches{parameters != nullptr && results != nullptr &&
                               parameters->size == args.size() &&
                               results->size == 1};
        if (signature_matches) {
            for (std::size_t index = 0; index < parameters->size; ++index) {
                if (wasm_valtype_kind(parameters->data[index]) != WASM_I32) {
                    signature_matches = false;
                    break;
                }
            }
            signature_matches = signature_matches &&
                                wasm_valtype_kind(results->data[0]) == WASM_I32;
        }
        wasm_functype_delete(function_type);
        if (!signature_matches) {
            return std::unexpected{fail(
                error_code::invalid_argument,
                "wasmtime: export '" + host::quote_guest_name(export_name) +
                    "' does not implement the (" + std::to_string(args.size()) +
                    " x i32) -> i32 call ABI")};
        }

        if (options.fuel != 0) {
            if (auto *error{wasmtime_context_set_fuel(context, options.fuel)}) {
                return std::unexpected{take_error(error)};
            }
        }
        imports.log_lines_remaining = host::max_log_messages_per_call;
        imports.logs.clear();
        auto armed{arm_deadline(options.timeout_ms)};
        if (!armed) {
            return std::unexpected{std::move(armed.error())};
        }
        if (options.timeout_ms != 0) {
            wasmtime_context_set_epoch_deadline(context, 1);
        } else if (config.epoch_interruption) {
            // The store starts with epoch deadline 0, which traps the guest
            // immediately when epoch interruption is enabled. A call without
            // a deadline clears the interruption instead.
            wasmtime_context_set_epoch_deadline(
                context, std::numeric_limits<std::uint64_t>::max());
        }

        auto wasm_args{std::vector<wasmtime_val_t>(args.size())};
        for (std::size_t index = 0; index < args.size(); ++index) {
            wasm_args[index].kind = WASMTIME_I32;
            wasm_args[index].of.i32 = args[index];
        }
        wasmtime_val_t result{};
        wasm_trap_t *trap{nullptr};
        auto *error{wasmtime_func_call(context, &item.of.func, wasm_args.data(),
                                       wasm_args.size(), &result, 1, &trap)};
        const auto ran_out_of_time{disarm_deadline()};
        if (error != nullptr) {
            return std::unexpected{take_error(error)};
        }
        if (trap != nullptr) {
            if (ran_out_of_time) {
                return std::unexpected{fail(
                    error_code::timeout,
                    "wasmtime: call to '" + std::string{export_name} +
                        "' exceeded its " + std::to_string(options.timeout_ms) +
                        "ms deadline")};
            }
            return std::unexpected{take_trap(trap)};
        }
        if (result.kind != WASMTIME_I32) {
            return std::unexpected{fail(error_code::mod_error,
                                        "wasmtime: exported function does "
                                        "not return an i32 value")};
        }
        return result.of.i32;
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

auto runtime::create(engine_config config, host_imports imports)
    -> result<runtime> {
    auto state{std::make_unique<impl>()};
    state->config = config;
    state->imports.grants = imports.grants;
    state->imports.environment = imports.environment;

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
    if (config.epoch_interruption) {
        wasmtime_config_epoch_interruption_set(wasm_config, true);
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
    for (const auto &shape : import_shapes) {
        // `log` needs no host state; the state-reading imports exist only when
        // the caller supplied the state they read.
        const bool available{shape.capability == host::capability::log ||
                             state->imports.environment != nullptr};
        if (!state->imports.grants.contains(shape.capability) || !available) {
            continue;
        }
        if (auto defined{define_import(state->linker, shape, &state->imports)};
            !defined) {
            return std::unexpected{std::move(defined.error())};
        }
    }

    if (config.consume_fuel) {
        if (auto *error{
                wasmtime_context_set_fuel(state->context, config.fuel)}) {
            return std::unexpected{take_error(error)};
        }
    }
    if (config.epoch_interruption) {
        // Epoch interruption starts armed at deadline 0, so every guest
        // instruction would trap; the load path runs without a wall-clock
        // deadline and only call dispatch arms one.
        wasmtime_context_set_epoch_deadline(
            state->context, std::numeric_limits<std::uint64_t>::max());
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
    auto instantiated{
        impl_->instantiate_module(impl_->modules[module], impl_->instance)};
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
                       std::span<const std::int32_t> args, call_options options)
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
    auto instantiated{
        impl_->instantiate_module(impl_->modules[module], instance)};
    if (!instantiated) {
        return std::unexpected{std::move(instantiated.error())};
    }
    return impl_->call_export(instance, export_name, args, options);
}

auto runtime::call_instance_i32(std::string_view export_name,
                                std::span<const std::int32_t> args,
                                call_options options) -> result<std::int32_t> {
    if (impl_ == nullptr || !impl_->instance_live) {
        return std::unexpected{
            fail(error_code::not_initialized,
                 "wasmtime: runtime has no instantiated guest")};
    }
    return impl_->call_export(impl_->instance, export_name, args, options);
}

auto runtime::write_guest_memory(std::uint64_t offset,
                                 std::span<const std::byte> bytes) -> status {
    if (impl_ == nullptr || !impl_->instance_live) {
        return std::unexpected{
            fail(error_code::not_initialized,
                 "wasmtime: runtime has no instantiated guest")};
    }
    wasmtime_extern_t item{};
    constexpr std::string_view export_name{"memory"};
    if (!wasmtime_instance_export_get(impl_->context, &impl_->instance,
                                      export_name.data(), export_name.size(),
                                      &item) ||
        item.kind != WASMTIME_EXTERN_MEMORY) {
        return std::unexpected{
            fail(error_code::not_found,
                 "wasmtime: guest does not export a linear memory")};
    }
    const auto available{static_cast<std::uint64_t>(
        wasmtime_memory_data_size(impl_->context, &item.of.memory))};
    if (offset > available || bytes.size() > available - offset) {
        return std::unexpected{fail(
            error_code::invalid_argument,
            "wasmtime: guest memory write of " + std::to_string(bytes.size()) +
                " bytes at offset " + std::to_string(offset) + " exceeds the " +
                std::to_string(available) + "-byte memory")};
    }
    if (!bytes.empty()) {
        std::memcpy(wasmtime_memory_data(impl_->context, &item.of.memory) +
                        offset,
                    bytes.data(), bytes.size());
    }
    return status_ok();
}

auto runtime::take_guest_logs() -> std::vector<guest_log_entry> {
    if (impl_ == nullptr) {
        return {};
    }
    auto logs{std::move(impl_->imports.logs)};
    impl_->imports.logs.clear();
    return logs;
}

auto runtime::fuel_remaining() const noexcept -> std::uint64_t {
    if (impl_ == nullptr) {
        return 0;
    }
    return impl_->fuel_left().value_or(0);
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

auto runtime::create(engine_config /*config*/, host_imports /*imports*/)
    -> result<runtime> {
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
                       std::span<const std::int32_t> /*args*/,
                       call_options /*options*/) -> result<std::int32_t> {
    return std::unexpected{make_error(error_code::unsupported,
                                      "wasmtime is not enabled in this build")};
}

auto runtime::call_instance_i32(std::string_view /*name*/,
                                std::span<const std::int32_t> /*args*/,
                                call_options /*options*/)
    -> result<std::int32_t> {
    return std::unexpected{make_error(error_code::unsupported,
                                      "wasmtime is not enabled in this build")};
}

auto runtime::write_guest_memory(std::uint64_t /*offset*/,
                                 std::span<const std::byte> /*bytes*/)
    -> status {
    return std::unexpected{make_error(error_code::unsupported,
                                      "wasmtime is not enabled in this build")};
}

auto runtime::take_guest_logs() -> std::vector<guest_log_entry> { return {}; }

auto runtime::fuel_remaining() const noexcept -> std::uint64_t { return 0; }

auto runtime::memory_bytes() const noexcept -> std::uint64_t { return 0; }

auto runtime::module_count() const noexcept -> std::size_t { return 0U; }

} // namespace refrain::script::wasm

#endif // REFRAIN_WITH_WASM
