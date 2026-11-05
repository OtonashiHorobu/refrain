// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/script/wasm.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#if REFRAIN_WITH_WASM

namespace {

using namespace boost::ut;
using refrain::script::wasm::runtime;

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
