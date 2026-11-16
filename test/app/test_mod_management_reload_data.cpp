// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// `mods reload-data`: the shared command surface exposes the content view
// epoch behind the hot gate, with the same result and audit record on both
// frontends.

#include <refrain/app/mod_management.hpp>

#include <boost/ut.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace app = refrain::app;

struct fixture {
    app::mod_management_context context{};
    std::vector<app::mod_audit_record> audit{};

    fixture() {
        context.admin_source = "config/mods_admin.toml";
        context.audit = [this](const app::mod_audit_record &record) {
            audit.push_back(record);
        };
    }

    [[nodiscard]] auto run(std::vector<std::string_view> tokens)
        -> app::mod_management_report {
        app::mod_management service{context};
        return service.execute(tokens, app::mod_management_actor{});
    }

    void provide_port() {
        context.content_reload.request = [this]() {
            ++requests;
            return app::content_reload_outcome{
                .lines = {"queued a content view "
                          "rebuild; generation 1 "
                          "stays live until the next "
                          "tick boundary"}};
        };
        requests = 0;
    }

    int requests{0};
};

} // namespace

const suite mod_management_reload_data_tests = [] {
    "mod management/reload-data is refused while the hot gate is off"_test =
        [] {
            fixture f{};
            const auto report{f.run({"reload-data"})};
            expect(!report.ok);
            expect(report.outcome == "refused");
            expect(report.error.find("hot_reload_data") != std::string::npos);
            expect(report.error.find("config/mods_admin.toml") !=
                   std::string::npos);
            expect(f.requests == 0);
            expect(f.audit.size() == 1U);
            if (!f.audit.empty()) {
                expect(f.audit.front().action == "reload-data");
                expect(f.audit.front().result == "refused");
            }
        };

    "mod management/reload-data reports an unavailable content view"_test = [] {
        fixture f{};
        f.context.hot_reload_data = true;
        const auto report{f.run({"reload-data"})};
        expect(!report.ok);
        expect(report.outcome == "unsupported");
        expect(report.error.find("content view") != std::string::npos);
    };

    "mod management/reload-data queues the rebuild through the port"_test = [] {
        fixture f{};
        f.context.hot_reload_data = true;
        f.provide_port();
        const auto report{f.run({"reload-data"})};
        expect(report.ok) << report.error;
        expect(report.outcome == "ok");
        expect(f.requests == 1);
        expect(!report.lines.empty());
        if (!report.lines.empty()) {
            expect(report.lines.front().find("queued a content view "
                                             "rebuild") != std::string::npos);
        }
        expect(f.audit.size() == 1U);
        if (!f.audit.empty()) {
            expect(f.audit.front().action == "reload-data");
            expect(f.audit.front().result == "ok");
        }
    };

    "mod management/reload-data --dry-run does not start a rebuild"_test = [] {
        fixture f{};
        f.context.hot_reload_data = true;
        f.provide_port();
        const auto report{f.run({"reload-data", "--dry-run"})};
        expect(report.ok);
        expect(report.outcome == "dry-run");
        expect(f.requests == 0);
    };

    "mod management/reload-data rejects positional arguments"_test = [] {
        fixture f{};
        f.context.hot_reload_data = true;
        f.provide_port();
        const auto report{f.run({"reload-data", "now"})};
        expect(!report.ok);
        expect(report.error.find("takes no arguments") != std::string::npos);
        expect(f.requests == 0);
    };

    "mod management/the usage line names reload-data"_test = [] {
        fixture f{};
        const auto report{f.run({})};
        expect(!report.lines.empty());
        if (!report.lines.empty()) {
            expect(report.lines.front().find("reload-data") !=
                   std::string::npos);
        }
    };
};
