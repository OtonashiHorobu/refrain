// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/core/semver.hpp>

#include <boost/ut.hpp>

#include <string>
#include <string_view>

namespace {

using namespace boost::ut;
using refrain::core::compare;
using refrain::core::parse_range;
using refrain::core::satisfies;
using refrain::core::semver;

[[nodiscard]] auto parse_ok(std::string_view text) {
    auto parsed{semver::parse(text)};
    expect(static_cast<bool>(parsed)) << "expected valid semver: " << text;
    return parsed ? *parsed : semver{};
}

/// Positive-case range match: asserts the range parsed and returns the match.
[[nodiscard]] auto sat(std::string_view version, std::string_view range) {
    auto matched{satisfies(version, range)};
    expect(static_cast<bool>(matched))
        << "expected valid version/range: " << version << " in " << range;
    return matched ? *matched : false;
}

const suite semver_tests = [] {
    "semver/parses release and metadata"_test = [] {
        const auto plain{parse_ok("1.2.3")};
        expect(plain.major == 1U);
        expect(plain.minor == 2U);
        expect(plain.patch == 3U);
        expect(!plain.is_prerelease());
        expect(plain.text() == "1.2.3");

        const auto rich{parse_ok("26.3.0-alpha.1+build.5")};
        expect(rich.prerelease == "alpha.1");
        expect(rich.build == "build.5");
        expect(rich.text() == "26.3.0-alpha.1+build.5");
    };

    "semver/rejects malformed versions"_test = [] {
        for (const std::string_view bad :
             {"", "1", "1.2", "1.2.3.4", "01.2.3", "1.02.3", "1.2.03", "1.2.3-",
              "1.2.3+", "1.2.3-01", "1.2.3-alpha..1", "1.2.3+bad!", "1.2.3+a+b",
              "v1.2.3"}) {
            expect(!semver::parse(bad)) << "expected reject: " << bad;
        }
    };

    "semver/precedence follows the spec"_test = [] {
        expect(compare(parse_ok("1.0.0"), parse_ok("1.0.0")) ==
               std::strong_ordering::equal);
        expect(compare(parse_ok("1.0.0"), parse_ok("2.0.0")) ==
               std::strong_ordering::less);
        expect(compare(parse_ok("1.10.0"), parse_ok("1.9.0")) ==
               std::strong_ordering::greater);
        expect(compare(parse_ok("1.0.0-alpha"), parse_ok("1.0.0")) ==
               std::strong_ordering::less);
        expect(compare(parse_ok("1.0.0-alpha"), parse_ok("1.0.0-alpha.1")) ==
               std::strong_ordering::less);
        expect(
            compare(parse_ok("1.0.0-alpha.1"), parse_ok("1.0.0-alpha.beta")) ==
            std::strong_ordering::less);
        expect(compare(parse_ok("1.0.0-beta"), parse_ok("1.0.0-beta.2")) ==
               std::strong_ordering::less);
        expect(compare(parse_ok("1.0.0-beta.2"), parse_ok("1.0.0-beta.11")) ==
               std::strong_ordering::less);
        expect(compare(parse_ok("1.0.0-rc.1"), parse_ok("1.0.0")) ==
               std::strong_ordering::less);
        // Build metadata is ignored.
        expect(compare(parse_ok("1.0.0+a"), parse_ok("1.0.0+b")) ==
               std::strong_ordering::equal);
    };

    "semver/ranges accept comparison and caret/tilde operators"_test = [] {
        expect(sat("1.5.0", ">=1.0.0"));
        expect(!sat("0.9.9", ">=1.0.0"));
        expect(sat("1.2.3", "=1.2.3"));
        expect(!sat("1.2.4", "=1.2.3"));
        expect(sat("1.9.9", "^1.2.3"));
        expect(!sat("2.0.0", "^1.2.3"));
        expect(sat("0.2.9", "^0.2.3"));
        expect(!sat("0.3.0", "^0.2.3"));
        expect(sat("0.0.3", "^0.0.3"));
        expect(!sat("0.0.4", "^0.0.3"));
        expect(sat("1.2.9", "~1.2.3"));
        expect(!sat("1.3.0", "~1.2.3"));
        expect(sat("1.5.0", ">=1.0.0 <2.0.0"));
        expect(!sat("2.0.0", ">=1.0.0 <2.0.0"));
        expect(sat("3.0.0", ">=3.0.0 || >=2.0.0 <2.5.0"));
    };

    "semver/prerelease is excluded unless the range targets it"_test = [] {
        expect(!sat("1.0.0-alpha.1", ">=0.9.0"));
        expect(sat("1.0.0-alpha.1", ">=1.0.0-alpha.1"));
        expect(sat("1.0.0-alpha.2", "^1.0.0-alpha.1"));
    };

    // SemVer §10: the no-leading-zero rule applies to prerelease identifiers
    // only; numeric build metadata identifiers may carry leading zeros.
    "semver/build metadata allows numeric leading zeros"_test = [] {
        const auto parsed{parse_ok("1.2.3+001")};
        expect(parsed.build == "001");
        expect(parsed.text() == "1.2.3+001");
        expect(parse_ok("1.2.3-alpha.1+001.002").build == "001.002");

        // The prerelease rule is unchanged.
        expect(!semver::parse("1.2.3-001"));
        expect(!semver::parse("1.2.3-01.2"));
    };

    // `^`/`~` on a component of UINT32_MAX would wrap the exclusive upper bound
    // to 0.x / 0.0.x, which makes the whole range match nothing - not even its
    // own bound. With no representable upper bound the range must degrade to
    // "only the lower bound".
    "semver/caret and tilde degrade when the upper bound would overflow"_test =
        [] {
            expect(sat("4294967295.0.0", "^4294967295.0.0"));
            expect(sat("4294967295.1.0", "^4294967295.0.0"));
            expect(!sat("4294967294.0.0", "^4294967295.0.0"));

            expect(sat("1.4294967295.0", "~1.4294967295.0"));
            expect(sat("1.4294967295.5", "~1.4294967295.0"));
            expect(!sat("1.4294967294.0", "~1.4294967295.0"));

            // 0.x carets bump minor/patch: same guard on both branches.
            expect(sat("0.4294967295.0", "^0.4294967295.0"));
            expect(sat("0.4294967295.9", "^0.4294967295.0"));
            expect(!sat("0.4294967294.0", "^0.4294967295.0"));
            expect(sat("0.0.4294967295", "^0.0.4294967295"));
            expect(!sat("0.0.4294967294", "^0.0.4294967295"));
        };

    "semver/rejects malformed ranges"_test = [] {
        for (const std::string_view bad : {"", ">=", "^", "~1.2", "1.2.3 ||",
                                           "|| 1.2.3", ">=1.2.3 <", "abc"}) {
            expect(!parse_range(bad)) << "expected reject: " << bad;
        }
    };

    "semver/satisfies reports parse errors"_test = [] {
        expect(!refrain::core::satisfies("1.2", ">=1.0.0").has_value());
        expect(!refrain::core::satisfies("1.2.3", "nonsense").has_value());
    };
};

} // namespace
