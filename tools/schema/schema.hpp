// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Public API of the schema toolchain: used by `refrain-tools schema gen` and by
// the unit tests. Generation is a pure function of the schema sources; writing
// and drift checking are separate steps so `--check` can never mutate files.

#pragma once

#include <schema/ast.hpp>
#include <schema/emitter_common.hpp>

#include <refrain/core/error.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace refrain::schema {

struct source_file {
    std::string path; ///< display path (for diagnostics and RSC placement)
    dialect kind{dialect::rsc};
    std::string text;
};

using generated_file = emit::generated_file;

struct generation {
    std::vector<generated_file> files;
    std::string schema_hash;
    std::size_t source_count{0};
};

/// Parse + analyse + emit, all in memory.
[[nodiscard]] auto generate_from_sources(std::vector<source_file> sources)
    -> result<generation>;

/// Discover `*.rsc`/`*.ridl` under the roots (deterministically sorted) and
/// generate. Diagnostic paths are root-relative.
[[nodiscard]] auto generate_from_roots(std::vector<std::filesystem::path> roots)
    -> result<generation>;

struct check_report {
    std::vector<std::string> missing;
    std::vector<std::string> changed;
    std::vector<std::string> stale;

    [[nodiscard]] auto ok() const noexcept -> bool {
        return missing.empty() && changed.empty() && stale.empty();
    }
};

[[nodiscard]] auto write_files(const generation &generated,
                               const std::filesystem::path &output_root)
    -> status;

[[nodiscard]] auto check_files(const generation &generated,
                               const std::filesystem::path &output_root)
    -> result<check_report>;

} // namespace refrain::schema
