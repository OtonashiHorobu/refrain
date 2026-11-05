// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/version.hpp>

#include <boost/ut.hpp>

#include <string_view>

namespace {
using namespace boost::ut;

[[maybe_unused]] const suite tests = [] {
    "version constants are populated"_test = [] {
        expect(refrain::version::release_version != nullptr);
        expect(refrain::version::branch_name != nullptr);
        expect(refrain::version::commit_hash != nullptr);
    };

    "version string is human readable"_test = [] {
        const std::string_view text{refrain::version::string()};
        expect(!text.empty());
        expect(text.starts_with("Refrain "));
        expect(text.find(refrain::version::release_version) !=
               std::string_view::npos);
    };
};

} // namespace
