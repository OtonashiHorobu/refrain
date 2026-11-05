// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <schema/parser.hpp>

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace {

using namespace boost::ut;

using refrain::schema::diagnostics;
using refrain::schema::dialect;

const suite schema_parser_tests = [] {
    "schema/parser reads RSC name:type fields, packets and records"_test = [] {
        diagnostics sink;
        auto unit{refrain::schema::parse_schema({dialect::rsc, "chunk.rsc"},
                                                R"(module refrain::storage;
version 2;
enum chunk_status : u8 { empty = 0, full = 1 }
struct block_pos { x: i32; y: i16; }
packet ping = 0x1F { value: u32; note: string = "hi"; }
record chunk { status: chunk_status; pos: block_pos; }
)",
                                                sink)};
        expect(sink.empty());
        expect(unit.has_value());
        if (!unit) {
            return;
        }
        expect(unit->module == "refrain::storage");
        expect(unit->version == "2");
        expect(unit->declarations.size() == 4U);
        if (unit->declarations.size() != 4U) {
            return;
        }
        expect(unit->declarations[0].kind ==
               refrain::schema::declaration_kind::enumeration);
        expect(unit->declarations[0].entries.size() == 2U);
        expect(unit->declarations[1].fields.size() == 2U);
        expect(unit->declarations[1].fields[0].name == "x");
        expect(unit->declarations[2].packet_id == 0x1F);
        expect(unit->declarations[2].fields[1].default_value ==
               std::optional<std::string>{"hi"});
        expect(unit->declarations[3].kind ==
               refrain::schema::declaration_kind::record);
    };

    "schema/parser reads RIDL services, imports and out parameters"_test = [] {
        diagnostics sink;
        auto unit{refrain::schema::parse_schema(
            {dialect::ridl, "demo.ridl"}, R"(module refrain::services::demo;
version 1.0.0;
import refrain::core;

handle<gen_ctx>;

enum demo_error : u32 { failed = 1 }

struct config { u64 seed; string name; }
struct outcome { u32 value; }

service demo : 1.0.0 {
    side = server;
    thread = worker;
    composition = instanced;
    provides = [ "refrain:demo" ];
    capabilities = [ "a", "b" ];

    #[thread(worker)] #[can_fail]
    fn run(gen_ctx ctx, in config cfg, out outcome result)
        -> result<outcome, demo_error>;
}
)",
            sink)};
        expect(sink.empty());
        expect(unit.has_value());
        if (!unit) {
            return;
        }
        expect(unit->imports.size() == 1U);
        expect(unit->imports.front() == "refrain::core");
        expect(unit->declarations.size() == 5U);
        const auto &service{unit->declarations.back()};
        expect(service.kind == refrain::schema::declaration_kind::service);
        expect(service.name == "demo");
        expect(service.service.version == "1.0.0");
        expect(service.service.side == "server");
        expect(service.service.thread == "worker");
        expect(service.service.composition == "instanced");
        expect(service.service.provides.size() == 1U);
        expect(service.service.capabilities.size() == 2U);
        expect(service.methods.size() == 1U);
        if (service.methods.size() != 1U) {
            return;
        }
        const auto &method{service.methods.front()};
        expect(method.name == "run");
        expect(method.has_return);
        expect(method.return_type.name == "result");
        expect(method.return_type.arguments.size() == 2U);
        expect(method.parameters.size() == 3U);
        expect(method.parameters[1].out == false);
        expect(method.parameters[2].out == true);
        expect(method.parameters[2].type.name == "outcome");
    };

    "schema/parser reports syntax errors with locations"_test = [] {
        diagnostics sink;
        auto unit{refrain::schema::parse_schema({dialect::rsc, "broken.rsc"},
                                                R"(module refrain::broken;
version 1;
struct a { x: u32 }     // missing semicolon
)",
                                                sink)};
        expect(!unit.has_value());
        expect(!sink.empty());
        const auto text{sink.format()};
        expect(text.find("broken.rsc:3:") != std::string::npos);
        expect(text.find("';'") != std::string::npos);
    };

    "schema/parser rejects unknown declaration keywords"_test = [] {
        diagnostics sink;
        auto unit{refrain::schema::parse_schema(
            {dialect::rsc, "bad.rsc"},
            "module refrain::bad;\nversion 1;\nclass nope {}\n", sink)};
        expect(!unit.has_value());
        expect(sink.format().find("expected a declaration") !=
               std::string::npos);
    };

    // `event` is a top-level RIDL declaration whose body is the struct field
    // grammar (no packet id), and named attribute arguments are representable
    // for `#[schedule(interval=..., phase=...)]`.
    "schema/parser reads RIDL events and named attribute arguments"_test = [] {
        diagnostics sink;
        auto unit{refrain::schema::parse_schema(
            {dialect::ridl, "events.ridl"}, R"(module refrain::services::events;
version 1.0.0;

/// Published once per tick.
#[cancellable]
event tick_event {
    i64 tick;
}

#[batch]
event block_changed { slice<u32> states; }

service events : 1.0.0 {
    side = server;
    thread = sim;
    provides = [ "refrain:events" ];

    #[thread(sim)] #[can_fail] #[schedule(interval=10, phase=pre_tick)]
    fn maintenance_tick() -> result<void, events_error>;
}
)",
            sink)};
        expect(sink.empty());
        expect(unit.has_value());
        if (!unit) {
            return;
        }
        expect(unit->declarations.size() == 3U);
        if (unit->declarations.size() != 3U) {
            return;
        }

        const auto &tick{unit->declarations[0]};
        expect(tick.kind == refrain::schema::declaration_kind::event);
        expect(tick.name == "tick_event");
        expect(tick.documentation == "Published once per tick.");
        expect(tick.attributes.size() == 1U);
        expect(tick.attributes.front().name == "cancellable");
        expect(tick.attributes.front().arguments.empty());
        expect(tick.packet_id.has_value() == false);
        expect(tick.fields.size() == 1U);
        if (tick.fields.size() == 1U) {
            expect(tick.fields.front().type.name == "i64");
            expect(tick.fields.front().name == "tick");
        }

        const auto &batch{unit->declarations[1]};
        expect(batch.kind == refrain::schema::declaration_kind::event);
        expect(batch.name == "block_changed");
        expect(batch.attributes.size() == 1U);
        expect(batch.attributes.front().name == "batch");
        expect(batch.fields.size() == 1U);
        if (batch.fields.size() == 1U) {
            const auto &type{batch.fields.front().type};
            expect(type.name == "slice");
            expect(type.arguments.size() == 1U);
            if (type.arguments.size() == 1U) {
                expect(type.arguments.front().name == "u32");
            }
            expect(batch.fields.front().name == "states");
        }

        const auto &service{unit->declarations[2]};
        expect(service.kind == refrain::schema::declaration_kind::service);
        expect(service.methods.size() == 1U);
        if (service.methods.size() != 1U) {
            return;
        }
        const auto &method{service.methods.front()};
        expect(method.attributes.size() == 3U);
        if (method.attributes.size() != 3U) {
            return;
        }
        // Positional attribute arguments keep working and stay unnamed.
        expect(method.attributes[0].name == "thread");
        expect(method.attributes[0].arguments.size() == 1U);
        expect(method.attributes[0].arguments.front().name.has_value() ==
               false);
        expect(method.attributes[0].arguments.front().text == "sim");

        const auto &schedule{method.attributes[2]};
        expect(schedule.name == "schedule");
        expect(schedule.arguments.size() == 2U);
        if (schedule.arguments.size() != 2U) {
            return;
        }
        expect(schedule.arguments[0].name ==
               std::optional<std::string>{"interval"});
        expect(schedule.arguments[0].kind ==
               refrain::schema::attribute_argument_kind::integer);
        expect(schedule.arguments[0].text == "10");
        expect(schedule.arguments[1].name ==
               std::optional<std::string>{"phase"});
        expect(schedule.arguments[1].kind ==
               refrain::schema::attribute_argument_kind::identifier);
        expect(schedule.arguments[1].text == "pre_tick");
    };

    // RSC attributes stay positional: the data schema grammar must not silently
    // accept `name=value` arguments.
    "schema/parser keeps RSC attribute arguments positional"_test = [] {
        diagnostics sink;
        auto unit{refrain::schema::parse_schema({dialect::rsc, "rsckw.rsc"},
                                                R"(module refrain::rsckw;
version 1;
struct block { #[max(10)] x: u32; }
)",
                                                sink)};
        expect(sink.empty());
        expect(unit.has_value());
        if (!unit || unit->declarations.empty() ||
            unit->declarations.front().fields.empty()) {
            return;
        }
        const auto &argument{unit->declarations.front()
                                 .fields.front()
                                 .attributes.front()
                                 .arguments.front()};
        expect(argument.name.has_value() == false);
        expect(argument.text == "10");

        diagnostics named_sink;
        const auto named{refrain::schema::parse_schema(
            {dialect::rsc, "rscnamed.rsc"},
            "module refrain::rscnamed;\nversion 1;\n"
            "#[max(limit=10)] struct block { x: u32; }\n",
            named_sink)};
        expect(!named.has_value());
        expect(!named_sink.empty());
    };

    // Event declarations need a body, and a named attribute argument needs a
    // value.
    "schema/parser rejects malformed event and attribute syntax"_test = [] {
        diagnostics event_sink;
        const auto bodiless{refrain::schema::parse_schema(
            {dialect::ridl, "bad_event.ridl"},
            "module refrain::services::bad;\nversion 1.0.0;\n"
            "event tick_event;\n",
            event_sink)};
        expect(!bodiless.has_value());
        expect(event_sink.format().find("'{' after type name") !=
               std::string::npos);

        diagnostics value_sink;
        const auto missing_value{refrain::schema::parse_schema(
            {dialect::ridl, "bad_attr.ridl"},
            "module refrain::services::bad;\nversion 1.0.0;\n"
            "#[schedule(interval=)] struct target { u32 value; }\n",
            value_sink)};
        expect(!missing_value.has_value());
        expect(value_sink.format().find(
                   "expected an attribute argument value after '='") !=
               std::string::npos);
    };

    // Unbounded type-expression recursion must report a diagnostic instead of
    // overflowing the stack (SIGSEGV) on deeply nested generics.
    "schema/parser bounds type nesting depth"_test = [] {
        std::string source{"module refrain::deep;\nversion 1;\nstruct x { f: "};
        constexpr int depth{200};
        for (int index{0}; index < depth; ++index) {
            source += "array<";
        }
        source += "u32";
        for (int index{0}; index < depth; ++index) {
            source += ">";
        }
        source += "; }\n";

        diagnostics sink;
        const auto unit{refrain::schema::parse_schema(
            {dialect::rsc, "deep.rsc"}, source, sink)};
        expect(!unit.has_value());
        expect(!sink.empty());
        expect(sink.format().find("type nesting exceeds the maximum depth") !=
               std::string::npos);
    };

    // Diagnostic storage is capped: a pathological schema must not make the
    // tool retain an unbounded number of entries, but the dropped errors still
    // have to be counted in the formatted summary.
    "schema/diagnostics caps stored entries and counts the rest"_test = [] {
        diagnostics sink;
        constexpr std::size_t total{300};
        for (std::size_t index = 0; index < total; ++index) {
            sink.error({"many.rsc", static_cast<std::uint32_t>(index + 1), 1},
                       "duplicate declaration 'x'");
        }
        expect(sink.size() == total);
        expect(sink.dropped() == total - diagnostics::max_entries);
        expect(sink.entries().size() == diagnostics::max_entries);
        const auto text{sink.format()};
        // Only the first 24 errors are rendered; the remaining 276 are
        // summarised, including the 44 that were never stored.
        expect(text.find("... and 276 more error(s)") != std::string::npos);
    };
};

} // namespace
