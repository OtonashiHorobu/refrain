// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <schema/lexer.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;

using refrain::schema::diagnostics;
using refrain::schema::lexer;
using refrain::schema::token;
using refrain::schema::token_kind;

[[nodiscard]] auto tokenize(std::string_view source, diagnostics &sink)
    -> std::vector<token> {
    auto lex{lexer{source, "test.rsc", sink}};
    auto tokens{std::vector<token>{}};
    for (;;) {
        auto next{lex.next()};
        if (next.kind == token_kind::end_of_file) {
            break;
        }
        tokens.push_back(std::move(next));
    }
    return tokens;
}

[[nodiscard]] auto count_kind(const std::vector<token> &tokens, token_kind kind)
    -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count_if(
        tokens, [kind](const token &value) { return value.kind == kind; }));
}

const suite schema_lexer_tests = [] {
    "schema/lexer tokenizes modules, versions and attributes"_test = [] {
        diagnostics sink;
        const auto tokens{tokenize(R"(/// shared types
module refrain::core;
version 1.0;
#[since(1.21)] #[thread(sim)]
)",
                                   sink)};
        expect(sink.empty());
        expect(!tokens.empty());
        expect(tokens.front().kind == token_kind::doc_comment);
        expect(tokens.front().text == "shared types");
        expect(count_kind(tokens, token_kind::colon_colon) == 1U);
        expect(count_kind(tokens, token_kind::floating) == 2U);
        expect(count_kind(tokens, token_kind::attribute) == 2U);
    };

    // SemVer: a full triple and a `-prerelease`/`+build` suffix are lexed as a
    // single version token, so sema can report a readable "invalid version"
    // diagnostic instead of the lexer failing on '-' or breaking at the second
    // dot.
    "schema/lexer tokenizes full SemVer version tokens"_test = [] {
        diagnostics sink;
        const auto tokens{
            tokenize(R"(service demo : 1.2.3-rc.1+build.7 {})", sink)};
        expect(sink.empty());
        const auto version{std::ranges::find_if(tokens, [](const token &value) {
            return value.kind == token_kind::floating;
        })};
        expect(version != tokens.end());
        if (version != tokens.end()) {
            expect(version->text == "1.2.3-rc.1+build.7");
        }
    };

    "schema/lexer decodes hex, strings and escapes"_test = [] {
        diagnostics sink;
        const auto tokens{tokenize(
            R"(packet status = 0x1F { text: string = "a\\b"; })", sink)};
        expect(sink.empty());
        expect(tokens.size() >= 5U);
        expect(tokens[3].kind == token_kind::integer);
        expect(tokens[3].integer == 0x1F);
        const auto string_token{
            std::ranges::find_if(tokens, [](const token &value) {
                return value.kind == token_kind::string;
            })};
        expect(string_token != tokens.end());
        expect(string_token->text == "a\\b");
    };

    "schema/lexer rejects unknown characters"_test = [] {
        diagnostics sink;
        const auto tokens{tokenize("%", sink)};
        expect(tokens.empty());
        expect(!sink.empty());
        expect(sink.format().find("unexpected character") != std::string::npos);
    };

    "schema/lexer rejects unterminated strings"_test = [] {
        diagnostics sink;
        static_cast<void>(tokenize(R"("unterminated)", sink));
        expect(!sink.empty());
        expect(sink.format().find("unterminated string") != std::string::npos);
    };

    // `0X` must be parsed as hexadecimal, and `0x` without digits is an invalid
    // literal, not a bogus "out of range" integer.
    "schema/lexer handles hex prefixes precisely"_test = [] {
        diagnostics sink;
        const auto uppercase{tokenize("0X1F", sink)};
        expect(sink.empty());
        expect(uppercase.size() == 1U);
        if (!uppercase.empty()) {
            expect(uppercase[0].kind == token_kind::integer);
            expect(uppercase[0].integer == 0x1F);
        }

        diagnostics empty_hex_sink;
        static_cast<void>(tokenize("0x", empty_hex_sink));
        expect(!empty_hex_sink.empty());
        expect(empty_hex_sink.format().find("invalid hexadecimal literal") !=
               std::string::npos);
    };

    // A raw newline inside a string would inject generated `//` comments, so
    // the lexer must reject it.
    "schema/lexer rejects raw newlines inside strings"_test = [] {
        diagnostics sink;
        static_cast<void>(tokenize("\"line one\nline two\"", sink));
        expect(!sink.empty());
        expect(sink.format().find("raw newline") != std::string::npos);
    };
};

} // namespace
