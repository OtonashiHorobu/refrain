// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <schema/schema.hpp>

#include <schema/schema_test_util.hpp>

#include <boost/ut.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace boost::ut;
using refrain::test::schema_source;

constexpr std::string_view demo_schema{R"(module refrain::demo;
version 1.0.0;

handle<thing>;

enum demo_error : u32 { failed = 1 }

struct config { u64 seed; string name; }
struct outcome { u32 value; }

service demo : 1.0.0 {
    side = server;
    thread = worker;
    provides = [ "refrain:demo" ];
    requires = [ "refrain:core>=1.0.0" ];
    capabilities = [ "demo-capability" ];

    #[thread(worker)] #[can_fail]
    fn run(in config cfg) -> result<outcome, demo_error>;
}
)"};

[[nodiscard]] auto generate_demo()
    -> refrain::result<refrain::schema::generation> {
    std::vector<refrain::schema::source_file> sources;
    sources.push_back(refrain::schema::source_file{
        "idl/refrain/demo.ridl", refrain::schema::dialect::ridl,
        std::string{demo_schema}});
    return refrain::schema::generate_from_sources(std::move(sources));
}

[[nodiscard]] auto find_file(const refrain::schema::generation &result,
                             std::string_view path) -> const std::string * {
    const auto found{std::ranges::find_if(
        result.files, [path](const refrain::schema::generated_file &file) {
            return file.path == path;
        })};
    return found == result.files.end() ? nullptr : &found->content;
}

const suite schema_generator_tests = [] {
    "schema/generator emits deterministic C ABI and C++ headers"_test = [] {
        const auto first{generate_demo()};
        expect(first.has_value());
        if (!first) {
            return;
        }
        expect(first->source_count == 1U);
        // c abi + cpp + 2 aggregators + the reflection index.
        expect(first->files.size() == 5U);
        expect(first->schema_hash.size() == 16U);

        const auto *abi{find_file(*first, "c/refrain/contracts/demo_abi.h")};
        expect(abi != nullptr);
        if (abi != nullptr) {
            expect(abi->find("REFRAIN_DEMO_SERVICE_ID \"refrain:demo\"") !=
                   std::string::npos);
            expect(abi->find("typedef struct refrain_demo_vtable") !=
                   std::string::npos);
            expect(abi->find("refrain_status (*run)") != std::string::npos);
            expect(abi->find("uint32_t struct_size;") != std::string::npos);
            // SemVer 2.0.0 ABI metadata: the full release triple is emitted.
            expect(abi->find("#define REFRAIN_DEMO_ABI_MAJOR 1u") !=
                   std::string::npos);
            expect(abi->find("#define REFRAIN_DEMO_ABI_MINOR 0u") !=
                   std::string::npos);
            expect(abi->find("#define REFRAIN_DEMO_ABI_PATCH 0u") !=
                   std::string::npos);
            // Composition defaults to `exclusive` when the schema omits it.
            expect(abi->find("#define REFRAIN_DEMO_COMPOSITION "
                             "REFRAIN_COMPOSITION_EXCLUSIVE") !=
                   std::string::npos);
            expect(abi->find("#define REFRAIN_DEMO_REQUIRES "
                             "\"refrain:core>=1.0.0\"") != std::string::npos);
            // The former `reserved_0` slot now carries the patch version; the
            // vtable layout (4 header words) is unchanged.
            expect(abi->find("    uint32_t abi_patch;") != std::string::npos);
            // The old vtable header word is gone (struct-padding members keep
            // their `uint8_t reserved_N[]` spelling).
            expect(abi->find("uint32_t reserved_0;") == std::string::npos);
            expect(abi->find("Schema hash: " + first->schema_hash) !=
                   std::string::npos);
        }

        const auto *cpp{find_file(*first, "cpp/refrain/contracts/demo.hpp")};
        expect(cpp != nullptr);
        if (cpp != nullptr) {
            expect(cpp->find("class provider final") != std::string::npos);
            expect(cpp->find("std::expected<") != std::string::npos);
            expect(cpp->find("static_assert(sizeof(config)") !=
                   std::string::npos);
            expect(cpp->find("offsetof(config, seed)") != std::string::npos);
            expect(cpp->find("inline constexpr std::uint32_t abi_patch = 0;") !=
                   std::string::npos);
            expect(cpp->find("::refrain::contracts::composition_kind "
                             "composition = "
                             "::refrain::contracts::composition_kind::"
                             "exclusive") != std::string::npos);
            // Module headers never carry reflection macros or the backend
            // include; metadata lives in `cpp/refrain/reflection.hpp`.
            expect(cpp->find("REFL_AUTO") == std::string::npos);
            expect(cpp->find("REFR_REFLECT") == std::string::npos);
            expect(cpp->find("<refrain/reflect.hpp>") == std::string::npos);
        }

        // The C++ output set also carries the reflection index. It must be
        // generated without REFR_REFLECT/REFL_AUTO so the normal module headers
        // never depend on the reflection backend.
        const auto *reflection{find_file(*first, "cpp/refrain/reflection.hpp")};
        expect(reflection != nullptr);
        if (reflection != nullptr) {
            expect(reflection->find("#if defined(REFRAIN_USE_REFLECTION) && "
                                    "REFRAIN_USE_REFLECTION") !=
                   std::string::npos);
            expect(
                reflection->find("template <> struct custom_traits<::refrain:"
                                 ":demo::config>") != std::string::npos);
            expect(reflection->find("custom_field{\"seed\", "
                                    "&::refrain::demo::config::seed}") !=
                   std::string::npos);
            expect(reflection->find("custom_field{\"name\", "
                                    "&::refrain::demo::config::name}") !=
                   std::string::npos);
            expect(reflection->find("REFL_AUTO") == std::string::npos);
            expect(reflection->find("REFR_REFLECT") == std::string::npos);
        }

        const auto second{generate_demo()};
        expect(second.has_value());
        if (!second) {
            return;
        }
        expect(second->schema_hash == first->schema_hash);
        expect(second->files.size() == first->files.size());
        for (std::size_t index = 0; index < first->files.size(); ++index) {
            expect(first->files[index].path == second->files[index].path);
            expect(first->files[index].content == second->files[index].content);
        }
    };

    "schema/generator reports parse diagnostics as errors"_test = [] {
        std::vector<refrain::schema::source_file> sources;
        sources.push_back(refrain::schema::source_file{
            "idl/broken.rsc", refrain::schema::dialect::rsc,
            "module refrain::broken;\nversion 1;\nstruct {}\n"});
        const auto result{
            refrain::schema::generate_from_sources(std::move(sources))};
        expect(!result.has_value());
        if (!result) {
            expect(result.error().code == refrain::error_code::parse_error);
            expect(result.error().message.find("error:") != std::string::npos);
        }
    };

    "schema/write + check detect drift, missing and stale files"_test = [] {
        const auto generated{generate_demo()};
        expect(generated.has_value());
        if (!generated) {
            return;
        }
        refrain::test::temp_directory output;
        expect(static_cast<bool>(
            refrain::schema::write_files(*generated, output.path())));

        auto report{refrain::schema::check_files(*generated, output.path())};
        expect(report.has_value());
        if (!report) {
            return;
        }
        expect(report->ok());

        // Change a generated file by hand: drift must be reported.
        const auto target{output.path() / "c/refrain/contracts/demo_abi.h"};
        {
            std::ofstream stream{target, std::ios::app};
            stream << "// hand edited\n";
        }
        report = refrain::schema::check_files(*generated, output.path());
        expect(report.has_value());
        if (report) {
            expect(!report->changed.empty());
            expect(std::ranges::find(
                       report->changed,
                       std::string{"c/refrain/contracts/demo_abi.h"}) !=
                   report->changed.end());
        }

        // Stale header (generated for a schema that no longer exists).
        {
            const auto stale{output.path() / "cpp/refrain/contracts/old.hpp"};
            std::ofstream stream{stale};
            stream << "// stale\n";
        }
        report = refrain::schema::check_files(*generated, output.path());
        expect(report.has_value());
        if (report) {
            expect(std::ranges::find(
                       report->stale,
                       std::string{"cpp/refrain/contracts/old.hpp"}) !=
                   report->stale.end());
        }

        // A leftover non-header artifact is stale too, not only .h/.hpp files.
        {
            const auto stale{output.path() / "notes.json"};
            std::ofstream stream{stale};
            stream << "{}\n";
        }
        report = refrain::schema::check_files(*generated, output.path());
        expect(report.has_value());
        if (report) {
            expect(
                std::ranges::find(report->stale, std::string{"notes.json"}) !=
                report->stale.end());
        }

        // Atomic writes never leave `.tmp` siblings behind.
        std::error_code error;
        for (const auto &entry : std::filesystem::recursive_directory_iterator{
                 output.path(), error}) {
            expect(!entry.path().string().ends_with(".tmp"));
        }
    };

    // The banner is self-describing (format version), and generated
    // constructors/wrappers keep `struct_size` valid across the ABI.
    "schema/generated headers describe the format and initialise struct_size"_test =
        [] {
            const auto generated{generate_demo()};
            expect(generated.has_value());
            if (!generated) {
                return;
            }
            const auto *abi{
                find_file(*generated, "c/refrain/contracts/demo_abi.h")};
            expect(abi != nullptr);
            if (abi != nullptr) {
                expect(abi->find("Schema format: v1") != std::string::npos);
                expect(abi->find("Schema hash: " + generated->schema_hash) !=
                       std::string::npos);
            }

            const auto *cpp{
                find_file(*generated, "cpp/refrain/contracts/demo.hpp")};
            expect(cpp != nullptr);
            if (cpp != nullptr) {
                expect(cpp->find("make_config()") != std::string::npos);
                expect(cpp->find("with_struct_size(cfg)") != std::string::npos);
                expect(cpp->find("init_struct(value);") != std::string::npos);
            }
        };

    // Every `provides` entry is exposed, not silently dropped after the first
    // one.
    "schema/all provides entries are generated"_test = [] {
        std::vector<refrain::schema::source_file> sources;
        sources.push_back(refrain::schema::source_file{
            "idl/refrain/multi.ridl", refrain::schema::dialect::ridl,
            R"(module refrain::multi;
version 1.0.0;

service multi : 1.0.0 {
    side = server;
    thread = any;
    provides = [ "refrain:multi", "refrain:multi-alias" ];
    fn ping() -> u32;
}
)"});
        const auto generated{
            refrain::schema::generate_from_sources(std::move(sources))};
        expect(generated.has_value());
        if (!generated) {
            return;
        }
        const auto *abi{
            find_file(*generated, "c/refrain/contracts/multi_abi.h")};
        expect(abi != nullptr);
        if (abi != nullptr) {
            expect(abi->find("_SERVICE_IDS \"refrain:multi,refrain:multi-"
                             "alias\"") != std::string::npos);
        }
        const auto *cpp{
            find_file(*generated, "cpp/refrain/contracts/multi.hpp")};
        expect(cpp != nullptr);
        if (cpp != nullptr) {
            expect(cpp->find("service_ids[] = {\"refrain:multi\", "
                             "\"refrain:multi-alias\"}") != std::string::npos);
        }
    };

    // Container field dependencies take part in declaration ordering, so a
    // later-declared element type still yields a compilable header.
    "schema/RSC container element dependencies are ordered first"_test = [] {
        std::vector<refrain::schema::source_file> sources;
        sources.push_back(refrain::schema::source_file{
            "schema/test/order.rsc", refrain::schema::dialect::rsc,
            R"(module refrain::rscorder;
version 1;
struct uses_optional { later: optional<later_struct>; }
struct uses_vector { later2: array<later2_struct>; }
struct later_struct { x: u32; }
struct later2_struct { y: u32; }
)"});
        const auto generated{
            refrain::schema::generate_from_sources(std::move(sources))};
        expect(generated.has_value());
        if (!generated) {
            return;
        }
        const auto *header{
            find_file(*generated, "cpp/refrain/schema/test/order.hpp")};
        expect(header != nullptr);
        if (header != nullptr) {
            const auto later{header->find("struct later_struct")};
            const auto uses{header->find("struct uses_optional")};
            expect(later != std::string::npos);
            expect(uses != std::string::npos);
            expect(later < uses);
            const auto later2{header->find("struct later2_struct")};
            const auto uses2{header->find("struct uses_vector")};
            expect(later2 != std::string::npos);
            expect(uses2 != std::string::npos);
            expect(later2 < uses2);
        }

        // The reflection index covers RSC-only output sets too: it includes
        // the RSC header that declares the records (and must not pull in the
        // RIDL aggregate when no RIDL module exists).
        const auto *reflection{
            find_file(*generated, "cpp/refrain/reflection.hpp")};
        expect(reflection != nullptr);
        if (reflection != nullptr) {
            expect(
                reflection->find("#include <refrain/schema/test/order.hpp>") !=
                std::string::npos);
            expect(reflection->find("#include <refrain/contracts.hpp>") ==
                   std::string::npos);
            expect(reflection->find(
                       "custom_traits<::refrain::rscorder::later_struct>") !=
                   std::string::npos);
        }
    };

    // Contract metadata: a declared composition mode and a non-zero SemVer
    // patch are emitted into both the C ABI macros and the C++ constexpr
    // surface.
    "schema/generator emits composition mode and SemVer patch metadata"_test =
        [] {
            std::vector<refrain::schema::source_file> sources;
            sources.push_back(refrain::schema::source_file{
                "idl/refrain/modes.ridl", refrain::schema::dialect::ridl,
                R"(module refrain::modes;
version 2.7.3;

service modes : 2.7.3 {
    side = client;
    thread = any;
    composition = instanced;
    provides = [ "refrain:modes" ];
    fn ping() -> u32;
}
)"});
            const auto generated{
                refrain::schema::generate_from_sources(std::move(sources))};
            expect(generated.has_value());
            if (!generated) {
                return;
            }
            const auto *abi{
                find_file(*generated, "c/refrain/contracts/modes_abi.h")};
            expect(abi != nullptr);
            if (abi != nullptr) {
                expect(abi->find("#define REFRAIN_MODES_ABI_MAJOR 2u") !=
                       std::string::npos);
                expect(abi->find("#define REFRAIN_MODES_ABI_MINOR 7u") !=
                       std::string::npos);
                expect(abi->find("#define REFRAIN_MODES_ABI_PATCH 3u") !=
                       std::string::npos);
                expect(abi->find("#define REFRAIN_MODES_COMPOSITION "
                                 "REFRAIN_COMPOSITION_INSTANCED") !=
                       std::string::npos);
                expect(abi->find("uint32_t abi_patch;") != std::string::npos);
            }
            const auto *cpp{
                find_file(*generated, "cpp/refrain/contracts/modes.hpp")};
            expect(cpp != nullptr);
            if (cpp != nullptr) {
                expect(cpp->find("std::uint32_t abi_patch = 3;") !=
                       std::string::npos);
                expect(cpp->find("composition_kind::instanced") !=
                       std::string::npos);
            }
        };

    // Emitter contract: event structs with explicit padding/assertions and
    // stable ids, `event_traits`, per-method `#[schedule]` trampolines and the
    // `schedule_tasks` descriptor table. Inline RIDL keeps the test
    // self-contained because production schemas do not declare schedules yet.
    "schema/generator emits event layout, ids, traits and schedule tables"_test =
        [] {
            constexpr std::string_view schema{
                R"(module refrain::services::schedprobe;
version 1.0.0;

enum schedprobe_error : u32 { failed = 1 }

/// Published once per tick.
event tick_event {
    i64 tick;
}

#[batch]
event block_changed {
    slice<u32> states;
}

#[cancellable]
event dismissed {
    u8 reason;
}

event padded {
    u8 a;
    i64 b;
    u8 c;
}

struct payload { u32 value; }

service schedprobe : 1.0.0 {
    side = server;
    thread = sim;
    provides = [ "refrain:schedprobe" ];

    #[thread(sim)] #[can_fail] #[schedule(interval=5, phase=pre_tick)]
    fn maintenance_tick() -> result<void, schedprobe_error>;

    #[thread(sim)] #[schedule(interval=20)]
    fn quiet_tick() -> void;
}
)"};
            std::vector<refrain::schema::source_file> sources;
            sources.push_back(refrain::schema::source_file{
                "idl/refrain/schedprobe.ridl", refrain::schema::dialect::ridl,
                std::string{schema}});
            const auto generated{
                refrain::schema::generate_from_sources(std::move(sources))};
            expect(generated.has_value());
            if (!generated) {
                return;
            }

            const auto *cpp{find_file(
                *generated, "cpp/refrain/contracts/services/schedprobe.hpp")};
            expect(cpp != nullptr);
            const auto *abi{find_file(
                *generated, "c/refrain/contracts/services/schedprobe_abi.h")};
            expect(abi != nullptr);
            if (cpp == nullptr || abi == nullptr) {
                return;
            }

            // Includes required by the generated code.
            expect(cpp->find("#include <array>") != std::string::npos);
            expect(cpp->find("#include <string>") != std::string::npos);
            expect(cpp->find("#include <refrain/contracts/event.hpp>") !=
                   std::string::npos);
            expect(cpp->find("#include <refrain/core/error.hpp>") !=
                   std::string::npos);
            expect(cpp->find("#include <refrain/core/schedule.hpp>") !=
                   std::string::npos);

            // Event struct: declaration-order fields, no `struct_size`, and
            // explicit padding computed by the contract struct algorithm.
            expect(cpp->find("/// Published once per tick.\nstruct tick_event "
                             "{\n    std::int64_t tick;\n};") !=
                   std::string::npos);
            expect(cpp->find("static_assert(sizeof(tick_event) == 8);") !=
                   std::string::npos);
            expect(cpp->find("static_assert(alignof(tick_event) == 8);") !=
                   std::string::npos);
            expect(
                cpp->find("static_assert(offsetof(tick_event, tick) == 0);") !=
                std::string::npos);
            expect(
                cpp->find("struct tick_event {\n    uint32_t struct_size;") ==
                std::string::npos);

            expect(cpp->find("    std::span<const std::uint32_t> states;") !=
                   std::string::npos);
            expect(cpp->find("static_assert(sizeof(block_changed) == 16);") !=
                   std::string::npos);
            expect(cpp->find("static_assert(alignof(block_changed) == 8);") !=
                   std::string::npos);

            // padding: u8 a; <7 bytes>; i64 b; u8 c; <7 bytes>; size 24
            expect(cpp->find("struct padded {\n"
                             "    std::uint8_t a;\n"
                             "    std::uint8_t reserved_0[7];\n"
                             "    std::int64_t b;\n"
                             "    std::uint8_t c;\n"
                             "    std::uint8_t reserved_1[7];\n"
                             "};") != std::string::npos);
            expect(cpp->find("static_assert(sizeof(padded) == 24);") !=
                   std::string::npos);
            expect(cpp->find("static_assert(alignof(padded) == 8);") !=
                   std::string::npos);
            expect(cpp->find("static_assert(offsetof(padded, b) == 8);") !=
                   std::string::npos);
            expect(cpp->find("static_assert(offsetof(padded, c) == 16);") !=
                   std::string::npos);

            // Stable ids: `refrain:event/<module '/'-separated>/<Event>`.
            expect(cpp->find("inline constexpr std::string_view tick_event_id "
                             "=") != std::string::npos);
            expect(cpp->find("\"refrain:event/refrain/services/schedprobe/"
                             "tick_event\"") != std::string::npos);
            expect(cpp->find("\"refrain:event/refrain/services/schedprobe/"
                             "block_changed\"") != std::string::npos);

            // Events sit after the handles and before the contract structs.
            const auto first_event{cpp->find("struct tick_event {")};
            const auto payload_alias{cpp->find("using payload =")};
            expect(first_event != std::string::npos);
            expect(payload_alias != std::string::npos);
            if (first_event != std::string::npos &&
                payload_alias != std::string::npos) {
                expect(first_event < payload_alias);
            }

            // `event_traits` specializations live in `refrain::contracts`,
            // after the module namespace closes.
            expect(cpp->find("} // namespace refrain::services::schedprobe") !=
                   std::string::npos);
            expect(cpp->find("namespace refrain::contracts {") !=
                   std::string::npos);
            const auto namespace_close{
                cpp->find("} // namespace refrain::services::schedprobe")};
            const auto traits_namespace{
                cpp->find("namespace refrain::contracts {")};
            if (namespace_close != std::string::npos &&
                traits_namespace != std::string::npos) {
                expect(namespace_close < traits_namespace);
            }
            expect(cpp->find("template <> struct event_traits<::refrain::"
                             "services::schedprobe::tick_event> {") !=
                   std::string::npos);
            expect(cpp->find("    static constexpr bool is_event = true;") !=
                   std::string::npos);
            expect(cpp->find("        ::refrain::services::schedprobe::"
                             "tick_event_id;") != std::string::npos);
            expect(cpp->find("template <> struct event_traits<::refrain::"
                             "services::schedprobe::block_changed> {") !=
                   std::string::npos);
            expect(cpp->find("template <> struct event_traits<::refrain::"
                             "services::schedprobe::dismissed> {") !=
                   std::string::npos);

            // Batch/cancellable flags: exactly one true each.
            const auto batch_block{
                cpp->find("template <> struct event_traits<::"
                          "refrain::services::schedprobe::"
                          "block_changed> {")};
            const auto dismiss_block{cpp->find(
                "template <> struct event_traits<::refrain::services::"
                "schedprobe::dismissed> {")};
            expect(batch_block != std::string::npos);
            expect(dismiss_block != std::string::npos);
            if (batch_block != std::string::npos &&
                dismiss_block != std::string::npos) {
                const auto batch_text{
                    cpp->substr(batch_block, dismiss_block - batch_block)};
                expect(batch_text.find("    static constexpr bool batch = "
                                       "true;") != std::string::npos);
                expect(batch_text.find("    static constexpr bool cancellable "
                                       "= false;") != std::string::npos);
                const auto dismiss_end{cpp->find("};", dismiss_block)};
                expect(dismiss_end != std::string::npos);
                const auto dismiss_text{
                    dismiss_end == std::string::npos
                        ? std::string{}
                        : cpp->substr(dismiss_block,
                                      dismiss_end - dismiss_block)};
                expect(dismiss_text.find("    static constexpr bool batch = "
                                         "false;") != std::string::npos);
                expect(
                    dismiss_text.find("    static constexpr bool cancellable "
                                      "= true;") != std::string::npos);
            }

            // Per-method trampolines with the fixed invoker signature.
            expect(cpp->find("namespace schedule_detail {") !=
                   std::string::npos);
            expect(cpp->find("inline auto invoke_0(void *self, const void "
                             "*vtable, std::int64_t tick) noexcept") !=
                   std::string::npos);
            expect(cpp->find("    -> ::refrain::status {") !=
                   std::string::npos);
            expect(cpp->find("    const auto provider{::refrain::services::"
                             "schedprobe::provider{") != std::string::npos);
            expect(cpp->find("*static_cast<const "
                             "::refrain_services_schedprobe_vtable "
                             "*>(vtable), self}};") != std::string::npos);
            expect(
                cpp->find("    if (auto result{provider.maintenance_tick()}; "
                          "!result) {") != std::string::npos);
            // A failed `result` maps to error_code::unknown with the
            // `<service_id>.<method> failed` message.
            expect(cpp->find("            ::refrain::error_code::unknown,") !=
                   std::string::npos);
            expect(cpp->find("            std::string{\"refrain:schedprobe."
                             "maintenance_tick failed\"})};") !=
                   std::string::npos);
            // A plain void method needs no error mapping.
            expect(cpp->find("inline auto invoke_1(void *self, const void "
                             "*vtable, std::int64_t tick) noexcept") !=
                   std::string::npos);
            expect(cpp->find("    static_cast<void>(provider.quiet_tick());") !=
                   std::string::npos);

            // Descriptor table in declaration order, with parsed interval and
            // phase (omitted phase defaults to post_tick).
            expect(cpp->find("inline constexpr std::array<::refrain::core::"
                             "schedule::task_descriptor, 2>") !=
                   std::string::npos);
            expect(cpp->find("    schedule_tasks{{") != std::string::npos);
            expect(cpp->find("            .service = service_id,") !=
                   std::string::npos);
            expect(cpp->find("            .method = \"maintenance_tick\",") !=
                   std::string::npos);
            expect(cpp->find("            .interval_ticks = 5,") !=
                   std::string::npos);
            expect(
                cpp->find("            .phase_of = ::refrain::core::schedule:"
                          ":phase::pre_tick,") != std::string::npos);
            expect(cpp->find("            .invoke = "
                             "&schedule_detail::invoke_0,") !=
                   std::string::npos);
            expect(cpp->find("            .method = \"quiet_tick\",") !=
                   std::string::npos);
            expect(cpp->find("            .interval_ticks = 20,") !=
                   std::string::npos);
            expect(
                cpp->find("            .phase_of = ::refrain::core::schedule:"
                          ":phase::post_tick,") != std::string::npos);
            expect(cpp->find("            .invoke = "
                             "&schedule_detail::invoke_1,") !=
                   std::string::npos);

            // C header: only the event id macro, never an event payload struct.
            expect(abi->find("#define "
                             "REFRAIN_SERVICES_SCHEDPROBE_EVENT_TICK_EVENT_ID "
                             "\"refrain:event/refrain/services/schedprobe/"
                             "tick_event\"") != std::string::npos);
            expect(abi->find("#define "
                             "REFRAIN_SERVICES_SCHEDPROBE_EVENT_BLOCK_CHANGED_"
                             "ID") != std::string::npos);
            expect(abi->find("typedef struct refrain_services_schedprobe_tick_"
                             "event") == std::string::npos);
            expect(abi->find("refrain_services_schedprobe_tick_event") ==
                   std::string::npos);
            // Contract structs still get their C ABI definition.
            expect(abi->find("typedef struct refrain_services_schedprobe_"
                             "payload") != std::string::npos);
        };

    // A service without `#[schedule]` methods still exposes the empty table so
    // hosts can uniformly take `std::span{schedule_tasks}`; it must not pull in
    // the event header when the module declares no events.
    "schema/generator emits an empty schedule table and no event include"_test =
        [] {
            const auto generated{generate_demo()};
            expect(generated.has_value());
            if (!generated) {
                return;
            }
            const auto *cpp{
                find_file(*generated, "cpp/refrain/contracts/demo.hpp")};
            expect(cpp != nullptr);
            if (cpp == nullptr) {
                return;
            }
            expect(cpp->find("inline constexpr std::array<::refrain::core::"
                             "schedule::task_descriptor, 0>\n"
                             "    schedule_tasks{};") != std::string::npos);
            expect(cpp->find("namespace schedule_detail") == std::string::npos);
            expect(cpp->find("#include <refrain/contracts/event.hpp>") ==
                   std::string::npos);
            expect(cpp->find("event_traits") == std::string::npos);
        };

    // Attribute strings accept `\n` escapes; the emitter must escape them
    // instead of emitting raw lines that escape the `//` comment.
    "schema/attribute text cannot inject generated code"_test = [] {
        std::vector<refrain::schema::source_file> sources;
        sources.push_back(refrain::schema::source_file{
            "idl/refrain/inject.ridl", refrain::schema::dialect::ridl,
            R"(module refrain::inject;
version 1.0.0;
#[since("1.0\nstruct injected { u32 evil; }\n//")] struct target { u32 value; }
)"});
        const auto generated{
            refrain::schema::generate_from_sources(std::move(sources))};
        expect(generated.has_value());
        if (!generated) {
            return;
        }
        // Struct-level attributes are echoed in the C ABI header's comment.
        const auto *abi{
            find_file(*generated, "c/refrain/contracts/inject_abi.h")};
        expect(abi != nullptr);
        if (abi != nullptr) {
            expect(abi->find("\nstruct injected") == std::string::npos);
            expect(abi->find("\\nstruct injected") != std::string::npos);
        }
    };

    // A doc line ending in a backslash would splice with the following line in
    // translation phase 2, turning the next declaration into part of the `//`
    // comment. The emitter must break the splice with a trailing space.
    "schema/doc lines ending in a backslash keep the next declaration"_test =
        [] {
            std::vector<refrain::schema::source_file> sources;
            sources.push_back(refrain::schema::source_file{
                "idl/refrain/docs.ridl", refrain::schema::dialect::ridl,
                R"(module refrain::docs;
version 1.0.0;
/// doc line ending in a backslash \
struct tag { u32 value; }
)"});
            const auto generated{
                refrain::schema::generate_from_sources(std::move(sources))};
            expect(generated.has_value());
            if (!generated) {
                return;
            }
            const auto *abi{
                find_file(*generated, "c/refrain/contracts/docs_abi.h")};
            expect(abi != nullptr);
            if (abi != nullptr) {
                // The backslash is followed by a space before the newline.
                expect(abi->find("// doc line ending in a backslash \\ \n") !=
                       std::string::npos);
                expect(abi->find("typedef struct refrain_docs_tag "
                                 "refrain_docs_tag;") != std::string::npos);
            }
            const auto *cpp{
                find_file(*generated, "cpp/refrain/contracts/docs.hpp")};
            expect(cpp != nullptr);
            if (cpp != nullptr) {
                expect(cpp->find("/// doc line ending in a backslash \\ \n") !=
                       std::string::npos);
                expect(cpp->find("using tag = ::refrain_docs_tag;") !=
                       std::string::npos);
            }
        };

    // Control bytes must use three-digit octal escapes: `\x01` followed by 'f'
    // is a single hexadecimal escape for 0x1F (maximal munch), which would
    // silently change the generated service id.
    "schema/control bytes use octal escapes in generated strings"_test = [] {
        std::string schema{R"(module refrain::control;
version 1.0.0;
service control : 1.0.0 {
    side = server;
    thread = any;
    provides = [ "refrain:ctl)"};
        schema += '\x01';
        schema += R"(f" ];
    fn ping() -> u32;
}
)";
        std::vector<refrain::schema::source_file> sources;
        sources.push_back(refrain::schema::source_file{
            "idl/refrain/control.ridl", refrain::schema::dialect::ridl,
            std::move(schema)});
        const auto generated{
            refrain::schema::generate_from_sources(std::move(sources))};
        expect(generated.has_value());
        if (!generated) {
            return;
        }
        const auto *abi{
            find_file(*generated, "c/refrain/contracts/control_abi.h")};
        expect(abi != nullptr);
        if (abi != nullptr) {
            expect(abi->find("refrain:ctl\\001f") != std::string::npos);
            expect(abi->find("refrain:ctl\\x1f") == std::string::npos);
            expect(abi->find('\x1f') == std::string::npos);
            expect(abi->find('\x01') == std::string::npos);
        }
        const auto *cpp{
            find_file(*generated, "cpp/refrain/contracts/control.hpp")};
        expect(cpp != nullptr);
        if (cpp != nullptr) {
            expect(cpp->find("refrain:ctl\\001f") != std::string::npos);
            expect(cpp->find('\x1f') == std::string::npos);
        }
    };
};

} // namespace
