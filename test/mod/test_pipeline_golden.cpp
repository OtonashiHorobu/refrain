// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Golden baseline for the mod composition contract.
//
// These tests freeze the byte/text contracts:
//   * `mod_lock::canonical_text()` / `fingerprint()` — the composition identity
//     consumed by `mods.lock` and world metadata/handshake;
//   * `format_mod_lock()` — the stable audit dump (`refrain-tools mods list`,
//     server startup log);
//   * `startup_plan::format()` — the stable `gamepack check|dump` /
//     `--dump-gamepack` plan output that CI greps.
//
// The expected values live in test/fixtures/golden/ as plain files, so any
// drift shows up as a reviewable diff instead of a silent behavior change.
// Regenerate deliberately after an intended contract change:
//
//   REFRAIN_UPDATE_GOLDEN=1 build/Release/bin/refrain-tests
//
// and review the resulting diff.

#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>

#include <boost/ut.hpp>

#include <fmt/format.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace boost::ut;

namespace fs = std::filesystem;
namespace mod = refrain::mod;

constexpr std::string_view source_root{REFRAIN_SOURCE_DIR};
constexpr std::string_view golden_root{REFRAIN_SOURCE_DIR
                                       "/test/fixtures/golden"};

[[nodiscard]] auto read_text(const fs::path &path) -> std::string {
    std::ifstream in{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{in},
                       std::istreambuf_iterator<char>{}};
}

[[nodiscard]] auto update_mode() -> bool {
    return std::getenv("REFRAIN_UPDATE_GOLDEN") != nullptr;
}

/// Replace the absolute source root with `<root>` so the golden text does not
/// depend on the checkout location (`format_mod_lock()` prints host-local
/// manifest paths; the identity/hash lines are path-free).
[[nodiscard]] auto normalize_paths(std::string text) -> std::string {
    const std::string root{source_root};
    for (auto position = text.find(root); position != std::string::npos;
         position = text.find(root, position)) {
        text.replace(position, root.size(), "<root>");
        position += std::string_view{"<root>"}.size();
    }
    return text;
}

/// Compare `actual` with `test/fixtures/golden/<file>`; in update mode write
/// the file instead. Returns false on drift and fills `message`.
[[nodiscard]] auto freeze(std::string_view file, const std::string &actual,
                          std::string &message) -> bool {
    const fs::path path{fs::path{golden_root} / file};
    if (update_mode()) {
        std::ofstream out{path, std::ios::binary | std::ios::trunc};
        out << actual;
        return static_cast<bool>(out);
    }
    const auto expected{read_text(path)};
    if (expected == actual) {
        return true;
    }
    message = fmt::format(
        "golden '{}' drifted; re-run with REFRAIN_UPDATE_GOLDEN=1 and review "
        "the diff",
        path.string());
    return false;
}

[[nodiscard]] auto load_plan() -> refrain::result<mod::composition> {
    auto spec{mod::load_gamepack_spec(
        fmt::format("{}/gamepack/gamepack.toml", source_root), "server")};
    if (!spec) {
        return std::unexpected{spec.error()};
    }
    return mod::preview_composition(*spec);
}

suite<"mod/pipeline-golden"> pipeline_golden_tests = [] {
    "golden/startup plan format is frozen"_test = [] {
        const auto plan = load_plan();
        expect(static_cast<bool>(plan));
        if (!plan) {
            return;
        }
        std::string message;
        expect(freeze("startup-plan.txt", normalize_paths(plan->format()),
                      message))
            << message;
    };

    "golden/mods.lock canonical text, fingerprint and audit dump are frozen"_test =
        [] {
            const auto plan = load_plan();
            expect(static_cast<bool>(plan));
            if (!plan) {
                return;
            }

            // The same roots the server scans in CI: the repository root
            // (its `mods/` tree) as origin=builtin plus the instance fixture as
            // origin=user.
            const std::vector<mod::discovery_root> roots{
                mod::discovery_root{
                    .path = std::string{source_root},
                    .origin = mod::mod_origin::builtin,
                },
                mod::discovery_root{
                    .path =
                        fmt::format("{}/test/fixtures/instance", source_root),
                    .origin = mod::mod_origin::user,
                },
            };
            const auto discovered = mod::scan(roots);
            expect(static_cast<bool>(discovered));
            if (!discovered) {
                return;
            }

            // `refrain-server --auto-scan` adds the instance mods on top of the
            // gamepack pins; the golden lock is that composition.
            auto spec = plan->spec;
            spec.auto_scan = true;
            const auto lock = mod::resolve(spec, *discovered);
            expect(static_cast<bool>(lock));
            if (!lock) {
                return;
            }

            std::string message;
            expect(freeze("mods-lock.canonical.txt", lock->canonical_text(),
                          message))
                << message;
            expect(freeze("mods-lock.fingerprint.txt",
                          fmt::format("{}\n", lock->fingerprint()), message))
                << message;
            expect(freeze("mods-lock.txt",
                          normalize_paths(mod::format_mod_lock(*lock)),
                          message))
                << message;
        };
};

} // namespace
