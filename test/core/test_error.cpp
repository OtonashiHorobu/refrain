// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/error.hpp>

#include <boost/ut.hpp>

#include <string>
#include <utility>

namespace {
using namespace boost::ut;

[[maybe_unused]] const suite tests = [] {
    "error_code has stable text"_test = [] {
        expect(!refrain::to_string(refrain::error_code::unknown).empty());
        expect(refrain::to_string(refrain::error_code::io_error) == "io_error");
        expect(refrain::to_string(refrain::error_code::mod_error) ==
               "mod_error");
    };

    "make_error carries code and message"_test = [] {
        const auto err = refrain::make_error(refrain::error_code::not_found,
                                             std::string{"missing entry"});
        expect(err.code == refrain::error_code::not_found);
        expect(err.message == "missing entry");
        expect(err.location.line() > 0U);
    };

    "status success is a value"_test = [] {
        const refrain::status ok = refrain::status_ok();
        expect(static_cast<bool>(ok));
        expect(ok.has_value());
    };

    "result<T> propagates failures"_test = [] {
        auto fail = []() -> refrain::result<int> {
            return std::unexpected(refrain::make_error(
                refrain::error_code::cancelled, "cancelled"));
        };
        auto value = []() -> refrain::result<int> { return 42; };

        auto bad = fail();
        expect(!bad);
        expect(bad.error().code == refrain::error_code::cancelled);

        auto good = value();
        expect(static_cast<bool>(good));
        expect(*good == 42);
    };

    "string_view overload copies its message"_test = [] {
        const auto err =
            refrain::make_error(refrain::error_code::unknown, "copy");
        expect(err.message == "copy");
    };
};

} // namespace
