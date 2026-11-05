// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Recursive-descent parser for RSC (data/packet/record schemas) and RIDL
// (service contracts). Errors are collected in `diagnostics` and parsing
// resumes at declaration boundaries where possible so one run reports multiple
// schema mistakes.

#pragma once

#include <schema/ast.hpp>
#include <schema/diagnostic.hpp>
#include <schema/token.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace refrain::schema {

struct parse_request {
    dialect kind{dialect::rsc};
    std::string path;
};

/// Parse one schema file. Returns nullopt when at least one syntax error was
/// found (all errors are appended to `sink`).
[[nodiscard]] auto parse_schema(parse_request request, std::string_view source,
                                diagnostics &sink)
    -> std::optional<source_unit>;

} // namespace refrain::schema
