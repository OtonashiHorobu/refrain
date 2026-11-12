// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// The scratch-directory contract: a directory belongs to one test process,
// and the suites that use the helper do not build a temporary path themselves.

#include <refrain/core/io.hpp>

#include "scratch_directory.hpp"

#include <boost/ut.hpp>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace test_support = refrain::test_support;

} // namespace

suite<"support/scratch-directory"> scratch_directory_tests = [] {
    "a scratch directory is private and removed when it leaves scope"_test =
        [] {
            std::filesystem::path first;
            std::filesystem::path second;
            {
                const test_support::scratch_directory root;
                first = root.path();
                expect(std::filesystem::is_directory(first));
                expect(refrain::core::io::write_text(
                           (first / "fixture.txt").string(), "fixture")
                           .has_value());
            }
            expect(!std::filesystem::exists(first));
            {
                const test_support::scratch_directory root;
                second = root.path();
                expect(first != second);
                expect(std::filesystem::is_directory(second));
            }
            expect(!std::filesystem::exists(second));
        };

    "converted tests obtain temporary paths only from the helper"_test = [] {
        const std::filesystem::path root{REFRAIN_SOURCE_DIR};
        std::vector<std::filesystem::path> sources{root / "test" / "worldgen"};
        for (const auto *file :
             {"kernel/test_crash.cpp", "host/test_host_core.cpp",
              "host/test_host_platform.cpp", "mod/test_manifest.cpp",
              "mod/test_host_loading.cpp", "net/test_config_fallback.cpp",
              "platform/test_process.cpp"}) {
            sources.emplace_back(root / "test" / file);
        }
        std::vector<test_support::temporary_path_use> violations;
        for (const auto &source : sources) {
            auto found = test_support::temporary_path_violations(source);
            violations.insert(violations.end(), found.begin(), found.end());
        }
        for (const auto &use : violations) {
            std::fprintf(stderr, "%s:%zu builds a temporary path itself: %s\n",
                         use.file.string().c_str(), use.line,
                         use.token.c_str());
        }
        expect(violations.empty());
    };

    "the source check reports a file that builds its own temporary path"_test =
        [] {
            const test_support::scratch_directory root;
            // Split so that this test's own source does not carry the token
            // the check looks for.
            const auto offending =
                std::string{"auto path = std::filesystem::"} + "temp_" +
                "directory" + "_path() /\n    \"fixed\";\n";
            expect(refrain::core::io::write_text(
                       (root.path() / "offending.cpp").string(), offending)
                       .has_value());
            const auto violations =
                test_support::temporary_path_violations(root.path());
            expect(violations.size() == 1U);
            if (!violations.empty()) {
                expect(violations.front().line == 1U);
            }
        };
};
