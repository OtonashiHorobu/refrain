// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <schema/sema.hpp>

#include <schema/schema_test_util.hpp>

#include <boost/ut.hpp>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;
using refrain::test::compile;
using refrain::test::schema_source;

/// RIDL module containing the given declarations (events/structs/enums).
[[nodiscard]] auto ridl_module(std::string_view declarations) -> std::string {
    return std::string{"module refrain::services::evt;\nversion 1.0.0;\n"} +
           std::string{declarations};
}

/// RIDL service module with a valid header around one method declaration.
[[nodiscard]] auto ridl_service(std::string_view methods) -> std::string {
    return std::string{R"(module refrain::services::sched;
version 1.0.0;
enum sched_error : u32 { failed = 1 }
service sched : 1.0.0 {
    side = server;
    thread = sim;
    provides = [ "refrain:sched" ];
)"} + std::string{methods} +
           "}\n";
}

[[nodiscard]] auto compile_ridl(std::string text,
                                std::string_view path = "evt.ridl")
    -> refrain::test::compile_result {
    return compile({schema_source{
        std::string{path}, refrain::schema::dialect::ridl, std::move(text)}});
}

/// The diagnostics must contain `marker` (and analysis must have failed).
void expect_rejected(const refrain::test::compile_result &result,
                     std::string_view marker) {
    expect(!result.program.has_value()) << "expected rejection: " << marker;
    expect(result.diagnostics.find(marker) != std::string::npos)
        << "missing diagnostic '" << marker << "' in: " << result.diagnostics;
}

const suite schema_sema_tests = [] {
    "schema/sema rejects unknown field types"_test = [] {
        const auto result{
            compile({schema_source{"bad.rsc", refrain::schema::dialect::rsc,
                                   R"(module refrain::bad;
version 1;
struct a { x: missing; }
)"}})};
        expect(!result.program.has_value());
        expect(result.diagnostics.find("unknown type 'missing'") !=
               std::string::npos);
    };

    "schema/sema rejects duplicate declarations and packet ids"_test = [] {
        const auto duplicate{
            compile({schema_source{"dup.rsc", refrain::schema::dialect::rsc,
                                   R"(module refrain::dup;
version 1;
struct a { x: u32; }
struct a { y: u32; }
)"}})};
        expect(!duplicate.program.has_value());
        expect(duplicate.diagnostics.find("duplicate declaration 'a'") !=
               std::string::npos);

        const auto packets{
            compile({schema_source{"ids.rsc", refrain::schema::dialect::rsc,
                                   R"(module refrain::ids;
version 1;
packet first = 0x10 { x: u32; }
packet second = 0x10 { y: u32; }
)"}})};
        expect(!packets.program.has_value());
        expect(packets.diagnostics.find("reuses wire id 0x10") !=
               std::string::npos);
    };

    "schema/sema resolves imported types and detects ambiguity"_test = [] {
        const auto resolved{
            compile({schema_source{"core.ridl", refrain::schema::dialect::ridl,
                                   R"(module refrain::core;
version 1.0.0;
struct chunk_pos { i32 x; i32 z; }
)"},
                     schema_source{"world.ridl", refrain::schema::dialect::ridl,
                                   R"(module refrain::services::world;
version 1.0.0;
import refrain::core;
struct view { chunk_pos pos; }
)"}})};
        expect(resolved.program.has_value());
        expect(resolved.diagnostics.empty());

        const auto ambiguous{
            compile({schema_source{"a.ridl", refrain::schema::dialect::ridl,
                                   R"(module refrain::a;
version 1.0.0;
struct thing { u32 x; }
)"},
                     schema_source{"b.ridl", refrain::schema::dialect::ridl,
                                   R"(module refrain::b;
version 1.0.0;
struct thing { u32 y; }
)"},
                     schema_source{"c.ridl", refrain::schema::dialect::ridl,
                                   R"(module refrain::c;
version 1.0.0;
import refrain::a;
import refrain::b;
struct holder { thing value; }
)"}})};
        expect(!ambiguous.program.has_value());
        expect(ambiguous.diagnostics.find("ambiguous type 'thing'") !=
               std::string::npos);
    };

    "schema/sema validates handles and service metadata"_test = [] {
        const auto handle{
            compile({schema_source{"bad.ridl", refrain::schema::dialect::ridl,
                                   R"(module refrain::bad;
version 1.0.0;
struct holder { handle<missing_handle> value; }
)"}})};
        expect(!handle.program.has_value());
        expect(handle.diagnostics.find("unknown handle tag 'missing_handle'") !=
               std::string::npos);

        const auto service{
            compile({schema_source{"svc.ridl", refrain::schema::dialect::ridl,
                                   R"(module refrain::services::svc;
version 1.0.0;
service svc : 1.0.0 {
    side = somewhere;
    thread = worker;
    fn ping() -> u32;
}
)"}})};
        expect(!service.program.has_value());
        expect(service.diagnostics.find("invalid service side 'somewhere'") !=
               std::string::npos);
        expect(
            service.diagnostics.find("must declare at least one 'provides'") !=
            std::string::npos);
    };

    "schema/sema requires error enums without a zero value"_test = [] {
        const auto result{
            compile({schema_source{"err.ridl", refrain::schema::dialect::ridl,
                                   R"(module refrain::err;
version 1.0.0;
handle<ctx>;
enum err : u32 { ok = 0, failed = 1 }
struct cfg { u32 x; }
service svc : 1.0.0 {
    side = server;
    thread = any;
    provides = [ "refrain:svc" ];
    fn run(in cfg config) -> result<u32, err>;
}
)"}})};
        expect(!result.program.has_value());
        expect(result.diagnostics.find("no zero value") != std::string::npos);
    };

    "schema/sema rejects recursive data types"_test = [] {
        const auto result{
            compile({schema_source{"cycle.rsc", refrain::schema::dialect::rsc,
                                   R"(module refrain::cycle;
version 1;
struct a { b: b; }
struct b { a: a; }
)"}})};
        expect(!result.program.has_value());
        expect(result.diagnostics.find("recursive data type") !=
               std::string::npos);
    };

    // Duplicate field names must be rejected instead of generating C structs
    // with duplicate members and bogus ABI layout assertions.
    "schema/sema rejects duplicate struct fields"_test = [] {
        const auto rsc{compile(
            {schema_source{"dupfield.rsc", refrain::schema::dialect::rsc,
                           R"(module refrain::dupfield;
version 1;
struct dup { a: u32; a: u16; }
)"}})};
        expect(!rsc.program.has_value());
        expect(rsc.diagnostics.find("duplicate field 'a'") !=
               std::string::npos);

        const auto ridl{compile(
            {schema_source{"dupfield.ridl", refrain::schema::dialect::ridl,
                           R"(module refrain::services::dupfield;
version 1.0.0;
struct dup { u32 a; u32 a; }
)"}})};
        expect(!ridl.program.has_value());
        expect(ridl.diagnostics.find("duplicate field 'a'") !=
               std::string::npos);
    };

    // Container types have no C ABI representation; they must not be silently
    // emitted as `void` (uncompilable generated C++).
    "schema/sema rejects non-representable method return types"_test = [] {
        constexpr std::string_view error_enum{
            "enum err : u32 { failed = 1 }\n"};
        const auto plain{compile(
            {schema_source{"badret.ridl", refrain::schema::dialect::ridl,
                           std::string{R"(module refrain::services::badret;
version 1.0.0;
struct point { i32 x; }
)"} + std::string{error_enum} +
                               R"(service svc : 1.0.0 {
    side = server;
    thread = any;
    provides = [ "refrain:svc" ];
    fn bad() -> array<point>;
}
)"}})};
        expect(!plain.program.has_value());
        expect(plain.diagnostics.find("not representable in the C ABI") !=
               std::string::npos);

        const auto result{compile(
            {schema_source{"badret2.ridl", refrain::schema::dialect::ridl,
                           std::string{R"(module refrain::services::badret2;
version 1.0.0;
struct point { i32 x; }
)"} + std::string{error_enum} +
                               R"(service svc : 1.0.0 {
    side = server;
    thread = any;
    provides = [ "refrain:svc" ];
    fn bad() -> result<optional<point>, err>;
}
)"}})};
        expect(!result.program.has_value());
        expect(result.diagnostics.find("not representable in the C ABI") !=
               std::string::npos);
    };

    // `next_value = INT64_MAX + 1` would be signed-overflow UB, so
    // auto-numbering must reject it.
    "schema/sema rejects enum auto-numbering overflow"_test = [] {
        const auto result{
            compile({schema_source{"enumovf.rsc", refrain::schema::dialect::rsc,
                                   R"(module refrain::enumovf;
version 1;
enum big : i64 { maximum = 9223372036854775807, following }
)"}})};
        expect(!result.program.has_value());
        expect(result.diagnostics.find("previous value is the maximum int64") !=
               std::string::npos);
    };

    // Auto-numbered flags past index 62 would evaluate `1 << 63/64` (UB) and
    // then report a second, misleading value-range error. The cap must be a
    // single diagnostic and the offending entries must be skipped entirely.
    "schema/sema bounds auto-numbered flags without shift UB"_test = [] {
        std::string source{R"(module refrain::flagbound;
version 1.0.0;
flags limit : u64 {
)"};
        for (std::size_t index = 0; index < 64; ++index) {
            source += "    bit" + std::to_string(index);
            source += index + 1 == 64 ? "\n" : ",\n";
        }
        source += "}\n";
        const auto result{compile(
            {schema_source{"flagbound.ridl", refrain::schema::dialect::ridl,
                           std::move(source)}})};
        expect(!result.program.has_value());
        const std::string marker{
            "auto-numbered flags support at most 63 entries"};
        const auto first{result.diagnostics.find(marker)};
        expect(first != std::string::npos);
        expect(result.diagnostics.find(marker, first + marker.size()) ==
               std::string::npos);
        expect(result.diagnostics.find("does not fit") == std::string::npos);
    };

    // Service versions are strictly parsed as SemVer 2.0.0 triples and are
    // parsed once (sema stores the result for the emitters).
    "schema/sema validates service version components"_test = [] {
        const auto overflow{
            compile({schema_source{"v1.ridl", refrain::schema::dialect::ridl,
                                   R"(module refrain::services::v1;
version 1.0.0;
service svc : 999999999999.0 {
    side = server;
    thread = any;
    provides = [ "refrain:svc" ];
    fn ping() -> u32;
}
)"}})};
        expect(!overflow.program.has_value());
        expect(overflow.diagnostics.find("invalid service version") !=
               std::string::npos);

        const auto leading_zero{
            compile({schema_source{"v2.ridl", refrain::schema::dialect::ridl,
                                   R"(module refrain::services::v2;
version 1.0.0;
service svc : 01.0.0 {
    side = server;
    thread = any;
    provides = [ "refrain:svc" ];
    fn ping() -> u32;
}
)"}})};
        expect(!leading_zero.program.has_value());
        expect(leading_zero.diagnostics.find("invalid service version") !=
               std::string::npos);

        // A two-part version is no longer a valid contract version.
        const auto partial{
            compile({schema_source{"v3.ridl", refrain::schema::dialect::ridl,
                                   R"(module refrain::services::v3;
version 1.0.0;
service svc : 2.7 {
    side = server;
    thread = any;
    provides = [ "refrain:svc" ];
    fn ping() -> u32;
}
)"}})};
        expect(!partial.program.has_value());
        expect(partial.diagnostics.find("invalid service version") !=
               std::string::npos);

        const auto valid{
            compile({schema_source{"v4.ridl", refrain::schema::dialect::ridl,
                                   R"(module refrain::services::v4;
version 1.0.0;
service svc : 2.7.3 {
    side = server;
    thread = any;
    provides = [ "refrain:svc" ];
    fn ping() -> u32;
}
)"}})};
        expect(valid.program.has_value());
        if (valid.program) {
            const auto &decl{
                valid.program->modules.front().declarations.back()};
            expect(decl.service_version_major == 2U);
            expect(decl.service_version_minor == 7U);
            expect(decl.service_version_patch == 3U);
        }
    };

    // Degenerate module names (no `refrain` root or a single segment) would
    // generate hidden files with reserved double-underscore guards, so sema
    // rejects them.
    "schema/sema rejects degenerate module names"_test = [] {
        const auto bare{
            compile({schema_source{"bare.rsc", refrain::schema::dialect::rsc,
                                   R"(module refrain;
version 1;
struct x { a: u32; }
)"}})};
        expect(!bare.program.has_value());
        expect(bare.diagnostics.find("at least two segments") !=
               std::string::npos);

        const auto not_rooted{compile(
            {schema_source{"notrooted.ridl", refrain::schema::dialect::ridl,
                           R"(module other::thing;
version 1.0.0;
struct x { u32 a; }
)"}})};
        expect(!not_rooted.program.has_value());
        expect(not_rooted.diagnostics.find("rooted at 'refrain'") !=
               std::string::npos);
    };

    // `a::b_c` and `a::b::c` collapse to the same C ABI prefix.
    "schema/sema rejects ambiguous C ABI prefixes"_test = [] {
        const auto result{
            compile({schema_source{"p1.ridl", refrain::schema::dialect::ridl,
                                   R"(module refrain::a_b;
version 1.0.0;
struct one { u32 x; }
)"},
                     schema_source{"p2.ridl", refrain::schema::dialect::ridl,
                                   R"(module refrain::a::b;
version 1.0.0;
struct two { u32 y; }
)"}})};
        expect(!result.program.has_value());
        expect(result.diagnostics.find("share the C ABI prefix") !=
               std::string::npos);
    };

    // Events are RIDL-only. RSC describes persisted data, so an `event` there
    // is a semantic (not a syntax) error.
    "schema/sema rejects event declarations in RSC"_test = [] {
        const auto result{
            compile({schema_source{"evt.rsc", refrain::schema::dialect::rsc,
                                   R"(module refrain::evt;
version 1;
event tick_event { tick: i64; }
)"}})};
        expect_rejected(result, "event is a RIDL declaration");
    };

    // Event field whitelist: scalars, string, bytes, slice<scalar> only.
    "schema/sema rejects event field types outside the whitelist"_test = [] {
        struct rejection_case {
            std::string_view declarations;
            std::string_view marker;
        };
        constexpr std::array cases{
            rejection_case{R"(struct payload { u32 x; }
event bad { payload value; }
)",
                           "references struct 'payload'"},
            rejection_case{R"(handle<ctx>;
event bad { ctx value; }
)",
                           "is a handle"},
            rejection_case{R"(event bad { array<u32> values; }
)",
                           "uses 'array<T>'"},
            rejection_case{R"(event bad { optional<u32> value; }
)",
                           "uses 'optional<T>'"},
            rejection_case{R"(event bad { repeated<u32> values; }
)",
                           "uses 'repeated<T>'"},
            rejection_case{R"(event bad { slice<string> names; }
)",
                           "must be 'slice<scalar>'"},
            rejection_case{R"(event bad { slice<slice<u32>> nested; }
)",
                           "must be 'slice<scalar>'"},
            rejection_case{R"(struct payload { u32 x; }
event bad { slice<payload> items; }
)",
                           "must be 'slice<scalar>'"},
            rejection_case{R"(enum mode : u32 { on = 1 }
event bad { mode value; }
)",
                           "references enum 'mode'"},
        };
        for (const auto &rejection : cases) {
            const auto result{
                compile_ridl(ridl_module(rejection.declarations))};
            expect_rejected(result, rejection.marker);
        }
    };

    // Events carry only the published payload: no default values, no
    // `#[default]`.
    "schema/sema rejects event field defaults"_test = [] {
        const auto implicit_default{
            compile_ridl(ridl_module("event bad { u32 a = 1; }\n"))};
        expect_rejected(implicit_default, "cannot have a default value");

        const auto attribute_default{
            compile_ridl(ridl_module("event bad { #[default(1)] u32 a; }\n"))};
        expect_rejected(attribute_default, "cannot use '#[default]'");
    };

    // A payload-less event has no defined layout: the padding algorithm yields
    // size 0 while a C++ empty struct is one byte, so the generated
    // `static_assert(sizeof(...) == 0)` could never hold. Sema fails closed
    // instead of inventing a layout.
    "schema/sema rejects payload-less events"_test = [] {
        const auto result{compile_ridl(ridl_module("event empty {}\n"))};
        expect_rejected(result, "must declare at least one field");
    };

    // `#[batch]` needs exactly one `slice<scalar>` field.
    "schema/sema validates #[batch] event shape"_test = [] {
        const auto not_slice{
            compile_ridl(ridl_module("#[batch]\nevent e { u32 a; }\n"))};
        expect_rejected(not_slice, "must declare a 'slice<scalar>' payload");

        const auto scalar_slice{compile_ridl(
            ridl_module("#[batch]\nevent e { slice<string> a; }\n"))};
        expect_rejected(scalar_slice, "must be 'slice<scalar>'");

        const auto two_fields{compile_ridl(ridl_module(
            "#[batch]\nevent e { slice<u32> a; slice<u32> b; }\n"))};
        expect_rejected(two_fields, "must declare exactly one field");

        const auto on_struct{
            compile_ridl(ridl_module("#[batch]\nstruct s { u32 a; }\n"))};
        expect_rejected(on_struct,
                        "'#[batch]' is only valid on event declarations");

        const auto with_arguments{compile_ridl(
            ridl_module("#[batch(1)]\nevent e { slice<u32> a; }\n"))};
        expect_rejected(with_arguments, "'#[batch]' takes no arguments");

        // Positive control: a scalar slice event is legal.
        const auto valid{compile_ridl(ridl_module(
            "#[batch]\nevent block_changed { slice<u32> states; }\n"))};
        expect(valid.program.has_value());
        expect(valid.diagnostics.empty());
    };

    // `#[cancellable]` is event-only metadata with no arguments.
    "schema/sema validates #[cancellable] context"_test = [] {
        const auto on_struct{
            compile_ridl(ridl_module("#[cancellable]\nstruct s { u32 a; }\n"))};
        expect_rejected(on_struct,
                        "'#[cancellable]' is only valid on event declarations");

        const auto with_arguments{compile_ridl(
            ridl_module("#[cancellable(x)]\nevent e { u32 a; }\n"))};
        expect_rejected(with_arguments, "'#[cancellable]' takes no arguments");

        const auto valid{
            compile_ridl(ridl_module("#[cancellable]\nevent e { u32 a; }\n"))};
        expect(valid.program.has_value());
        if (valid.program) {
            const auto &decl{valid.program->modules.front().declarations[0]};
            expect(decl.event_cancellable);
            expect(decl.event_batch == false);
        }
    };

    // Events are not value types (a struct field cannot name one); naming one
    // would otherwise be a silent ABI-layout dead end.
    "schema/sema rejects events used as field types"_test = [] {
        const auto result{
            compile_ridl(ridl_module("event tick_event { i64 tick; }\n"
                                     "struct holder { tick_event value; }\n"))};
        expect_rejected(result,
                        "events cannot be referenced by name as field types");
    };

    // `#[schedule]` is service-method only.
    "schema/sema rejects #[schedule] outside service methods"_test = [] {
        const auto result{compile_ridl(ridl_module("#[schedule(interval=1)]\n"
                                                   "struct s { u32 a; }\n"))};
        expect_rejected(result,
                        "'#[schedule]' is only valid on service methods");
    };

    // `#[schedule]` arguments: named only, interval required and in range,
    // phase vocabulary fixed, unknown/repeated arguments rejected.
    "schema/sema validates #[schedule] arguments"_test = [] {
        struct rejection_case {
            std::string_view declaration;
            std::string_view marker;
        };
        constexpr std::array cases{
            rejection_case{"#[schedule] fn m() -> void;",
                           "requires the named argument 'interval'"},
            rejection_case{"#[schedule(phase=pre_tick)] fn m() -> void;",
                           "requires the named argument 'interval'"},
            rejection_case{"#[schedule(1)] fn m() -> void;",
                           "arguments must be named"},
            rejection_case{"#[schedule(interval=0)] fn m() -> void;",
                           "interval 0 is out of range"},
            rejection_case{"#[schedule(interval=1000001)] fn m() -> void;",
                           "interval 1000001 is out of range"},
            rejection_case{"#[schedule(interval=1.5)] fn m() -> void;",
                           "must be a decimal integer"},
            rejection_case{"#[schedule(interval=abc)] fn m() -> void;",
                           "must be a decimal integer"},
            rejection_case{"#[schedule(interval=0x10)] fn m() -> void;",
                           "must be a decimal integer"},
            rejection_case{"#[schedule(interval=1, phase=mid_tick)] fn m() -> "
                           "void;",
                           "invalid '#[schedule]' phase 'mid_tick'"},
            rejection_case{"#[schedule(interval=1, delay=2)] fn m() -> void;",
                           "unknown '#[schedule]' named argument 'delay'"},
            rejection_case{"#[schedule(interval=1, interval=2)] fn m() -> "
                           "void;",
                           "repeats the named argument 'interval'"},
            rejection_case{"#[schedule(interval=1, phase=pre_tick, "
                           "phase=post_tick)] fn m() -> void;",
                           "repeats the named argument 'phase'"},
            rejection_case{"#[schedule(interval=1)] fn m(u32 x) -> void;",
                           "must take no parameters"},
            rejection_case{"#[schedule(interval=1)] fn m() -> u32;",
                           "must return 'void' or 'result<void, E>'"},
            rejection_case{"#[schedule(interval=1)] fn m() -> "
                           "result<u32, sched_error>;",
                           "must return 'void' or 'result<void, E>'"},
            rejection_case{"#[schedule(interval=1)] fn m() -> "
                           "result<void, sched_error>;",
                           ""},
        };
        for (const auto &rejection : cases) {
            const auto result{
                compile_ridl(ridl_service(rejection.declaration))};
            if (rejection.marker.empty()) {
                expect(result.program.has_value())
                    << "expected acceptance: " << rejection.declaration;
                continue;
            }
            expect_rejected(result, rejection.marker);
        }
    };

    // The parsed metadata (interval + phase, default post_tick) reaches the
    // model the emitter consumes.
    "schema/sema records #[schedule] metadata"_test = [] {
        const auto result{compile_ridl(ridl_service(
            "#[thread(sim)] #[can_fail] "
            "#[schedule(interval=5, phase=pre_tick)] "
            "fn maintenance_tick() -> result<void, sched_error>;\n"
            "#[schedule(interval=20)] fn quiet_tick() -> void;\n"))};
        expect(result.program.has_value());
        if (!result.program) {
            return;
        }
        const auto &service{result.program->modules.front().declarations[1]};
        expect(service.methods.size() == 2U);
        if (service.methods.size() != 2U) {
            return;
        }
        expect(service.methods[0].scheduled);
        expect(service.methods[0].schedule_interval_ticks == 5);
        expect(service.methods[0].schedule_phase_of ==
               refrain::schema::schedule_phase_kind::pre_tick);
        expect(service.methods[1].scheduled);
        expect(service.methods[1].schedule_interval_ticks == 20);
        // `phase` defaults to post_tick.
        expect(service.methods[1].schedule_phase_of ==
               refrain::schema::schedule_phase_kind::post_tick);
    };
};

} // namespace
