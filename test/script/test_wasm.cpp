// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/script/wasm.hpp>

#include <boost/ut.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#if REFRAIN_WITH_WASM

namespace {

using namespace boost::ut;
using refrain::script::wasm::runtime;
namespace wasm = refrain::script::wasm;
namespace host = refrain::script::host;

constexpr std::string_view add_wat{R"((module
  (func (export "add") (param i32 i32) (result i32)
    local.get 0
    local.get 1
    i32.add)
  (func (export "mul") (param i32 i32) (result i32)
    local.get 0
    local.get 1
    i32.mul)))"};

constexpr std::string_view spin_wat{R"((module
  (func (export "spin") (result i32)
    (local $sum i32)
    (local $i i32)
    (loop $loop
      local.get $sum
      local.get $i
      i32.add
      local.set $sum
      local.get $i
      i32.const 1
      i32.add
      local.tee $i
      i32.const 1000000
      i32.lt_s
      br_if $loop)
    local.get $sum)))"};

constexpr std::string_view import_log_wat{R"((module
  (import "refrain" "log" (func $log (param i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 16) "hello")
  (data (i32.const 32) "a\0ab")
  (func (export "emit") (result i32)
    i32.const 2
    i32.const 16
    i32.const 5
    call $log)
  (func (export "emit_oversized") (result i32)
    i32.const 2
    i32.const 16
    i32.const 2048
    call $log)
  (func (export "emit_control") (result i32)
    i32.const 2
    i32.const 32
    i32.const 3
    call $log)
  (func (export "emit_bad_level") (result i32)
    i32.const 7
    i32.const 16
    i32.const 5
    call $log)
  (func (export "emit_many") (result i32)
    (local $index i32)
    (local $sum i32)
    (loop $loop
      local.get $sum
      i32.const 2
      i32.const 16
      i32.const 5
      call $log
      i32.add
      local.set $sum
      local.get $index
      i32.const 1
      i32.add
      local.tee $index
      i32.const 100
      i32.lt_u
      br_if $loop)
    local.get $sum)))"};

constexpr std::string_view import_state_wat{R"((module
  (import "refrain" "config_get" (func $config (param i32 i32 i32 i32) (result i32)))
  (import "refrain" "tick" (func $tick (result i64)))
  (memory (export "memory") 1)
  (data (i32.const 16) "greeting")
  (data (i32.const 32) "missing")
  (func (export "read_config") (result i32)
    i32.const 16
    i32.const 8
    i32.const 64
    i32.const 64
    call $config)
  (func (export "read_config_small") (result i32)
    i32.const 16
    i32.const 8
    i32.const 64
    i32.const 2
    call $config)
  (func (export "read_missing") (result i32)
    i32.const 32
    i32.const 7
    i32.const 64
    i32.const 64
    call $config)
  (func (export "read_tick") (result i32)
    call $tick
    i32.wrap_i64)))"};

constexpr std::string_view ungranted_import_wat{R"((module
  (import "refrain" "config_get" (func $config (param i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (func (export "call") (result i32)
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    call $config)))"};

constexpr std::string_view unknown_import_wat{R"((module
  (import "refrain" "register_provider" (func $register (result i32)))
  (func (export "call") (result i32) call $register)))"};

constexpr std::string_view bad_signature_wat{R"((module
  (import "refrain" "log" (func $log (param i32) (result i32)))
  (func (export "call") (result i32) i32.const 0 call $log)))"};

constexpr std::string_view forever_wat{R"((module
  (func (export "forever") (result i32)
    (loop $loop
      br $loop)
    i32.const 0)))"};

constexpr std::string_view burn_wat{R"((module
  (func (export "burn") (result i32)
    (local $index i32)
    (loop $loop
      local.get $index
      i32.const 1
      i32.add
      local.set $index
      local.get $index
      i32.const 3000
      i32.lt_u
      br_if $loop)
    local.get $index)))"};

} // namespace

namespace {

using namespace boost::ut;

const suite wasm_tests = [] {
    "wasm/compile WAT and call an exported function"_test = [] {
        auto runtime{runtime::create()};
        expect(static_cast<bool>(runtime));
        if (!runtime) {
            return;
        }
        auto module{runtime->compile_wat(add_wat)};
        expect(static_cast<bool>(module));
        if (!module) {
            return;
        }
        expect(runtime->module_count() == 1U);

        const std::array<std::int32_t, 2> args{2, 3};
        auto result{runtime->call_i32(*module, "add", args)};
        expect(static_cast<bool>(result));
        if (result) {
            expect(*result == 5);
        }

        const std::array<std::int32_t, 2> mul_args{6, 7};
        auto mul{runtime->call_i32(*module, "mul", mul_args)};
        expect(static_cast<bool>(mul));
        if (mul) {
            expect(*mul == 42);
        }

        // Re-instantiation is allowed and does not consume the module.
        auto again{runtime->call_i32(*module, "add", args)};
        expect(again && *again == 5);
    };

    "wasm/compile invalid WAT reports a typed error"_test = [] {
        auto runtime{runtime::create()};
        expect(static_cast<bool>(runtime));
        if (!runtime) {
            return;
        }
        auto module{runtime->compile_wat("(module (func (export \"x\")")};
        expect(!module);
        expect(module.error().code == refrain::error_code::mod_error);
        expect(runtime->module_count() == 0U);

        const std::array<std::byte, 4> invalid{
            std::byte{0x00}, std::byte{0x61}, std::byte{0x73}, std::byte{0x6D}};
        auto bytes{runtime->compile_bytes(invalid)};
        expect(!bytes);
        expect(bytes.error().code == refrain::error_code::mod_error);
    };

    "wasm/missing exports and bad handles are reported"_test = [] {
        auto runtime{runtime::create()};
        expect(static_cast<bool>(runtime));
        if (!runtime) {
            return;
        }
        auto module{runtime->compile_wat(add_wat)};
        expect(static_cast<bool>(module));
        if (!module) {
            return;
        }

        const std::array<std::int32_t, 2> args{1, 2};
        auto missing{runtime->call_i32(*module, "missing", args)};
        expect(!missing);
        expect(missing.error().code == refrain::error_code::not_found);

        auto bad_handle{runtime->call_i32(refrain::script::wasm::invalid_module,
                                          "add", args)};
        expect(!bad_handle);
        expect(bad_handle.error().code == refrain::error_code::not_found);

        // Calling the wrong number of arguments fails inside Wasmtime.
        const std::array<std::int32_t, 1> one_arg{1};
        auto wrong_args{runtime->call_i32(*module, "add", one_arg)};
        expect(!wrong_args);
        expect(wrong_args.error().code == refrain::error_code::mod_error);
    };

    "wasm/grant sets format and parse deterministically"_test = [] {
        const auto grants{host::capability::clock | host::capability::config |
                          host::capability::log};
        expect(host::format_grants(grants) == "clock, config, log");
        expect(host::format_grants(host::grant_set{}) == "");
        expect(host::parse_capability("config") == host::capability::config);
        expect(!host::parse_capability("services"));
    };

    "wasm/a granted host import reaches the host and collects a log line"_test =
        [] {
            wasm::host_imports imports{};
            imports.grants = host::capability::log;
            auto runtime{runtime::create({}, imports)};
            expect(static_cast<bool>(runtime));
            if (!runtime) {
                return;
            }
            auto module{runtime->compile_wat(import_log_wat)};
            expect(static_cast<bool>(module));
            if (!module) {
                return;
            }
            auto emitted{runtime->call_i32(*module, "emit", {})};
            expect(emitted && *emitted == host::import_ok);
            const auto logs{runtime->take_guest_logs()};
            expect(logs.size() == 1U);
            if (logs.size() == 1U) {
                expect(logs.front().level == 2U);
                expect(logs.front().message == "hello");
            }
        };

    "wasm/an ungranted host import is refused naming the grant set"_test = [] {
        wasm::host_imports imports{};
        imports.grants = host::capability::log;
        auto runtime{runtime::create({}, imports)};
        expect(static_cast<bool>(runtime));
        if (!runtime) {
            return;
        }
        auto module{runtime->compile_wat(ungranted_import_wat)};
        expect(static_cast<bool>(module));
        if (!module) {
            return;
        }
        auto called{runtime->call_i32(*module, "call", {})};
        expect(!called);
        if (!called) {
            expect(called.error().code ==
                   refrain::error_code::permission_denied);
            expect(called.error().message.find("refrain.config_get") !=
                   std::string::npos);
            expect(called.error().message.find("capability 'config'") !=
                   std::string::npos);
            expect(called.error().message.find("active grant set is [log]") !=
                   std::string::npos);
        }
    };

    "wasm/an unknown host import is refused before instantiation"_test = [] {
        wasm::host_imports imports{};
        imports.grants = host::capability::log;
        auto runtime{runtime::create({}, imports)};
        expect(static_cast<bool>(runtime));
        if (!runtime) {
            return;
        }
        auto module{runtime->compile_wat(unknown_import_wat)};
        expect(static_cast<bool>(module));
        if (!module) {
            return;
        }
        auto called{runtime->call_i32(*module, "call", {})};
        expect(!called);
        if (!called) {
            expect(called.error().code ==
                   refrain::error_code::permission_denied);
            expect(called.error().message.find("host import") !=
                   std::string::npos);
            expect(called.error().message.find("register_provider") !=
                   std::string::npos);
            expect(called.error().message.find("active grant set is [log]") !=
                   std::string::npos);
        }
    };

    "wasm/an import with the wrong signature is refused"_test = [] {
        wasm::host_imports imports{};
        imports.grants = host::capability::log;
        auto runtime{runtime::create({}, imports)};
        expect(static_cast<bool>(runtime));
        if (!runtime) {
            return;
        }
        auto module{runtime->compile_wat(bad_signature_wat)};
        expect(static_cast<bool>(module));
        if (!module) {
            return;
        }
        auto called{runtime->call_i32(*module, "call", {})};
        expect(!called);
        if (!called) {
            expect(called.error().code ==
                   refrain::error_code::permission_denied);
            expect(called.error().message.find("incompatible signature") !=
                   std::string::npos);
        }
    };

    "wasm/log import bounds length, level and text"_test = [] {
        wasm::host_imports imports{};
        imports.grants = host::capability::log;
        auto runtime{runtime::create({}, imports)};
        expect(static_cast<bool>(runtime));
        if (!runtime) {
            return;
        }
        auto module{runtime->compile_wat(import_log_wat)};
        expect(static_cast<bool>(module));
        if (!module) {
            return;
        }
        const auto expect_refused = [&](std::string_view export_name) {
            auto called{runtime->call_i32(*module, export_name, {})};
            expect(called.has_value());
            if (called) {
                expect(*called == host::import_invalid_argument);
            }
            expect(runtime->take_guest_logs().empty());
        };
        expect_refused("emit_oversized");
        expect_refused("emit_control");
        expect_refused("emit_bad_level");

        // A guest cannot turn its instruction budget into unbounded log
        // volume: past the per-call quota the import refuses and the guest
        // sees `import_quota_exceeded` (36 refusals after 64 accepted lines).
        auto many{runtime->call_i32(*module, "emit_many", {})};
        expect(many.has_value());
        if (many) {
            expect(*many == 36 * host::import_quota_exceeded);
        }
        expect(runtime->take_guest_logs().size() ==
               host::max_log_messages_per_call);
    };

    "wasm/config and tick imports read the sealed environment"_test = [] {
        host::environment environment{};
        environment.config.emplace("greeting", "hello");
        environment.tick_index = 42;
        wasm::host_imports imports{};
        imports.grants = host::capability::log | host::capability::config |
                         host::capability::clock;
        imports.environment = &environment;
        auto runtime{runtime::create({}, imports)};
        expect(static_cast<bool>(runtime));
        if (!runtime) {
            return;
        }
        auto module{runtime->compile_wat(import_state_wat)};
        expect(static_cast<bool>(module));
        if (!module) {
            return;
        }
        auto read{runtime->call_i32(*module, "read_config", {})};
        expect(read && *read == 5)
            << (read ? "value " + std::to_string(*read) : read.error().message);
        auto small{runtime->call_i32(*module, "read_config_small", {})};
        expect(small && *small == host::import_buffer_too_small)
            << (small ? "value " + std::to_string(*small)
                      : small.error().message);
        auto missing{runtime->call_i32(*module, "read_missing", {})};
        expect(missing && *missing == host::import_not_found)
            << (missing ? "value " + std::to_string(*missing)
                        : missing.error().message);
        auto tick{runtime->call_i32(*module, "read_tick", {})};
        expect(tick && *tick == 42)
            << (tick ? "value " + std::to_string(*tick) : tick.error().message);
    };

    "wasm/a call deadline interrupts a runaway guest"_test = [] {
        wasm::engine_config config{};
        config.consume_fuel = true;
        config.fuel = 1'000'000;
        config.epoch_interruption = true;
        auto runtime{runtime::create(config)};
        expect(static_cast<bool>(runtime));
        if (!runtime) {
            return;
        }
        auto module{runtime->compile_wat(forever_wat)};
        expect(static_cast<bool>(module));
        if (!module) {
            return;
        }
        const auto started{std::chrono::steady_clock::now()};
        auto called{runtime->call_i32(
            *module, "forever", {},
            wasm::call_options{.fuel = 1'000'000'000, .timeout_ms = 20})};
        const auto elapsed{std::chrono::steady_clock::now() - started};
        expect(!called);
        if (!called) {
            expect(called.error().code == refrain::error_code::timeout);
            expect(called.error().message.find("deadline") !=
                   std::string::npos);
        }
        expect(elapsed < std::chrono::seconds{10});
    };

    "wasm/a call deadline needs an interrupting runtime"_test = [] {
        auto runtime{runtime::create(
            wasm::engine_config{.consume_fuel = true, .fuel = 1'000'000})};
        expect(static_cast<bool>(runtime));
        if (!runtime) {
            return;
        }
        auto module{runtime->compile_wat(forever_wat)};
        expect(static_cast<bool>(module));
        if (!module) {
            return;
        }
        auto called{runtime->call_i32(*module, "forever", {},
                                      wasm::call_options{.timeout_ms = 5})};
        expect(!called);
        if (!called) {
            expect(called.error().code == refrain::error_code::unsupported);
            expect(called.error().message.find("epoch interruption") !=
                   std::string::npos);
        }
    };

    "wasm/each call provisions its own instruction budget"_test = [] {
        // The store starts with a load-path budget far too small for the
        // fixture; a call still runs on the budget it provisions itself.
        wasm::engine_config config{};
        config.consume_fuel = true;
        config.fuel = 64;
        auto runtime{runtime::create(config)};
        expect(static_cast<bool>(runtime));
        if (!runtime) {
            return;
        }
        auto module{runtime->compile_wat(burn_wat)};
        expect(static_cast<bool>(module));
        if (!module) {
            return;
        }
        auto first{runtime->call_i32(*module, "burn", {},
                                     wasm::call_options{.fuel = 1'000'000})};
        expect(first && *first == 3000);
        const auto consumed{1'000'000U - runtime->fuel_remaining()};
        expect(consumed > 0U);
        // One call needs `consumed`; the budget below covers one call but not
        // two, so the second success proves the budget is replenished, not
        // shared with the previous call.
        const auto per_call{consumed + consumed / 2};
        auto second{runtime->call_i32(*module, "burn", {},
                                      wasm::call_options{.fuel = per_call})};
        expect(second && *second == 3000);
        auto third{runtime->call_i32(*module, "burn", {},
                                     wasm::call_options{.fuel = per_call})};
        expect(third && *third == 3000);
    };

    "wasm/a call that exhausts its instruction budget traps"_test = [] {
        wasm::engine_config config{};
        config.consume_fuel = true;
        config.fuel = 1'000'000;
        auto runtime{runtime::create(config)};
        expect(static_cast<bool>(runtime));
        if (!runtime) {
            return;
        }
        auto module{runtime->compile_wat(burn_wat)};
        expect(static_cast<bool>(module));
        if (!module) {
            return;
        }
        auto called{runtime->call_i32(*module, "burn", {},
                                      wasm::call_options{.fuel = 100})};
        expect(!called);
        expect(runtime->fuel_remaining() == 0U);
    };

    "wasm/fuel exhaustion traps the module"_test = [] {
        refrain::script::wasm::engine_config config{};
        config.consume_fuel = true;
        config.fuel = 1000;
        auto runtime{runtime::create(config)};
        expect(static_cast<bool>(runtime));
        if (!runtime) {
            return;
        }
        auto module{runtime->compile_wat(spin_wat)};
        expect(static_cast<bool>(module));
        if (!module) {
            return;
        }
        auto result{runtime->call_i32(*module, "spin", {})};
        expect(!result);
        expect(result.error().code == refrain::error_code::mod_error);
    };
};

} // namespace

#else // REFRAIN_WITH_WASM

namespace {

using namespace boost::ut;

const suite wasm_disabled_tests = [] {
    "wasm/disabled builds report unsupported"_test = [] {
        auto runtime{refrain::script::wasm::runtime::create()};
        expect(!runtime);
        expect(runtime.error().code == refrain::error_code::unsupported);
    };
};

} // namespace

#endif // REFRAIN_WITH_WASM
