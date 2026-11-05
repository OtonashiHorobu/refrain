// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// SemVer 2.0.0 / `requires` / `composition` contract metadata tests for the
// RIDL front end. The generator tests assert the emitted constants; these tests
// pin the sema rules and their diagnostics.

#include <schema/sema.hpp>

#include <schema/schema_test_util.hpp>

#include <boost/ut.hpp>

#include <array>
#include <string>
#include <string_view>

namespace {

using namespace boost::ut;
using refrain::test::compile;
using refrain::test::schema_source;

/// Build a one-service RIDL module with a parameterised service version,
/// composition line (`""` for the default) and requires line (`""` for none).
[[nodiscard]] auto service_schema(std::string_view semver,
                                  std::string_view composition_line,
                                  std::string_view requires_line)
    -> std::string {
    std::string source{"module refrain::services::ver;\nversion 1.0.0;\n"};
    source += "service ver : ";
    source += semver;
    source += " {\n    side = server;\n    thread = any;\n";
    source += composition_line;
    source += requires_line;
    source += "    provides = [ \"refrain:ver\" ];\n    fn ping() -> u32;\n}\n";
    return source;
}

[[nodiscard]] auto compile_service(std::string_view semver,
                                   std::string_view composition_line = {},
                                   std::string_view requires_line = {})
    -> refrain::test::compile_result {
    return compile({schema_source{
        "ver.ridl", refrain::schema::dialect::ridl,
        service_schema(semver, composition_line, requires_line)}});
}

const suite schema_version_tests = [] {
    "schema/version accepts every SemVer integer level"_test = [] {
        const std::array<std::string, 3> sources{std::string{"0.0.0"},
                                                 std::string{"1.2.3"},
                                                 std::string{"4294967295.0.1"}};
        for (const auto &semver : sources) {
            const auto result{compile_service(semver)};
            expect(result.program.has_value()) << semver;
            if (!result.program) {
                expect(result.diagnostics.empty())
                    << semver << ": " << result.diagnostics;
            }
        }

        const auto result{compile_service("2.7.3")};
        expect(result.program.has_value());
        if (result.program) {
            const auto &decl{
                result.program->modules.front().declarations.back()};
            expect(decl.service_version_major == 2U);
            expect(decl.service_version_minor == 7U);
            expect(decl.service_version_patch == 3U);
        }
    };

    "schema/version rejects malformed SemVer with a readable error"_test = [] {
        const std::array<std::string, 7> invalid{
            "1", "1.0", "1.0.0.0", "01.0.0", "1.00.0", "1.0.0-", "1.0.0+",
        };
        for (const auto &semver : invalid) {
            const auto result{compile_service(semver)};
            expect(!result.program.has_value()) << semver;
            expect(result.diagnostics.find("invalid service version") !=
                   std::string::npos)
                << semver << ": " << result.diagnostics;
        }
    };

    "schema/version rejects prerelease service versions but accepts build"_test =
        [] {
            const auto prerelease{compile_service("1.0.0-rc.1")};
            expect(!prerelease.program.has_value());
            expect(prerelease.diagnostics.find("prerelease") !=
                   std::string::npos);

            // Build metadata is valid SemVer; it is ignored for ABI purposes
            // (major/minor/patch are emitted).
            const auto build{compile_service("1.2.3+build.7")};
            expect(build.program.has_value());
            if (build.program) {
                const auto &decl{
                    build.program->modules.front().declarations.back()};
                expect(decl.service_version_major == 1U);
                expect(decl.service_version_minor == 2U);
                expect(decl.service_version_patch == 3U);
            }
        };

    "schema/version validates the RIDL module version as SemVer"_test = [] {
        const auto partial{
            compile({schema_source{"m.ridl", refrain::schema::dialect::ridl,
                                   R"(module refrain::services::m;
version 1.0;
service svc : 1.0.0 {
    side = server;
    thread = any;
    provides = [ "refrain:svc" ];
    fn ping() -> u32;
}
)"}})};
        expect(!partial.program.has_value());
        expect(partial.diagnostics.find("invalid version") !=
               std::string::npos);

        // The module version itself may carry prerelease/build metadata; only
        // service versions (emitted as ABI constants) must be releases.
        const auto prerelease{
            compile({schema_source{"m2.ridl", refrain::schema::dialect::ridl,
                                   R"(module refrain::services::m2;
version 2.0.0-rc.1+dev;
service svc : 2.0.0 {
    side = server;
    thread = any;
    provides = [ "refrain:svc" ];
    fn ping() -> u32;
}
)"}})};
        expect(prerelease.program.has_value());
        expect(prerelease.diagnostics.empty()) << prerelease.diagnostics;
    };

    "schema/requires accepts the four supported operators"_test = [] {
        const std::array<std::string, 4> ranges{
            "refrain:core>=1.0.0", "refrain:core^1.2.3",
            "refrain:data.packs~0.9.0", "refrain:core=2.0.0"};
        for (const auto &range : ranges) {
            std::string requires_line{"    requires = [ \""};
            requires_line += range;
            requires_line += "\" ];\n";
            const auto result{compile_service("1.0.0", {}, requires_line)};
            expect(result.program.has_value()) << range;
            if (!result.program) {
                expect(result.diagnostics.empty())
                    << range << ": " << result.diagnostics;
            }
        }
    };

    "schema/requires rejects malformed entries"_test = [] {
        const std::array<std::string, 7> invalid{
            "refrain:core1.0.0",    // no operator
            "refrain:core>1.0.0",   // unsupported operator
            "refrain:core<=1.0.0",  // unsupported operator
            "refrain:core>=1.0",    // partial SemVer
            "refrain:core>=",       // missing version
            ">=1.0.0",              // empty service id
            "refrain:core >=1.0.0", // whitespace in the service id
        };
        for (const auto &entry : invalid) {
            std::string requires_line{"    requires = [ \""};
            requires_line += entry;
            requires_line += "\" ];\n";
            const auto result{compile_service("1.0.0", {}, requires_line)};
            expect(!result.program.has_value()) << entry;
            expect(result.diagnostics.find("invalid 'requires' entry") !=
                   std::string::npos)
                << entry << ": " << result.diagnostics;
        }
    };

    "schema/composition defaults to exclusive and validates the enum"_test =
        [] {
            const auto defaulted{compile_service("1.0.0")};
            expect(defaulted.program.has_value());
            if (defaulted.program) {
                const auto &decl{
                    defaulted.program->modules.front().declarations.back()};
                expect(decl.service.composition == "exclusive");
            }

            const std::array<std::string, 4> modes{"exclusive", "instanced",
                                                   "chain", "registry"};
            for (const auto &mode : modes) {
                std::string composition_line{"    composition = "};
                composition_line += mode;
                composition_line += ";\n";
                const auto result{compile_service("1.0.0", composition_line)};
                expect(result.program.has_value()) << mode;
                if (result.program) {
                    const auto &decl{
                        result.program->modules.front().declarations.back()};
                    expect(decl.service.composition == mode);
                }
            }

            const auto invalid{
                compile_service("1.0.0", "    composition = shared;\n")};
            expect(!invalid.program.has_value());
            expect(invalid.diagnostics.find("invalid service composition") !=
                   std::string::npos);
            expect(invalid.diagnostics.find("shared") != std::string::npos);
        };
};

} // namespace
