// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// The vendored zlib package is zlib-ng in `ZLIB_COMPAT` mode. Its
// crc32_combine()/crc32_combine_gen() passed a negative length straight into
// the signed right-shift loop of x2nmodp(), which never terminates, so the
// recipe backports the upstream negative-length guard (CVE-2026-27171).
//
// These tests pin both halves of that behavior: a negative length returns zero
// instead of running forever, and a non-negative length still combines the
// checksums of the concatenated bytes. Without the guard the negative-length
// cases do not fail, they hang, so the cost of a regression is the test
// suite's timeout rather than a failed expectation.

#include <boost/ut.hpp>

#include <zlib.h>

#include <cstddef>
#include <string_view>

namespace {

using namespace boost::ut;

constexpr std::string_view head_text{"refrain"};
constexpr std::string_view tail_text{"-zlib-ng"};

[[nodiscard]] auto crc32_of(std::string_view text) -> unsigned long {
    return crc32(0UL, reinterpret_cast<const unsigned char *>(text.data()),
                 static_cast<unsigned int>(text.size()));
}

const suite zlib_combine_tests = [] {
    "zlib/crc32_combine rejects a negative length"_test = [] {
        expect(crc32_combine(0UL, 0UL, -1) == 0UL);
        expect(crc32_combine(crc32_of(head_text), crc32_of(tail_text), -1) ==
               0UL);
    };

    "zlib/crc32_combine_gen rejects a negative length"_test = [] {
        expect(crc32_combine_gen(-1) == 0UL);
    };

    "zlib/crc32_combine joins the checksums of the concatenated bytes"_test =
        [] {
            const auto head_crc{crc32_of(head_text)};
            const auto tail_crc{crc32_of(tail_text)};
            const auto tail_length{static_cast<z_off_t>(tail_text.size())};
            const auto joined_crc{
                crc32(head_crc,
                      reinterpret_cast<const unsigned char *>(tail_text.data()),
                      static_cast<unsigned int>(tail_text.size()))};

            expect(crc32_combine(head_crc, tail_crc, tail_length) ==
                   joined_crc);
            expect(crc32_combine_op(head_crc, tail_crc,
                                    crc32_combine_gen(tail_length)) ==
                   joined_crc);
        };
};

} // namespace
