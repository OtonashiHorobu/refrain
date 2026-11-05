// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// Drift guard: the committed sdk/generated headers must match what the schema
// toolchain produces from the committed schemas. This is the in-test twin of
// `refrain-tools schema gen --check` (CI runs both).

#include <schema/schema.hpp>

#include <boost/ut.hpp>

#include <filesystem>
#include <string>

namespace {

using namespace boost::ut;

const suite schema_drift_tests = [] {
    "schema/committed generated headers match the schemas"_test = [] {
        const std::filesystem::path root{REFRAIN_SOURCE_DIR};
        auto generated{refrain::schema::generate_from_roots(
            {root / "schema", root / "sdk/idl"})};
        expect(generated.has_value());
        if (!generated) {
            return;
        }
        auto report{
            refrain::schema::check_files(*generated, root / "sdk/generated")};
        expect(report.has_value());
        if (!report) {
            return;
        }
        if (!report->ok()) {
            for (const auto &path : report->missing) {
                expect(false) << "missing generated file: " << path;
            }
            for (const auto &path : report->changed) {
                expect(false) << "outdated generated file: " << path;
            }
            for (const auto &path : report->stale) {
                expect(false) << "stale generated file: " << path;
            }
        }
        expect(report->ok());
    };
};

} // namespace
